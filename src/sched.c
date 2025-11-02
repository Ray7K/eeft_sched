#include "sched.h"
#include "ipc.h"
#include "list.h"
#include "log.h"
#include "platform.h"
#include "power_management.h"
#include "ring_buffer.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DEMAND_PADDING_PERCENT 0.25f

CoreState core_states[TOTAL_CORES];

static bool decision_point;

static const Task *task_lookup[MAX_TASKS + 1];

static inline float rand_between(float min, float max) {
  return min + (float)rand() / (float)RAND_MAX * (max - min);
}

static float generate_acet(Job *job) {
  uint8_t criticality_chance = rand() % 100;
  CriticalityLevel criticality_level = job->parent_task->criticality_level;

  if (criticality_chance < 1) {
    criticality_level = ASIL_D;
  } else if (criticality_chance < 5) {
    criticality_level = ASIL_C;
  } else if (criticality_chance < 15) {
    criticality_level = ASIL_B;
  } else if (criticality_chance < 30) {
    criticality_level = ASIL_A;
  } else {
    criticality_level = QM;
  }

  if (criticality_level < processor_state.system_criticality_level) {
    criticality_level = processor_state.system_criticality_level;
  }

  float acet = rand_between(0.01f, 1.0f) *
               (float)job->parent_task->wcet[criticality_level];

  return acet;
}

static const Task *find_task_by_id(uint32_t task_id) {
  if (task_id > MAX_TASKS)
    return NULL;
  return task_lookup[task_id];
}

float find_slack(uint16_t global_core_id, uint32_t t, float scaling_factor) {
  CoreState *core_state = &core_states[global_core_id];
  uint32_t current_time = processor_state.system_time;

  if (t <= current_time) {
    return 0;
  }

  float demand = 0;

  // Demand from the currently running job
  if (core_state->running_job != NULL) {
    if (core_state->running_job->virtual_deadline <= t) {
      demand += ceilf((core_state->running_job->wcet -
                       core_state->running_job->executed_time) /
                      scaling_factor);
    }
  }

  // Demand from jobs already in the ready queue
  Job *job;
  list_for_each_entry(job, &core_state->ready_queue, link) {
    if (job->virtual_deadline <= t) {
      demand += ceilf((job->wcet - job->executed_time) / scaling_factor);
    }
  }
  list_for_each_entry(job, &core_state->replica_queue, link) {
    if (job->virtual_deadline <= t) {
      demand += ceilf((job->wcet - job->executed_time) / scaling_factor);
    }
  }

  // Demand from future job arrivals of high-criticality tasks
  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const TaskAllocationMap *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const Task *task = find_task_by_id(instance->task_id);

      if (task->criticality_level >= core_state->local_criticality_level) {
        uint32_t wcet = task->wcet[core_state->local_criticality_level];
        uint32_t period = task->period;

        uint32_t arrival_time = (current_time / period + 1) * period;
        while (1) {
          uint32_t future_job_deadline =
              arrival_time +
              instance->tuned_deadlines[core_state->local_criticality_level];

          if (future_job_deadline > t) {
            break;
          }
          demand += ceilf((float)wcet / scaling_factor);
          arrival_time += period;
        }
      }
    }
  }

  float interval_length = (float)(t - current_time);

  if (demand >= interval_length) {
    return 0;
  }

  return interval_length - demand;
}

const Task *find_next_arrival_task(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];
  const Task *next_task = NULL;
  uint32_t min_arrival_time = UINT32_MAX;

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const TaskAllocationMap *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const Task *task = find_task_by_id(instance->task_id);
      if (!task || task->period == 0) {
        continue;
      }

      uint32_t current_time = processor_state.system_time;
      uint32_t remainder = current_time % task->period;
      uint32_t next_arrival = (remainder == 0)
                                  ? current_time
                                  : current_time + (task->period - remainder);

      if (next_arrival < min_arrival_time) {
        min_arrival_time = next_arrival;
        next_task = task;
      }
    }
  }
  return next_task;
}

static bool is_admissible(uint16_t global_core_id, Job *candidate_job) {
  CoreState *core_state = &core_states[global_core_id];
  float max_scaling_factor = 1.0f;
  float time_needed_for_candidate =
      ceilf((candidate_job->wcet - candidate_job->executed_time) /
            max_scaling_factor * (1 + DEMAND_PADDING_PERCENT));
  if (find_slack(global_core_id, candidate_job->virtual_deadline,
                 max_scaling_factor) < time_needed_for_candidate) {
    return false;
  }
  Job *cur;
  list_for_each_entry(cur, &core_state->replica_queue, link) {
    uint32_t deadline = cur->virtual_deadline;
    float slack = find_slack(global_core_id, deadline, max_scaling_factor);
    if (slack < time_needed_for_candidate) {
      return false;
    }
  }
  list_for_each_entry(cur, &core_state->ready_queue, link) {
    uint32_t deadline = cur->virtual_deadline;
    float slack = find_slack(global_core_id, deadline, max_scaling_factor);
    if (slack < time_needed_for_candidate) {
      return false;
    }
  }
  return true;
}

static void handle_job_completion(uint16_t global_core_id) {
  decision_point = true;
  CoreState *core_state = &core_states[global_core_id];
  Job *completed_job = core_state->running_job;

  if (completed_job == NULL) {
    LOG(LOG_LEVEL_ERROR, "No running job to complete");
    return;
  }

  LOG(LOG_LEVEL_INFO, "Job %d completed", completed_job->parent_task->id);

  completed_job->state = JOB_STATE_COMPLETED;

  CompletionMessage completion_message = {
      .completed_task_id = completed_job->parent_task->id,
      .job_arrival_time = completed_job->arrival_time,
      .system_time = processor_state.system_time};

  ring_buffer_enqueue(&processor_state.outgoing_completion_msg_queue,
                      &completion_message);

  release_job(completed_job);
  core_state->running_job = NULL;
  core_state->is_idle = true;
}

static void remove_completed_jobs(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];
  CompletionMessage *completion_message;
  ring_buffer_t *incoming_queue =
      &processor_state.incoming_completion_msg_queue;
  ring_buffer_iter_read_unsafe(incoming_queue, completion_message) {
    Job *cur, *next;
    list_for_each_entry_safe(cur, next, &core_state->replica_queue, link) {
      if (cur->parent_task->id == completion_message->completed_task_id &&
          cur->arrival_time == completion_message->job_arrival_time) {
        list_del(&cur->link);
        release_job(cur);
        LOG(LOG_LEVEL_INFO, "Removed replica job %d",
            completion_message->completed_task_id);
      }
    }
    list_for_each_entry_safe(cur, next, &core_state->ready_queue, link) {
      if (cur->parent_task->id == completion_message->completed_task_id &&
          cur->arrival_time == completion_message->job_arrival_time) {
        list_del(&cur->link);
        release_job(cur);
        LOG(LOG_LEVEL_INFO, "Removed ready job %d",
            completion_message->completed_task_id);
      }
    }

    if (core_state->running_job != NULL &&
        core_state->running_job->parent_task->id ==
            completion_message->completed_task_id) {
      Job *running_job = core_state->running_job;
      core_state->running_job = NULL;
      running_job->state = JOB_STATE_COMPLETED;
      release_job(running_job);
      core_state->is_idle = true;
      LOG(LOG_LEVEL_INFO, "Removed running job %d",
          completion_message->completed_task_id);
    }
  }
}

static void handle_mode_change(uint16_t global_core_id,
                               CriticalityLevel new_criticality_level) {
  decision_point = true;
  CoreState *core_state = &core_states[global_core_id];

  atomic_store(&processor_state.system_criticality_level,
               new_criticality_level);
  core_state->local_criticality_level = new_criticality_level;

  LOG(LOG_LEVEL_WARN, "Mode Change to %d", core_state->local_criticality_level);

  if (core_state->running_job != NULL) {
    Job *running_job = core_state->running_job;
    core_state->running_job = NULL;
    running_job->state = JOB_STATE_READY;
    core_state->is_idle = true;

    if (running_job->is_replica) {
      add_to_queue_sorted(&core_state->replica_queue, running_job);
    } else {
      add_to_queue_sorted(&core_state->ready_queue, running_job);
    }
  }

  LIST_HEAD(new_ready_queue);
  LIST_HEAD(new_replica_queue);

  while (peek_next_job(&core_state->ready_queue) != NULL) {
    Job *current_job = pop_next_job(&core_state->ready_queue);
    current_job->virtual_deadline =
        current_job->arrival_time +
        current_job
            ->relative_tuned_deadlines[core_state->local_criticality_level];
    current_job->wcet =
        (float)
            current_job->parent_task->wcet[core_state->local_criticality_level];

    if (current_job->parent_task->criticality_level <
        core_state->local_criticality_level) {
      add_to_queue_sorted(&core_state->discard_list, current_job);
    } else {
      add_to_queue_sorted(&new_ready_queue, current_job);
    }
  }

  while (peek_next_job(&core_state->replica_queue) != NULL) {
    Job *current_job = pop_next_job(&core_state->replica_queue);
    current_job->virtual_deadline =
        current_job->arrival_time +
        current_job
            ->relative_tuned_deadlines[core_state->local_criticality_level];
    current_job->wcet =
        (float)
            current_job->parent_task->wcet[core_state->local_criticality_level];

    if (current_job->parent_task->criticality_level <
        core_state->local_criticality_level) {
      add_to_queue_sorted(&core_state->discard_list, current_job);
    } else {
      add_to_queue_sorted(&new_replica_queue, current_job);
    }
  }

  list_splice_init(&new_ready_queue, &core_state->ready_queue);
  list_splice_init(&new_replica_queue, &core_state->replica_queue);
}

static void handle_job_arrivals(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const TaskAllocationMap *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const Task *task = find_task_by_id(instance->task_id);

      if (processor_state.system_time % task->period != 0) {
        continue;
      }
      Job *new_job = create_job(task, global_core_id);
      if (new_job == NULL) {
        continue;
      }

      // update job parameters
      new_job->arrival_time = processor_state.system_time;
      for (uint8_t level = 0; level < MAX_CRITICALITY_LEVELS; level++) {
        new_job->relative_tuned_deadlines[level] =
            instance->tuned_deadlines[level];
      }
      new_job->actual_deadline =
          processor_state.system_time + new_job->parent_task->deadline;
      new_job->virtual_deadline =
          processor_state.system_time +
          instance->tuned_deadlines[core_state->local_criticality_level];
      new_job->wcet =
          (float)
              new_job->parent_task->wcet[core_state->local_criticality_level];

      new_job->acet = generate_acet(new_job);
      new_job->executed_time = 0;

      new_job->is_replica = (instance->task_type == Replica);
      new_job->state = JOB_STATE_READY;

      LOG(LOG_LEVEL_INFO,
          "Job %d arrived with deadline (actual: %d, virtual: "
          "%d) with ACET %.2f and "
          "WCET %.2f",
          new_job->parent_task->id, new_job->actual_deadline,
          new_job->virtual_deadline, new_job->acet, new_job->wcet);

      // in case of a job arrival where job's criticality is less than system
      // criticality
      if (new_job->parent_task->criticality_level <
          core_state->local_criticality_level) {
        add_to_queue_sorted(&core_state->discard_list, new_job);
      } else {
        decision_point = true;
        if (new_job->is_replica) {
          add_to_queue_sorted(&core_state->replica_queue, new_job);
        } else {
          add_to_queue_sorted(&core_state->ready_queue, new_job);
        }
      }
    }
  }
}

static void handle_running_job(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];

  if (core_state->running_job != NULL) {
    core_state->running_job->executed_time +=
        power_get_current_scaling_factor(global_core_id);

    // Incase job isn't able to meet deadline
    if (core_state->running_job->state != JOB_STATE_COMPLETED &&
        processor_state.system_time >
            core_state->running_job->actual_deadline) {
      LOG(LOG_LEVEL_ERROR, "Job %d missed its deadline",
          core_state->running_job->parent_task->id);
      LOG(LOG_LEVEL_FATAL, "System Halted due to Deadline Miss");
      exit(1);
    }

    if (core_state->running_job->acet <=
        core_state->running_job->executed_time) {
      handle_job_completion(global_core_id);
    } else if (core_state->running_job->wcet <=
               core_state->running_job->executed_time) {
      CriticalityLevel new_criticality_level =
          core_state->local_criticality_level;
      for (uint8_t level = new_criticality_level + 1;
           level < MAX_CRITICALITY_LEVELS; level++) {
        if (core_state->running_job->executed_time <
            (float)core_state->running_job->parent_task->wcet[level]) {
          new_criticality_level = (CriticalityLevel)level;
          break;
        }
      }
      ipc_broadcast_criticality_change(new_criticality_level);
      handle_mode_change(global_core_id, new_criticality_level);
    }
  }
}

static Job *select_next_job(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];
  bool from_ready_queue = false;

  Job *next_job_candidate;

  if (!list_empty(&core_state->ready_queue) &&
      !list_empty(&core_state->replica_queue)) {
    Job *ready_job = peek_next_job(&core_state->ready_queue);
    Job *replica_job = peek_next_job(&core_state->replica_queue);

    if (ready_job->virtual_deadline <= replica_job->virtual_deadline) {
      next_job_candidate = ready_job;
      from_ready_queue = true;
    } else {
      next_job_candidate = replica_job;
    }
  } else if (list_empty(&core_state->ready_queue) == false) {
    next_job_candidate = peek_next_job(&core_state->ready_queue);
    from_ready_queue = true;
  } else if (list_empty(&core_state->replica_queue) == false) {
    next_job_candidate = peek_next_job(&core_state->replica_queue);
  } else {
    return NULL;
  }

  if (core_state->running_job == NULL ||
      core_state->running_job->virtual_deadline >
          next_job_candidate->virtual_deadline) {
    if (from_ready_queue) {
      return pop_next_job(&core_state->ready_queue);
    } else {
      return pop_next_job(&core_state->replica_queue);
    }
  }

  return NULL;
}

static void dispatch_job(uint16_t global_core_id, Job *job_to_dispatch) {
  CoreState *core_state = &core_states[global_core_id];

  if (core_state->running_job != NULL) {
    Job *current_job = core_state->running_job;
    LOG(LOG_LEVEL_INFO, "Preempting Job %d", current_job->parent_task->id);
    current_job->state = JOB_STATE_READY;
    if (current_job->is_replica) {
      add_to_queue_sorted(&core_state->replica_queue, current_job);
    } else {
      add_to_queue_sorted(&core_state->ready_queue, current_job);
    }
  }

  core_state->running_job = job_to_dispatch;
  core_state->is_idle = false;
  job_to_dispatch->state = JOB_STATE_RUNNING;
  LOG(LOG_LEVEL_INFO, "Dispatching Job %d", job_to_dispatch->parent_task->id);
}

static void reclaim_discarded_jobs(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];

  while (peek_next_job(&core_state->discard_list) != NULL) {
    Job *discarded_job = pop_next_job(&core_state->discard_list);

    if (is_admissible(global_core_id, discarded_job)) {
      LOG(LOG_LEVEL_INFO,
          "Accommodating discarded job %d (Original Core ID: %u)",
          discarded_job->parent_task->id, discarded_job->owner_core_id);
      decision_point = true;
      if (discarded_job->is_replica) {
        add_to_queue_sorted(&core_state->replica_queue, discarded_job);
      } else {
        add_to_queue_sorted(&core_state->ready_queue, discarded_job);
      }
    } else {
      pthread_mutex_lock(&processor_state.discard_queue_lock);
      discarded_job->virtual_deadline = discarded_job->actual_deadline;
      add_to_queue_sorted(&processor_state.discard_queue, discarded_job);
      pthread_mutex_unlock(&processor_state.discard_queue_lock);
    }
  }

  Job *cur, *next;
  pthread_mutex_lock(&processor_state.discard_queue_lock);
  list_for_each_entry_safe(cur, next, &processor_state.discard_queue, link) {
    if (is_admissible(global_core_id, cur)) {
      LOG(LOG_LEVEL_INFO,
          "Accommodating discarded job %d (Original Core ID: %u)",
          cur->parent_task->id, cur->owner_core_id);
      decision_point = true;
      list_del(&cur->link);
      if (cur->is_replica) {
        add_to_queue_sorted(&core_state->replica_queue, cur);
      } else {
        add_to_queue_sorted(&core_state->ready_queue, cur);
      }
    }
  }
  pthread_mutex_unlock(&processor_state.discard_queue_lock);
}

void scheduler_init(void) {
  LOG(LOG_LEVEL_INFO, "Initializing Scheduler...");

  task_management_init();
  power_management_init();

  for (int i = 0; i < TOTAL_CORES; i++) {
    core_states[i].proc_id = i / NUM_CORES_PER_PROC;
    core_states[i].core_id = i % NUM_CORES_PER_PROC;
    core_states[i].running_job = NULL;
    core_states[i].is_idle = true;
    core_states[i].current_dvfs_level = 0;

    INIT_LIST_HEAD(&core_states[i].ready_queue);
    INIT_LIST_HEAD(&core_states[i].replica_queue);
    INIT_LIST_HEAD(&core_states[i].discard_list);
  }

  for (uint32_t i = 0; i < SYSTEM_TASKS_SIZE; i++) {
    task_lookup[system_tasks[i].id] = &system_tasks[i];
  }

  LOG(LOG_LEVEL_INFO, "Scheduler Initialization Complete.");
}

static inline void log_core_state(uint16_t global_core_id) {
  CoreState *core_state = &core_states[global_core_id];

  if (core_state->is_idle) {
    LOG(LOG_LEVEL_DEBUG, "Status: IDLE");
  } else {
    LOG(LOG_LEVEL_DEBUG, "Status: RUNNING -> Job %d",
        core_state->running_job ? core_state->running_job->parent_task->id
                                : -1);
  }

  LOG(LOG_LEVEL_DEBUG, "DVFS Level: %u, Frequency Scaling: %.2f",
      core_state->current_dvfs_level,
      power_get_current_scaling_factor(global_core_id));
  LOG(LOG_LEVEL_DEBUG, "Criticality Level: %u",
      core_state->local_criticality_level);

  log_job_queue(LOG_LEVEL_DEBUG, "Ready Queue", &core_state->ready_queue);
  log_job_queue(LOG_LEVEL_DEBUG, "Replica Queue", &core_state->replica_queue);
}

void scheduler_tick(uint16_t global_core_id) {
  if (global_core_id >= TOTAL_CORES) {
    return;
  }

  CoreState *core_state = &core_states[global_core_id];
  if (core_state->local_criticality_level !=
      atomic_load(&processor_state.system_criticality_level)) {
    handle_mode_change(global_core_id,
                       processor_state.system_criticality_level);
  }

  if (core_state->dpm_control_block.in_low_power_state) {
    if (core_state->dpm_control_block.dpm_end_time <=
        processor_state.system_time) {
      core_state->dpm_control_block.in_low_power_state = false;
      LOG(LOG_LEVEL_INFO, "Exiting low power state");
    } else {
      LOG(LOG_LEVEL_DEBUG, "Core in low power state");
      return;
    }
  }

  handle_job_arrivals(global_core_id);

  handle_running_job(global_core_id);

  reclaim_discarded_jobs(global_core_id);

  remove_completed_jobs(global_core_id);

  Job *next_job = select_next_job(global_core_id);

  if (next_job != NULL) {
    decision_point = true;
    dispatch_job(global_core_id, next_job);
  }

  if (decision_point) {
    if (core_state->running_job != NULL &&
        core_state->running_job->parent_task->criticality_level <
            atomic_load(&processor_state.system_criticality_level)) {
      power_set_dvfs_level(global_core_id, 0);
    } else {
      power_set_dvfs_level(global_core_id,
                           calc_required_dvfs_level(global_core_id));
    }
    decision_point = false;
  }

  if (core_state->is_idle) {
    power_management_set_dpm_interval(global_core_id);
  }

  log_core_state(global_core_id);
}

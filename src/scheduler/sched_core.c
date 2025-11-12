#include "lib/list.h"
#include "lib/log.h"
#include "lib/ring_buffer.h"

#include "scheduler/sched_core.h"
#include "scheduler/sched_migration.h"
#include "scheduler/sched_util.h"

#include "ipc.h"
#include "power_management.h"
#include "processor.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"

#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

core_state core_states[NUM_CORES_PER_PROC];

const task_struct *task_lookup[MAX_TASKS + 1];

static void handle_job_completion(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];
  core_state->decision_point = true;
  job_struct *completed_job = core_state->running_job;

  if (completed_job == NULL) {
    LOG(LOG_LEVEL_ERROR, "No running job to complete");
    return;
  }

  LOG(LOG_LEVEL_INFO, "Job %d completed", completed_job->parent_task->id);

  completed_job->state = JOB_STATE_COMPLETED;

  completion_message outgoing_msg = {
      .completed_task_id = completed_job->parent_task->id,
      .job_arrival_time = completed_job->arrival_time,
      .system_time = proc_state.system_time};

  ring_buffer_enqueue(&proc_state.outgoing_completion_msg_queue, &outgoing_msg);

  core_state->running_job = NULL;
  core_state->is_idle = true;
  put_job_ref(completed_job, core_id);
}

static void remove_completed_jobs(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];
  completion_message *incoming_msg;
  ring_buffer *incoming_queue = &proc_state.incoming_completion_msg_queue;
  ring_buffer_iter_read_unsafe(incoming_queue, incoming_msg) {
    job_struct *cur, *next;
    list_for_each_entry_safe(cur, next, &core_state->replica_queue, link) {
      if (cur->parent_task->id == incoming_msg->completed_task_id &&
          cur->arrival_time == incoming_msg->job_arrival_time) {
        cur->state = JOB_STATE_REMOVED;
        list_del(&cur->link);
        put_job_ref(cur, core_id);
        LOG(LOG_LEVEL_INFO, "Removed replica job %d",
            incoming_msg->completed_task_id);
      }
    }
    list_for_each_entry_safe(cur, next, &core_state->ready_queue, link) {
      if (cur->parent_task->id == incoming_msg->completed_task_id &&
          cur->arrival_time == incoming_msg->job_arrival_time) {
        cur->state = JOB_STATE_REMOVED;
        list_del(&cur->link);
        put_job_ref(cur, core_id);
        LOG(LOG_LEVEL_INFO, "Removed ready job %d",
            incoming_msg->completed_task_id);
      }
    }

    if (core_state->running_job != NULL &&
        core_state->running_job->parent_task->id ==
            incoming_msg->completed_task_id) {
      job_struct *running_job = core_state->running_job;
      core_state->running_job = NULL;
      running_job->state = JOB_STATE_COMPLETED;
      put_job_ref(running_job, core_id);
      core_state->is_idle = true;
      LOG(LOG_LEVEL_INFO, "Removed running job %d",
          incoming_msg->completed_task_id);
    }
  }
}

static void handle_mode_change(uint8_t core_id,
                               criticality_level new_criticality_level) {
  core_state *core_state = &core_states[core_id];
  core_state->decision_point = true;

  atomic_store(&proc_state.system_criticality_level, new_criticality_level);
  core_state->local_criticality_level = new_criticality_level;

  LOG(LOG_LEVEL_WARN, "Mode Change to %d", core_state->local_criticality_level);

  if (core_state->running_job != NULL) {
    job_struct *running_job = core_state->running_job;
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

  while (!list_empty(&core_state->ready_queue)) {
    job_struct *current_job = pop_next_job(&core_state->ready_queue);
    current_job->virtual_deadline =
        current_job->arrival_time +
        current_job
            ->relative_tuned_deadlines[core_state->local_criticality_level];
    current_job->wcet =
        (float)
            current_job->parent_task->wcet[core_state->local_criticality_level];

    if (current_job->parent_task->crit_level <
            core_state->local_criticality_level &&
        !atomic_load_explicit(&current_job->is_being_offered,
                              memory_order_acquire)) {
      add_to_queue_sorted(&core_state->discard_list, current_job);
    } else {
      add_to_queue_sorted(&new_ready_queue, current_job);
    }
  }

  while (!list_empty(&core_state->replica_queue)) {
    job_struct *current_job = pop_next_job(&core_state->replica_queue);
    current_job->virtual_deadline =
        current_job->arrival_time +
        current_job
            ->relative_tuned_deadlines[core_state->local_criticality_level];
    current_job->wcet =
        (float)
            current_job->parent_task->wcet[core_state->local_criticality_level];

    if (current_job->parent_task->crit_level <
            core_state->local_criticality_level &&
        !atomic_load_explicit(&current_job->is_being_offered,
                              memory_order_acquire)) {
      add_to_queue_sorted(&core_state->discard_list, current_job);
    } else {
      add_to_queue_sorted(&new_replica_queue, current_job);
    }
  }

  list_splice_init(&new_ready_queue, &core_state->ready_queue);
  list_splice_init(&new_replica_queue, &core_state->replica_queue);
}

static void handle_job_arrivals(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  job_struct *new_job, *next;
  list_for_each_entry_safe(new_job, next, &core_state->pending_jobs_queue,
                           link) {
    if (new_job->arrival_time > proc_state.system_time) {
      continue;
    }

    list_del(&new_job->link);

    LOG(LOG_LEVEL_INFO,
        "Job %d (from pending) arrived with deadline (actual: %d, virtual: "
        "%d) with ACET %.2f and "
        "WCET %.2f",
        new_job->parent_task->id, new_job->actual_deadline,
        new_job->virtual_deadline, new_job->acet, new_job->wcet);

    new_job->arrival_time = proc_state.system_time;

    if (new_job->parent_task->crit_level <
        core_state->local_criticality_level) {
      add_to_queue_sorted(&core_state->discard_list, new_job);
    } else {
      core_state->decision_point = true;
      if (new_job->is_replica) {
        add_to_queue_sorted(&core_state->replica_queue, new_job);
      } else {
        add_to_queue_sorted(&core_state->ready_queue, new_job);
      }
    }
  }

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const task_struct *task = find_task_by_id(instance->task_id);

      if (!task || task->period == 0) {
        continue;
      }

      if (proc_state.system_time % task->period != 0) {
        continue;
      }

      DelegatedJob *dj, *tmp;
      bool delegated = false;
      list_for_each_entry_safe(dj, tmp, &core_state->delegated_job_queue,
                               link) {
        if (dj->arrival_tick < proc_state.system_time) {
          list_del(&dj->link);
          release_delegation(dj, core_id);
          continue;
        }
        if (dj->task_id == task->id &&
            dj->arrival_tick >= proc_state.system_time) {
          LOG(LOG_LEVEL_DEBUG,
              "Skipping delegated arrival for Task %u (delegated until tick "
              "%u)",
              task->id, dj->arrival_tick);
          delegated = true;
          break;
        }
      }
      if (delegated)
        goto skip_arrival;

      new_job = create_job(task, core_id);
      if (new_job == NULL) {
        continue;
      }

      // update job parameters
      new_job->arrival_time = proc_state.system_time;
      for (uint8_t level = 0; level < MAX_CRITICALITY_LEVELS; level++) {
        new_job->relative_tuned_deadlines[level] =
            instance->tuned_deadlines[level];
      }
      new_job->actual_deadline =
          proc_state.system_time + new_job->parent_task->deadline;
      new_job->virtual_deadline =
          proc_state.system_time +
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
      if (new_job->parent_task->crit_level <
          core_state->local_criticality_level) {
        add_to_queue_sorted(&core_state->discard_list, new_job);
      } else {
        core_state->decision_point = true;
        if (new_job->is_replica) {
          add_to_queue_sorted(&core_state->replica_queue, new_job);
        } else {
          add_to_queue_sorted(&core_state->ready_queue, new_job);
        }
      }
    }
  skip_arrival:
    continue;
  }
}

static void handle_running_job(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  if (core_state->running_job != NULL) {
    core_state->running_job->executed_time +=
        power_get_current_scaling_factor(core_id);

    // Incase job isn't able to meet deadline
    if (core_state->running_job->state == JOB_STATE_RUNNING &&
        proc_state.system_time > core_state->running_job->actual_deadline) {
      LOG(LOG_LEVEL_ERROR, "Job %d missed its deadline %d",
          core_state->running_job->parent_task->id,
          core_state->running_job->actual_deadline);
      LOG(LOG_LEVEL_FATAL, "System Halted due to Deadline Miss");
      fputs("System Halted due to Deadline Miss\n", stderr);
      kill(getppid(), SIGINT);
    }

    if (core_state->running_job->acet <=
        core_state->running_job->executed_time) {
      handle_job_completion(core_id);
    } else if (core_state->running_job->wcet <=
               core_state->running_job->executed_time) {
      criticality_level new_criticality_level =
          core_state->local_criticality_level;
      for (uint8_t level = new_criticality_level + 1;
           level < MAX_CRITICALITY_LEVELS; level++) {
        if (core_state->running_job->executed_time <
            (float)core_state->running_job->parent_task->wcet[level]) {
          new_criticality_level = (criticality_level)level;
          break;
        }
      }
      ipc_broadcast_criticality_change(new_criticality_level);
      handle_mode_change(core_id, new_criticality_level);
    }
  }
}

static job_struct *select_next_job(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];
  bool from_ready_queue = false;

  job_struct *next_job_candidate;

  if (!list_empty(&core_state->ready_queue) &&
      !list_empty(&core_state->replica_queue)) {
    job_struct *ready_job = peek_next_job(&core_state->ready_queue);
    job_struct *replica_job = peek_next_job(&core_state->replica_queue);

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

static void dispatch_job(uint8_t core_id, job_struct *job_to_dispatch) {
  core_state *core_state = &core_states[core_id];

  if (core_state->running_job != NULL) {
    job_struct *current_job = core_state->running_job;
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

static void reclaim_discarded_jobs(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  while (!list_empty(&core_state->discard_list)) {
    job_struct *discarded_job = pop_next_job(&core_state->discard_list);

    if (is_admissible(core_id, discarded_job)) {
      LOG(LOG_LEVEL_INFO,
          "Accommodating discarded job %d (Original Core ID: %u)",
          discarded_job->parent_task->id, discarded_job->job_pool_id);
      core_state->decision_point = true;
      if (discarded_job->is_replica) {
        add_to_queue_sorted(&core_state->replica_queue, discarded_job);
      } else {
        add_to_queue_sorted(&core_state->ready_queue, discarded_job);
      }
    } else if (!atomic_load(&discarded_job->is_being_offered)) {
      pthread_mutex_lock(&proc_state.discard_queue_lock);
      discarded_job->virtual_deadline = discarded_job->actual_deadline;
      add_to_queue_sorted(&proc_state.discard_queue, discarded_job);
      pthread_mutex_unlock(&proc_state.discard_queue_lock);
    }
  }

  job_struct *cur, *next;
  pthread_mutex_lock(&proc_state.discard_queue_lock);
  list_for_each_entry_safe(cur, next, &proc_state.discard_queue, link) {
    if (is_admissible(core_id, cur)) {
      LOG(LOG_LEVEL_INFO,
          "Accommodating discarded job %d (Original Core ID: %u)",
          cur->parent_task->id, cur->job_pool_id);
      core_state->decision_point = true;
      list_del(&cur->link);
      if (cur->is_replica) {
        add_to_queue_sorted(&core_state->replica_queue, cur);
      } else {
        add_to_queue_sorted(&core_state->ready_queue, cur);
      }
    }
  }
  pthread_mutex_unlock(&proc_state.discard_queue_lock);
}

void scheduler_init(void) {
  LOG(LOG_LEVEL_INFO, "Initializing Scheduler...");

  task_management_init();
  power_management_init();

  for (int i = 0; i < NUM_CORES_PER_PROC; i++) {
    core_states[i].proc_id = proc_state.processor_id;
    core_states[i].core_id = i;
    core_states[i].running_job = NULL;
    core_states[i].is_idle = true;
    core_states[i].current_dvfs_level = 0;

    INIT_LIST_HEAD(&core_states[i].ready_queue);
    INIT_LIST_HEAD(&core_states[i].replica_queue);
    INIT_LIST_HEAD(&core_states[i].discard_list);
    INIT_LIST_HEAD(&core_states[i].pending_jobs_queue);
    INIT_LIST_HEAD(&core_states[i].bid_history_queue);
    INIT_LIST_HEAD(&core_states[i].delegated_job_queue);

    ring_buffer_init(&core_states[i].award_notification_queue,
                     MAX_CONCURRENT_OFFERS, core_states[i].award_buf,
                     core_states[i].seq, sizeof(job_struct *));

    core_states[i].local_criticality_level = 0;
    core_states[i].decision_point = false;
    core_states[i].cached_slack_horizon = calculate_allocated_horizon(i);
  }

  init_migration();

  for (uint32_t i = 0; i < SYSTEM_TASKS_SIZE; i++) {
    task_lookup[system_tasks[i].id] = &system_tasks[i];
  }

  LOG(LOG_LEVEL_INFO, "Scheduler Initialization Complete.");
}

static inline void log_core_state(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  if (core_state->is_idle) {
    LOG(LOG_LEVEL_DEBUG, "Status: IDLE");
  } else {
    LOG(LOG_LEVEL_DEBUG, "Status: RUNNING -> Job %d",
        core_state->running_job ? core_state->running_job->parent_task->id
                                : -1);
  }

  LOG(LOG_LEVEL_DEBUG, "DVFS Level: %u, Frequency Scaling: %.2f",
      core_state->current_dvfs_level,
      power_get_current_scaling_factor(core_id));
  LOG(LOG_LEVEL_DEBUG, "Criticality Level: %u",
      core_state->local_criticality_level);

  log_job_queue(LOG_LEVEL_DEBUG, "Ready Queue", &core_state->ready_queue);
  log_job_queue(LOG_LEVEL_DEBUG, "Replica Queue", &core_state->replica_queue);
}

void scheduler_tick(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  if (core_state->local_criticality_level !=
      atomic_load(&proc_state.system_criticality_level)) {
    handle_mode_change(core_id, proc_state.system_criticality_level);
  }

  if (core_state->dpm_control_block.in_low_power_state) {
    if (core_state->dpm_control_block.dpm_end_time <= proc_state.system_time) {
      core_state->dpm_control_block.in_low_power_state = false;
      LOG(LOG_LEVEL_INFO, "Exiting low power state");
    } else {
      LOG(LOG_LEVEL_DEBUG, "Core in low power state");
      return;
    }
  }

  handle_running_job(core_id);

  remove_completed_jobs(core_id);

  handle_job_arrivals(core_id);

  handle_offer_cleanup(core_id);

  process_award_notifications(core_id);

  reclaim_discarded_jobs(core_id);

  attempt_migration_push(core_id);

  participate_in_auctions(core_id);

  job_struct *next_job = select_next_job(core_id);

  if (next_job != NULL) {
    core_state->decision_point = true;
    dispatch_job(core_id, next_job);
  }

  if (core_state->decision_point) {
    if (core_state->running_job != NULL &&
        core_state->running_job->parent_task->crit_level <
            atomic_load(&proc_state.system_criticality_level)) {
      power_set_dvfs_level(core_id, 0);
    } else {
      power_set_dvfs_level(core_id, calc_required_dvfs_level(core_id));
    }
    core_state->decision_point = false;
  }

  if (proc_state.system_time >= core_state->next_dpm_eligible_tick &&
      core_state->is_idle && list_empty(&core_state->bid_history_queue)) {
    uint32_t next_eff_arrival_time = find_next_effective_arrival_time(core_id);
    power_management_set_dpm_interval(core_id, next_eff_arrival_time);
  }

  remove_expired_bid_entries(core_id);

  log_core_state(core_id);
}

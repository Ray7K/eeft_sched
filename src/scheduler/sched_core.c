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

#include <float.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

core_state core_states[NUM_CORES_PER_PROC];

core_summary core_summaries[NUM_CORES_PER_PROC];

pthread_mutex_t core_summary_locks[NUM_CORES_PER_PROC];

const task_struct *task_lookup[MAX_TASKS + 1];

static void handle_job_completion(uint8_t core_id) {
  core_state *cs = &core_states[core_id];
  cs->decision_point = true;

  LOCK_RQ(core_id);
  job_struct *completed_job = cs->running_job;

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

  cs->running_job = NULL;
  UNLOCK_RQ(core_id);

  cs->is_idle = true;
  put_job_ref(completed_job, core_id);
}

static void remove_completed_jobs(uint8_t core_id) {
  core_state *cs = &core_states[core_id];
  completion_message *incoming_msg;
  ring_buffer *incoming_queue = &proc_state.incoming_completion_msg_queue;
  ring_buffer_iter_read_unsafe(incoming_queue, incoming_msg) {
    LOCK_RQ(core_id);
    job_struct *cur, *next;
    list_for_each_entry_safe(cur, next, &cs->replica_queue, link) {
      if (cur->parent_task->id == incoming_msg->completed_task_id &&
          cur->arrival_time == incoming_msg->job_arrival_time) {
        cur->state = JOB_STATE_REMOVED;
        LOG(LOG_LEVEL_INFO, "Removed replica job %d, Reclaimed %.2f ticks",
            incoming_msg->completed_task_id, cur->acet - cur->executed_time);
        list_del(&cur->link);
        put_job_ref(cur, core_id);
      }
    }
    list_for_each_entry_safe(cur, next, &cs->ready_queue, link) {
      if (cur->parent_task->id == incoming_msg->completed_task_id &&
          cur->arrival_time == incoming_msg->job_arrival_time) {
        cur->state = JOB_STATE_REMOVED;
        LOG(LOG_LEVEL_INFO, "Removed ready job %d, Reclaimed %.2f ticks",
            incoming_msg->completed_task_id, cur->acet - cur->executed_time);
        list_del(&cur->link);
        put_job_ref(cur, core_id);
      }
    }

    if (cs->running_job != NULL &&
        cs->running_job->parent_task->id == incoming_msg->completed_task_id) {
      job_struct *running_job = cs->running_job;
      LOG(LOG_LEVEL_INFO, "Preempting Job %d", running_job->parent_task->id);
      cs->running_job = NULL;
      running_job->state = JOB_STATE_REMOVED;
      cs->is_idle = true;
      LOG(LOG_LEVEL_INFO, "Removed running job %d, Reclaimed %.2f ticks",
          incoming_msg->completed_task_id,
          running_job->acet - running_job->executed_time);
      put_job_ref(running_job, core_id);
    }

    UNLOCK_RQ(core_id);
  }
}

static void filter_queue_for_mode_change(struct list_head *src_queue,
                                         struct list_head *dest_queue,
                                         core_state *cs) {
  while (!list_empty(src_queue)) {
    job_struct *job = pop_next_job(src_queue);
    job->virtual_deadline =
        job->arrival_time +
        job->relative_tuned_deadlines[cs->local_criticality_level];
    job->wcet = (float)job->parent_task->wcet[cs->local_criticality_level];

    if (job->parent_task->crit_level < cs->local_criticality_level &&
        !atomic_load_explicit(&job->is_being_offered, memory_order_acquire)) {
      add_to_queue_sorted(&cs->discard_list, job);
    } else {
      add_to_queue_sorted(dest_queue, job);
    }
  }
}

static void handle_mode_change(uint8_t core_id,
                               criticality_level new_criticality_level) {
  core_state *cs = &core_states[core_id];
  cs->decision_point = true;

  atomic_store(&proc_state.system_criticality_level, new_criticality_level);
  cs->local_criticality_level = new_criticality_level;

  LOG(LOG_LEVEL_WARN, "Mode Change to %d", cs->local_criticality_level);

  LOCK_RQ(core_id);

  if (cs->running_job != NULL) {
    job_struct *running_job = cs->running_job;
    cs->running_job = NULL;
    running_job->state = JOB_STATE_READY;
    cs->is_idle = true;

    if (running_job->is_replica) {
      add_to_queue_sorted(&cs->replica_queue, running_job);
    } else {
      add_to_queue_sorted(&cs->ready_queue, running_job);
    }
  }

  LIST_HEAD(new_ready_queue);
  LIST_HEAD(new_replica_queue);

  filter_queue_for_mode_change(&cs->ready_queue, &new_ready_queue, cs);
  filter_queue_for_mode_change(&cs->replica_queue, &new_replica_queue, cs);

  list_splice_init(&new_ready_queue, &cs->ready_queue);
  list_splice_init(&new_replica_queue, &cs->replica_queue);

  UNLOCK_RQ(core_id);
}

static void handle_job_arrivals(uint8_t core_id) {
  core_state *cs = &core_states[core_id];

  job_struct *new_job, *next;
  list_for_each_entry_safe(new_job, next, &cs->pending_jobs_queue, link) {
    if (new_job->arrival_time < proc_state.system_time) {
      if (new_job->parent_task->crit_level < cs->local_criticality_level) {
        list_del(&new_job->link);
        put_job_ref(new_job, core_id);
      } else {
        LOG(LOG_LEVEL_ERROR, "Missed Pending Job %d Arrival!",
            new_job->parent_task->id);
      }
      continue;
    }

    if (new_job->arrival_time > proc_state.system_time) {
      break;
    }

    list_del(&new_job->link);

    new_job->state = JOB_STATE_READY;
    new_job->virtual_deadline =
        proc_state.system_time +
        new_job->relative_tuned_deadlines[cs->local_criticality_level];
    new_job->wcet =
        (float)new_job->parent_task->wcet[cs->local_criticality_level];

    LOG(LOG_LEVEL_INFO,
        "Job %d (from pending) arrived with deadline (actual: %d, virtual: "
        "%d) with ACET %.2f and "
        "WCET %.2f",
        new_job->parent_task->id, new_job->actual_deadline,
        new_job->virtual_deadline, new_job->acet, new_job->wcet);

    new_job->arrival_time = proc_state.system_time;

    LOCK_RQ(core_id);
    if (new_job->parent_task->crit_level < cs->local_criticality_level) {
      add_to_queue_sorted(&cs->discard_list, new_job);
    } else {
      cs->decision_point = true;
      if (new_job->is_replica) {
        add_to_queue_sorted(&cs->replica_queue, new_job);
      } else {
        add_to_queue_sorted(&cs->ready_queue, new_job);
      }
    }
    UNLOCK_RQ(core_id);
  }

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *instance = &allocation_map[i];

    if (instance->proc_id == cs->proc_id && instance->core_id == cs->core_id) {
      const task_struct *task = find_task_by_id(instance->task_id);

      if (!task || task->period == 0) {
        continue;
      }

      if (proc_state.system_time % task->period != 0) {
        continue;
      }

      delegated_job *dj, *tmp;
      bool delegated = false;
      list_for_each_entry_safe(dj, tmp, &cs->delegated_job_queue, link) {
        if (dj->arrival_tick < proc_state.system_time) {
          list_del(&dj->link);
          release_delegation(dj, core_id);
          continue;
        }
        if (dj->task_id == task->id &&
            dj->arrival_tick >= proc_state.system_time && dj->owned_by_remote) {
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
          instance->tuned_deadlines[cs->local_criticality_level];
      new_job->wcet =
          (float)new_job->parent_task->wcet[cs->local_criticality_level];

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
      LOCK_RQ(core_id);
      if (new_job->parent_task->crit_level < cs->local_criticality_level) {
        add_to_queue_sorted(&cs->discard_list, new_job);
      } else {
        cs->decision_point = true;
        if (new_job->is_replica) {
          add_to_queue_sorted(&cs->replica_queue, new_job);
        } else {
          add_to_queue_sorted(&cs->ready_queue, new_job);
        }
      }
      UNLOCK_RQ(core_id);
    }
  skip_arrival:
    continue;
  }
}

static void handle_running_job(uint8_t core_id) {
  core_state *cs = &core_states[core_id];

  bool trigger_completion = false;
  bool trigger_mode_change = false;
  criticality_level new_crit_level = 0;

  LOCK_RQ(core_id);

  if (cs->running_job != NULL) {
    cs->running_job->executed_time += power_get_current_scaling_factor(core_id);

    if (cs->running_job->state == JOB_STATE_RUNNING &&
        proc_state.system_time > cs->running_job->actual_deadline) {

      job_struct *missed_job = cs->running_job;
      cs->running_job->state = JOB_STATE_COMPLETED;
      cs->running_job = NULL;
      cs->is_idle = true;

      uint32_t task_id = missed_job->parent_task->id;
      uint32_t deadline = missed_job->actual_deadline;

      put_job_ref(missed_job, core_id);

      UNLOCK_RQ(core_id);

      LOG(LOG_LEVEL_ERROR, "Job %d missed its deadline %d", task_id, deadline);
      LOG(LOG_LEVEL_FATAL, "System Halted due to Deadline Miss");
      fputs("System Halted due to Deadline Miss\n", stderr);
      atomic_store(&core_fatal_shutdown_requested, 1);
      return;
    }

    if (cs->running_job->acet <= cs->running_job->executed_time) {
      trigger_completion = true;
    } else if (cs->running_job->wcet <= cs->running_job->executed_time) {

      criticality_level current = cs->local_criticality_level;
      new_crit_level = current; // Default to current

      for (uint8_t level = current + 1; level < MAX_CRITICALITY_LEVELS;
           level++) {
        if (cs->running_job->executed_time <
            (float)cs->running_job->parent_task->wcet[level]) {
          new_crit_level = (criticality_level)level;
          break;
        }
      }
      trigger_mode_change = true;
    }
  }

  UNLOCK_RQ(core_id);

  if (trigger_completion) {
    handle_job_completion(core_id);
  } else if (trigger_mode_change) {
    ipc_broadcast_criticality_change(new_crit_level);
    handle_mode_change(core_id, new_crit_level);
  }
}

static job_struct *select_next_job(uint8_t core_id) {
  core_state *cs = &core_states[core_id];

  job_struct *next_job_candidate;

  LOCK_RQ(core_id);
  if (!list_empty(&cs->ready_queue) && !list_empty(&cs->replica_queue)) {
    job_struct *ready_job = peek_next_job(&cs->ready_queue);
    job_struct *replica_job = peek_next_job(&cs->replica_queue);

    if (ready_job->virtual_deadline <= replica_job->virtual_deadline) {
      next_job_candidate = ready_job;
    } else {
      next_job_candidate = replica_job;
    }
  } else if (list_empty(&cs->ready_queue) == false) {
    next_job_candidate = peek_next_job(&cs->ready_queue);
  } else if (list_empty(&cs->replica_queue) == false) {
    next_job_candidate = peek_next_job(&cs->replica_queue);
  } else {
    next_job_candidate = NULL;
  }

  if (next_job_candidate != NULL &&
      (cs->running_job == NULL || cs->running_job->virtual_deadline >
                                      next_job_candidate->virtual_deadline)) {
    if (!next_job_candidate->is_replica) {
      next_job_candidate = pop_next_job(&cs->ready_queue);
    } else {
      next_job_candidate = pop_next_job(&cs->replica_queue);
    }
  } else {
    next_job_candidate = NULL;
  }
  UNLOCK_RQ(core_id);

  return next_job_candidate;
}

static void dispatch_job(uint8_t core_id, job_struct *job_to_dispatch) {
  core_state *cs = &core_states[core_id];

  LOCK_RQ(core_id);

  if (cs->running_job != NULL) {
    job_struct *current_job = cs->running_job;
    LOG(LOG_LEVEL_INFO, "Preempting Job %d", current_job->parent_task->id);
    current_job->state = JOB_STATE_READY;

    if (current_job->is_replica) {
      add_to_queue_sorted(&cs->replica_queue, current_job);
    } else {
      add_to_queue_sorted(&cs->ready_queue, current_job);
    }
  }

  cs->running_job = job_to_dispatch;
  cs->is_idle = false;
  job_to_dispatch->state = JOB_STATE_RUNNING;

  UNLOCK_RQ(core_id);

  LOG(LOG_LEVEL_INFO, "Dispatching Job %d", job_to_dispatch->parent_task->id);
}

static void reclaim_discarded_jobs(uint8_t core_id) {
  core_state *cs = &core_states[core_id];

  LOCK_RQ(core_id);
  while (!list_empty(&cs->discard_list)) {
    job_struct *discarded_job = pop_next_job(&cs->discard_list);

    if (is_admissible_locked(core_id, discarded_job, 0.0f)) {
      LOG(LOG_LEVEL_INFO,
          "Accommodating discarded job %d (Original Core ID: %u)",
          discarded_job->parent_task->id, discarded_job->job_pool_id);
      cs->decision_point = true;
      if (discarded_job->is_replica) {
        add_to_queue_sorted(&cs->replica_queue, discarded_job);
      } else {
        add_to_queue_sorted(&cs->ready_queue, discarded_job);
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
    if (is_admissible_locked(core_id, cur, MIGRATION_PENALTY_TICKS)) {
      LOG(LOG_LEVEL_INFO,
          "Accommodating discarded job %d (Original Core ID: %u)",
          cur->parent_task->id, cur->job_pool_id);
      cs->decision_point = true;
      list_del(&cur->link);

      if (cur->is_replica) {
        add_to_queue_sorted(&cs->replica_queue, cur);
      } else {
        add_to_queue_sorted(&cs->ready_queue, cur);
      }
    }
  }
  pthread_mutex_unlock(&proc_state.discard_queue_lock);

  UNLOCK_RQ(core_id);
}

static inline void update_core_summary(uint8_t core_id) {
  core_state *cs = &core_states[core_id];
  core_summary *summary = &core_summaries[core_id];

  float utilization = get_util(core_id);
  float slack =
      find_slack(core_id, cs->local_criticality_level, proc_state.system_time,
                 power_get_current_scaling_factor(core_id), NULL);
  uint32_t next_arrival = find_next_effective_arrival_time(core_id);
  pthread_mutex_lock(&core_summary_locks[core_id]);
  summary->util = utilization;
  summary->slack = slack;
  summary->is_idle = cs->is_idle;
  summary->dvfs_level = cs->current_dvfs_level;
  summary->next_arrival = next_arrival;
  pthread_mutex_unlock(&core_summary_locks[core_id]);
}

void scheduler_init(void) {
  LOG(LOG_LEVEL_INFO, "Initializing Scheduler...");

  task_management_init();
  power_management_init();

  for (uint32_t i = 0; i < SYSTEM_TASKS_SIZE; i++) {
    task_lookup[system_tasks[i].id] = &system_tasks[i];
  }

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
    INIT_LIST_HEAD(&core_states[i].delegated_job_queue);

    ring_buffer_init(&core_states[i].migration_request_queue,
                     MAX_MIGRATION_REQUESTS, core_states[i].migration_buf,
                     core_states[i].migration_seq, sizeof(migration_request));

    ring_buffer_init(&core_states[i].delegation_ack_queue,
                     MAX_FUTURE_DELEGATIONS, core_states[i].delegation_ack_buf,
                     core_states[i].delegation_ack_seq, sizeof(delegation_ack));

    core_states[i].local_criticality_level = 0;
    core_states[i].decision_point = false;
    core_states[i].cached_slack_horizon = calculate_allocated_horizon(i);

    pthread_mutex_init(&core_states[i].rq_lock, NULL);

    core_summaries[i].util = 0.0f;
    core_summaries[i].slack = 0.0f;
    core_summaries[i].next_arrival = UINT32_MAX;
    core_summaries[i].is_idle = true;
    core_summaries[i].dvfs_level = 0;

    pthread_mutex_init(&core_summary_locks[i], NULL);
  }

  init_migration();

  LOG(LOG_LEVEL_INFO, "Scheduler Initialization Complete.");
}

static inline void log_core_state(uint8_t core_id) {
  core_state *cs = &core_states[core_id];

  LOCK_RQ(core_id);
  if (cs->is_idle) {
    LOG(LOG_LEVEL_DEBUG, "Status: IDLE");
  } else {
    LOG(LOG_LEVEL_DEBUG, "Status: RUNNING -> Job %d",
        cs->running_job ? cs->running_job->parent_task->id : -1);
  }

  LOG(LOG_LEVEL_DEBUG, "DVFS Level: %u, Frequency Scaling: %.2f",
      cs->current_dvfs_level, power_get_current_scaling_factor(core_id));
  LOG(LOG_LEVEL_DEBUG, "Criticality Level: %u", cs->local_criticality_level);

  log_job_queue(LOG_LEVEL_DEBUG, "Ready Queue", &cs->ready_queue);
  log_job_queue(LOG_LEVEL_DEBUG, "Replica Queue", &cs->replica_queue);
  log_job_queue(LOG_LEVEL_DEBUG, "Pending Jobs", &cs->pending_jobs_queue);
  UNLOCK_RQ(core_id);
}

void scheduler_tick(uint8_t core_id) {
  core_state *cs = &core_states[core_id];

  if (cs->local_criticality_level !=
      atomic_load(&proc_state.system_criticality_level)) {
    handle_mode_change(core_id, proc_state.system_criticality_level);
  }

  if (cs->dpm_control_block.in_low_power_state) {
    if (cs->dpm_control_block.dpm_end_time <= proc_state.system_time) {
      cs->dpm_control_block.in_low_power_state = false;
      LOG(LOG_LEVEL_INFO, "Exiting low power state");
    } else {
      LOG(LOG_LEVEL_DEBUG, "Core in low power state");
      return;
    }
  }

  handle_running_job(core_id);

  handle_job_arrivals(core_id);

  remove_completed_jobs(core_id);

  reclaim_discarded_jobs(core_id);

  // Migration
  update_delegations(core_id);
  attempt_migration_push(core_id);
  process_migration_requests(core_id);

  job_struct *next_job = select_next_job(core_id);

  if (next_job != NULL) {
    cs->decision_point = true;
    dispatch_job(core_id, next_job);
  }

  if (power_management_try_procrastination(core_id)) {
    goto pmsp_skip;
  }

  if (cs->decision_point) {
    power_set_dvfs_level(core_id, calc_required_dvfs_level(core_id));
  }

  if (cs->is_idle) {
    uint32_t next_eff_arrival_time = find_next_effective_arrival_time(core_id);
    power_management_set_dpm_interval(core_id, next_eff_arrival_time);
  }

pmsp_skip:
  update_core_summary(core_id);

  log_core_state(core_id);
}

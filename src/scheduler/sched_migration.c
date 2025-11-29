#include "lib/ring_buffer.h"
#include "processor.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"

#include "scheduler/sched_core.h"
#include "scheduler/sched_migration.h"
#include "scheduler/sched_util.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>

static delegated_job delegated_jobs_pool[NUM_CORES_PER_PROC]
                                        [MAX_FUTURE_DELEGATIONS];
static struct list_head delegated_jobs_free_list[NUM_CORES_PER_PROC];

static inline bool is_migration_profitable(job_struct *job,
                                           uint32_t current_time) {

  if (current_time < job->next_migration_eligible_tick) {
    return false;
  }

  float remaining = fmaxf(0.0f, job->wcet - job->executed_time);

  if (remaining < MIN_MIGRATION_BENEFIT_THRESHOLD) {
    return false;
  }

  return true;
}

void init_migration(void) {
  for (int i = 0; i < NUM_CORES_PER_PROC; i++) {
    INIT_LIST_HEAD(&delegated_jobs_free_list[i]);
    for (int j = 0; j < MAX_FUTURE_DELEGATIONS; j++) {
      INIT_LIST_HEAD(&delegated_jobs_pool[i][j].link);
      list_add(&delegated_jobs_pool[i][j].link, &delegated_jobs_free_list[i]);
    }
  }
}

static inline delegated_job *create_delegation(uint8_t core_id) {
  if (list_empty(&delegated_jobs_free_list[core_id])) {
    return NULL;
  }
  delegated_job *dj =
      list_first_entry(&delegated_jobs_free_list[core_id], delegated_job, link);
  list_del(&dj->link);
  return dj;
}

static inline void __release_delegation(delegated_job *dj, uint8_t core_id) {
  if (dj == NULL)
    return;

  dj->owned_by_remote = false;

  list_add(&dj->link, &delegated_jobs_free_list[core_id]);
}

void release_delegation(delegated_job *dj, uint8_t core_id) {
  __release_delegation(dj, core_id);
}

static inline void add_delegation_sorted(delegated_job *dj, uint8_t core_id) {
  delegated_job *cursor, *n;

  list_for_each_entry_safe(cursor, n, &core_states[core_id].delegated_job_queue,
                           link) {
    if (dj->arrival_tick < cursor->arrival_tick) {
      list_add_tail(&dj->link, &cursor->link);
      return;
    }
  }

  list_add_tail(&dj->link, &core_states[core_id].delegated_job_queue);
}

void update_delegations(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  delegation_ack ack;
  while (ring_buffer_try_dequeue(&core_state->delegation_ack_queue, &ack) ==
         0) {

    delegated_job *dj;
    list_for_each_entry(dj, &core_state->delegated_job_queue, link) {
      if (dj->task_id == ack.task_id && dj->arrival_tick == ack.arrival_tick) {
        if (ack.accepted) {
          dj->owned_by_remote = true;
        }
        break;
      }
    }
  }
}

static inline void double_rq_lock(uint8_t a, uint8_t b) {
  if (a < b) {
    pthread_mutex_lock(&core_states[a].rq_lock);
    pthread_mutex_lock(&core_states[b].rq_lock);
  } else if (a > b) {
    pthread_mutex_lock(&core_states[b].rq_lock);
    pthread_mutex_lock(&core_states[a].rq_lock);
  } else {
    pthread_mutex_lock(&core_states[a].rq_lock);
  }
}

static inline void double_rq_unlock(uint8_t a, uint8_t b) {
  if (a < b) {
    pthread_mutex_unlock(&core_states[b].rq_lock);
    pthread_mutex_unlock(&core_states[a].rq_lock);
  } else if (a > b) {
    pthread_mutex_unlock(&core_states[a].rq_lock);
    pthread_mutex_unlock(&core_states[b].rq_lock);
  } else {
    pthread_mutex_unlock(&core_states[a].rq_lock);
  }
}

static inline uint8_t find_best_core_for_migration(job_struct *job,
                                                   uint8_t core_id) {
  uint8_t best_core = core_id;
  float max_util = LIGHT_DONOR_UTIL_THRESHOLD;

  float demand = job->wcet - job->executed_time;
  for (uint8_t i = 0; i < NUM_CORES_PER_PROC; i++) {
    core_summary *summary = &core_summaries[i];
    pthread_mutex_lock(&core_summary_locks[i]);
    if (summary->is_idle) {
      pthread_mutex_unlock(&core_summary_locks[i]);
      continue;
    }
    if (summary->slack >= demand && summary->util > max_util) {
      max_util = summary->util;
      best_core = i;
    }
    pthread_mutex_unlock(&core_summary_locks[i]);
  }

  return best_core;
}

static inline void attempt_rq_load_shedding(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  job_struct *job;

  LOCK_RQ(core_id);
  list_for_each_entry_rev(job, &core_state->ready_queue, link) {

    if (!is_migration_profitable(job, proc_state.system_time)) {
      continue;
    }

    if (atomic_exchange(&job->is_being_offered, true)) {
      continue;
    }

    uint8_t dest_core_id = find_best_core_for_migration(job, core_id);

    if (dest_core_id == core_id) {
      atomic_store_explicit(&job->is_being_offered, false,
                            memory_order_release);
      continue;
    }

    migration_request mig_req = {.job = get_job_ref(job), .from_core = core_id};
    ring_buffer *dest_queue =
        &core_states[dest_core_id].migration_request_queue;
    ring_buffer_enqueue(dest_queue, &mig_req);
    LOG(LOG_LEVEL_INFO, "Offered job %d to core %d", job->parent_task->id,
        dest_core_id);
  }
  list_for_each_entry_rev(job, &core_state->replica_queue, link) {

    if (!is_migration_profitable(job, proc_state.system_time)) {
      continue;
    }

    if (atomic_exchange(&job->is_being_offered, true)) {
      continue;
    }

    uint8_t dest_core_id = find_best_core_for_migration(job, core_id);

    if (dest_core_id == core_id) {
      atomic_store_explicit(&job->is_being_offered, false,
                            memory_order_release);
      continue;
    }

    migration_request mig_req = {.job = get_job_ref(job), .from_core = core_id};
    ring_buffer *dest_queue =
        &core_states[dest_core_id].migration_request_queue;
    ring_buffer_enqueue(dest_queue, &mig_req);

    core_state->next_migration_eligible_tick =
        proc_state.system_time + CORE_MIGRATION_COOLDOWN_TICKS;

    LOG(LOG_LEVEL_INFO, "Offered replica job %d to core %d",
        job->parent_task->id, dest_core_id);
  }
  UNLOCK_RQ(core_id);
}

static inline void attempt_future_load_shedding(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];
  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const task_struct *task = find_task_by_id(instance->task_id);

      if (task == NULL) {
        continue;
      }

      if ((float)task->wcet[core_state->local_criticality_level] <
          MIN_MIGRATION_BENEFIT_THRESHOLD) {
        continue;
      }

      uint32_t arrival_time =
          ((proc_state.system_time / task->period) + 1) * task->period;

      if (arrival_time >=
          proc_state.system_time + DPM_MIGRATION_LOOKAHEAD_TICKS) {
        continue;
      }

      delegated_job *dj;
      list_for_each_entry(dj, &core_state->delegated_job_queue, link) {
        if (dj->task_id == task->id && dj->arrival_tick == arrival_time) {
          goto skip;
        }
        if (dj->arrival_tick > arrival_time) {
          break;
        }
      }

      job_struct *new_job = create_job(task, core_id);
      if (new_job == NULL) {
        continue;
      }

      new_job->arrival_time = arrival_time;

      for (uint8_t level = 0; level < MAX_CRITICALITY_LEVELS; level++) {
        new_job->relative_tuned_deadlines[level] =
            instance->tuned_deadlines[level];
      }
      new_job->actual_deadline = arrival_time + new_job->parent_task->deadline;
      new_job->virtual_deadline =
          new_job->arrival_time +
          new_job
              ->relative_tuned_deadlines[core_state->local_criticality_level];
      new_job->acet = generate_acet(new_job);
      new_job->wcet =
          (float)
              new_job->parent_task->wcet[core_state->local_criticality_level];
      new_job->executed_time = 0;

      new_job->is_replica = (instance->task_type == Replica);
      new_job->state = JOB_STATE_IDLE;

      uint8_t best_core_id = find_best_core_for_migration(new_job, core_id);
      if (best_core_id == core_id) {
        put_job_ref(new_job, core_id);
        continue;
      }

      delegated_job *new_dj = create_delegation(core_id);
      if (new_dj == NULL) {
        LOG(LOG_LEVEL_WARN,
            "Failed to create delegation for future job %d, pool empty",
            new_job->parent_task->id);
        put_job_ref(new_job, core_id);
        continue;
      }
      new_dj->task_id = task->id;
      new_dj->arrival_tick = arrival_time;
      new_dj->owned_by_remote = false;

      add_delegation_sorted(new_dj, core_id);

      migration_request mig_req = {.job = new_job, .from_core = core_id};

      ring_buffer_enqueue(&core_states[best_core_id].migration_request_queue,
                          &mig_req);

      core_state->next_migration_eligible_tick =
          proc_state.system_time + CORE_MIGRATION_COOLDOWN_TICKS;

      LOG(LOG_LEVEL_INFO, "Offering future job %d arriving at %d",
          new_job->parent_task->id, new_job->arrival_time);
    }
  skip:
    continue;
  }
}

void attempt_migration_push(uint8_t core_id) {

  core_state *core_state = &core_states[core_id];
  float util = get_util(core_id);

  if (!core_state->is_idle &&
      core_state->next_migration_eligible_tick <= proc_state.system_time &&
      util < LIGHT_DONOR_UTIL_THRESHOLD) {

    bool is_about_to_become_idle = false;

    LOCK_RQ(core_id);
    if (list_empty(&core_state->ready_queue) &&
        list_empty(&core_state->replica_queue)) {
      is_about_to_become_idle = true;
    }
    UNLOCK_RQ(core_id);

    if (is_about_to_become_idle) {
      attempt_future_load_shedding(core_id);
    } else {
      attempt_rq_load_shedding(core_id);
    }
    return;
  }
}

void process_migration_requests(uint8_t core_id) {
  core_state *core_st = &core_states[core_id];
  ring_buffer *mig_queue = &core_st->migration_request_queue;

  migration_request mig_req;
  while (ring_buffer_try_dequeue(mig_queue, &mig_req) == 0) {
    job_struct *job_to_migrate = mig_req.job;

    uint8_t from_core = mig_req.from_core;

    if (!is_admissible(core_id, job_to_migrate, MIGRATION_PENALTY_TICKS)) {
      atomic_store_explicit(&job_to_migrate->is_being_offered, false,
                            memory_order_release);
      LOG(LOG_LEVEL_INFO,
          "Rejected migration of job %d to core %d due to inadmissibility",
          job_to_migrate->parent_task->id, core_id);
      put_job_ref(job_to_migrate, from_core);
      continue;
    }

    if (job_to_migrate->state == JOB_STATE_IDLE &&
        job_to_migrate->arrival_time > proc_state.system_time) {

      add_to_queue_sorted_by_arrival(&core_st->pending_jobs_queue,
                                     job_to_migrate);

      delegation_ack ack = {.task_id = job_to_migrate->parent_task->id,
                            .arrival_tick = job_to_migrate->arrival_time,
                            .accepted = true};

      ring_buffer_enqueue(&core_states[from_core].delegation_ack_queue, &ack);

      atomic_store_explicit(&job_to_migrate->is_being_offered, false,
                            memory_order_release);

      LOG(LOG_LEVEL_INFO, "Migrated future job %d from core %d to core %d",
          job_to_migrate->parent_task->id, from_core, core_id);
      job_to_migrate->next_migration_eligible_tick =
          proc_state.system_time + JOB_MIGRATION_COOLDOWN_TICKS;

      continue;
    }

    double_rq_lock(core_id, from_core);

    if (job_to_migrate->link.prev != NULL &&
        job_to_migrate->link.next != NULL) {
      list_del(&job_to_migrate->link);
    } else {
      atomic_store_explicit(&job_to_migrate->is_being_offered, false,
                            memory_order_release);
      put_job_ref(job_to_migrate, from_core);
      double_rq_unlock(core_id, from_core);
      continue;
    }

    if (job_to_migrate->parent_task->crit_level <
        core_st->local_criticality_level) {
      add_to_queue_sorted(&core_st->discard_list, job_to_migrate);
    } else if (job_to_migrate->is_replica) {
      add_to_queue_sorted(&core_st->replica_queue, job_to_migrate);
    } else {
      add_to_queue_sorted(&core_st->ready_queue, job_to_migrate);
    }

    double_rq_unlock(core_id, from_core);

    atomic_store_explicit(&job_to_migrate->is_being_offered, false,
                          memory_order_release);
    put_job_ref(job_to_migrate, from_core);

    LOG(LOG_LEVEL_INFO, "Migrated job %d from core %d to core %d",
        job_to_migrate->parent_task->id, from_core, core_id);
    job_to_migrate->next_migration_eligible_tick =
        proc_state.system_time + JOB_MIGRATION_COOLDOWN_TICKS;
  }
}

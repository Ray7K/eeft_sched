#include "processor.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"

#include "scheduler/sched_core.h"
#include "scheduler/sched_migration.h"
#include "scheduler/sched_util.h"

#include <stdatomic.h>
#include <stdint.h>

static bid_entry bid_entries[NUM_CORES_PER_PROC][MAX_CONCURRENT_OFFERS];
static struct list_head bid_entry_free_list[NUM_CORES_PER_PROC];

static DelegatedJob delegated_jobs_pool[NUM_CORES_PER_PROC]
                                       [MAX_FUTURE_DELEGATIONS];
static struct list_head delegated_jobs_free_list[NUM_CORES_PER_PROC];

static Offer offer_pool[MAX_CONCURRENT_OFFERS];
struct list_head offer_free_list;

pthread_mutex_t offer_free_list_lock;

void init_migration(void) {
  for (int i = 0; i < NUM_CORES_PER_PROC; i++) {
    INIT_LIST_HEAD(&bid_entry_free_list[i]);

    for (int j = 0; j < MAX_CONCURRENT_OFFERS; j++) {
      INIT_LIST_HEAD(&bid_entries[i][j].link);
      list_add(&bid_entries[i][j].link, &bid_entry_free_list[i]);
    }
  }

  INIT_LIST_HEAD(&offer_free_list);
  pthread_mutex_init(&offer_free_list_lock, NULL);

  for (uint32_t i = 0; i < MAX_CONCURRENT_OFFERS; i++) {
    INIT_LIST_HEAD(&offer_pool[i].link);
    list_add(&offer_pool[i].link, &offer_free_list);
  }

  for (int i = 0; i < NUM_CORES_PER_PROC; i++) {
    INIT_LIST_HEAD(&delegated_jobs_free_list[i]);
    for (int j = 0; j < MAX_FUTURE_DELEGATIONS; j++) {
      INIT_LIST_HEAD(&delegated_jobs_pool[i][j].link);
      list_add(&delegated_jobs_pool[i][j].link, &delegated_jobs_free_list[i]);
    }
  }
}

static inline bid_entry *create_bid_entry(uint8_t core_id) {
  bid_entry *entry = NULL;
  if (!list_empty(&bid_entry_free_list[core_id])) {
    entry = list_first_entry(&bid_entry_free_list[core_id], bid_entry, link);
    list_del(&entry->link);
  }
  return entry;
}

static inline void release_bid_entry(bid_entry *entry, uint8_t core_id) {
  if (entry == NULL) {
    return;
  }
  list_add(&entry->link, &bid_entry_free_list[core_id]);
}

void remove_expired_bid_entries(uint8_t core_id) {
  bid_entry *cur, *next;
  list_for_each_entry_safe(cur, next, &core_states[core_id].bid_history_queue,
                           link) {
    if (cur->expiry_time <= proc_state.system_time) {
      LOG(LOG_LEVEL_DEBUG, "Removing expired bid entry for job %d, Expiry: %d",
          cur->bidded_job->parent_task->id, cur->expiry_time);
      list_del(&cur->link);
      put_job_ref(cur->bidded_job, core_id);
      release_bid_entry(cur, core_id);
    } else {
      break;
    }
  }
}

static inline void add_bid_entry_sorted(bid_entry *new_entry, uint8_t core_id) {
  if (new_entry == NULL) {
    LOG(LOG_LEVEL_ERROR, "Attempted to add NULL bid entry\n");
    return;
  }

  bid_entry *cursor, *n;

  list_for_each_entry_safe(cursor, n, &core_states[core_id].bid_history_queue,
                           link) {
    if (new_entry->expiry_time < cursor->expiry_time) {
      list_add(&new_entry->link, cursor->link.prev);
      return;
    }
  }

  list_add(&new_entry->link, &core_states[core_id].bid_history_queue);
}

static inline Offer *create_offer(void) {
  Offer *offer = NULL;
  pthread_mutex_lock(&offer_free_list_lock);
  if (!list_empty(&offer_free_list)) {
    offer = list_first_entry(&offer_free_list, Offer, link);
    list_del(&offer->link);
  }
  pthread_mutex_unlock(&offer_free_list_lock);
  return offer;
}

static inline void release_offer(Offer *offer) {
  if (offer == NULL) {
    return;
  }

  pthread_mutex_lock(&offer_free_list_lock);
  list_add(&offer->link, &offer_free_list);
  pthread_mutex_unlock(&offer_free_list_lock);
}

static inline DelegatedJob *create_delegation(uint8_t core_id) {
  if (list_empty(&delegated_jobs_free_list[core_id])) {
    return NULL;
  }
  DelegatedJob *dj =
      list_first_entry(&delegated_jobs_free_list[core_id], DelegatedJob, link);
  list_del(&dj->link);
  return dj;
}

static inline void __release_delegation(DelegatedJob *dj, uint8_t core_id) {
  if (dj == NULL)
    return;

  dj->owned_by_remote = false;

  list_add(&dj->link, &delegated_jobs_free_list[core_id]);
}

void release_delegation(DelegatedJob *dj, uint8_t core_id) {
  __release_delegation(dj, core_id);
}

static inline void add_delegation_sorted(DelegatedJob *dj, uint8_t core_id) {
  DelegatedJob *cursor, *n;

  list_for_each_entry_safe(cursor, n, &core_states[core_id].delegated_job_queue,
                           link) {
    if (dj->arrival_tick < cursor->arrival_tick) {
      list_add(&dj->link, cursor->link.prev);
      return;
    }
  }

  list_add(&dj->link, &core_states[core_id].delegated_job_queue);
}

static inline void handle_light_donor_push(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  job_struct *job;
  uint32_t num_offers_sent = 0;

  list_for_each_entry_rev(job, &core_state->ready_queue, link) {
    if (num_offers_sent >= 4) {
      break;
    }

    if (atomic_exchange(&job->is_being_offered, true))
      continue;

    job_struct *cloned_job = clone_job(job, core_id);
    if (cloned_job == NULL) {
      LOG(LOG_LEVEL_WARN, "Failed to clone job %d for migration, pool empty",
          job->parent_task->id);
      atomic_store_explicit(&job->is_being_offered, false,
                            memory_order_release);
      continue;
    }
    cloned_job->virtual_deadline = cloned_job->actual_deadline;
    cloned_job->arrival_time = proc_state.system_time + 2;

    Offer *offer = create_offer();
    if (offer == NULL) {
      LOG(LOG_LEVEL_WARN, "No available offer slots");
      atomic_store_explicit(&job->is_being_offered, false,
                            memory_order_release);
      put_job_ref(cloned_job, core_id);
      continue;
    }

    offer->j_orig = get_job_ref(job);
    offer->j_copy = cloned_job;
    offer->donor_core_id = core_id;
    offer->best_bidder_id = core_id;
    offer->best_bid_metric = get_util(core_id);
    offer->expiry_time = proc_state.system_time + 2;

    LOG(LOG_LEVEL_INFO, "Offering job %d from ready queue",
        offer->j_copy->parent_task->id);

    pthread_mutex_lock(&proc_state.ready_job_offer_queue_lock);
    list_add(&offer->link, &proc_state.ready_job_offer_queue);
    pthread_mutex_unlock(&proc_state.ready_job_offer_queue_lock);

    num_offers_sent++;
  }
  list_for_each_entry_rev(job, &core_state->replica_queue, link) {
    if (num_offers_sent >= 4) {
      break;
    }

    if (atomic_exchange(&job->is_being_offered, true))
      continue;

    job_struct *cloned_job = clone_job(job, core_id);
    if (cloned_job == NULL) {
      LOG(LOG_LEVEL_WARN, "Failed to clone job %d for migration, pool empty",
          job->parent_task->id);
      atomic_store_explicit(&job->is_being_offered, false,
                            memory_order_release);
      continue;
    }
    cloned_job->virtual_deadline = cloned_job->actual_deadline;

    Offer *offer = create_offer();
    if (offer == NULL) {
      LOG(LOG_LEVEL_WARN, "No available offer slots");
      atomic_store_explicit(&job->is_being_offered, false,
                            memory_order_release);
      put_job_ref(cloned_job, core_id);
      continue;
    }

    offer->j_orig = get_job_ref(job);
    offer->j_copy = cloned_job;
    offer->donor_core_id = core_id;
    offer->best_bidder_id = core_id;
    offer->best_bid_metric = get_util(core_id);
    offer->expiry_time = proc_state.system_time + 2;

    LOG(LOG_LEVEL_INFO, "Offering job %d from replica queue",
        offer->j_copy->parent_task->id);

    if (offer->expiry_time > core_state->next_dpm_eligible_tick) {
      core_state->next_dpm_eligible_tick = offer->expiry_time;
    }

    pthread_mutex_lock(&proc_state.ready_job_offer_queue_lock);
    list_add(&offer->link, &proc_state.ready_job_offer_queue);
    pthread_mutex_unlock(&proc_state.ready_job_offer_queue_lock);

    num_offers_sent++;
  }
}

static inline void handle_idle_donor_push(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  uint8_t num_offers = 0;
  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *instance = &allocation_map[i];

    if (num_offers >= 2) {
      return;
    }

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const task_struct *task = find_task_by_id(instance->task_id);

      if (task == NULL) {
        continue;
      }

      uint32_t arrival_time =
          ((proc_state.system_time / task->period) + 1) * task->period;

      if (arrival_time >=
          proc_state.system_time + DPM_MIGRATION_LOOKAHEAD_TICKS) {
        continue;
      }

      DelegatedJob *dj;
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

      Offer *offer = create_offer();

      if (offer == NULL) {
        LOG(LOG_LEVEL_WARN, "No available offer slots");
        release_offer(offer);
        put_job_ref(new_job, core_id);
        continue;
      }

      job_struct *cloned_job = clone_job(new_job, core_id);
      if (cloned_job == NULL) {
        LOG(LOG_LEVEL_WARN,
            "Failed to clone future job %d for migration, pool "
            "empty",
            new_job->parent_task->id);
        release_offer(offer);
        put_job_ref(new_job, core_id);
        continue;
      }

      cloned_job->virtual_deadline = cloned_job->actual_deadline;
      cloned_job->arrival_time =
          new_job->arrival_time >= proc_state.system_time + 2
              ? new_job->arrival_time
              : proc_state.system_time + 2;

      offer->j_orig = new_job;
      offer->j_copy = cloned_job;
      if (offer->j_copy == NULL) {
        perror("copy is null\n");
      }
      offer->donor_core_id = core_id;
      offer->best_bidder_id = core_id;
      offer->best_bid_metric = -1;
      offer->expiry_time = proc_state.system_time + 2;

      LOG(LOG_LEVEL_INFO, "Offering future job %d arriving at %d",
          new_job->parent_task->id, new_job->arrival_time);

      if (offer->expiry_time > core_state->next_dpm_eligible_tick) {
        core_state->next_dpm_eligible_tick = offer->expiry_time;
      }

      if (core_state->next_migration_allowed_tick <=
          core_state->next_dpm_eligible_tick) {
        core_state->next_migration_allowed_tick =
            core_state->next_dpm_eligible_tick + 1;
      }

      pthread_mutex_lock(&proc_state.future_job_offer_queue_lock);
      list_add(&offer->link, &proc_state.future_job_offer_queue);
      pthread_mutex_unlock(&proc_state.future_job_offer_queue_lock);
      num_offers++;
    }
  skip:
    continue;
  }
}

#define LIGHT_DONOR_UTIL_THRESHOLD 0.3f

void attempt_migration_push(uint8_t core_id) {

  core_state *core_state = &core_states[core_id];
  float util = get_util(core_id);

  if (core_state->is_idle &&
      proc_state.system_time > core_state->next_dpm_eligible_tick) {
    handle_idle_donor_push(core_id);
  } else if (proc_state.system_time >=
                 core_state->next_migration_allowed_tick &&
             util < LIGHT_DONOR_UTIL_THRESHOLD) {
    handle_light_donor_push(core_id);
  }
}

#define UTIL_UPPER_CAP 0.85f

void participate_in_auctions(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  if (core_state->is_idle) {
    return;
  }

  pthread_mutex_lock(&proc_state.ready_job_offer_queue_lock);
  Offer *cur, *next;
  list_for_each_entry_safe(cur, next, &proc_state.ready_job_offer_queue, link) {
    if (cur->expiry_time > proc_state.system_time &&
        cur->donor_core_id != core_id) {
      job_struct *candidate = cur->j_copy;
      LOG(LOG_LEVEL_INFO, "Evaluating job offer for Job %d",
          candidate->parent_task->id);
      if (is_admissible(core_id, candidate)) {
        float util = get_util(core_id);
        LOG(LOG_LEVEL_INFO,
            "Bidding for Job %d (Arrival: %d Expiry: %d) with utility %.2f",
            candidate->parent_task->id, candidate->arrival_time,
            cur->expiry_time, util);
        if (util <= UTIL_UPPER_CAP && util > cur->best_bid_metric) {
          bid_entry *new_bid_entry = create_bid_entry(core_id);
          if (new_bid_entry == NULL) {
            LOG(LOG_LEVEL_WARN, "No available bid entry slots");
            continue;
          }
          cur->best_bid_metric = util;
          cur->best_bidder_id = core_id;
          new_bid_entry->bidded_job = get_job_ref(candidate);
          new_bid_entry->bid_time = proc_state.system_time;
          new_bid_entry->expiry_time = cur->expiry_time;

          add_bid_entry_sorted(new_bid_entry, core_id);
        }
      }
    }
  }
  pthread_mutex_unlock(&proc_state.ready_job_offer_queue_lock);

  pthread_mutex_lock(&proc_state.future_job_offer_queue_lock);
  list_for_each_entry_safe(cur, next, &proc_state.future_job_offer_queue,
                           link) {
    if (cur->expiry_time > proc_state.system_time &&
        cur->donor_core_id != core_id) {
      job_struct *candidate = cur->j_copy;
      LOG(LOG_LEVEL_INFO, "Evaluating future job offer for Job %d",
          candidate->parent_task->id);
      if (is_admissible(core_id, candidate)) {
        float util = get_util(core_id);
        LOG(LOG_LEVEL_INFO,
            "Bidding for future Job %d (Arrival: %d) with utility %.2f",
            candidate->parent_task->id, candidate->arrival_time, util);
        if (util <= UTIL_UPPER_CAP && util > cur->best_bid_metric) {
          bid_entry *new_bid_entry = create_bid_entry(core_id);
          if (new_bid_entry == NULL) {
            LOG(LOG_LEVEL_WARN, "No available bid entry slots");
            continue;
          }
          cur->best_bid_metric = util;
          cur->best_bidder_id = core_id;
          new_bid_entry->bidded_job = get_job_ref(candidate);
          new_bid_entry->bid_time = proc_state.system_time;
          new_bid_entry->expiry_time = cur->expiry_time;

          add_bid_entry_sorted(new_bid_entry, core_id);
        }
      }
    }
  }
  pthread_mutex_unlock(&proc_state.future_job_offer_queue_lock);
}

void handle_offer_cleanup(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  Offer *cur, *next;
  pthread_mutex_lock(&proc_state.ready_job_offer_queue_lock);
  list_for_each_entry_safe(cur, next, &proc_state.ready_job_offer_queue, link) {
    if (cur->expiry_time <= proc_state.system_time &&
        cur->donor_core_id == core_id) {

      atomic_store_explicit(&cur->j_orig->is_being_offered, false,
                            memory_order_release);

      LOG(LOG_LEVEL_INFO, "Cleaning up offer for Job %d Expired %d",
          cur->j_copy->parent_task->id, cur->expiry_time);
      list_del(&cur->link);
      if (cur->best_bidder_id != core_id) {
        LOG(LOG_LEVEL_INFO, "Bids received for Job %d",
            cur->j_copy->parent_task->id);
        LOG(LOG_LEVEL_INFO, "Awarding Job %d (Arrival: %d) to Core %d",
            cur->j_copy->parent_task->id, cur->j_copy->arrival_time,
            cur->best_bidder_id);

        if (cur->j_orig->state == JOB_STATE_COMPLETED ||
            cur->j_orig->state == JOB_STATE_REMOVED) {
          goto release;
        } else if (cur->j_orig->state == JOB_STATE_RUNNING) {
          LOG(LOG_LEVEL_INFO, "Preempting Job %d",
              cur->j_orig->parent_task->id);
          core_state->running_job = NULL;
          core_state->is_idle = true;
          core_state->decision_point = true;
          cur->j_orig->state = JOB_STATE_READY;
          put_job_ref(cur->j_orig,
                      core_id); // release: running job owned ref
        } else {
          list_del(&cur->j_orig->link);
          put_job_ref(cur->j_orig,
                      core_id); // release: ready/replica queue owned ref
        }

        job_struct *awarded_job = get_job_ref(cur->j_orig);
        awarded_job->virtual_deadline = awarded_job->actual_deadline;

        core_state->next_migration_allowed_tick =
            proc_state.system_time + MIGRATION_COOLDOWN_TICKS;

        ring_buffer_enqueue(
            &core_states[cur->best_bidder_id].award_notification_queue,
            &awarded_job);
      }
    release:
      put_job_ref(cur->j_orig, core_id); // release: offer owned ref
      put_job_ref(cur->j_copy, core_id);
      release_offer(cur);
    }
  }
  pthread_mutex_unlock(&proc_state.ready_job_offer_queue_lock);

  pthread_mutex_lock(&proc_state.future_job_offer_queue_lock);
  list_for_each_entry_safe(cur, next, &proc_state.future_job_offer_queue,
                           link) {
    if (cur->expiry_time <= proc_state.system_time &&
        cur->donor_core_id == core_id) {

      atomic_store_explicit(&cur->j_orig->is_being_offered, false,
                            memory_order_release);

      LOG(LOG_LEVEL_INFO, "Cleaning up future job offer for Job %d, Expired %d",
          cur->j_copy->parent_task->id, cur->expiry_time);
      list_del(&cur->link);

      if (cur->best_bidder_id != core_id) {
        LOG(LOG_LEVEL_INFO, "Bids received for future Job %d",
            cur->j_copy->parent_task->id);
        LOG(LOG_LEVEL_INFO,
            "Awarding future Job %d (Arrival: %d WCET: %.2f) to Core %d",
            cur->j_copy->parent_task->id, cur->j_copy->arrival_time,
            cur->j_copy->wcet, cur->best_bidder_id % NUM_CORES_PER_PROC);

        job_struct *awarded_job =
            get_job_ref(cur->j_orig); // get: awardee owned ref
        awarded_job->virtual_deadline = awarded_job->actual_deadline;

        core_state->next_migration_allowed_tick =
            proc_state.system_time + MIGRATION_COOLDOWN_TICKS;

        DelegatedJob *dj = create_delegation(core_id);
        if (!dj) {
          LOG(LOG_LEVEL_WARN,
              "Delegation pool exhausted, cannot track delegation");
        } else {
          dj->task_id = awarded_job->parent_task->id;
          dj->arrival_tick = awarded_job->arrival_time;
          dj->owned_by_remote = true;
          add_delegation_sorted(dj, core_id);
        }

        ring_buffer_enqueue(
            &core_states[cur->best_bidder_id].award_notification_queue,
            &awarded_job);
      }

      put_job_ref(cur->j_orig, core_id); // release: offer owned ref
      put_job_ref(cur->j_copy, core_id);
      release_offer(cur);
    }
  }
  pthread_mutex_unlock(&proc_state.future_job_offer_queue_lock);
}

void process_award_notifications(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  job_struct *awarded_job;
  while (ring_buffer_try_dequeue(&core_state->award_notification_queue,
                                 &awarded_job) == 0) {
    LOG(LOG_LEVEL_INFO, "Received award notification for Job %d",
        awarded_job->parent_task->id);

    awarded_job->wcet =
        (float)
            awarded_job->parent_task->wcet[core_state->local_criticality_level];
    awarded_job->virtual_deadline = awarded_job->actual_deadline;

    if (awarded_job->arrival_time > proc_state.system_time) {
      LOG(LOG_LEVEL_INFO,
          "Awarded Job %d is a future job arriving at %d with wcet %0.2f",
          awarded_job->parent_task->id, awarded_job->arrival_time,
          awarded_job->wcet);

      add_to_queue_sorted_by_arrival(&core_state->pending_jobs_queue,
                                     awarded_job);
    } else {
      core_state->decision_point = true;
      awarded_job->state = JOB_STATE_READY;

      if (awarded_job->parent_task->crit_level <
          core_state->local_criticality_level) {
        add_to_queue_sorted(&core_state->discard_list, awarded_job);
      } else {
        core_state->decision_point = true;
        if (awarded_job->is_replica) {
          add_to_queue_sorted(&core_state->replica_queue, awarded_job);
        } else {
          add_to_queue_sorted(&core_state->ready_queue, awarded_job);
        }
      }
    }
  }
}

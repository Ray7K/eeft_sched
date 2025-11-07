#include "processor.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"

#include "scheduler/sched_core.h"
#include "scheduler/sched_migration.h"
#include "scheduler/sched_util.h"

#include <math.h>

#define DEMAND_PADDING_PERCENT 0.25f

float generate_acet(Job *job) {
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

  float acet = rand_between(0.1f, 1.0f) *
               (float)job->parent_task->wcet[criticality_level];

  return acet;
}

float find_slack(uint16_t core_id, CriticalityLevel crit_lvl, uint32_t tstart,
                 uint32_t tend, float scaling_factor) {
  CoreState *core_state = &core_states[core_id];
  uint32_t current_time = processor_state.system_time;

  if (tend <= current_time) {
    return 0;
  }
  if (tstart >= tend) {
    return 0;
  }
  if (crit_lvl >= MAX_CRITICALITY_LEVELS) {
    return 0;
  }
  if (scaling_factor <= 0.0f) {
    scaling_factor = 1.0f;
  }

  tstart = tstart > current_time ? tstart : current_time;

  float demand = 0;

  float interval_length = (float)(tend - tstart);

  if (core_state->running_job != NULL) {
    uint32_t virtual_deadline =
        core_state->running_job->arrival_time +
        core_state->running_job->relative_tuned_deadlines[crit_lvl];
    if (virtual_deadline > tstart && virtual_deadline <= tend) {
      float wcet = (float)core_state->running_job->parent_task->wcet[crit_lvl];
      float executed_time = core_state->running_job->executed_time;

      demand += ceilf((wcet - executed_time) / scaling_factor);
    }
  }

  Job *job;
  list_for_each_entry(job, &core_state->ready_queue, link) {
    uint32_t virtual_deadline =
        job->arrival_time + job->relative_tuned_deadlines[crit_lvl];
    if (virtual_deadline > tstart && virtual_deadline <= tend) {
      float wcet = (float)job->parent_task->wcet[crit_lvl];
      float executed_time = job->executed_time;

      demand += ceilf((wcet - executed_time) / scaling_factor);
    }
  }
  list_for_each_entry(job, &core_state->replica_queue, link) {
    uint32_t virtual_deadline =
        job->arrival_time + job->relative_tuned_deadlines[crit_lvl];
    if (virtual_deadline > tstart && virtual_deadline <= tend) {
      float wcet = (float)job->parent_task->wcet[crit_lvl];
      float executed_time = job->executed_time;

      demand += ceilf((wcet - executed_time) / scaling_factor);
    }
  }
  list_for_each_entry(job, &core_state->pending_jobs_queue, link) {
    uint32_t virtual_deadline =
        job->arrival_time + job->relative_tuned_deadlines[crit_lvl];
    if (virtual_deadline > tstart && virtual_deadline <= tend) {
      float wcet = (float)job->parent_task->wcet[crit_lvl];
      float executed_time = job->executed_time;

      demand += ceilf((wcet - executed_time) / scaling_factor);
    }
  }

  bid_entry *entry;
  list_for_each_entry(entry, &core_state->bid_history_queue, link) {
    job = entry->bidded_job;
    uint32_t virtual_deadline =
        job->arrival_time + job->relative_tuned_deadlines[crit_lvl];
    if (virtual_deadline > tstart && virtual_deadline <= tend) {
      float wcet = (float)job->parent_task->wcet[crit_lvl];
      float executed_time = job->executed_time;

      demand += ceilf((wcet - executed_time) / scaling_factor);
    }
  }

  if (demand >= interval_length) {
    return 0;
  }

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const TaskAllocationMap *instance = &allocation_map[i];

    if (instance->proc_id == core_state->proc_id &&
        instance->core_id == core_state->core_id) {
      const Task *task = find_task_by_id(instance->task_id);

      if (task->criticality_level >= crit_lvl) {
        uint32_t wcet = task->wcet[crit_lvl];
        uint32_t period = task->period;

        uint32_t arrival_time = (tstart / period + 1) * period;
        while (1) {
          uint32_t future_job_deadline =
              arrival_time + instance->tuned_deadlines[crit_lvl];

          if (future_job_deadline > tend)
            break;

          if (future_job_deadline > tstart)
            demand += ceilf((float)wcet / scaling_factor);

          arrival_time += period;

          if (demand >= interval_length) {
            return 0;
          }
        }
      }
    }
  }

  if (demand >= interval_length) {
    return 0;
  }

  return interval_length - demand;
}

uint32_t find_next_effective_arrival_time(uint16_t core_id) {
  CoreState *core_state = &core_states[core_id];
  uint32_t min_arrival_time = UINT32_MAX;

  Job *pending;
  list_for_each_entry(pending, &core_state->pending_jobs_queue, link) {
    if (pending->arrival_time >= processor_state.system_time &&
        pending->arrival_time < min_arrival_time) {
      min_arrival_time = pending->arrival_time;
    }
  }

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const TaskAllocationMap *instance = &allocation_map[i];

    if (instance->proc_id != core_state->proc_id ||
        instance->core_id != core_state->core_id)
      continue;

    const Task *task = find_task_by_id(instance->task_id);
    if (!task || task->period == 0)
      continue;

    uint32_t current_time = processor_state.system_time;
    uint32_t remainder = current_time % task->period;
    uint32_t next_arrival = (remainder == 0)
                                ? current_time
                                : current_time + (task->period - remainder);

    struct list_head *deleg_list = &core_state->delegated_job_queue;
    DelegatedJob *dj;

    list_for_each_entry(dj, deleg_list, link) {
      if (dj->arrival_tick < next_arrival)
        continue;
      if (dj->arrival_tick == next_arrival && dj->task_id == task->id) {
        goto skip_task;
      }
      break;
    }

    if (next_arrival < min_arrival_time)
      min_arrival_time = next_arrival;

  skip_task:
    continue;
  }

  return min_arrival_time;
}

bool is_admissible(uint16_t core_id, Job *candidate_job) {
  CoreState *core_state = &core_states[core_id];
  float max_scaling_factor = 1.0f;

  CriticalityLevel cur_criticality_level = core_state->local_criticality_level;
  uint32_t tstart = candidate_job->arrival_time;

  for (uint8_t crit_lvl = cur_criticality_level;
       crit_lvl < MAX_CRITICALITY_LEVELS; crit_lvl++) {
    float wcet = (float)candidate_job->parent_task->wcet[crit_lvl];
    float executed_time = candidate_job->executed_time;
    float time_needed_for_candidate =
        ceilf((wcet - executed_time) / max_scaling_factor *
              (1 + DEMAND_PADDING_PERCENT));
    uint32_t virtual_deadline =
        candidate_job->arrival_time +
        candidate_job->relative_tuned_deadlines[crit_lvl];
    uint32_t tend = virtual_deadline;

    if (find_slack(core_id, crit_lvl, tstart, tend, max_scaling_factor) <
        time_needed_for_candidate) {
      return false;
    }

    Job *cur;
    list_for_each_entry(cur, &core_state->replica_queue, link) {
      virtual_deadline =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];
      tend = virtual_deadline;

      float slack =
          find_slack(core_id, crit_lvl, tstart, tend, max_scaling_factor);
      if (slack < time_needed_for_candidate) {
        return false;
      }
    }
    list_for_each_entry(cur, &core_state->ready_queue, link) {
      virtual_deadline =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];
      tend = virtual_deadline;

      float slack =
          find_slack(core_id, crit_lvl, tstart, tend, max_scaling_factor);
      if (slack < time_needed_for_candidate) {
        return false;
      }
    }
    list_for_each_entry(cur, &core_state->pending_jobs_queue, link) {
      virtual_deadline =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];
      tend = virtual_deadline;

      float slack =
          find_slack(core_id, crit_lvl, tstart, tend, max_scaling_factor);
      if (slack < time_needed_for_candidate) {
        return false;
      }
    }
    bid_entry *entry;
    list_for_each_entry(entry, &core_state->bid_history_queue, link) {
      cur = entry->bidded_job;
      virtual_deadline =
          cur->arrival_time + cur->relative_tuned_deadlines[crit_lvl];
      tend = virtual_deadline;

      float slack =
          find_slack(core_id, crit_lvl, tstart, tend, max_scaling_factor);
      if (slack < time_needed_for_candidate) {
        return false;
      }
    }
  }

  return true;
}

float get_util(uint16_t core_id) {
  CoreState *core_state = &core_states[core_id];

  float util = 0.0f;

  Job *cur;
  list_for_each_entry(cur, &core_state->ready_queue, link) {
    util += cur->wcet / (float)cur->parent_task->period;
  }

  list_for_each_entry(cur, &core_state->replica_queue, link) {
    util += cur->wcet / (float)cur->parent_task->period;
  }

  return util;
}

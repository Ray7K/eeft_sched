#include "processor.h"
#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"

#include "lib/math.h"

#include "scheduler/sched_core.h"
#include "scheduler/sched_migration.h"
#include "scheduler/sched_util.h"

#include <float.h>
#include <math.h>

#define DEMAND_PADDING_PERCENT 0.1f
#define SLACK_CALC_HORIZON_TICKS_CAP 5000
#define MAX_DEADLINES (MAX_TASKS * 64)

float generate_acet(job_struct *job) {
  uint8_t criticality_chance = rand() % 100;

  criticality_level criticality_level = job->parent_task->crit_level;

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

  if (criticality_level < proc_state.system_criticality_level) {
    criticality_level = proc_state.system_criticality_level;
  }

  criticality_level = 0;

  float acet = rand_between(0.9f, 0.9f) *
               (float)job->parent_task->wcet[criticality_level];

  return acet;
}

uint32_t calculate_allocated_horizon(uint8_t core_id) {
  CoreState *core_state = &core_states[core_id];

  uint32_t horizon = 0;

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *m = &allocation_map[i];
    if (m->proc_id == core_state->proc_id &&
        m->core_id == core_state->core_id) {

      const task_struct *t = find_task_by_id(m->task_id);
      if (!t || t->period == 0)
        continue;

      if (horizon == 0) {
        horizon = t->period;
      } else {
        horizon = safe_lcm(horizon, t->period, SLACK_CALC_HORIZON_TICKS_CAP);
        if (horizon >= SLACK_CALC_HORIZON_TICKS_CAP) {
          horizon = SLACK_CALC_HORIZON_TICKS_CAP;
          break;
        }
      }
    }
  }

  return horizon;
}

static uint32_t calculate_horizon(uint8_t core_id) {
  CoreState *core_state = &core_states[core_id];

  uint32_t horizon = core_state->cached_slack_horizon;

  job_struct *job;
  list_for_each_entry(job, &core_state->ready_queue, link) {
    if (horizon == 0) {
      horizon = job->parent_task->period;
    } else {
      horizon = safe_lcm(horizon, job->parent_task->period,
                         SLACK_CALC_HORIZON_TICKS_CAP);
      if (horizon >= SLACK_CALC_HORIZON_TICKS_CAP) {
        horizon = SLACK_CALC_HORIZON_TICKS_CAP;
        break;
      }
    }
  }

  list_for_each_entry(job, &core_state->replica_queue, link) {
    if (horizon == 0) {
      horizon = job->parent_task->period;
    } else {
      horizon = safe_lcm(horizon, job->parent_task->period,
                         SLACK_CALC_HORIZON_TICKS_CAP);
      if (horizon >= SLACK_CALC_HORIZON_TICKS_CAP) {
        horizon = SLACK_CALC_HORIZON_TICKS_CAP;
        break;
      }
    }
  }

  list_for_each_entry(job, &core_state->pending_jobs_queue, link) {
    if (horizon == 0) {
      horizon = job->parent_task->period;
    } else {
      horizon = safe_lcm(horizon, job->parent_task->period,
                         SLACK_CALC_HORIZON_TICKS_CAP);
      if (horizon >= SLACK_CALC_HORIZON_TICKS_CAP) {
        horizon = SLACK_CALC_HORIZON_TICKS_CAP;
        break;
      }
    }
  }

  bid_entry *be;
  list_for_each_entry(be, &core_state->bid_history_queue, link) {
    job = be->bidded_job;
    if (horizon == 0) {
      horizon = job->parent_task->period;
    } else {
      horizon = safe_lcm(horizon, job->parent_task->period,
                         SLACK_CALC_HORIZON_TICKS_CAP);
      if (horizon >= SLACK_CALC_HORIZON_TICKS_CAP) {
        horizon = SLACK_CALC_HORIZON_TICKS_CAP;
        break;
      }
    }
  }
  return horizon;
}

static uint32_t collect_active_and_future_deadlines(
    uint8_t core_id, criticality_level crit_lvl, uint32_t tstart,
    uint32_t deadlines[], uint32_t max_deadlines, const job_struct *extra_job) {
  if (max_deadlines == 0 || crit_lvl >= MAX_CRITICALITY_LEVELS) {
    return 0;
  }

  CoreState *core_state = &core_states[core_id];
  uint32_t count = 0;
  uint32_t horizon = calculate_horizon(core_id);

  if (extra_job) {
    uint32_t d = (extra_job->job_pool_id != core_state->core_id)
                     ? extra_job->actual_deadline
                     : (extra_job->arrival_time +
                        extra_job->relative_tuned_deadlines[crit_lvl]);
    if (d > tstart && count < max_deadlines)
      deadlines[count++] = d;
  }

  if (core_state->running_job) {
    uint32_t d =
        (core_state->running_job->job_pool_id != core_state->core_id)
            ? core_state->running_job->actual_deadline
            : core_state->running_job->arrival_time +
                  core_state->running_job->relative_tuned_deadlines[crit_lvl];
    if (d > tstart && count < max_deadlines)
      deadlines[count++] = d;
  }

  job_struct *job;
  list_for_each_entry(job, &core_state->ready_queue, link) {
    uint32_t d =
        (job->job_pool_id != core_state->core_id)
            ? job->actual_deadline
            : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
    if (d > tstart && count < max_deadlines) {
      deadlines[count++] = d;
    }
  }

  list_for_each_entry(job, &core_state->replica_queue, link) {
    uint32_t d =
        (job->job_pool_id != core_state->core_id)
            ? job->actual_deadline
            : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
    if (d > tstart && count < max_deadlines) {
      deadlines[count++] = d;
    }
  }

  list_for_each_entry(job, &core_state->pending_jobs_queue, link) {
    uint32_t d =
        (job->job_pool_id != core_state->core_id)
            ? job->actual_deadline
            : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
    if (d > tstart && count < max_deadlines) {
      deadlines[count++] = d;
    }
  }

  bid_entry *be;
  list_for_each_entry(be, &core_state->bid_history_queue, link) {
    job = be->bidded_job;
    uint32_t d =
        (job->job_pool_id != core_state->core_id)
            ? job->actual_deadline
            : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
    if (d > tstart && count < max_deadlines) {
      deadlines[count++] = d;
    }
  }

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *m = &allocation_map[i];
    if (m->proc_id != core_state->proc_id || m->core_id != core_state->core_id)
      continue;

    const task_struct *task = find_task_by_id(m->task_id);
    if (task->crit_level < crit_lvl)
      continue;

    uint32_t period = task->period;
    uint32_t deadline = m->tuned_deadlines[crit_lvl];
    uint32_t arrival = (tstart / period + 1) * period;

    while (arrival + deadline > tstart &&
           arrival + deadline <= tstart + horizon && count < max_deadlines) {
      deadlines[count++] = arrival + deadline;
      arrival += period;
    }
  }

  qsort(deadlines, count, sizeof(uint32_t), cmp_uint32);
  uint32_t unique = 0;
  for (uint32_t i = 1; i < count; i++) {
    if (deadlines[i] != deadlines[unique]) {
      deadlines[++unique] = deadlines[i];
    }
  }
  return unique + 1;
}

float find_slack(uint8_t core_id, criticality_level crit_lvl, uint32_t tstart,
                 float scaling_factor, const job_struct *extra_job) {
  if (crit_lvl >= MAX_CRITICALITY_LEVELS)
    return 0.0f;
  if (scaling_factor <= 0.0f)
    scaling_factor = 1.0f;

  CoreState *core_state = &core_states[core_id];
  const uint32_t current_time = proc_state.system_time;
  tstart = tstart > current_time ? tstart : current_time;

  uint32_t deadlines[MAX_DEADLINES];
  uint32_t dcount = collect_active_and_future_deadlines(
      core_id, crit_lvl, tstart, deadlines, MAX_DEADLINES, extra_job);

  if (dcount == 0)
    return FLT_MAX;

  float min_slack = FLT_MAX;

  for (uint32_t i = 0; i < dcount; i++) {
    uint32_t d = deadlines[i];
    float demand = 0.0f;

    job_struct *job;
    list_for_each_entry(job, &core_state->ready_queue, link) {
      uint32_t vdl =
          (job->job_pool_id != core_state->core_id)
              ? job->actual_deadline
              : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
      if (vdl <= d) {
        float wcet = (float)job->parent_task->wcet[crit_lvl];
        float exec = job->executed_time;
        demand += fmaxf(0.0f, (wcet - exec) / scaling_factor);
      }
    }

    list_for_each_entry(job, &core_state->replica_queue, link) {
      uint32_t vdl =
          (job->job_pool_id != core_state->core_id)
              ? job->actual_deadline
              : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
      if (vdl <= d) {
        float wcet = (float)job->parent_task->wcet[crit_lvl];
        float exec = job->executed_time;
        demand += fmaxf(0.0f, (wcet - exec) / scaling_factor);
      }
    }

    list_for_each_entry(job, &core_state->pending_jobs_queue, link) {
      uint32_t vdl =
          (job->job_pool_id != core_state->core_id)
              ? job->actual_deadline
              : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
      if (vdl <= d) {
        float wcet = (float)job->parent_task->wcet[crit_lvl];
        float exec = job->executed_time;
        demand += fmaxf(0.0f, (wcet - exec) / scaling_factor);
      }
    }

    bid_entry *be;
    list_for_each_entry(be, &core_state->bid_history_queue, link) {
      job = be->bidded_job;
      uint32_t vdl =
          (job->job_pool_id != core_state->core_id)
              ? job->actual_deadline
              : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
      if (vdl <= d) {
        float wcet = (float)job->parent_task->wcet[crit_lvl];
        float exec = job->executed_time;
        demand += fmaxf(0.0f, (wcet - exec) / scaling_factor);
      }
    }
    if (core_state->running_job) {
      job = core_state->running_job;
      uint32_t vdl =
          (job->job_pool_id != core_state->core_id)
              ? job->actual_deadline
              : (job->arrival_time + job->relative_tuned_deadlines[crit_lvl]);
      if (vdl <= d) {
        float wcet =
            (float)core_state->running_job->parent_task->wcet[crit_lvl];
        float exec = core_state->running_job->executed_time;
        demand += fmaxf(0.0f, (wcet - exec) / scaling_factor);
      }
    }

    for (uint32_t k = 0; k < ALLOCATION_MAP_SIZE; k++) {
      const task_alloc_map *m = &allocation_map[k];
      if (m->proc_id != core_state->proc_id ||
          m->core_id != core_state->core_id)
        continue;

      const task_struct *task = find_task_by_id(m->task_id);
      if (!task || task->crit_level < crit_lvl)
        continue;

      uint32_t wcet = task->wcet[crit_lvl];
      uint32_t period = task->period;
      uint32_t tuned_dl = m->tuned_deadlines[crit_lvl];
      uint32_t arrival = (tstart / period + 1) * period;

      while (arrival + tuned_dl <= d) {
        demand += (float)wcet / scaling_factor;
        arrival += period;
      }
    }

    if (extra_job) {
      uint32_t vdl = (extra_job->job_pool_id != core_state->core_id)
                         ? extra_job->actual_deadline
                         : (extra_job->arrival_time +
                            extra_job->relative_tuned_deadlines[crit_lvl]);

      float rem = (float)extra_job->parent_task->wcet[crit_lvl] -
                  extra_job->executed_time;
      if (vdl <= d) {
        demand += fmaxf(0.0f, rem / scaling_factor);
      } else if (extra_job->arrival_time < d) {
        float available_before_d = (float)(d - extra_job->arrival_time);
        demand += fminf(fmaxf(0.0f, rem / scaling_factor), available_before_d);
      }
    }

    float interval = (float)(d - tstart);
    float slack = interval - demand;
    if (slack < min_slack) {
      min_slack = slack;
    }
  }

  if (min_slack < 0.0f) {
    min_slack = 0.0f;
  }

  return min_slack;
}

uint32_t find_next_effective_arrival_time(uint8_t core_id) {
  CoreState *core_state = &core_states[core_id];
  uint32_t min_arrival_time = UINT32_MAX;

  job_struct *pending;
  list_for_each_entry(pending, &core_state->pending_jobs_queue, link) {
    if (pending->arrival_time >= proc_state.system_time &&
        pending->arrival_time < min_arrival_time) {
      min_arrival_time = pending->arrival_time;
    }
  }

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *instance = &allocation_map[i];

    if (instance->proc_id != core_state->proc_id ||
        instance->core_id != core_state->core_id)
      continue;

    const task_struct *task = find_task_by_id(instance->task_id);
    if (!task || task->period == 0)
      continue;

    uint32_t current_time = proc_state.system_time;
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

bool is_admissible(uint8_t core_id, job_struct *candidate_job) {
  CoreState *core_state = &core_states[core_id];
  uint32_t tstart = candidate_job->arrival_time;
  float scaling = 1.0f;

  for (uint8_t crit_lvl = core_state->local_criticality_level;
       crit_lvl < MAX_CRITICALITY_LEVELS; crit_lvl++) {

    uint32_t now = proc_state.system_time;
    uint32_t virtual_deadline =
        candidate_job->arrival_time +
        candidate_job->relative_tuned_deadlines[crit_lvl];
    if (virtual_deadline <= now) {
      return false;
    }

    float wcet = (float)candidate_job->parent_task->wcet[crit_lvl];
    float executed = candidate_job->executed_time;
    float needed = fmaxf(0.0f, (wcet - executed) / scaling) *
                   (1.0f + DEMAND_PADDING_PERCENT);

    float available =
        find_slack(core_id, crit_lvl, tstart, scaling, candidate_job);

    if (available < needed) {
      return false;
    }
  }

  return true;
}

float get_util(uint8_t core_id) {
  CoreState *core_state = &core_states[core_id];

  float util = 0.0f;

  job_struct *cur;
  list_for_each_entry(cur, &core_state->ready_queue, link) {
    util += cur->wcet / (float)cur->parent_task->period;
  }

  list_for_each_entry(cur, &core_state->replica_queue, link) {
    util += cur->wcet / (float)cur->parent_task->period;
  }

  return util;
}

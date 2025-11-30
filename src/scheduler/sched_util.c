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

#define SLACK_MARGIN_TICKS 0.05f
#define SLACK_CALC_HORIZON_TICKS_CAP 5000
#define MAX_DEADLINES (MAX_TASKS * 64)

float generate_acet(job_struct *job) {
  const float bias_factor = 2.0f; // >1 biases toward lower criticalities
  uint8_t max_lvl = MAX_CRITICALITY_LEVELS - 1;

  float r = rand_between(0.0f, 1.0f);

  uint8_t crit = (uint8_t)((float)max_lvl * powf(r, bias_factor));

  if (crit < proc_state.system_criticality_level)
    crit = proc_state.system_criticality_level;

  float acet_fraction = rand_between(0.1f, 1.0f);

  float acet = acet_fraction * (float)job->parent_task->wcet[crit];

  return acet;
}

uint32_t calculate_allocated_horizon(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  uint32_t horizon = 1;

  for (uint32_t i = 0; i < ALLOCATION_MAP_SIZE; i++) {
    const task_alloc_map *m = &allocation_map[i];
    if (m->proc_id == core_state->proc_id &&
        m->core_id == core_state->core_id) {

      const task_struct *t = find_task_by_id(m->task_id);

      if (!t || t->period == 0)
        continue;

      horizon = safe_lcm(horizon, t->period, SLACK_CALC_HORIZON_TICKS_CAP);
      if (horizon >= SLACK_CALC_HORIZON_TICKS_CAP) {
        horizon = SLACK_CALC_HORIZON_TICKS_CAP;
        break;
      }
    }
  }

  return horizon;
}

static inline void calculate_queue_horizon(struct list_head *queue,
                                           uint32_t *horizon) {
  job_struct *job;
  list_for_each_entry(job, queue, link) {
    if (*horizon == 0) {
      *horizon = job->parent_task->period;
    } else {
      *horizon = safe_lcm(*horizon, job->parent_task->period,
                          SLACK_CALC_HORIZON_TICKS_CAP);
      if (*horizon >= SLACK_CALC_HORIZON_TICKS_CAP) {
        *horizon = SLACK_CALC_HORIZON_TICKS_CAP;
        return;
      }
    }
  }
}

static uint32_t calculate_horizon(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];

  uint32_t horizon = core_state->cached_slack_horizon;

  if (core_state->running_job) {
    horizon = safe_lcm(horizon, core_state->running_job->parent_task->period,
                       SLACK_CALC_HORIZON_TICKS_CAP);
  }
  calculate_queue_horizon(&core_state->ready_queue, &horizon);
  calculate_queue_horizon(&core_state->replica_queue, &horizon);
  calculate_queue_horizon(&core_state->pending_jobs_queue, &horizon);

  return horizon;
}

static inline uint32_t get_job_deadline(const job_struct *job,
                                        criticality_level crit_lvl) {
  return job->arrival_time + job->relative_tuned_deadlines[crit_lvl];
}

static inline void add_deadline_to_array(uint32_t d, uint32_t tstart,
                                         uint32_t *deadlines, uint32_t *count,
                                         uint32_t max_deadlines) {
  if (d > tstart && *count < max_deadlines) {
    deadlines[(*count)++] = d;
  }
}

static inline void process_job_deadline(const job_struct *job,
                                        criticality_level crit_lvl,
                                        uint32_t tstart, uint32_t *deadlines,
                                        uint32_t *count,
                                        uint32_t max_deadlines) {
  if (job) {
    uint32_t d = get_job_deadline(job, crit_lvl);
    add_deadline_to_array(d, tstart, deadlines, count, max_deadlines);
  }
}

static inline void process_queue_deadlines(struct list_head *queue,
                                           criticality_level crit_lvl,
                                           uint32_t tstart, uint32_t *deadlines,
                                           uint32_t *count,
                                           uint32_t max_deadlines) {
  job_struct *job;
  list_for_each_entry(job, queue, link) {
    process_job_deadline(job, crit_lvl, tstart, deadlines, count,
                         max_deadlines);
  }
}

static uint32_t collect_active_and_future_deadlines(
    uint8_t core_id, criticality_level crit_lvl, uint32_t tstart,
    uint32_t deadlines[], uint32_t max_deadlines, const job_struct *extra_job) {

  if (max_deadlines == 0 || crit_lvl >= MAX_CRITICALITY_LEVELS) {
    return 0;
  }

  core_state *core_state = &core_states[core_id];
  uint32_t count = 0;
  uint32_t horizon = calculate_horizon(core_id);

  process_job_deadline(extra_job, crit_lvl, tstart, deadlines, &count,
                       max_deadlines);
  process_job_deadline(core_state->running_job, crit_lvl, tstart, deadlines,
                       &count, max_deadlines);

  process_queue_deadlines(&core_state->ready_queue, crit_lvl, tstart, deadlines,
                          &count, max_deadlines);
  process_queue_deadlines(&core_state->replica_queue, crit_lvl, tstart,
                          deadlines, &count, max_deadlines);
  process_queue_deadlines(&core_state->pending_jobs_queue, crit_lvl, tstart,
                          deadlines, &count, max_deadlines);

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

  if (count == 0)
    return 0;

  qsort(deadlines, count, sizeof(uint32_t), cmp_uint32);

  uint32_t unique = 0;
  for (uint32_t i = 1; i < count; i++) {
    if (deadlines[i] != deadlines[unique]) {
      deadlines[++unique] = deadlines[i];
    }
  }
  return unique + 1;
}

static inline float calculate_job_demand(const job_struct *j,
                                         criticality_level crit_lvl, uint32_t d,
                                         float scaling_factor) {

  uint32_t vdl = j->arrival_time + j->relative_tuned_deadlines[crit_lvl];
  if (vdl <= d) {
    float wcet = (float)j->parent_task->wcet[crit_lvl];
    float exec = j->executed_time;
    return fmaxf(0.0f, (wcet - exec) / scaling_factor);
  }

  return 0.0f;
}

static float __find_slack(uint8_t core_id, criticality_level crit_lvl,
                          uint32_t tstart, float scaling_factor,
                          const job_struct *extra_job) {
  if (crit_lvl >= MAX_CRITICALITY_LEVELS)
    return 0.0f;
  if (scaling_factor <= 0.0f)
    scaling_factor = 1.0f;

  core_state *core_state = &core_states[core_id];
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

    if (core_state->running_job) {
      demand += calculate_job_demand(core_state->running_job, crit_lvl, d,
                                     scaling_factor);
    }

    list_for_each_entry(job, &core_state->ready_queue, link) {
      demand += calculate_job_demand(job, crit_lvl, d, scaling_factor);
    }

    list_for_each_entry(job, &core_state->replica_queue, link) {
      demand += calculate_job_demand(job, crit_lvl, d, scaling_factor);
    }

    list_for_each_entry(job, &core_state->pending_jobs_queue, link) {
      demand += calculate_job_demand(job, crit_lvl, d, scaling_factor);
    }

    if (extra_job) {
      demand += calculate_job_demand(extra_job, crit_lvl, d, scaling_factor);
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

    float slack = (float)(d - tstart) - demand;
    if (slack < min_slack) {
      min_slack = slack;
    }
  }

  return (min_slack < 0.0f) ? 0.0f : min_slack;
}

static bool __is_admissible(uint8_t core_id, job_struct *candidate_job,
                            float extra_margin) {
  core_state *core_state = &core_states[core_id];
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

    float needed = SLACK_MARGIN_TICKS + extra_margin;

    float available =
        __find_slack(core_id, crit_lvl, tstart, scaling, candidate_job);

    if (available < needed) {
      return false;
    }
  }

  return true;
}

float find_slack(uint8_t core_id, criticality_level crit_lvl, uint32_t tstart,
                 float scaling_factor, const job_struct *extra_job) {
  float retval;

  LOCK_RQ(core_id);
  retval = __find_slack(core_id, crit_lvl, tstart, scaling_factor, extra_job);
  UNLOCK_RQ(core_id);

  return retval;
}

float find_slack_locked(uint8_t core_id, criticality_level crit_lvl,
                        uint32_t tstart, float scaling_factor,
                        const job_struct *extra_job) {
  return __find_slack(core_id, crit_lvl, tstart, scaling_factor, extra_job);
}

bool is_admissible(uint8_t core_id, job_struct *candidate_job,
                   float extra_margin) {
  bool retval;

  LOCK_RQ(core_id);
  retval = __is_admissible(core_id, candidate_job, extra_margin);
  UNLOCK_RQ(core_id);

  return retval;
}

bool is_admissible_locked(uint8_t core_id, job_struct *candidate_job,
                          float extra_margin) {
  return __is_admissible(core_id, candidate_job, extra_margin);
}

uint32_t find_next_effective_arrival_time(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];
  uint32_t min_arrival_time = UINT32_MAX;

  job_struct *pending;
  list_for_each_entry(pending, &core_state->pending_jobs_queue, link) {
    if (pending->arrival_time > proc_state.system_time &&
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
    if (!task || task->period == 0 ||
        task->crit_level < core_state->local_criticality_level)
      continue;

    uint32_t current_time = proc_state.system_time;
    uint32_t remainder = current_time % task->period;
    uint32_t next_arrival = current_time + (task->period - remainder);

    struct list_head *deleg_list = &core_state->delegated_job_queue;
    delegated_job *dj;

    list_for_each_entry(dj, deleg_list, link) {
      if (dj->arrival_tick == next_arrival && dj->task_id == task->id &&
          dj->owned_by_remote) {
        next_arrival += task->period;
      }
    }

    if (next_arrival < min_arrival_time)
      min_arrival_time = next_arrival;
  }

  return min_arrival_time;
}

float get_util(uint8_t core_id) {
  core_state *core_state = &core_states[core_id];
  float util = 0.0f;

  LOCK_RQ(core_id);

  if (core_state->running_job) {
    float remaining = fmaxf(0.0f, core_state->running_job->wcet -
                                      core_state->running_job->executed_time);
    util += remaining / (float)core_state->running_job->parent_task->period;
  }

  job_struct *cur;
  list_for_each_entry(cur, &core_state->ready_queue, link) {
    float remaining = fmaxf(0.0f, cur->wcet - cur->executed_time);
    util += remaining / (float)cur->parent_task->period;
  }

  list_for_each_entry(cur, &core_state->replica_queue, link) {
    float remaining = fmaxf(0.0f, cur->wcet - cur->executed_time);
    util += remaining / (float)cur->parent_task->period;
  }

  UNLOCK_RQ(core_id);

  return util;
}

#include "processor.h"
#include "task_alloc.h"
#include "task_management.h"

#include "scheduler/sched_core.h"
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

float find_slack(uint16_t core_id, uint32_t t, float scaling_factor) {
  CoreState *core_state = &core_states[core_id];
  uint32_t current_time = processor_state.system_time;

  if (t <= current_time) {
    return 0;
  }

  float demand = 0;

  if (core_state->running_job != NULL) {
    if (core_state->running_job->virtual_deadline <= t) {
      demand += ceilf((core_state->running_job->wcet -
                       core_state->running_job->executed_time) /
                      scaling_factor);
    }
  }

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
  list_for_each_entry(job, &core_state->pending_jobs_queue, link) {
    if (job->virtual_deadline <= t) {
      demand += ceilf((job->wcet - job->executed_time) / scaling_factor);
    }
  }

  bid_entry *entry;
  list_for_each_entry(entry, &core_state->bid_history_queue, link) {
    job = entry->bidded_job;
    if (job->virtual_deadline <= t) {
      demand += ceilf((job->wcet - job->executed_time) / scaling_factor);
    }
  }

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

const Task *find_next_arrival_task(uint16_t core_id) {
  CoreState *core_state = &core_states[core_id];
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

bool is_admissible(uint16_t core_id, Job *candidate_job) {
  CoreState *core_state = &core_states[core_id];
  float max_scaling_factor = 1.0f;
  float time_needed_for_candidate =
      ceilf((candidate_job->wcet - candidate_job->executed_time) /
            max_scaling_factor * (1 + DEMAND_PADDING_PERCENT));
  if (find_slack(core_id, candidate_job->virtual_deadline, max_scaling_factor) <
      time_needed_for_candidate) {
    return false;
  }

  Job *cur;
  list_for_each_entry(cur, &core_state->replica_queue, link) {
    uint32_t deadline = cur->virtual_deadline;

    float slack = find_slack(core_id, deadline, max_scaling_factor);
    if (slack < time_needed_for_candidate) {
      return false;
    }
  }
  list_for_each_entry(cur, &core_state->ready_queue, link) {
    uint32_t deadline = cur->virtual_deadline;

    float slack = find_slack(core_id, deadline, max_scaling_factor);
    if (slack < time_needed_for_candidate) {
      return false;
    }
  }
  list_for_each_entry(cur, &core_state->pending_jobs_queue, link) {
    uint32_t deadline = cur->virtual_deadline;
    float slack = find_slack(core_id, deadline, max_scaling_factor);
    if (slack < time_needed_for_candidate) {
      return false;
    }
  }
  bid_entry *entry;
  list_for_each_entry(entry, &core_state->bid_history_queue, link) {
    cur = entry->bidded_job;
    uint32_t deadline = cur->virtual_deadline;
    float slack = find_slack(core_id, deadline, max_scaling_factor);
    if (slack < time_needed_for_candidate) {
      return false;
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

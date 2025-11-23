#ifndef SCHEDULER_SCHED_UTIL_H
#define SCHEDULER_SCHED_UTIL_H

#include "task_management.h"
#include <stdlib.h>

extern const task_struct *task_lookup[MAX_TASKS + 1];

static inline const task_struct *find_task_by_id(uint32_t task_id) {
  if (task_id > MAX_TASKS)
    return NULL;
  return task_lookup[task_id];
}

float generate_acet(job_struct *job);

uint32_t find_next_effective_arrival_time(uint8_t core_id);

uint32_t calculate_allocated_horizon(uint8_t core_id);

float find_slack(uint8_t core_id, criticality_level crit_lvl, uint32_t tstart,
                 float scaling_factor, const job_struct *extra_job);

bool is_admissible(uint8_t core_id, job_struct *candidate_job,
                   float extra_margin);

float get_util(uint8_t core_id);

#endif

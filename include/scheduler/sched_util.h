#ifndef SCHEDULER_SCHED_UTIL_H
#define SCHEDULER_SCHED_UTIL_H

#include "task_management.h"
#include <stdlib.h>

extern const Task *task_lookup[MAX_TASKS + 1];

static inline float rand_between(float min, float max) {
  return min + (float)rand() / (float)RAND_MAX * (max - min);
}

static inline const Task *find_task_by_id(uint32_t task_id) {
  if (task_id > MAX_TASKS)
    return NULL;
  return task_lookup[task_id];
}

float generate_acet(Job *job);

uint32_t find_next_effective_arrival_time(uint16_t core_id);

float find_slack(uint16_t core_id, CriticalityLevel crit_lvl, uint32_t tstart,
                 uint32_t tend, float scaling_factor);

bool is_admissible(uint16_t core_id, Job *candidate_job);

float get_util(uint16_t core_id);

#endif

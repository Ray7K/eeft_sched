#ifndef SCHED_H
#define SCHED_H

#include "list.h"
#include "platform.h"
#include "task_management.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  struct list_head ready_queue;
  struct list_head replica_queue;
  struct list_head discard_list;

  Job *running_job;

  float busy_time;

  float work_done;

  bool is_idle;

  uint8_t current_dvfs_level;

  uint8_t proc_id;

  uint8_t core_id;

  uint8_t local_criticality_level;
} CoreState;

void scheduler_init();

void scheduler_tick(uint16_t global_core_id);

float find_slack(uint16_t global_core_id, uint32_t time, float scaling_factor);

extern CoreState core_states[TOTAL_CORES];

#endif

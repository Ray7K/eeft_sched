#ifndef SCHED_H
#define SCHED_H

#include "sys_config.h"
#include "task_management.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint8_t proc_id;
  uint8_t core_id;

  Job *ready_queue;
  Job *replica_queue;

  Job *running_job;

  CriticalityLevel cur_crit_level;

  bool is_idle;

} CoreState;

void scheduler_init();

void scheduler_tick(uint16_t global_core_id);

void handle_job_completion(uint16_t global_core_id);

void handle_mode_change(uint16_t global_core_id, CriticalityLevel new_level);

#endif

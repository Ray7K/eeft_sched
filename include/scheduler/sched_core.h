#ifndef SCHEDULER_SCHED_CORE_H
#define SCHEDULER_SCHED_CORE_H

#include "lib/list.h"
#include "power_management.h"
#include "scheduler/sched_migration.h"
#include "task_management.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  struct list_head ready_queue;
  struct list_head replica_queue;
  struct list_head discard_list;
  struct list_head pending_jobs_queue;
  struct list_head bid_history_queue;

  Job *award_buf[MAX_CONCURRENT_OFFERS];
  _Atomic uint64_t seq[MAX_CONCURRENT_OFFERS];
  ring_buffer award_notification_queue;

  Job *running_job;

  DPMControlBlock dpm_control_block;

  bool is_idle;

  uint8_t current_dvfs_level;

  uint8_t proc_id;
  uint8_t core_id;

  uint8_t local_criticality_level;

  bool decision_point;

} CoreState;

void scheduler_init(void);

void scheduler_tick(uint16_t core_id);

extern CoreState core_states[NUM_CORES_PER_PROC];

#endif

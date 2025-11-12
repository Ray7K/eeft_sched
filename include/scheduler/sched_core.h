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
  struct list_head delegated_job_queue;

  job_struct *award_buf[MAX_CONCURRENT_OFFERS];
  _Atomic uint64_t seq[MAX_CONCURRENT_OFFERS];
  ring_buffer award_notification_queue;

  job_struct *running_job;

  uint32_t next_migration_allowed_tick;

  uint32_t next_dpm_eligible_tick;

  uint32_t cached_slack_horizon;

  dpm_control_block dpm_control_block;

  bool is_idle;

  uint8_t current_dvfs_level;

  uint8_t proc_id;
  uint8_t core_id;

  uint8_t local_criticality_level;

  bool decision_point;

} CoreState;

void scheduler_init(void);

void scheduler_tick(uint8_t core_id);

extern CoreState core_states[NUM_CORES_PER_PROC];

#endif

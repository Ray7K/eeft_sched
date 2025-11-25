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

  struct list_head delegated_job_queue;

  migration_request migration_buf[MAX_MIGRATION_REQUESTS];
  _Atomic uint64_t migration_seq[MAX_MIGRATION_REQUESTS];
  ring_buffer migration_request_queue;

  delegation_ack delegation_ack_buf[MAX_FUTURE_DELEGATIONS];
  _Atomic uint64_t delegation_ack_seq[MAX_FUTURE_DELEGATIONS];
  ring_buffer delegation_ack_queue;

  pthread_mutex_t rq_lock;

  job_struct *running_job;

  uint32_t next_migration_eligible_tick;

  uint32_t cached_slack_horizon;

  dpm_control_block dpm_control_block;

  bool is_idle;

  uint8_t current_dvfs_level;

  uint8_t proc_id;
  uint8_t core_id;

  uint8_t local_criticality_level;

  bool decision_point;

} core_state;

typedef struct {
  float util;
  float slack;
  uint32_t next_arrival;
  bool is_idle;
  uint8_t dvfs_level;
} core_summary;

void scheduler_init(void);

void scheduler_tick(uint8_t core_id);

extern core_state core_states[NUM_CORES_PER_PROC];

extern core_summary core_summaries[NUM_CORES_PER_PROC];

extern pthread_mutex_t core_summary_locks[NUM_CORES_PER_PROC];

#define LOCK_RQ(core_id) pthread_mutex_lock(&core_states[core_id].rq_lock)

#define UNLOCK_RQ(core_id) pthread_mutex_unlock(&core_states[core_id].rq_lock)

#endif

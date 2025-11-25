#ifndef SCHEDULER_SCHED_MIGRATION
#define SCHEDULER_SCHED_MIGRATION

#include "power_management.h"
#include "task_management.h"

#define MAX_FUTURE_DELEGATIONS 200
#define MAX_MIGRATION_REQUESTS 32

#define DPM_MIGRATION_LOOKAHEAD_TICKS 100
#define CORE_MIGRATION_COOLDOWN_TICKS 15
#define JOB_MIGRATION_COOLDOWN_TICKS 50

#define MIGRATION_PENALTY_TICKS 0.05f

#define UTIL_UPPER_CAP 0.85f
#define LIGHT_DONOR_UTIL_THRESHOLD 0.3f

#define MIN_MIGRATION_BENEFIT_THRESHOLD                                        \
  (MIGRATION_PENALTY_TICKS + DPM_ENTRY_PHYSICAL_COST_TICKS +                   \
   DPM_EXIT_PHYSICAL_COST_TICKS)

typedef struct {
  job_struct *job;
  uint8_t from_core;
} migration_request;

typedef struct {
  struct list_head link;
  uint32_t arrival_tick;
  uint32_t task_id;
  bool owned_by_remote;
} delegated_job;

typedef struct {
  uint32_t task_id;
  uint32_t arrival_tick;
  bool accepted;
} delegation_ack;

void init_migration(void);
void release_delegation(delegated_job *dj, uint8_t core_id);
void update_delegations(uint8_t core_id);

void attempt_migration_push(uint8_t core_id);
void process_migration_requests(uint8_t core_id);

#endif

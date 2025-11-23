#ifndef SCHEDULER_SCHED_MIGRATION
#define SCHEDULER_SCHED_MIGRATION

#include "task_management.h"

#define MAX_CONCURRENT_OFFERS 128
#define MAX_FUTURE_DELEGATIONS 200

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
  job_struct *bidded_job;
  struct list_head link;
  uint32_t bid_time;
  uint32_t expiry_time;
} bid_entry;

typedef struct {
  job_struct *j_orig;
  job_struct *j_copy;

  uint8_t donor_core_id;
  uint32_t expiry_time;

  float best_bid_metric;
  uint8_t best_bidder_id;

  struct list_head link;
} offer;

typedef struct {
  struct list_head link;
  uint32_t arrival_tick;
  uint32_t task_id;
  bool owned_by_remote;
} delegated_job;

void init_migration(void);
void remove_expired_bid_entries(uint8_t core_id);
void release_delegation(delegated_job *dj, uint8_t core_id);

void attempt_migration_push(uint8_t core_id);
void participate_in_auctions(uint8_t core_id);
void handle_offer_cleanup(uint8_t core_id);
void process_award_notifications(uint8_t core_id);

#endif

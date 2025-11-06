#ifndef SCHEDULER_SCHED_MIGRATION
#define SCHEDULER_SCHED_MIGRATION

#include "task_management.h"

#define MAX_CONCURRENT_OFFERS 128

typedef struct {
  Job *bidded_job;
  struct list_head link;
  uint32_t bid_time;
  uint32_t expiry_time;
} bid_entry;

typedef struct {
  Job *j_orig;
  Job *j_copy;

  uint16_t donor_core_id;
  uint32_t expiry_time;

  float best_bid_metric;
  uint16_t best_bidder_id;

  struct list_head link;
} Offer;

void init_migration(void);
bid_entry *create_bid_entry(uint16_t core_id);
void release_bid_entry(bid_entry *entry, uint16_t core_id);
void remove_expired_bid_entries(uint16_t core_id);
void add_bid_entry_sorted(bid_entry *new_entry, uint16_t core_id);
Offer *create_offer(void);
void release_offer(Offer *offer);

void handle_light_donor_push(uint16_t core_id);
void handle_idle_donor_push(uint16_t core_id);
void attempt_migration_push(uint16_t core_id);
void participate_in_auctions(uint16_t core_id);
void handle_offer_cleanup(uint16_t core_id);
void process_award_notifications(uint16_t core_id);

#endif

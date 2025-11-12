#ifndef TASK_MANAGEMENT_H
#define TASK_MANAGEMENT_H

#include "sys_config.h"

#include "lib/list.h"
#include "lib/log.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t id;

  uint32_t period;
  uint32_t deadline;
  uint32_t wcet[MAX_CRITICALITY_LEVELS];

  criticality_level crit_level;
  uint8_t num_replicas;
} task_struct;

typedef enum {
  JOB_STATE_IDLE = 0,
  JOB_STATE_READY,
  JOB_STATE_RUNNING,
  JOB_STATE_COMPLETED,
  JOB_STATE_REMOVED,
} job_state;

typedef struct job {
  void *next_free;
  const task_struct *parent_task;

  uint32_t arrival_time;
  uint32_t relative_tuned_deadlines[MAX_CRITICALITY_LEVELS];
  uint32_t actual_deadline;
  uint32_t virtual_deadline;
  float wcet;
  float acet;
  float executed_time;

  uint8_t job_pool_id;
  bool is_replica;
  job_state state;

  struct list_head link;

  uint32_t migration_cooldown;

  _Atomic int refcount;

  _Atomic bool is_being_offered;
} job_struct;

void __release_job_to_pool(job_struct *job, uint8_t core_id);

static inline job_struct *get_job_ref(job_struct *job) {
  if (job == NULL) {
    return NULL;
  }
  atomic_fetch_add_explicit(&job->refcount, 1, memory_order_acq_rel);
  return job;
}

static inline void put_job_ref(job_struct *job, uint8_t core_id) {
  if (job == NULL) {
    return;
  }
  int prev = atomic_fetch_sub_explicit(&job->refcount, 1, memory_order_acq_rel);
  if (prev == 1) {
    __release_job_to_pool(job, core_id);
  }
}

void task_management_init(void);

job_struct *create_job(const task_struct *parent_task, uint8_t core_id);
job_struct *clone_job(const job_struct *job, uint8_t core_id);
void add_to_queue_sorted(struct list_head *queue_head, job_struct *job_to_add);
void add_to_queue_sorted_by_arrival(struct list_head *queue_head,
                                    job_struct *job_to_add);
job_struct *peek_next_job(struct list_head *queue_head);
job_struct *pop_next_job(struct list_head *queue_head);
void remove_job_with_parent_task_id(struct list_head *queue_head,
                                    uint32_t task_id, uint8_t core_id);
void log_job_queue(log_level level, const char *name,
                   struct list_head *queue_head);

#endif

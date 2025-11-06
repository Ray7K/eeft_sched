#ifndef TASK_MANAGEMENT_H
#define TASK_MANAGEMENT_H

#include "lib/list.h"
#include "lib/log.h"
#include "sys_config.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t id;

  uint32_t period;
  uint32_t deadline;
  uint32_t wcet[MAX_CRITICALITY_LEVELS];

  CriticalityLevel criticality_level;
  uint8_t num_replicas;
} Task;

typedef enum {
  JOB_STATE_IDLE = 0,
  JOB_STATE_READY,
  JOB_STATE_RUNNING,
  JOB_STATE_COMPLETED,
  JOB_STATE_REMOVED,
} JobState;

typedef struct Job {
  void *next_free;
  const Task *parent_task;

  uint32_t arrival_time;
  uint32_t relative_tuned_deadlines[MAX_CRITICALITY_LEVELS];
  uint32_t actual_deadline;
  uint32_t virtual_deadline;
  float wcet;
  float acet;
  float executed_time;

  uint16_t job_pool_id;
  bool is_replica;
  JobState state;

  struct list_head link;

  uint32_t migration_cooldown;

  _Atomic int refcount;

  _Atomic bool is_being_offered;
} Job;

void __release_job_to_pool(Job *job, uint16_t global_core_id);

static inline Job *get_job_ref(Job *job) {
  if (job == NULL) {
    return NULL;
  }
  atomic_fetch_add_explicit(&job->refcount, 1, memory_order_acq_rel);
  return job;
}

static inline void put_job_ref(Job *job, uint16_t global_core_id) {
  if (job == NULL) {
    return;
  }
  int prev = atomic_fetch_sub_explicit(&job->refcount, 1, memory_order_acq_rel);
  if (prev == 1) {
    __release_job_to_pool(job, global_core_id);
  } else if (prev < 1) {
    printf("Error: Job %d refcount dropped below zero!\n",
           job->parent_task->id);
  }
}

void task_management_init(void);

Job *create_job(const Task *parent_task, uint16_t global_core_id);
Job *clone_job(const Job *job, uint16_t global_core_id);
void add_to_queue_sorted(struct list_head *queue_head, Job *job_to_add);
Job *peek_next_job(struct list_head *queue_head);
Job *pop_next_job(struct list_head *queue_head);
void remove_job_with_parent_task_id(struct list_head *queue_head,
                                    uint32_t task_id, uint16_t global_core_id);
void log_job_queue(LogLevel level, const char *name,
                   struct list_head *queue_head);

#endif

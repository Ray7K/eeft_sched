#ifndef TASK_MANAGEMENT_H
#define TASK_MANAGEMENT_H

#include "list.h"
#include "sys_config.h"
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
  JOB_STATE_IDLE,
  JOB_STATE_READY,
  JOB_STATE_RUNNING,
  JOB_STATE_COMPLETED
} JobState;

typedef struct Job {
  const Task *parent_task;

  uint32_t arrival_time;
  uint32_t relative_tuned_deadlines[MAX_CRITICALITY_LEVELS];
  uint32_t actual_deadline;
  uint32_t virtual_deadline;
  float wcet;
  float acet;
  float executed_time;

  bool is_replica;
  JobState state;

  struct list_head link;
} Job;

void task_management_init();

Task *create_task(uint32_t id, uint32_t period[], uint32_t deadline[],
                  uint32_t wcet[], uint8_t criticality_level,
                  uint8_t num_replicas);

Job *create_job(const Task *parent_task, uint16_t global_core_id);
void release_job(Job *job, uint16_t global_core_id);
void add_to_queue_sorted(struct list_head *queue_head, Job *job_to_add);
Job *peek_next_job(struct list_head *queue_head);
Job *pop_next_job(struct list_head *queue_head);
void remove_job_with_parent_task_id(struct list_head *queue_head,
                                    uint32_t task_id, uint16_t global_core_id);
void print_queue(struct list_head *queue_head);

#endif

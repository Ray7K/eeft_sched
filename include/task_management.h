#ifndef TASK_MANAGEMENT_H
#define TASK_MANAGEMENT_H

#include "sys_config.h"
#include <stdint.h>

typedef struct {
  uint32_t id;

  uint32_t period[MAX_CRITICALITY_LEVELS];
  uint32_t deadline[MAX_CRITICALITY_LEVELS];
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

typedef struct {
  const Task *parent_task;
  uint32_t arrival_time;
  uint32_t absolute_deadline;
  uint32_t virtual_deadline;
  uint32_t remaining_wcet;

  uint8_t is_replica;
  JobState state;

  struct Job *next;
} Job;

void task_management_init();

Task *create_task(uint32_t id, uint32_t period[], uint32_t deadline[],
                  uint32_t wcet[], uint8_t criticality_level,
                  uint8_t num_replicas);

Job *create_job(const Task *parent_task);
void release_job(Job *job);
void add_to_queue_sorted(Job **queue_head, Job *job);
Job *peek_next_job(Job *queue_head);
Job *pop_next_job(Job *queue_head);

#endif

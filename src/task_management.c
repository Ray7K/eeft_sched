#include "task_management.h"
#include <stddef.h>

#define JOB_POOL_SIZE 50

static Job job_pool[JOB_POOL_SIZE];

static Job *job_free_list_head = NULL;

void task_management_init(void) {
  job_free_list_head = NULL;

  for (int i = 0; i < JOB_POOL_SIZE; i++) {
    job_pool[i].next = job_free_list_head;
    job_free_list_head = &job_pool[i];
  }
}

Job *create_job(const Task *parent_task) {
  if (job_free_list_head == NULL) {
    return NULL;
  }

  Job *new_job = job_free_list_head;
  job_free_list_head = job_free_list_head->next;

  new_job->parent_task = parent_task;
  new_job->next = NULL;
  new_job->state = JOB_STATE_IDLE;

  return new_job;
}

void release_job(Job *job) {
  if (job == NULL) {
    return;
  }
  job->next = job_free_list_head;
  job_free_list_head = job;
}

void add_to_queue_sorted(Job **queue_head, Job *job_to_add) {
  if (*queue_head == NULL ||
      job_to_add->virtual_deadline < (*queue_head)->virtual_deadline) {
    job_to_add->next = *queue_head;
    *queue_head = job_to_add;
  } else {
    Job *current = *queue_head;
    while (current->next != NULL &&
           current->next->virtual_deadline <= job_to_add->virtual_deadline) {
      current = current->next;
    }
    job_to_add->next = current->next;
    current->next = job_to_add;
  }
}

Job *peek_next_job(Job *queue_head) { return queue_head; }

Job *pop_next_job(Job **queue_head) {
  if (*queue_head == NULL) {
    return NULL;
  }
  Job *job = *queue_head;
  *queue_head = (*queue_head)->next;
  job->next = NULL;
  return job;
}

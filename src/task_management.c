#include "task_management.h"
#include "list.h"
#include "sys_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define JOBS_PER_CORE 30

static Job per_core_job_pool[NUM_CORES_PER_PROC][JOBS_PER_CORE];

static struct list_head per_core_free_list[NUM_CORES_PER_PROC];

void task_management_init(void) {
  for (int i = 0; i < NUM_CORES_PER_PROC; i++) {
    INIT_LIST_HEAD(&per_core_free_list[i]);
    for (int j = 0; j < JOBS_PER_CORE; j++) {
      list_add(&per_core_job_pool[i][j].link, &per_core_free_list[i]);
    }
  }
}

Job *create_job(const Task *parent_task, uint16_t global_core_id) {
  Job *new_job =
      list_first_entry(&per_core_free_list[global_core_id], Job, link);

  if (new_job != NULL) {
    list_del(&new_job->link);
    new_job->parent_task = parent_task;
    new_job->state = JOB_STATE_IDLE;
    INIT_LIST_HEAD(&new_job->link);
  }

  return new_job;
}

void release_job(Job *job, uint16_t global_core_id) {
  if (job == NULL) {
    return;
  }
  list_add(&job->link, &per_core_free_list[global_core_id]);
}

void add_to_queue_sorted(struct list_head *queue_head, Job *job_to_add) {
  if (queue_head == NULL || job_to_add == NULL) {
    fprintf(stderr, "ERROR: Attempted to add job to a NULL queue.\n");
    return;
  }
  Job *cursor, *n;

  list_for_each_entry_safe(cursor, n, queue_head, link) {
    if (job_to_add->virtual_deadline < cursor->virtual_deadline) {
      list_add(&job_to_add->link, cursor->link.prev);
      return;
    }
  }

  list_add(&job_to_add->link, queue_head->prev);
}

Job *peek_next_job(struct list_head *queue_head) {
  return list_first_entry(queue_head, Job, link);
}

Job *pop_next_job(struct list_head *queue_head) {
  Job *job = list_first_entry(queue_head, Job, link);
  if (job) {
    list_del(&job->link);
  }
  return job;
}

void remove_job_with_parent_task_id(struct list_head *queue_head,
                                    uint32_t task_id, uint16_t global_core_id) {
  Job *cursor, *next;

  list_for_each_entry_safe(cursor, next, queue_head, link) {
    if (cursor->parent_task->id == task_id) {
      list_del(&cursor->link);
      release_job(cursor, global_core_id);
    }
  }
}

void print_queue(struct list_head *queue_head) {
  Job *cursor, *n;

  if (queue_head->next == queue_head) {
    printf("(Queue is empty)\n");
    return;
  }

  list_for_each_entry_safe(cursor, n, queue_head, link) {
    printf("Job(ID: %d, Deadline: %d)", cursor->parent_task->id,
           cursor->virtual_deadline);

    if (cursor->link.next != queue_head) {
      printf(" -> ");
    }
  }
  printf("\n");
}

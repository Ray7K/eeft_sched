#include "task_management.h"
#include "list.h"
#include "log.h"
#include "sys_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define JOBS_PER_CORE 100

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
  Job *new_job = list_first_entry(
      &per_core_free_list[global_core_id % NUM_CORES_PER_PROC], Job, link);

  if (new_job != NULL) {
    list_del(&new_job->link);
    new_job->parent_task = parent_task;
    new_job->state = JOB_STATE_IDLE;
    new_job->owner_core_id = global_core_id;
    INIT_LIST_HEAD(&new_job->link);
  }

  return new_job;
}

void release_job(Job *job) {
  if (job == NULL) {
    return;
  }
  list_add(&job->link,
           &per_core_free_list[job->owner_core_id % NUM_CORES_PER_PROC]);
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
                                    uint32_t task_id) {
  Job *cursor, *next;

  list_for_each_entry_safe(cursor, next, queue_head, link) {
    if (cursor->parent_task->id == task_id) {
      list_del(&cursor->link);
      release_job(cursor);
    }
  }
}

static void job_to_str(const Job *job, char *buffer, size_t size) {
  snprintf(buffer, size, "Job(ID:%u VDL:%u REM:%.2f)", job->parent_task->id,
           job->virtual_deadline, job->acet - job->executed_time);
}

void log_job_queue(LogLevel level, const char *name,
                   struct list_head *queue_head) {
  if (level < current_log_level) {
    return;
  }

  char queue_str[256];
  snprintf(queue_str, sizeof(queue_str), "Queue '%s': ", name);

  if (queue_head->next == queue_head) {
    snprintf(queue_str + strlen(queue_str),
             sizeof(queue_str) - strlen(queue_str), "%s", "(Empty)");
  }

  Job *job;
  list_for_each_entry(job, queue_head, link) {
    char job_info[64];
    job_to_str(job, job_info, sizeof(job_info));
    snprintf(queue_str + strlen(queue_str),
             sizeof(queue_str) - strlen(queue_str), "%s", job_info);
    if (job->link.next != queue_head) {
      strncat(queue_str, " -> ", sizeof(queue_str) - strlen(queue_str) - 1);
    }
  }

  LOG(level, "%s", queue_str);
}

#include "task_management.h"
#include "list.h"
#include "log.h"
#include "sys_config.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define JOBS_PER_CORE 100

typedef struct {
  Job job_pool[JOBS_PER_CORE];
  void *free_list;
  void *remote_free_list;
  pthread_mutex_t remote_lock;
} core_job_pool;

static core_job_pool core_pools[NUM_CORES_PER_PROC];

void task_management_init(void) {
  for (int i = 0; i < NUM_CORES_PER_PROC; i++) {
    core_pools[i].free_list = NULL;
    core_pools[i].remote_free_list = NULL;
    pthread_mutex_init(&core_pools[i].remote_lock, NULL);

    for (int j = 0; j < JOBS_PER_CORE; j++) {
      core_pools[i].job_pool[j].next_free = core_pools[i].free_list;
      core_pools[i].free_list = (void *)&core_pools[i].job_pool[j];
    }
  }
}

Job *create_job(const Task *parent_task, uint16_t global_core_id) {
  uint16_t cid = global_core_id % NUM_CORES_PER_PROC;

  if (core_pools[cid].free_list == NULL) {
    pthread_mutex_lock(&core_pools[cid].remote_lock);

    core_pools[cid].free_list = core_pools[cid].remote_free_list;
    core_pools[cid].remote_free_list = NULL;

    pthread_mutex_unlock(&core_pools[cid].remote_lock);
  }

  Job *new_job = (Job *)core_pools[cid].free_list;

  if (new_job != NULL) {
    core_pools[cid].free_list = new_job->next_free;
    new_job->parent_task = parent_task;
    new_job->state = JOB_STATE_IDLE;
    new_job->owner_core_id = global_core_id;
    INIT_LIST_HEAD(&new_job->link);
  }

  return new_job;
}

void release_job(Job *job, uint16_t global_core_id) {
  if (job == NULL) {
    return;
  }
  uint16_t owner = job->owner_core_id % NUM_CORES_PER_PROC;
  uint16_t me = global_core_id % NUM_CORES_PER_PROC;

  if (owner == me) {
    job->next_free = core_pools[owner].free_list;
    core_pools[owner].free_list = (void *)job;
  } else {
    pthread_mutex_lock(&core_pools[owner].remote_lock);
    job->next_free = core_pools[owner].remote_free_list;
    core_pools[owner].remote_free_list = (void *)job;
    pthread_mutex_unlock(&core_pools[owner].remote_lock);
  }
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

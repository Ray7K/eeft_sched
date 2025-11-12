#include "task_management.h"
#include "sys_config.h"

#include "lib/list.h"
#include "lib/log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define JOBS_PER_CORE 200

typedef struct {
  job_struct job_pool[JOBS_PER_CORE];
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

job_struct *create_job(const task_struct *parent_task, uint8_t core_id) {
  if (core_pools[core_id].free_list == NULL) {
    pthread_mutex_lock(&core_pools[core_id].remote_lock);

    core_pools[core_id].free_list = core_pools[core_id].remote_free_list;
    core_pools[core_id].remote_free_list = NULL;

    pthread_mutex_unlock(&core_pools[core_id].remote_lock);
  }

  job_struct *new_job = (job_struct *)core_pools[core_id].free_list;

  if (new_job != NULL) {
    core_pools[core_id].free_list = new_job->next_free;
    new_job->parent_task = parent_task;
    new_job->state = JOB_STATE_IDLE;
    new_job->job_pool_id = core_id;
    INIT_LIST_HEAD(&new_job->link);

    atomic_store_explicit(&new_job->refcount, 1, memory_order_release);
    atomic_store_explicit(&new_job->is_being_offered, false,
                          memory_order_release);
  }

  return new_job;
}

job_struct *clone_job(const job_struct *job, uint8_t core_id) {
  if (job == NULL) {
    return NULL;
  }

  job_struct *new_job = create_job(job->parent_task, core_id);
  if (new_job == NULL) {
    return NULL;
  }
  new_job->state = job->state;
  new_job->arrival_time = job->arrival_time;
  new_job->executed_time = job->executed_time;
  new_job->acet = job->acet;
  new_job->wcet = job->wcet;
  new_job->actual_deadline = job->actual_deadline;
  new_job->virtual_deadline = job->virtual_deadline;
  new_job->is_replica = job->is_replica;
  memcpy(new_job->relative_tuned_deadlines, job->relative_tuned_deadlines,
         sizeof(uint32_t) * MAX_CRITICALITY_LEVELS);
  new_job->is_being_offered =
      atomic_load_explicit(&job->is_being_offered, memory_order_acquire);

  return new_job;
}

void __release_job_to_pool(job_struct *job, uint8_t core_id) {
  if (job == NULL) {
    return;
  }
  uint8_t owner = job->job_pool_id;
  uint8_t me = core_id;

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

void add_to_queue_sorted(struct list_head *queue_head, job_struct *job_to_add) {
  if (queue_head == NULL || job_to_add == NULL) {
    LOG(LOG_LEVEL_ERROR, "Attempted to add job to a NULL queue\n");
    return;
  }
  job_struct *cursor, *n;

  list_for_each_entry_safe(cursor, n, queue_head, link) {
    if (job_to_add->virtual_deadline < cursor->virtual_deadline) {
      list_add(&job_to_add->link, cursor->link.prev);
      return;
    }
  }

  list_add(&job_to_add->link, queue_head->prev);
}

void add_to_queue_sorted_by_arrival(struct list_head *queue_head,
                                    job_struct *job_to_add) {
  if (!queue_head || !job_to_add) {
    LOG(LOG_LEVEL_ERROR, "Attempted to add job to a NULL queue\n");
    return;
  }

  job_struct *cursor, *n;
  list_for_each_entry_safe(cursor, n, queue_head, link) {
    if (job_to_add->arrival_time < cursor->arrival_time) {
      list_add(&job_to_add->link, cursor->link.prev);
      return;
    }
  }

  list_add(&job_to_add->link, queue_head->prev);
}

job_struct *peek_next_job(struct list_head *queue_head) {
  return list_first_entry(queue_head, job_struct, link);
}

job_struct *pop_next_job(struct list_head *queue_head) {
  job_struct *job = list_first_entry(queue_head, job_struct, link);
  if (job) {
    list_del(&job->link);
  }
  return job;
}

void remove_job_with_parent_task_id(struct list_head *queue_head,
                                    uint32_t task_id, uint8_t core_id) {
  job_struct *cursor, *next;

  list_for_each_entry_safe(cursor, next, queue_head, link) {
    if (cursor->parent_task->id == task_id) {
      list_del(&cursor->link);
      put_job_ref(cursor, core_id);
    }
  }
}

static void job_to_str(job_struct *job, char *buffer, size_t size) {
  snprintf(buffer, size, "Job(ID:%u VDL:%u REM:%.2f)", job->parent_task->id,
           job->virtual_deadline, job->acet - job->executed_time);
}

void log_job_queue(log_level level, const char *name,
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

  job_struct *job;
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

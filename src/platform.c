#include "platform.h"
#include "list.h"
#include "log.h"
#include "pthread.h"
#include "sched.h"
#include "sys_config.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

ProcessorState processor_state;

#define SYSTEM_TICK_MS 1

__thread LogThreadContext log_thread_context = {0, 0, false};

static void *timer_thread_func() {
  while (1) {
    barrier_wait(&processor_state.core_completion_barrier);

    Job *cur, *next;
    pthread_mutex_lock(&processor_state.discard_queue_lock);
    list_for_each_entry_safe(cur, next, &processor_state.discard_queue, link) {
      if (cur->actual_deadline <= processor_state.system_time) {
        LOG(LOG_LEVEL_INFO, "Releasing job with parent task ID %d",
            cur->parent_task->id);
        list_del(&cur->link);
        release_job(cur);
      }
    }
    pthread_mutex_unlock(&processor_state.discard_queue_lock);

    __sync_fetch_and_add(&processor_state.system_time, 1);

    barrier_wait(&processor_state.time_sync_barrier);
  }
  return NULL;
}

static void *core_thread_func(void *arg) {
  uint8_t local_core_id = *((uint8_t *)arg);

  log_thread_context.proc_id = processor_state.processor_id;
  log_thread_context.core_id = local_core_id;
  log_thread_context.is_set = true;

  while (1) {
    scheduler_tick(local_core_id);
    LOG(LOG_LEVEL_DEBUG, "Core %u finished tick %u", local_core_id,
        processor_state.system_time);

    barrier_wait(&processor_state.core_completion_barrier);
    barrier_wait(&processor_state.time_sync_barrier);
  }
  return NULL;
}

void platform_init(uint8_t proc_id) {
  LOG(LOG_LEVEL_INFO, "Initializing System for Processor %d...", proc_id);

  processor_state.system_time = 0;
  processor_state.processor_id = proc_id;
  INIT_LIST_HEAD(&processor_state.discard_queue);
  pthread_mutex_init(&processor_state.discard_queue_lock, NULL);
  atomic_store(&processor_state.system_criticality_level, QM);

  // Initialize barrier to wait for all cores + the timer thread.
  barrier_init(&processor_state.core_completion_barrier,
               NUM_CORES_PER_PROC + 1);
  barrier_init(&processor_state.time_sync_barrier, NUM_CORES_PER_PROC + 1);

  scheduler_init();

  LOG(LOG_LEVEL_INFO, "Processor %d Initialization Complete.", proc_id);
}

void platform_run(void) {
  pthread_t timer_thread;
  pthread_t core_threads[NUM_CORES_PER_PROC];
  uint8_t local_core_ids[NUM_CORES_PER_PROC];

  LOG(LOG_LEVEL_INFO, "Launching threads...");

  if (pthread_create(&timer_thread, NULL, timer_thread_func, NULL)) {
    perror("pthread_create timer");
    return;
  }

  for (uint8_t i = 0; i < NUM_CORES_PER_PROC; ++i) {
    local_core_ids[i] = i;
    if (pthread_create(&core_threads[i], NULL, core_thread_func,
                       &local_core_ids[i])) {
      perror("pthread_create core");
      return;
    }
  }

  LOG(LOG_LEVEL_INFO, "All threads running.");

  for (int i = 0; i < NUM_CORES_PER_PROC; ++i) {
    pthread_join(core_threads[i], NULL);
  }
  pthread_join(timer_thread, NULL);
}

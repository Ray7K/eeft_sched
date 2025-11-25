#include "processor.h"
#include "ipc.h"
#include "sys_config.h"

#include "lib/list.h"
#include "lib/log.h"

#include "scheduler/sched_core.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

processor_state proc_state;

barrier *proc_barrier __attribute__((weak)) = NULL;

static volatile sig_atomic_t shutdown_requested = 0;

#define SYSTEM_TICK_MS 1

#ifndef TOTAL_TICKS
#define TOTAL_TICKS 0
#endif

__thread log_thread_context log_thread_ctx = {0, 0, false};

static void *timer_thread_func(void *arg) {
  (void)arg;

  while (!shutdown_requested) {
    barrier_wait(&proc_state.core_completion_barrier);
    ring_buffer_clear(&proc_state.incoming_completion_msg_queue);

    ipc_receive_completion_messages();

    job_struct *cur, *next;
    pthread_mutex_lock(&proc_state.discard_queue_lock);
    list_for_each_entry_safe(cur, next, &proc_state.discard_queue, link) {
      if (cur->actual_deadline <= proc_state.system_time) {
        LOG(LOG_LEVEL_INFO, "Releasing job with parent task ID %d",
            cur->parent_task->id);
        list_del(&cur->link);
        put_job_ref(cur, NUM_CORES_PER_PROC);
      }
    }
    pthread_mutex_unlock(&proc_state.discard_queue_lock);

    atomic_fetch_add_explicit(&proc_state.system_time, 1, memory_order_relaxed);

    if (TOTAL_TICKS > 0 && proc_state.system_time >= TOTAL_TICKS) {
      shutdown_requested = 1;
    }

    ipc_send_completion_messages();

    barrier_wait(&proc_state.time_sync_barrier);

    if (proc_barrier) {
      barrier_wait(proc_barrier);
    }
  }
  return NULL;
}

static void *core_thread_func(void *arg) {
  uint8_t core_id = *((uint8_t *)arg);
  log_thread_ctx.proc_id = proc_state.processor_id;
  log_thread_ctx.core_id = core_id;
  log_thread_ctx.is_set = true;

  while (!shutdown_requested) {
    scheduler_tick(core_id);
    barrier_wait(&proc_state.core_completion_barrier);
    barrier_wait(&proc_state.time_sync_barrier);
  }
  return NULL;
}

void processor_cleanup(void) {
  LOG(LOG_LEVEL_INFO, "Cleaning up processor...");
  log_system_shutdown();
  pthread_mutex_destroy(&proc_state.discard_queue_lock);
  barrier_destroy(&proc_state.core_completion_barrier);
  barrier_destroy(&proc_state.time_sync_barrier);
  ipc_cleanup();
}

static void processor_sigint_handler(int sig) {
  (void)sig;
  shutdown_requested = 1;
}

void processor_init(uint8_t proc_id) {
  signal(SIGTERM, processor_sigint_handler);
  signal(SIGINT, processor_sigint_handler);

  log_system_init(proc_id);

  LOG(LOG_LEVEL_INFO, "Initializing System for Processor %d...", proc_id);

  proc_state.system_time = 0;
  proc_state.processor_id = proc_id;
  INIT_LIST_HEAD(&proc_state.discard_queue);
  pthread_mutex_init(&proc_state.discard_queue_lock, NULL);
  atomic_store(&proc_state.system_criticality_level, 0);

  // Initialize barrier to wait for all cores + the timer thread.
  barrier_init(&proc_state.core_completion_barrier, NUM_CORES_PER_PROC + 1, 0);
  barrier_init(&proc_state.time_sync_barrier, NUM_CORES_PER_PROC + 1, 0);

  ipc_thread_init();
  scheduler_init();

  LOG(LOG_LEVEL_INFO, "Processor %d Initialization Complete.", proc_id);
}

void processor_run(void) {
  pthread_t timer_thread;
  pthread_t core_threads[NUM_CORES_PER_PROC];
  uint8_t core_ids[NUM_CORES_PER_PROC];

  LOG(LOG_LEVEL_INFO, "Launching threads...");

  if (pthread_create(&timer_thread, NULL, timer_thread_func, NULL)) {
    perror("pthread_create timer");
    return;
  }

  for (uint8_t i = 0; i < NUM_CORES_PER_PROC; ++i) {
    core_ids[i] = i;
    if (pthread_create(&core_threads[i], NULL, core_thread_func,
                       &core_ids[i])) {
      perror("pthread_create core");
      return;
    }
  }

  LOG(LOG_LEVEL_INFO, "All threads running.");

  for (int i = 0; i < NUM_CORES_PER_PROC; ++i) {
    pthread_join(core_threads[i], NULL);
  }
  pthread_join(timer_thread, NULL);
  processor_cleanup();
}

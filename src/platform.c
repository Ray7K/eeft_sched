#include "platform.h"
#include "list.h"
#include "pthread.h"
#include "sched.h"
#include "sys_config.h"
#include "unistd.h"
#include <stdio.h>

// TODO: make processor state thread safe
ProcessorState processor_state;

#define SYSTEM_TICK_MS 1

static void *timer_thread_func() {
  while (1) {
    // printf("[Proc %u] System Time: %u ms\n", processor_state.processor_id,
    // processor_state.system_time);
    usleep(1000 * SYSTEM_TICK_MS);
    Job *cur, *next;
    list_for_each_entry_safe(cur, next, &processor_state.discard_queue, link) {
      if (cur->actual_deadline >= processor_state.system_time) {
        remove_job_with_parent_task_id(&processor_state.discard_queue,
                                       cur->parent_task->id, 0);
      }
    }
    __sync_fetch_and_add(&processor_state.system_time, 1);
  }
  return NULL;
}

/**
 * @brief The main execution loop for a single core thread.
 */
static void *core_thread_func(void *arg) {
  uint8_t local_core_id = *((uint8_t *)arg);
  uint32_t last_tick = processor_state.system_time;

  while (1) {

    if (processor_state.system_time > last_tick) {
      last_tick = processor_state.system_time;
      scheduler_tick(local_core_id);
      printf("[Proc %u] Core %u Tick at System Time %u ms\n",
             processor_state.processor_id, local_core_id,
             processor_state.system_time);
    }
    // Small sleep to prevent the simulation from using 100% host CPU.
    // This would not exist on bare metal.
    usleep(500);
  }
  return NULL;
}

void platform_init(uint8_t proc_id) {
  printf("Initializing System for Processor %d...\n", proc_id);

  processor_state.system_time = 0;
  processor_state.processor_id = proc_id;
  INIT_LIST_HEAD(&processor_state.discard_queue);
  processor_state.system_criticality_level = QM;

  scheduler_init();

  printf("System Initialized for Processor %d.\n", proc_id);
}

void platform_run() {
  while (1) {
    printf("\n\n\n[Proc %u] System Time: %u ms\n", processor_state.processor_id,
           processor_state.system_time);
    scheduler_tick(0);

    Job *cur, *next;
    list_for_each_entry_safe(cur, next, &processor_state.discard_queue, link) {
      if (cur->actual_deadline <= processor_state.system_time) {
        list_del(&cur->link);
        release_job(cur, 0);
      }
    }
    processor_state.system_time += 1;
    // usleep(500000);
  }
}

// void platform_run(void) {
//   pthread_t timer_thread;
//   pthread_t core_threads[NUM_CORES_PER_PROC];
//   uint8_t local_core_ids[NUM_CORES_PER_PROC];
//
//   printf("[Proc %u] Launching threads...\n", processor_state.processor_id);
//
//   if (pthread_create(&timer_thread, NULL, timer_thread_func, NULL)) {
//     perror("pthread_create timer");
//     return;
//   }
//
//   for (uint8_t i = 0; i < NUM_CORES_PER_PROC; ++i) {
//     local_core_ids[i] = i;
//     if (pthread_create(&core_threads[i], NULL, core_thread_func,
//                        &local_core_ids[i])) {
//       perror("pthread_create core");
//       return;
//     }
//   }
//
//   printf("[Proc %u] All threads running.\n", processor_state.processor_id);
//
//   for (int i = 0; i < NUM_CORES_PER_PROC; ++i) {
//     pthread_join(core_threads[i], NULL);
//   }
//   pthread_join(timer_thread, NULL);
// }

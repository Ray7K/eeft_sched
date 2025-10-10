#ifndef PLATFORM_H
#define PLATFORM_H

#include "barrier.h"
#include "ipc.h"
#include "list.h"
#include "sys_config.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define TOTAL_CORES (NUM_PROC * NUM_CORES_PER_PROC)

typedef struct {
  _Atomic CriticalityLevel system_criticality_level;
  volatile uint32_t system_time;

  struct list_head discard_queue;
  pthread_mutex_t discard_queue_lock;

  MessageQueue completion_signal_queue;
  uint8_t processor_id;
  barrier_t core_completion_barrier;
  barrier_t time_sync_barrier;
} ProcessorState;

extern ProcessorState processor_state;

void platform_init(uint8_t proc_id);

void platform_run();

#endif

#ifndef PLATFORM_H
#define PLATFORM_H

#include "barrier.h"
#include "list.h"
#include "ring_buffer.h"
#include "sys_config.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define TOTAL_CORES (NUM_PROC * NUM_CORES_PER_PROC)
#define MAX_LOG_MSG_SIZE 256

typedef struct {
  _Atomic CriticalityLevel system_criticality_level;
  volatile uint32_t system_time;

  struct list_head discard_queue;
  pthread_mutex_t discard_queue_lock;

  ring_buffer incoming_completion_msg_queue;
  ring_buffer outgoing_completion_msg_queue;

  uint8_t processor_id;
  barrier core_completion_barrier;
  barrier time_sync_barrier;
} ProcessorState;

extern barrier *proc_barrier;

extern ProcessorState processor_state;

void platform_init(uint8_t proc_id);

void platform_run(void);

void platform_cleanup(void);

#endif

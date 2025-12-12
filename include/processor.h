#ifndef PROCESSOR_H
#define PROCESSOR_H

#include "sys_config.h"

#include "lib/barrier.h"
#include "lib/list.h"
#include "lib/ring_buffer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define TOTAL_CORES (NUM_PROC * NUM_CORES_PER_PROC)

#define MAX_LOG_MSG_SIZE 256

extern _Atomic int core_fatal_shutdown_requested;

typedef struct {
  _Atomic criticality_level system_criticality_level;
  _Atomic uint32_t system_time;

  struct list_head discard_queue;
  pthread_mutex_t discard_queue_lock;

  ring_buffer incoming_completion_msg_queue;
  ring_buffer outgoing_completion_msg_queue;

  uint8_t processor_id;
  barrier core_completion_barrier;
  barrier time_sync_barrier;
} processor_state;

extern barrier *proc_barrier;

extern processor_state proc_state;

void processor_init(uint8_t proc_id);

void processor_run(void);

void processor_cleanup(void);

#endif

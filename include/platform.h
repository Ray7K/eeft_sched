#ifndef PLATFORM_H
#define PLATFORM_H

#include "ipc.h"
#include "list.h"
#include "sys_config.h"
#include <stdint.h>

#define TOTAL_CORES (NUM_PROC * NUM_CORES_PER_PROC)

typedef struct {
  CriticalityLevel system_criticality_level;
  volatile uint32_t system_time;
  struct list_head discard_queue;
  MessageQueue completion_signal_queue;
  uint8_t processor_id;
} ProcessorState;

extern ProcessorState processor_state;

void platform_init(uint8_t proc_id);

void platform_run();

#endif

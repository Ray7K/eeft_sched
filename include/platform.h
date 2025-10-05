#ifndef PLATFORM_H
#define PLATFORM_H

#include "ipc.h"
#include "list.h"
#include "sys_config.h"
#include <stdint.h>

typedef struct {
  uint8_t processor_id;
  struct list_head discard_queue;
  CriticalityLevel system_criticality_level;
  MessageQueue completion_signal_queue;
} ProcessorState;

void platform_init(uint8_t proc_id);

void platform_run();

#endif

#ifndef SCHED_H
#define SCHED_H

#include "list.h"
#include "platform.h"
#include "task_management.h"
#include <stdbool.h>
#include <stdint.h>

extern ProcessorState processor_state;
extern volatile uint32_t system_time;

typedef struct {
  uint8_t proc_id;
  uint8_t core_id;

  struct list_head ready_queue;
  struct list_head replica_queue;
  struct list_head discard_list;

  Job *running_job;

  bool is_idle;

  uint32_t busy_time;

} CoreState;

void scheduler_init();

void scheduler_tick(uint16_t global_core_id);

#endif

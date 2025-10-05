#ifndef TASK_ALLOC_H
#define TASK_ALLOC_H

#include "sys_config.h"
#include "task_management.h"
#include <stdint.h>

typedef enum { Primary, Replica } TaskType;

typedef struct {
  uint32_t task_id;
  TaskType task_type;
  uint8_t proc_id;
  uint8_t core_id;
  uint32_t tuned_deadlines[MAX_CRITICALITY_LEVELS];
} TaskAllocationMap;

extern const Task system_tasks[];
extern const uint32_t SYSTEM_TASKS_SIZE;

extern const TaskAllocationMap allocation_map[];
extern const uint32_t ALLOCATION_MAP_SIZE;

#endif

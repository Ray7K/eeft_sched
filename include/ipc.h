#ifndef IPC_H
#define IPC_H

#include "sys_config.h"

#include <stdbool.h>
#include <stdint.h>

#define MESSAGE_QUEUE_SIZE 64

typedef enum {
  PACKET_TYPE_COMPLETION = 0x01,
  PACKET_TYPE_CRITICALITY_CHANGE = 0x02,
} packet_type;

typedef struct {
  uint32_t completed_task_id;
  uint32_t job_arrival_time;
  uint32_t system_time;
} completion_message;

typedef struct {
  criticality_level new_level;
} criticality_change_message;

void ipc_thread_init(void);
void ipc_broadcast_criticality_change(criticality_level new_level);
void ipc_send_completion_messages(void);
void ipc_receive_completion_messages(void);
void ipc_cleanup(void);

#endif

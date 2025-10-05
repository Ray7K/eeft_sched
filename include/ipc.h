#ifndef IPC_H
#define IPC_H

#include <stdbool.h>
#include <stdint.h>

#define MESSAGE_QUEUE_SIZE 32

typedef struct {
  uint32_t completed_task_id;
} CompletionMessage;

typedef struct {
  CompletionMessage buffer[MESSAGE_QUEUE_SIZE];
  volatile uint8_t head;
  volatile uint8_t tail;
} MessageQueue;

void ipc_init(uint8_t my_proc_id);
void ipc_send_completion_signal(uint32_t task_id);
bool ipc_receive_message(CompletionMessage *msg);
void ipc_cleanup(void);

#endif

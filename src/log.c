#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

ring_buffer_t log_queue;

static FILE *log_file = NULL;
static pthread_t logger_thread;

#define LOG_QUEUE_SIZE 512

static char log_buffer[LOG_QUEUE_SIZE][MAX_LOG_MSG_SIZE];
static _Atomic uint64_t log_seq[LOG_QUEUE_SIZE];

_Atomic int log_wakeup_pending;

sem_t log_sem;

LogLevel __attribute__((weak)) current_log_level = LOG_LEVEL_INFO;

static void *logger_thread_func(void *arg) {
  (void)arg;
  char msg[MAX_LOG_MSG_SIZE];

  atomic_store(&log_wakeup_pending, 0);

  while (1) {
    sem_wait(&log_sem);
    while (ring_buffer_try_dequeue(&log_queue, msg) == 0) {
      if (log_file) {
        fwrite(msg, strlen(msg), 1, log_file);
      }
    }
  }
}

void log_system_init(uint8_t proc_id) {
  system("mkdir -p target/logs");

  char log_filename[64];
  snprintf(log_filename, sizeof(log_filename), "target/logs/log_p%u.txt",
           proc_id);
  log_file = fopen(log_filename, "w");
  if (log_file == NULL) {
    perror("Failed to open log file");
  }

  ring_buffer_init(&log_queue, LOG_QUEUE_SIZE, log_buffer, log_seq,
                   MAX_LOG_MSG_SIZE);
  sem_init(&log_sem, 0, 0);
  atomic_store(&log_wakeup_pending, 0);

  if (pthread_create(&logger_thread, NULL, logger_thread_func, NULL)) {
    perror("pthread_create logger failed");
  }
}

void log_system_shutdown(void) {
  if (log_file) {
    fflush(log_file);
    fclose(log_file);
    log_file = NULL;
  }
}

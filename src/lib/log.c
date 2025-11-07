#include "lib/log.h"
#include "lib/ring_buffer.h"
#include "processor.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

ring_buffer log_queue;

static FILE *log_file = NULL;
static pthread_t logger_thread;
static _Atomic int shutdown_requested = 0;

dispatch_semaphore_t log_sem;

#define LOG_QUEUE_SIZE 512

static char log_buffer[LOG_QUEUE_SIZE][MAX_LOG_MSG_SIZE];
static _Atomic uint64_t log_seq[LOG_QUEUE_SIZE];

_Atomic int log_wakeup_pending;

log_level __attribute__((weak)) current_log_level = LOG_LEVEL_INFO;

static void *logger_thread_func(void *arg) {
  (void)arg;
  char msg[MAX_LOG_MSG_SIZE];

  atomic_store(&log_wakeup_pending, 0);

  while (!atomic_load(&shutdown_requested)) {
    dispatch_semaphore_wait(log_sem, DISPATCH_TIME_FOREVER);

    while (ring_buffer_try_dequeue(&log_queue, msg) == 0) {
      if (log_file) {
        fwrite(msg, strlen(msg), 1, log_file);
      }
    }
    atomic_store(&log_wakeup_pending, 0);
  }

  return NULL;
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

  log_sem = dispatch_semaphore_create(0);

  atomic_store(&log_wakeup_pending, 0);

  if (pthread_create(&logger_thread, NULL, logger_thread_func, NULL)) {
    perror("pthread_create logger failed");
  }
}

void log_system_shutdown(void) {
  atomic_store(&shutdown_requested, 1);

  dispatch_semaphore_signal(log_sem);

  pthread_join(logger_thread, NULL);

  dispatch_release(log_sem);

  if (log_file) {
    fflush(log_file);
    fclose(log_file);
    log_file = NULL;
  }
}

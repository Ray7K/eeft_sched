#ifndef LOG_H
#define LOG_H

#include "lib/platform_sem.h"
#include "lib/ring_buffer.h"

#include "processor.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern ring_buffer log_queue;

typedef enum {
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARN,
  LOG_LEVEL_ERROR,
  LOG_LEVEL_FATAL
} log_level;

typedef struct {
  uint8_t proc_id;
  uint8_t core_id;
  bool is_set;
} log_thread_context;

extern log_level current_log_level;

extern platform_sem_t log_sem;

extern _Atomic int log_wakeup_pending;

extern __thread log_thread_context log_thread_ctx;

void log_system_init(uint8_t proc_id);
void log_system_shutdown(void);

#define FILENAME                                                               \
  (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG(level, fmt, ...)                                                   \
  do {                                                                         \
    if (level >= current_log_level) {                                          \
      char msg_buf[MAX_LOG_MSG_SIZE];                                          \
      const char *level_str;                                                   \
      switch (level) {                                                         \
      case LOG_LEVEL_DEBUG:                                                    \
        level_str = "DEBUG";                                                   \
        break;                                                                 \
      case LOG_LEVEL_INFO:                                                     \
        level_str = "INFO";                                                    \
        break;                                                                 \
      case LOG_LEVEL_WARN:                                                     \
        level_str = "WARN";                                                    \
        break;                                                                 \
      case LOG_LEVEL_ERROR:                                                    \
        level_str = "ERROR";                                                   \
        break;                                                                 \
      case LOG_LEVEL_FATAL:                                                    \
        level_str = "FATAL";                                                   \
        break;                                                                 \
      default:                                                                 \
        level_str = "UNKNOWN";                                                 \
        break;                                                                 \
      }                                                                        \
      if (log_thread_ctx.is_set) {                                             \
        snprintf(msg_buf, sizeof(msg_buf),                                     \
                 "[%u] [P%u: C%u] [%s] [%s:%d] " fmt "\n",                     \
                 proc_state.system_time, log_thread_ctx.proc_id,               \
                 log_thread_ctx.core_id, level_str, FILENAME, __LINE__,        \
                 ##__VA_ARGS__);                                               \
      } else {                                                                 \
        snprintf(msg_buf, sizeof(msg_buf),                                     \
                 "[%u] [SYS] [%s] [%s:%d] " fmt "\n", proc_state.system_time,  \
                 level_str, FILENAME, __LINE__, ##__VA_ARGS__);                \
      }                                                                        \
      if (ring_buffer_try_enqueue(&log_queue, msg_buf) == 0) {                 \
        if (atomic_exchange(&log_wakeup_pending, 1) == 0) {                    \
          platform_sem_post(&log_sem);                                         \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

#endif

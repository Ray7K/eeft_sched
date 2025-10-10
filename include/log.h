#ifndef LOG_H
#define LOG_H

#include "platform.h"
#include <stdio.h>
#include <string.h>

typedef enum {
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARN,
  LOG_LEVEL_ERROR,
  LOG_LEVEL_FATAL
} LogLevel;

typedef struct {
  uint8_t proc_id;
  uint8_t core_id;
  bool is_set;
} LogThreadContext;

extern LogLevel current_log_level;

extern __thread LogThreadContext log_thread_context;

#define __FILENAME__                                                           \
  (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG(level, fmt, ...)                                                   \
  do {                                                                         \
    if (level >= current_log_level) {                                          \
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
      if (log_thread_context.is_set) {                                         \
        fprintf(stdout, "[%u] [P%u: C%u] [%s] [%s:%d] " fmt "\n",              \
                processor_state.system_time, log_thread_context.proc_id,       \
                log_thread_context.core_id, level_str, __FILENAME__, __LINE__, \
                ##__VA_ARGS__);                                                \
      } else {                                                                 \
        fprintf(stdout, "[%u] [SYS] [%s] [%s:%d] " fmt "\n",                   \
                processor_state.system_time, level_str, __FILENAME__,          \
                __LINE__, ##__VA_ARGS__);                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

#endif

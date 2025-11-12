#ifndef LIB_PLATFORM_SEM_H
#define LIB_PLATFORM_SEM_H

#if defined(__APPLE__)
#include <dispatch/dispatch.h>

typedef dispatch_semaphore_t platform_sem_t;

static inline void platform_sem_init(platform_sem_t *s, unsigned int value) {
  *s = dispatch_semaphore_create(value);
}

static inline void platform_sem_wait(platform_sem_t *s) {
  dispatch_semaphore_wait(*s, DISPATCH_TIME_FOREVER);
}

static inline void platform_sem_post(platform_sem_t *s) {
  dispatch_semaphore_signal(*s);
}

static inline void platform_sem_destroy(platform_sem_t *s) {
  dispatch_release(*s);
}

#else
#include <errno.h>
#include <semaphore.h>

typedef sem_t platform_sem_t;

static inline void platform_sem_init(platform_sem_t *s, unsigned int value) {
  sem_init(s, 0, value);
}

static inline void platform_sem_wait(platform_sem_t *s) {
  while (sem_wait(s) == -1 && errno == EINTR) {
  }
}

static inline void platform_sem_post(platform_sem_t *s) { sem_post(s); }

static inline void platform_sem_destroy(platform_sem_t *s) { sem_destroy(s); }
#endif

#endif

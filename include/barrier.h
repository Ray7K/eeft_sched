#ifndef BARRIER_H
#define BARRIER_H

#include <errno.h>
#include <pthread.h>

#define BARRIER_SERIAL_THREAD 1

typedef struct {
  unsigned count;
  unsigned target;
  unsigned cycle;
  pthread_mutex_t mut;
  pthread_cond_t cond;
} barrier_t;

int barrier_init(barrier_t *barrier, unsigned n);
int barrier_destroy(barrier_t *barrier);
int barrier_wait(barrier_t *barrier);

#endif

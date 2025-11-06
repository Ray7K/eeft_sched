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
} barrier;

int barrier_init(barrier *barrier, unsigned n, int pshared);
int barrier_destroy(barrier *barrier);
int barrier_wait(barrier *barrier);

#endif

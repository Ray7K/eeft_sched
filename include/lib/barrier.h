#ifndef BARRIER_H
#define BARRIER_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#define BARRIER_SERIAL_THREAD 1

typedef struct {
  uint64_t count;
  uint64_t target;
  uint64_t cycle;
  pthread_mutex_t mut;
  pthread_cond_t cond;
} barrier;

int barrier_init(barrier *barrier, unsigned n, int pshared);
int barrier_destroy(barrier *barrier);
int barrier_wait(barrier *barrier);

#endif

#include "barrier.h"

int barrier_init(barrier_t *barrier, unsigned n) {
  if (n == 0) {
    return EINVAL;
  }

  if (pthread_mutex_init(&barrier->mut, NULL) != 0) {
    return EPERM;
  }

  if (pthread_cond_init(&barrier->cond, NULL) != 0) {
    pthread_mutex_destroy(&barrier->mut);
    return EPERM;
  }

  barrier->target = n;
  barrier->count = 0;
  barrier->cycle = 0;

  return 0;
}

int barrier_destroy(barrier_t *barrier) {
  pthread_mutex_lock(&barrier->mut);
  if (barrier->count != 0) {
    pthread_mutex_unlock(&barrier->mut);
    return EBUSY;
  }
  pthread_mutex_unlock(&barrier->mut);

  pthread_mutex_destroy(&barrier->mut);
  pthread_cond_destroy(&barrier->cond);

  return 0;
}

int barrier_wait(barrier_t *barrier) {
  pthread_mutex_lock(&barrier->mut);

  unsigned cur_cycle = barrier->cycle;
  barrier->count++;

  if (barrier->count == barrier->target) {
    barrier->cycle++;
    barrier->count = 0;
    pthread_cond_broadcast(&barrier->cond);
    pthread_mutex_unlock(&barrier->mut);
    return BARRIER_SERIAL_THREAD;
  } else {
    while (cur_cycle == barrier->cycle) {
      pthread_cond_wait(&barrier->cond, &barrier->mut);
    }
    pthread_mutex_unlock(&barrier->mut);
    return 0;
  }
}

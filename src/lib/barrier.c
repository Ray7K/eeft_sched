#include "lib/barrier.h"

int barrier_init(barrier *barrier, unsigned n, int pshared) {
  if (n == 0) {
    return EINVAL;
  }

  pthread_mutexattr_t mut_attr;
  pthread_condattr_t cond_attr;

  pthread_mutexattr_init(&mut_attr);
  pthread_condattr_init(&cond_attr);

  if (pshared) {
    if (pthread_mutexattr_setpshared(&mut_attr, PTHREAD_PROCESS_SHARED) != 0) {
      goto fail;
    }

    if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
      goto fail;
    }
  }

  if (pthread_mutex_init(&barrier->mut, &mut_attr) != 0) {
    goto fail;
  }

  if (pthread_cond_init(&barrier->cond, &cond_attr) != 0) {
    pthread_mutex_destroy(&barrier->mut);
    goto fail;
  }

  barrier->target = n;
  barrier->count = 0;
  barrier->cycle = 0;

  return 0;

fail:
  pthread_mutexattr_destroy(&mut_attr);
  pthread_condattr_destroy(&cond_attr);
  return EPERM;
}

int barrier_destroy(barrier *barrier) {
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

int barrier_wait(barrier *barrier) {
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

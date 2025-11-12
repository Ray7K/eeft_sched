#include "tests/test_assert.h"
#include "tests/test_core.h"

#include "lib/barrier.h"
#include "lib/ring_buffer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define BUF_SIZE 64lu
#define MPMC_PROD 4
#define MPMC_CONS 4
#define ITEMS_PER_PROD 10000lu
#define TOTAL_ITEMS (MPMC_PROD * ITEMS_PER_PROD)

typedef struct {
  ring_buffer rb;
  void *buffer;
  _Atomic uint64_t *seq;
} rb_ctx_t;

static int rb_tests_init(test_ctx *ctx) {
  rb_ctx_t *priv = malloc(sizeof(rb_ctx_t));
  ASSERT_RET(ctx, priv != NULL, -ENOMEM, "failed alloc");

  priv->buffer = malloc(BUF_SIZE * sizeof(uint64_t));
  priv->seq = malloc(BUF_SIZE * sizeof(_Atomic uint64_t));
  ASSERT_RET(ctx, priv->buffer && priv->seq, -ENOMEM, "alloc fail");

  ring_buffer_init(&priv->rb, BUF_SIZE, priv->buffer, priv->seq,
                   sizeof(uint64_t));
  ctx->priv = priv;
  return 0;
}

static void rb_tests_exit(test_ctx *ctx) {
  rb_ctx_t *priv = ctx->priv;
  free(priv->buffer);
  free(priv->seq);
  free(priv);
}

static void test_rb_init_state(test_ctx *ctx) {
  rb_ctx_t *priv = ctx->priv;
  EXPECT_EQ(ctx, atomic_load(&priv->rb.head), 0lu);
  EXPECT_EQ(ctx, atomic_load(&priv->rb.tail), 0lu);
  EXPECT_EQ(ctx, priv->rb.buf_size, BUF_SIZE);
  for (uint64_t i = 0; i < BUF_SIZE; i++)
    EXPECT_EQ(ctx, atomic_load(&priv->seq[i]), i);
}

static void test_rb_single_enqueue_dequeue(test_ctx *ctx) {
  rb_ctx_t *priv = ctx->priv;
  uint64_t val = 42, out = 0;
  EXPECT_OK(ctx, ring_buffer_enqueue(&priv->rb, &val));
  EXPECT_OK(ctx, ring_buffer_dequeue(&priv->rb, &out));
  EXPECT_EQ(ctx, val, out);
}

static void test_rb_fill_empty_wrap(test_ctx *ctx) {
  rb_ctx_t *priv = ctx->priv;
  for (uint64_t i = 0; i < BUF_SIZE; i++)
    EXPECT_OK(ctx, ring_buffer_try_enqueue(&priv->rb, &i));

  uint64_t extra = 999;
  EXPECT_EQ(ctx, ring_buffer_try_enqueue(&priv->rb, &extra), -ENOSPC);

  for (uint64_t i = 0; i < BUF_SIZE; i++) {
    uint64_t out = 0;
    EXPECT_OK(ctx, ring_buffer_try_dequeue(&priv->rb, &out));
    EXPECT_EQ(ctx, out, i);
  }

  EXPECT_EQ(ctx, ring_buffer_try_dequeue(&priv->rb, &extra), -EAGAIN);

  for (uint64_t i = 0; i < BUF_SIZE * 2; i++) {
    EXPECT_OK(ctx, ring_buffer_enqueue(&priv->rb, &i));
    uint64_t out;
    EXPECT_OK(ctx, ring_buffer_dequeue(&priv->rb, &out));
    EXPECT_EQ(ctx, out, i);
  }
}

static void test_rb_clear(test_ctx *ctx) {
  rb_ctx_t *priv = ctx->priv;
  for (uint64_t i = 0; i < BUF_SIZE / 2; i++)
    EXPECT_OK(ctx, ring_buffer_try_enqueue(&priv->rb, &i));

  ring_buffer_clear(&priv->rb);
  uint64_t extra = 0;
  EXPECT_EQ(ctx, ring_buffer_try_dequeue(&priv->rb, &extra), -EAGAIN);

  for (uint64_t i = 0; i < BUF_SIZE; i++) {
    uint64_t seq = atomic_load(&priv->seq[i]);
    EXPECT(ctx, seq == i || seq >= BUF_SIZE);
  }
}

typedef struct {
  ring_buffer *rb;
  barrier *bar;
  _Atomic int64_t *remaining;
  pthread_mutex_t *lock;
  uint64_t *counts;
  uint64_t id;
} thread_data_t;

static void *producer(void *arg) {
  thread_data_t *d = arg;
  barrier_wait(d->bar);
  for (uint64_t i = 0; i < ITEMS_PER_PROD; i++) {
    uint64_t val = (d->id << 32) | i;
    ring_buffer_enqueue(d->rb, &val);
  }
  return NULL;
}

static void *consumer(void *arg) {
  thread_data_t *d = arg;
  barrier_wait(d->bar);
  while (atomic_fetch_sub(d->remaining, 1) > 0) {
    uint64_t val = 0;
    ring_buffer_dequeue(d->rb, &val);
    uint32_t pid = val >> 32;
    pthread_mutex_lock(d->lock);
    d->counts[pid]++;
    pthread_mutex_unlock(d->lock);
  }
  return NULL;
}

static void test_rb_mpmc_stress(test_ctx *ctx) {
  rb_ctx_t *priv = ctx->priv;
  barrier bar;
  pthread_mutex_t lock;
  pthread_t prod[MPMC_PROD], cons[MPMC_CONS];
  thread_data_t td[MPMC_PROD + MPMC_CONS];
  uint64_t counts[MPMC_PROD] = {0};
  _Atomic int64_t remaining;
  atomic_init(&remaining, TOTAL_ITEMS);

  pthread_mutex_init(&lock, NULL);
  barrier_init(&bar, MPMC_PROD + MPMC_CONS, 0);

  for (uint64_t i = 0; i < MPMC_PROD; i++) {
    td[i] = (thread_data_t){.rb = &priv->rb, .bar = &bar, .id = i};
    pthread_create(&prod[i], NULL, producer, &td[i]);
  }

  for (uint64_t i = 0; i < MPMC_CONS; i++) {
    uint64_t idx = i + MPMC_PROD;
    td[idx] = (thread_data_t){.rb = &priv->rb,
                              .bar = &bar,
                              .remaining = &remaining,
                              .lock = &lock,
                              .counts = counts};
    pthread_create(&cons[i], NULL, consumer, &td[idx]);
  }

  for (uint64_t i = 0; i < MPMC_PROD; i++)
    pthread_join(prod[i], NULL);
  for (uint64_t i = 0; i < MPMC_CONS; i++)
    pthread_join(cons[i], NULL);

  barrier_destroy(&bar);
  pthread_mutex_destroy(&lock);

  uint64_t total = 0;
  for (uint64_t i = 0; i < MPMC_PROD; i++) {
    EXPECT_EQ(ctx, counts[i], ITEMS_PER_PROD);
    total += counts[i];
  }
  EXPECT_EQ(ctx, total, TOTAL_ITEMS);
}

static void test_rb_small_buffer(test_ctx *ctx) {
  ring_buffer rb;
  uint64_t buf[2];
  _Atomic uint64_t seq[2];
  EXPECT_EQ(ctx, ring_buffer_init(&rb, 2, buf, seq, sizeof(uint64_t)), -EINVAL);
}

static test_case rb_cases[] = {
    TEST_CASE(test_rb_init_state),
    TEST_CASE(test_rb_single_enqueue_dequeue),
    TEST_CASE(test_rb_fill_empty_wrap),
    TEST_CASE(test_rb_clear),
    TEST_CASE(test_rb_mpmc_stress),
    TEST_CASE(test_rb_small_buffer),
    {NULL, NULL},
};

test_suite ring_buffer_suite = {
    .name = "ring_buffer_suite",
    .init = rb_tests_init,
    .exit = rb_tests_exit,
    .cases = rb_cases,
};

REGISTER_SUITE(ring_buffer_suite);

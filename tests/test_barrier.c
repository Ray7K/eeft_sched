#include "lib/barrier.h"
#include "tests/test_assert.h"
#include "tests/test_core.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#define THREAD_COUNT 4

typedef struct {
  barrier b;
  int shared_counter;
  pthread_mutex_t lock;
} test_barrier_ctx_t;

static int barrier_tests_init(test_ctx *ctx) {
  test_barrier_ctx_t *priv = malloc(sizeof(test_barrier_ctx_t));
  ASSERT_RET(ctx, priv != NULL, -ENOMEM, "Failed to allocate test context");
  pthread_mutex_init(&priv->lock, NULL);

  int ret = barrier_init(&priv->b, THREAD_COUNT, 0);
  ASSERT_RET(ctx, ret == 0, ret, "Failed to initialize barrier");

  priv->shared_counter = 0;
  ctx->priv = priv;
  return 0;
}

static void barrier_tests_exit(test_ctx *ctx) {
  if (!ctx->priv)
    return;
  test_barrier_ctx_t *priv = ctx->priv;
  barrier_destroy(&priv->b);
  pthread_mutex_destroy(&priv->lock);
  free(priv);
}

static void test_barrier_init_valid_invalid(test_ctx *ctx) {
  barrier b;
  int ret;

  ret = barrier_init(&b, 2, 0);
  EXPECT_EQ(ctx, ret, 0);
  EXPECT_EQ(ctx, b.target, 2lu);
  EXPECT_EQ(ctx, b.count, 0lu);
  EXPECT_EQ(ctx, b.cycle, 0lu);
  EXPECT_EQ(ctx, barrier_destroy(&b), 0);

  ret = barrier_init(&b, 0, 0);
  EXPECT_EQ(ctx, ret, EINVAL);

  ret = barrier_init(&b, 2, 1); // shared barrier; may fail or succeed
  EXPECT(ctx, ret == 0 || ret == EPERM);
  if (ret == 0)
    EXPECT_EQ(ctx, barrier_destroy(&b), 0);
}

typedef struct {
  test_barrier_ctx_t *priv;
  int result;
} worker_t;

static void *worker_func(void *arg) {
  worker_t *w = arg;
  pthread_mutex_lock(&w->priv->lock);
  w->priv->shared_counter++;
  pthread_mutex_unlock(&w->priv->lock);
  w->result = barrier_wait(&w->priv->b);
  return NULL;
}

static void test_barrier_basic_sync(test_ctx *ctx) {
  test_barrier_ctx_t *priv = ctx->priv;
  pthread_t threads[THREAD_COUNT];
  worker_t workers[THREAD_COUNT];

  for (int i = 0; i < THREAD_COUNT; i++) {
    workers[i].priv = priv;
    pthread_create(&threads[i], NULL, worker_func, &workers[i]);
  }

  for (int i = 0; i < THREAD_COUNT; i++)
    pthread_join(threads[i], NULL);

  EXPECT_EQ(ctx, priv->shared_counter, THREAD_COUNT);

  int serial_count = 0;
  for (int i = 0; i < THREAD_COUNT; i++)
    if (workers[i].result == BARRIER_SERIAL_THREAD)
      serial_count++;
  EXPECT_EQ(ctx, serial_count, 1);
}

static void test_barrier_destroy_unused_busy(test_ctx *ctx) {
  barrier b;
  int ret = barrier_init(&b, 2, 0);
  ASSERT_OK(ctx, ret);

  EXPECT_EQ(ctx, barrier_destroy(&b), 0);

  // Busy barrier test: simulate active threads
  ret = barrier_init(&b, 2, 0);
  ASSERT_OK(ctx, ret);
  pthread_mutex_lock(&b.mut);
  b.count = 1; // pretend one thread is waiting
  pthread_mutex_unlock(&b.mut);
  EXPECT_EQ(ctx, barrier_destroy(&b), EBUSY);

  // cleanup manually
  pthread_mutex_destroy(&b.mut);
  pthread_cond_destroy(&b.cond);
}

static void test_barrier_single_thread(test_ctx *ctx) {
  barrier b;
  int ret = barrier_init(&b, 1, 0);
  ASSERT_OK(ctx, ret);

  int result = barrier_wait(&b);
  EXPECT_EQ(ctx, result, BARRIER_SERIAL_THREAD);

  EXPECT_EQ(ctx, barrier_destroy(&b), 0);
}

static test_case barrier_cases[] = {
    TEST_CASE(test_barrier_init_valid_invalid),
    TEST_CASE(test_barrier_basic_sync),
    TEST_CASE(test_barrier_destroy_unused_busy),
    TEST_CASE(test_barrier_single_thread),
    {NULL, NULL},
};

test_suite barrier_suite = {
    .name = "barrier_suite",
    .init = barrier_tests_init,
    .exit = barrier_tests_exit,
    .cases = barrier_cases,
};

REGISTER_SUITE(barrier_suite);

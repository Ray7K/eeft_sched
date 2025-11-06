#include "lib/barrier.h"
#include "lib/ring_buffer.h"
#include "tests/tests.h"
#include <pthread.h>
#include <stdlib.h>

#define BUFFER_SIZE 128
#define STRESS_NUM_PRODUCERS 4
#define STRESS_NUM_CONSUMERS 4
#define STRESS_ITEMS_PER_PRODUCER 100000
#define STRESS_TOTAL_ITEMS (STRESS_NUM_PRODUCERS * STRESS_ITEMS_PER_PRODUCER)

// Context for each test, holding the ring buffer and its allocated memory
typedef struct {
  ring_buffer rb;
  void *buffer;
  _Atomic uint64_t *seq_array;
} test_rb_context_t;

// Setup function to initialize the ring buffer before each test
static void setup_ring_buffer(TestCase *test) {
  test_rb_context_t *ctx = malloc(sizeof(test_rb_context_t));
  ASSERT(test, ctx != NULL);

  ctx->buffer = malloc(BUFFER_SIZE * sizeof(uint64_t));
  ASSERT(test, ctx->buffer != NULL);

  ctx->seq_array = malloc(BUFFER_SIZE * sizeof(_Atomic uint64_t));
  ASSERT(test, ctx->seq_array != NULL);

  ring_buffer_init(&ctx->rb, BUFFER_SIZE, ctx->buffer, ctx->seq_array,
                   sizeof(uint64_t));

  test->priv = ctx; // Store context in the test case for access
}

// Teardown function to free memory after each test
static void teardown_ring_buffer(TestCase *test) {
  if (test->priv) {
    test_rb_context_t *ctx = test->priv;
    free(ctx->buffer);
    free(ctx->seq_array);
    free(ctx);
  }
}

// --- Single-Threaded Tests ---

static void test_rb_init_state(TestCase *test) {
  test_rb_context_t *ctx = test->priv;
  EXPECT(test, atomic_load(&ctx->rb.head) == 0);
  EXPECT(test, atomic_load(&ctx->rb.tail) == 0);
  EXPECT(test, ctx->rb.buf_size == BUFFER_SIZE);
  for (uint64_t i = 0; i < BUFFER_SIZE; i++) {
    EXPECT(test, atomic_load(&ctx->rb.seq[i]) == i);
  }
}

static void test_rb_single_enqueue_dequeue(TestCase *test) {
  test_rb_context_t *ctx = test->priv;
  uint64_t in_val = 12345;
  uint64_t out_val = 0;

  int enq_ret = ring_buffer_enqueue(&ctx->rb, &in_val);
  EXPECT(test, enq_ret == 0);

  int deq_ret = ring_buffer_dequeue(&ctx->rb, &out_val);
  EXPECT(test, deq_ret == 0);
  EXPECT(test, out_val == in_val);
}

static void test_rb_empty_dequeue_fails(TestCase *test) {
  test_rb_context_t *ctx = test->priv;
  uint64_t out_val = 0;
  int deq_ret = ring_buffer_try_dequeue(&ctx->rb, &out_val);
  EXPECT(test, deq_ret == -1);
}

static void test_rb_fill_and_empty(TestCase *test) {
  test_rb_context_t *ctx = test->priv;

  // Fill the buffer
  for (uint64_t i = 0; i < BUFFER_SIZE; i++) {
    int enq_ret = ring_buffer_try_enqueue(&ctx->rb, &i);
    EXPECT(test, enq_ret == 0);
  }

  // Next enqueue should fail because the buffer is full
  uint64_t extra_val = 999;
  int full_ret = ring_buffer_try_enqueue(&ctx->rb, &extra_val);
  EXPECT(test, full_ret == -1);

  // Empty the buffer and check values
  for (uint64_t i = 0; i < BUFFER_SIZE; i++) {
    uint64_t out_val = 0;
    int deq_ret = ring_buffer_try_dequeue(&ctx->rb, &out_val);
    EXPECT(test, deq_ret == 0);
    EXPECT(test, out_val == i);
  }

  // Next dequeue should fail because the buffer is empty
  int empty_ret = ring_buffer_try_dequeue(&ctx->rb, &extra_val);
  EXPECT(test, empty_ret == -1);
}

static void test_rb_wrap_around(TestCase *test) {
  test_rb_context_t *ctx = test->priv;

  // Enqueue and dequeue more than BUFFER_SIZE items to test wrap-around
  for (uint64_t i = 0; i < BUFFER_SIZE * 3; i++) {
    int enq_ret = ring_buffer_enqueue(&ctx->rb, &i);
    ASSERT(test, enq_ret == 0);

    uint64_t out_val = 0;
    int deq_ret = ring_buffer_dequeue(&ctx->rb, &out_val);
    ASSERT(test, deq_ret == 0);
    ASSERT(test, out_val == i);
  }
  EXPECT(test, atomic_load(&ctx->rb.head) == BUFFER_SIZE * 3);
  EXPECT(test, atomic_load(&ctx->rb.tail) == BUFFER_SIZE * 3);
}

// --- Multi-Threaded Stress Test ---

// Shared data for MPMC test
typedef struct {
  ring_buffer *rb;
  barrier *barrier;
  _Atomic int64_t *items_to_consume;
  // Results need a mutex because multiple consumers write to it
  pthread_mutex_t *results_lock;
  uint64_t *producer_counts; // Array of size STRESS_NUM_PRODUCERS
  int thread_id;
} mpmc_thread_data_t;

// Producer thread function
static void *producer_func(void *arg) {
  mpmc_thread_data_t *data = arg;
  barrier_wait(data->barrier);

  for (uint64_t i = 0; i < STRESS_ITEMS_PER_PRODUCER; i++) {
    // Pack producer ID and item ID into a single value
    uint64_t val = ((uint64_t)data->thread_id << 32) | (uint64_t)i;
    ring_buffer_enqueue(data->rb, &val);
  }
  return NULL;
}

// Consumer thread function
static void *consumer_func(void *arg) {
  mpmc_thread_data_t *data = arg;
  barrier_wait(data->barrier);

  // Each consumer tries to claim an item to consume until none are left
  while (atomic_fetch_sub_explicit(data->items_to_consume, 1,
                                   memory_order_relaxed) > 0) {
    uint64_t out_val = 0;
    ring_buffer_dequeue(data->rb, &out_val);

    uint32_t producer_id = out_val >> 32;
    // uint32_t item_id = out_val & 0xFFFFFFFF;

    // Lock results to safely update the counts
    pthread_mutex_lock(data->results_lock);
    if (producer_id < STRESS_NUM_PRODUCERS) {
      data->producer_counts[producer_id]++;
    }
    pthread_mutex_unlock(data->results_lock);
  }
  return NULL;
}

static void test_rb_mpmc_stress(TestCase *test) {
  test_rb_context_t *ctx = test->priv;
  pthread_t producers[STRESS_NUM_PRODUCERS];
  pthread_t consumers[STRESS_NUM_CONSUMERS];
  mpmc_thread_data_t thread_data[STRESS_NUM_PRODUCERS + STRESS_NUM_CONSUMERS];
  barrier barrier;

  // Shared results data
  uint64_t producer_counts[STRESS_NUM_PRODUCERS] = {0};
  pthread_mutex_t results_lock;
  _Atomic int64_t items_to_consume;

  pthread_mutex_init(&results_lock, NULL);
  barrier_init(&barrier, STRESS_NUM_PRODUCERS + STRESS_NUM_CONSUMERS, 0);
  atomic_init(&items_to_consume, STRESS_TOTAL_ITEMS);

  // Create producer threads
  for (int i = 0; i < STRESS_NUM_PRODUCERS; i++) {
    thread_data[i] = (mpmc_thread_data_t){
        .rb = &ctx->rb, .barrier = &barrier, .thread_id = i};
    pthread_create(&producers[i], NULL, producer_func, &thread_data[i]);
  }

  // Create consumer threads
  for (int i = 0; i < STRESS_NUM_CONSUMERS; i++) {
    int data_idx = i + STRESS_NUM_PRODUCERS;
    thread_data[data_idx] =
        (mpmc_thread_data_t){.rb = &ctx->rb,
                             .barrier = &barrier,
                             .thread_id = i,
                             .items_to_consume = &items_to_consume,
                             .results_lock = &results_lock,
                             .producer_counts = producer_counts};
    pthread_create(&consumers[i], NULL, consumer_func, &thread_data[data_idx]);
  }

  // Wait for all threads to complete
  for (int i = 0; i < STRESS_NUM_PRODUCERS; i++) {
    pthread_join(producers[i], NULL);
  }
  for (int i = 0; i < STRESS_NUM_CONSUMERS; i++) {
    pthread_join(consumers[i], NULL);
  }

  barrier_destroy(&barrier);
  pthread_mutex_destroy(&results_lock);

  // --- Verification ---
  uint64_t total_consumed = 0;
  for (int i = 0; i < STRESS_NUM_PRODUCERS; i++) {
    // Check that each producer's items were all consumed
    EXPECT(test, producer_counts[i] == STRESS_ITEMS_PER_PRODUCER);
    total_consumed += producer_counts[i];
  }
  // Check that the total number of items matches
  EXPECT(test, total_consumed == STRESS_TOTAL_ITEMS);
  // Check that the head and tail pointers match the total number of items
  EXPECT(test, atomic_load(&ctx->rb.head) == STRESS_TOTAL_ITEMS);
  EXPECT(test, atomic_load(&ctx->rb.tail) == STRESS_TOTAL_ITEMS);
}

// Register all the tests to be run
REGISTER_TEST("ring_buffer_init_state", test_rb_init_state, setup_ring_buffer,
              teardown_ring_buffer);
REGISTER_TEST("ring_buffer_single_enq_deq", test_rb_single_enqueue_dequeue,
              setup_ring_buffer, teardown_ring_buffer);
REGISTER_TEST("ring_buffer_empty_dequeue_fails", test_rb_empty_dequeue_fails,
              setup_ring_buffer, teardown_ring_buffer);
REGISTER_TEST("ring_buffer_fill_and_empty", test_rb_fill_and_empty,
              setup_ring_buffer, teardown_ring_buffer);
REGISTER_TEST("ring_buffer_wrap_around", test_rb_wrap_around, setup_ring_buffer,
              teardown_ring_buffer);
REGISTER_TEST("ring_buffer_mpmc_stress", test_rb_mpmc_stress, setup_ring_buffer,
              teardown_ring_buffer);

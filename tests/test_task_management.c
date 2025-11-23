#include "tests/test_assert.h"
#include "tests/test_core.h"

#include "task_management.h"

#include "lib/list.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static int tm_tests_init(test_ctx *ctx) {
  task_management_init();
  (void)ctx;
  return 0;
}

static void tm_tests_exit(test_ctx *ctx) { (void)ctx; }

static void test_task_management_init(test_ctx *ctx) {
  task_management_init();
  EXPECT(ctx, true);
}

static void test_create_and_clone_job(test_ctx *ctx) {
  static task_struct t = {
      .id = 42,
      .period = 100,
      .deadline = 100,
      .wcet = {10, 10},
      .crit_level = 0,
      .num_replicas = 0,
  };

  job_struct *j = create_job(&t, 0);
  ASSERT_NOT_NULL(ctx, j);
  EXPECT_EQ(ctx, j->parent_task->id, t.id);
  EXPECT_EQ(ctx, atomic_load(&j->refcount), 1);
  EXPECT_EQ(ctx, j->next_migration_eligible_tick, 0u);

  j->arrival_time = 10;
  j->virtual_deadline = 50;
  j->actual_deadline = 60;
  j->wcet = 5.0f;
  j->acet = 4.0f;
  j->executed_time = 1.0f;
  j->state = JOB_STATE_READY;
  j->next_migration_eligible_tick = 60;
  for (int i = 0; i < MAX_CRITICALITY_LEVELS; i++)
    j->relative_tuned_deadlines[i] = 100 + i;

  job_struct *cj = clone_job(j, 0);
  ASSERT_NOT_NULL(ctx, cj);
  ASSERT_NE(ctx, cj, j);
  EXPECT_EQ(ctx, cj->parent_task->id, j->parent_task->id);
  EXPECT_EQ(ctx, cj->arrival_time, j->arrival_time);
  EXPECT_EQ(ctx, cj->actual_deadline, j->actual_deadline);
  EXPECT_EQ(ctx, cj->virtual_deadline, j->virtual_deadline);
  EXPECT_EQ(ctx, cj->state, j->state);
  EXPECT_NEAR(ctx, cj->wcet, j->wcet, 1.0e-10);
  EXPECT_NEAR(ctx, cj->acet, j->acet, 1.0e-10);
  EXPECT_NEAR(ctx, cj->executed_time, j->executed_time, 1.0e-10);
  EXPECT_EQ(ctx, cj->next_migration_eligible_tick,
            j->next_migration_eligible_tick);
  EXPECT_EQ(ctx, atomic_load(&cj->refcount), 1);
  for (int i = 0; i < MAX_CRITICALITY_LEVELS; i++)
    EXPECT_EQ(ctx, cj->relative_tuned_deadlines[i],
              j->relative_tuned_deadlines[i]);

  put_job_ref(j, 0);
  put_job_ref(cj, 0);
}

/* Refcounting behaviour */
static void test_refcounting_and_release(test_ctx *ctx) {
  static task_struct t = {.id = 100};
  job_struct *jobs[512];
  size_t n = 0;

  job_struct *j = create_job(&t, 0);
  ASSERT_NOT_NULL(ctx, j);
  EXPECT_EQ(ctx, atomic_load(&j->refcount), 1);

  get_job_ref(j);
  EXPECT_EQ(ctx, atomic_load(&j->refcount), 2);

  put_job_ref(j, 0);
  EXPECT_EQ(ctx, atomic_load(&j->refcount), 1);

  put_job_ref(j, 0);

  while ((jobs[n] = create_job(&t, 0)) != NULL &&
         n < sizeof(jobs) / sizeof(jobs[0]))
    n++;

  for (size_t i = 0; i < n; i++)
    put_job_ref(jobs[i], 0);

  EXPECT(ctx, true);
}

static void test_pool_exhaustion_and_restore(test_ctx *ctx) {
  static task_struct t = {.id = 200};
  job_struct **allocated = malloc(1024 * sizeof(*allocated));
  size_t i = 0;

  job_struct *j;
  while ((j = create_job(&t, 1)) != NULL) {
    allocated[i++] = j;
    if (i > 2000)
      break;
  }

  ASSERT(ctx, i > 0);
  job_struct *after = create_job(&t, 1);
  EXPECT(ctx, after == NULL);

  for (size_t k = 0; k < i; k++)
    put_job_ref(allocated[k], 1);

  free(allocated);

  job_struct *re = create_job(&t, 1);
  EXPECT_NOT_NULL(ctx, re);
  put_job_ref(re, 1);
}

static void test_queue_operations_sorted_and_pop(test_ctx *ctx) {
  LIST_HEAD(q);
  static task_struct t1 = {.id = 1}, t2 = {.id = 2}, t3 = {.id = 3};

  job_struct *j1 = create_job(&t1, 0);
  job_struct *j2 = create_job(&t2, 0);
  job_struct *j3 = create_job(&t3, 0);
  ASSERT_NOT_NULL(ctx, j1);
  ASSERT_NOT_NULL(ctx, j2);
  ASSERT_NOT_NULL(ctx, j3);

  j1->virtual_deadline = 50;
  j2->virtual_deadline = 30;
  j3->virtual_deadline = 40;

  add_to_queue_sorted(&q, j1);
  add_to_queue_sorted(&q, j2);
  add_to_queue_sorted(&q, j3);

  job_struct *p = peek_next_job(&q);
  ASSERT_NOT_NULL(ctx, p);
  EXPECT_EQ(ctx, p->virtual_deadline, 30u);

  job_struct *pop = pop_next_job(&q);
  ASSERT_NOT_NULL(ctx, pop);
  EXPECT_EQ(ctx, pop->virtual_deadline, 30u);
  put_job_ref(pop, 0);

  pop = pop_next_job(&q);
  ASSERT_NOT_NULL(ctx, pop);
  EXPECT_EQ(ctx, pop->virtual_deadline, 40u);
  put_job_ref(pop, 0);

  pop = pop_next_job(&q);
  ASSERT_NOT_NULL(ctx, pop);
  EXPECT_EQ(ctx, pop->virtual_deadline, 50u);
  put_job_ref(pop, 0);

  EXPECT(ctx, list_empty(&q));
}

static void test_remove_job_with_parent_task_id(test_ctx *ctx) {
  LIST_HEAD(q);
  static task_struct tA = {.id = 77}, tB = {.id = 88};

  job_struct *a1 = create_job(&tA, 0);
  job_struct *b1 = create_job(&tB, 0);
  job_struct *a2 = create_job(&tA, 0);
  ASSERT_NOT_NULL(ctx, a1);
  ASSERT_NOT_NULL(ctx, b1);
  ASSERT_NOT_NULL(ctx, a2);

  add_to_queue_sorted(&q, a1);
  add_to_queue_sorted(&q, b1);
  add_to_queue_sorted(&q, a2);

  remove_job_with_parent_task_id(&q, tA.id, 0);

  job_struct *p;
  int foundA = 0;
  list_for_each_entry(p, &q, link) {
    if (p->parent_task->id == tA.id)
      foundA++;
  }

  EXPECT_EQ(ctx, foundA, 0);

  while ((p = pop_next_job(&q)) != NULL)
    put_job_ref(p, 0);
}

static void *ref_thread_inc(void *arg) {
  job_struct *j = arg;
  for (int i = 0; i < 1000; i++)
    get_job_ref(j);
  return NULL;
}

static void *ref_thread_dec(void *arg) {
  job_struct *j = arg;
  for (int i = 0; i < 1000; i++)
    put_job_ref(j, 0);
  return NULL;
}

static void test_refcount_concurrent(test_ctx *ctx) {
  static task_struct t = {.id = 300};
  job_struct *j = create_job(&t, 0);
  ASSERT_NOT_NULL(ctx, j);

  pthread_t thr[4];
  for (int i = 0; i < 2; i++)
    pthread_create(&thr[i], NULL, ref_thread_inc, j);
  for (int i = 2; i < 4; i++)
    pthread_create(&thr[i], NULL, ref_thread_dec, j);

  for (int i = 0; i < 4; i++)
    pthread_join(thr[i], NULL);

  int cnt = atomic_load(&j->refcount);
  while (cnt > 1) {
    put_job_ref(j, 0);
    cnt = atomic_load(&j->refcount);
  }

  put_job_ref(j, 0);
}

static void test_release_to_remote_pool(test_ctx *ctx) {
  static task_struct t = {.id = 999};
  job_struct *j = create_job(&t, 0);
  ASSERT_NOT_NULL(ctx, j);

  uint8_t other_core = (NUM_CORES_PER_PROC > 1) ? 1 : 0;
  if (other_core == 0) {
    EXPECT(ctx, true);
    put_job_ref(j, 0);
    return;
  }

  j->job_pool_id = other_core;
  put_job_ref(j, 0);

  job_struct *r = create_job(&t, other_core);
  EXPECT_NOT_NULL(ctx, r);
  put_job_ref(r, other_core);
}

static test_case tm_cases[] = {
    TEST_CASE(test_task_management_init),
    TEST_CASE(test_create_and_clone_job),
    TEST_CASE(test_refcounting_and_release),
    TEST_CASE(test_pool_exhaustion_and_restore),
    TEST_CASE(test_queue_operations_sorted_and_pop),
    TEST_CASE(test_remove_job_with_parent_task_id),
    TEST_CASE(test_refcount_concurrent),
    TEST_CASE(test_release_to_remote_pool),
    {NULL, NULL},
};

test_suite task_management_suite = {
    .name = "task_management_suite",
    .init = tm_tests_init,
    .exit = tm_tests_exit,
    .cases = tm_cases,
};

REGISTER_SUITE(task_management_suite);

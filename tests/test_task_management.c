#include "sys_config.h"
#include "task_alloc.h"
#include "task_management.h"
#include "tests/tests.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

// This is a private macro in task_management.c, redefined here for testing.
#define JOBS_PER_CORE 100

static void setup(TestCase *test) {
  (void)test;
  task_management_init();
}

static void test_job_clone(TestCase *test) {
  const Task *parent_task = &system_tasks[0];

  Job *job = create_job(parent_task, 0);

  job->arrival_time = 10;
  job->actual_deadline = 50;
  job->virtual_deadline = 40;
  job->wcet = 5.0f;
  job->acet = 4.5f;
  job->executed_time = 1.0f;
  job->state = JOB_STATE_READY;

  Job *cloned_job = clone_job(job, 0);
  ASSERT(test, cloned_job != NULL);
  ASSERT(test, cloned_job != job);
  ASSERT(test, cloned_job->parent_task == job->parent_task);
  ASSERT(test, cloned_job->arrival_time == job->arrival_time);
  ASSERT(test, cloned_job->actual_deadline == job->actual_deadline);
  ASSERT(test, cloned_job->virtual_deadline == job->virtual_deadline);
  ASSERT(test, cloned_job->wcet == job->wcet);
  ASSERT(test, cloned_job->acet == job->acet);
  ASSERT(test, cloned_job->executed_time == job->executed_time);
  ASSERT(test, cloned_job->state == job->state);
  ASSERT(test, cloned_job->is_replica == job->is_replica);
  ASSERT(test, atomic_load(&cloned_job->refcount) == 1);
  ASSERT(test, atomic_load(&cloned_job->is_being_offered) == false);

  for (int i = 0; i < MAX_CRITICALITY_LEVELS; i++)
    ASSERT(test, cloned_job->relative_tuned_deadlines[i] ==
                     job->relative_tuned_deadlines[i]);

  put_job_ref(job, 0);
  put_job_ref(cloned_job, 0);
}

REGISTER_TEST("Job clone test", test_job_clone, setup, NULL);

// --- One-to-One Threading Test ---

#define NUM_THREADS_ONE_TO_ONE NUM_CORES_PER_PROC
#define ITERATIONS_PER_THREAD 5000

typedef struct {
  TestCase *test;
  uint16_t core_id;
  int iterations;
  const Task *parent_task;
} test_thread_data_t;

// Each thread operates on its own unique core_id
static void *one_to_one_worker(void *arg) {
  test_thread_data_t *data = (test_thread_data_t *)arg;

  for (int i = 0; i < data->iterations; i++) {
    Job *job = create_job(data->parent_task, data->core_id);
    if (job) {
      put_job_ref(job, data->core_id); // Release to own pool
    }
  }
  return NULL;
}

static void test_job_pool_one_to_one(TestCase *test) {
  ASSERT(test, NUM_THREADS_ONE_TO_ONE <= NUM_CORES_PER_PROC);

  pthread_t threads[NUM_THREADS_ONE_TO_ONE];
  test_thread_data_t thread_data[NUM_THREADS_ONE_TO_ONE];

  for (int i = 0; i < NUM_THREADS_ONE_TO_ONE; i++) {
    thread_data[i].test = test;
    thread_data[i].core_id = i;
    thread_data[i].iterations = ITERATIONS_PER_THREAD;
    thread_data[i].parent_task = &system_tasks[0];

    int rc =
        pthread_create(&threads[i], NULL, one_to_one_worker, &thread_data[i]);
    ASSERT(test, rc == 0);
  }

  for (int i = 0; i < NUM_THREADS_ONE_TO_ONE; i++) {
    pthread_join(threads[i], NULL);
  }

  // Verification: Check that each pool is full and not corrupted.
  for (int i = 0; i < NUM_THREADS_ONE_TO_ONE; i++) {
    Job *jobs[JOBS_PER_CORE];
    int allocated_count = 0;
    for (int j = 0; j < JOBS_PER_CORE; j++) {
      jobs[j] = create_job(&system_tasks[0], i);
      if (jobs[j] == NULL) {
        break; // Pool is empty
      }
      allocated_count++;
    }

    // Release them all back
    for (int j = 0; j < allocated_count; j++) {
      put_job_ref(jobs[j], i);
    }

    // This assertion will fail if the pool was corrupted and jobs were lost.
    ASSERT(test, allocated_count == JOBS_PER_CORE);
  }
}

REGISTER_TEST("Job pool one-to-one thread test", test_job_pool_one_to_one,
              setup, NULL);

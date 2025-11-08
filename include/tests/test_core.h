#ifndef TEST_CORE_H
#define TEST_CORE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_LOG_SIZE 512

typedef struct test_ctx test_ctx;
typedef struct test_case test_case;
typedef struct test_suite test_suite;

typedef void (*test_func)(test_ctx *test);

enum test_status {
  TEST_PASS,
  TEST_FAIL,
  TEST_SKIP,
};

struct test_ctx {
  const test_case *tcase;
  const test_suite *suite;
  void *priv;

  unsigned int assertions_executed;
  unsigned int expects_executed;
  unsigned int expects_failed;

  enum test_status status;
  char log[TEST_LOG_SIZE];
  size_t log_size;
  size_t log_offset;
};

struct test_case {
  const char *name;
  test_func func;
};

#define TEST_CASE(fn) {.name = #fn, .func = (fn)}

struct test_suite {
  const char *name;
  int (*init)(test_ctx *test);
  void (*exit)(test_ctx *test);
  int (*setup_suite)(void);
  void (*teardown_suite)(void);
  test_case *cases;
  FILE *output_stream;
  void *priv;
};

#define TEST_SUITE(suite_name, case_array)                                     \
  static test_suite suite_name = {.name = #suite_name,                         \
                                  .cases = (case_array),                       \
                                  .output_stream = NULL,                       \
                                  .priv = NULL};                               \
  REGISTER_SUITE(suite_name)

#ifdef __APPLE__
#define TEST_SECTION "__DATA,test_suites"
#else
#define TEST_SECTION "test_suites"

extern test_suite *__start_test_suites[];
extern test_suite *__stop_test_suites[];

#endif

#define REGISTER_SUITE(suite_name)                                             \
  __attribute__((no_sanitize("address"), used, aligned(8),                     \
                 section(TEST_SECTION))) static struct test_suite              \
      *__test_suite_##suite_name = &(suite_name);

#endif

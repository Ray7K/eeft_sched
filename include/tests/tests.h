#ifndef TESTS_H
#define TESTS_H

#include <stdbool.h>
#include <stdio.h>

typedef struct test_case test_case;

typedef void (*test_func_t)(test_case *test);

struct test_case {
  const char *name;
  test_func_t func;
  test_func_t setup;
  test_func_t teardown;
  void *priv;
  unsigned int assertions_executed;
  unsigned int expects_executed;
  unsigned int expects_failed;
  bool passed;
};

#define REGISTER_TEST(test_name_str, test_func_ptr, setup_func_ptr,            \
                      teardown_func_ptr)                                       \
  static test_case __test_case##test_func_ptr __attribute__((                  \
      used, section("__DATA,test_cases"), no_sanitize("address"))) = {         \
      .name = (test_name_str),                                                 \
      .func = (test_func_ptr),                                                 \
      .setup = (setup_func_ptr),                                               \
      .teardown = (teardown_func_ptr),                                         \
      .assertions_executed = 0,                                                \
      .expects_executed = 0,                                                   \
      .expects_failed = 0,                                                     \
      .passed = true};

#define ASSERT(testcase, cond)                                                 \
  do {                                                                         \
    (testcase)->assertions_executed++;                                         \
    if (!(cond)) {                                                             \
      printf("  ASSERT FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      (testcase)->passed = false;                                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define EXPECT(testcase, cond)                                                 \
  do {                                                                         \
    (testcase)->expects_executed++;                                            \
    if (!(cond)) {                                                             \
      printf("  EXPECT FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      (testcase)->expects_failed++;                                            \
      (testcase)->passed = false;                                              \
    }                                                                          \
  } while (0)

#endif

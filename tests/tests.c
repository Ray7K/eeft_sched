#include "tests.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define COLOR_RESET "\x1b[0m"
#define COLOR_RED "\x1b[31m"
#define COLOR_GREEN "\x1b[32m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_CYAN "\x1b[36m"
#define COLOR_GREY "\x1b[38;5;250m"
#define COLOR_LIGHT_GREY "\033[38;2;190;225;255m"

unsigned int tests_run = 0;
unsigned int tests_passed = 0;
unsigned int tests_failed = 0;

static double get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ts.tv_sec * 1000.0) + (ts.tv_nsec / 1e6);
}

static void run_tests(const char *filter) {
  tests_run = 0;
  tests_passed = 0;
  tests_failed = 0;

  printf(COLOR_CYAN "--- Starting Test Run --- \n\n" COLOR_RESET);

  size_t size;
  TestCase *tests = (TestCase *)getsectiondata(&_mh_execute_header, "__DATA",
                                               "test_cases", &size);
  if (tests == NULL) {
    printf("No test cases found.\n");
    return;
  }
  size_t num_tests = size / sizeof(TestCase);

  for (size_t i = 0; i < num_tests; i += 1) {
    TestCase *t = &tests[i];

    if (filter && strstr(t->name, filter) == NULL) {
      printf(COLOR_YELLOW "  Skipping test: %s (filtered)\n\n" COLOR_RESET,
             t->name);
      continue;
    }

    printf(COLOR_GREY "Running test: %s...\n" COLOR_RESET, t->name);

    double start = get_time_ms();

    if (t->setup)
      t->setup(t);

    t->func(t);

    if (t->teardown)
      t->teardown(t);

    double end = get_time_ms();

    unsigned int expects_passed = t->expects_executed - t->expects_failed;
    if (t->passed) {
      tests_passed++;
      printf(COLOR_GREEN "  PASSED (ASSERTs: %d, EXPECTs: %d total, %d "
                         "passed)\n" COLOR_RESET,
             t->assertions_executed, t->expects_executed, expects_passed);
    } else {
      tests_failed++;
      printf(COLOR_RED "  FAILED (ASSERTs: %d, EXPECTs: %d total, %d "
                       "passed)\n" COLOR_RESET,
             t->assertions_executed, t->expects_executed, expects_passed);
    }
    double duration = end - start;
    if (duration < 1.0) {
      printf(COLOR_LIGHT_GREY "  Duration: %.2f µs\n\n" COLOR_RESET,
             duration * 1000);
    } else {
      printf(COLOR_LIGHT_GREY "  Duration: %.2f ms\n\n" COLOR_RESET,
             end - start);
    }
    tests_run++;
  }

  printf(COLOR_CYAN "\nTests run: %d, Passed: %d, Failed: %d\n", tests_run,
         tests_passed, tests_failed);
}

int main(int argc, char *argv[]) {
  const char *filter = (argc > 1) ? argv[1] : NULL;

  run_tests(filter);
  return 0;
}

#include "tests/test_runner.h"
#include "tests/test_core.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

#define COLOR_RESET "\x1b[0m"
#define COLOR_RED "\x1b[31m"
#define COLOR_GREEN "\x1b[32m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_CYAN "\x1b[36m"
#define COLOR_GREY "\x1b[38;5;250m"
#define COLOR_LIGHT_GREY "\033[38;2;190;225;255m"

static unsigned int total_tests = 0;
static unsigned int passed_tests = 0;
static unsigned int failed_tests = 0;
static unsigned int skipped_tests = 0;

static FILE *runner_output = NULL;

static double get_time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((double)ts.tv_sec * 1e6) + ((double)ts.tv_nsec / 1e3);
}

void test_runner_set_output(FILE *stream) { runner_output = stream; }

enum test_status test_run_case(test_suite *suite, test_case *tcase) {
  test_ctx ctx = {
      .tcase = tcase,
      .suite = suite,
      .priv = NULL,

      .assertions_executed = 0,
      .expects_executed = 0,
      .expects_failed = 0,

      .status = TEST_PASS,
      .log_size = TEST_LOG_SIZE,
      .log_offset = 0,
  };

  if (suite->init)
    suite->init(&ctx);

  double start = get_time_us();
  tcase->func(&ctx);
  double end = get_time_us();

  if (suite->exit)
    suite->exit(&ctx);

  double duration_us = end - start;

  total_tests++;
  unsigned int expects_passed = ctx.expects_executed - ctx.expects_failed;

  switch (ctx.status) {
  case TEST_PASS:
    passed_tests++;
    fprintf(runner_output,
            COLOR_GREEN
            "  PASSED" COLOR_RESET
            " (ASSERTs: %u, EXPECTs: %u total, %u passed, %.2f µs)\n",
            ctx.assertions_executed, ctx.expects_executed, expects_passed,
            duration_us);
    break;
  case TEST_FAIL:
    failed_tests++;
    fprintf(runner_output,
            COLOR_RED "  FAILED" COLOR_RESET
                      " (ASSERTs: %u, EXPECTs: %u total, %u passed, %.2f µs)\n",
            ctx.assertions_executed, ctx.expects_executed, expects_passed,
            duration_us);
    break;
  case TEST_SKIP:
    skipped_tests++;
    fprintf(runner_output, COLOR_YELLOW "  SKIPPED" COLOR_RESET " (%.2f µs)\n",
            duration_us);
    break;
  }

  if (ctx.log_offset > 0) {
    fprintf(runner_output, COLOR_LIGHT_GREY "  Log:\n" COLOR_RESET);

    for (size_t i = 0; i < ctx.log_offset; i++) {
      if (i == 0 || ctx.log[i - 1] == '\n')
        fputs("    ", runner_output);
      fputc(ctx.log[i], runner_output);
    }

    if (ctx.log[ctx.log_offset - 1] != '\n')
      fputc('\n', runner_output);
  }

  if (duration_us < 1000.0)
    fprintf(runner_output,
            COLOR_LIGHT_GREY "  Duration: %.2f µs\n\n" COLOR_RESET,
            duration_us);
  else
    fprintf(runner_output,
            COLOR_LIGHT_GREY "  Duration: %.2f ms\n\n" COLOR_RESET,
            duration_us / 1000.0);

  return ctx.status;
}

unsigned int test_run_suite(test_suite *suite, const char *filter) {
  if (!runner_output)
    runner_output = suite->output_stream ? suite->output_stream : stdout;

  fprintf(runner_output, COLOR_CYAN "--- Running suite: %s ---\n" COLOR_RESET,
          suite->name);

  if (suite->setup_suite) {
    suite->setup_suite();
  }

  for (test_case *c = suite->cases; c && c->name; ++c) {
    if (filter && !strstr(c->name, filter)) {
      fprintf(runner_output,
              COLOR_YELLOW "  Skipping test: %s (filtered)\n\n" COLOR_RESET,
              c->name);
      skipped_tests++;
      continue;
    }

    fprintf(runner_output, COLOR_GREY "Running test: %s...\n" COLOR_RESET,
            c->name);
    test_run_case(suite, c);
  }

  if (suite->teardown_suite)
    suite->teardown_suite();

  fprintf(runner_output, "\n");
  return failed_tests;
}

extern struct test_suite *__start_test_suites[];
extern struct test_suite *__stop_test_suites[];

unsigned int test_run_all(const char *filter) {
  if (!runner_output)
    runner_output = stdout;

  fprintf(runner_output,
          COLOR_CYAN "\n--- Starting Test Run ---\n\n" COLOR_RESET);

#if defined(__APPLE__)
  unsigned long size = 0;
  struct test_suite **suites = (struct test_suite **)getsectiondata(
      &_mh_execute_header, "__DATA", "test_suites", &size);

  if (!suites || size == 0) {
    fprintf(runner_output, COLOR_YELLOW "No test suites found.\n" COLOR_RESET);
    return -1;
  }

  size_t count = size / sizeof(*suites);
  for (size_t i = 0; i < count; i++) {
    test_run_suite(suites[i], filter);
  }
#else
  for (struct test_suite **suite = __start_test_suites;
       suite < __stop_test_suites; ++suite) {
    test_run_suite(*suite, filter);
  }
#endif

  fprintf(runner_output,
          COLOR_CYAN "\nTests run: %u, " COLOR_GREEN "Passed: %u" COLOR_RESET
                     ", " COLOR_RED "Failed: %u" COLOR_RESET ", " COLOR_YELLOW
                     "Skipped: %u\n" COLOR_RESET,
          total_tests, passed_tests, failed_tests, skipped_tests);

  return failed_tests ? -1 : 0;
}

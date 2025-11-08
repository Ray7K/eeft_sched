#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include "test_core.h"

enum test_status test_run_case(test_suite *suite, test_case *tcase);

unsigned int test_run_suite(test_suite *suite, const char *filter);

unsigned int test_run_all(const char *filter);

void test_runner_set_output(FILE *stream);
void test_runner_summary(void);

#endif

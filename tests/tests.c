#include "tests/test_runner.h"

int main(int argc, char *argv[]) {
  const char *filter = (argc > 1) ? argv[1] : NULL;
  test_run_all(filter);
  return 0;
}

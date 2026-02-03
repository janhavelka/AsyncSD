#include <unity.h>

// Tests are implemented in separate translation units.
void test_debounce();
void test_backoff();
void test_failure_hysteresis();
void test_stall_detector();
void test_shutdown_policy();
void test_copy_write_pool();
void test_dropped_results_counter();

void setUp() {}
void tearDown() {}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  UNITY_BEGIN();
  RUN_TEST(test_debounce);
  RUN_TEST(test_backoff);
  RUN_TEST(test_failure_hysteresis);
  RUN_TEST(test_stall_detector);
  RUN_TEST(test_shutdown_policy);
  RUN_TEST(test_copy_write_pool);
  RUN_TEST(test_dropped_results_counter);
  return UNITY_END();
}


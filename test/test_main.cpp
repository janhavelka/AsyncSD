#include <unity.h>

// Tests are implemented in separate translation units.
void test_debounce();
void test_backoff();
void test_failure_hysteresis();
void test_stall_detector();
void test_shutdown_policy();
void test_copy_write_pool();
void test_dropped_results_counter();
void test_rename_success();
void test_rename_missing_source_returns_not_found();
void test_rename_existing_destination_fail_mode();
void test_rename_existing_destination_replace_mode();
void test_rename_same_path_is_noop_success();
void test_rename_failure_maps_not_found_from_filesystem();
void test_rename_path_normalization_and_length_limits();
void test_rename_mountpoint_strip_allows_long_input_when_stripped_fits();
void test_rename_queue_full_returns_busy();
void test_rename_timeout_behavior();
void test_rename_large_timeout_not_immediate_timeout();
void test_rename_result_overflow_telemetry();
void test_rename_reinit_lifecycle();

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
  RUN_TEST(test_rename_success);
  RUN_TEST(test_rename_missing_source_returns_not_found);
  RUN_TEST(test_rename_existing_destination_fail_mode);
  RUN_TEST(test_rename_existing_destination_replace_mode);
  RUN_TEST(test_rename_same_path_is_noop_success);
  RUN_TEST(test_rename_failure_maps_not_found_from_filesystem);
  RUN_TEST(test_rename_path_normalization_and_length_limits);
  RUN_TEST(test_rename_mountpoint_strip_allows_long_input_when_stripped_fits);
  RUN_TEST(test_rename_queue_full_returns_busy);
  RUN_TEST(test_rename_timeout_behavior);
  RUN_TEST(test_rename_large_timeout_not_immediate_timeout);
  RUN_TEST(test_rename_result_overflow_telemetry);
  RUN_TEST(test_rename_reinit_lifecycle);
  return UNITY_END();
}

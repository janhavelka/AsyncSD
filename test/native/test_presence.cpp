#include <unity.h>

#include "Presence.h"

using AsyncSD::internal::Backoff;
using AsyncSD::internal::Debounce;
using AsyncSD::internal::FailureHysteresis;

void setUp() {}
void tearDown() {}

void test_debounce() {
  Debounce d;
  d.debounceMs = 50;
  d.reset(false, 0);

  TEST_ASSERT_FALSE(d.update(true, 10));
  TEST_ASSERT_FALSE(d.stable);
  TEST_ASSERT_FALSE(d.update(true, 30));
  TEST_ASSERT_FALSE(d.stable);
  TEST_ASSERT_TRUE(d.update(true, 60));
  TEST_ASSERT_TRUE(d.stable);
}

void test_backoff() {
  Backoff b;
  b.minMs = 100;
  b.maxMs = 800;
  b.reset(0);
  TEST_ASSERT_FALSE(b.shouldFire(50));
  TEST_ASSERT_TRUE(b.shouldFire(100));

  b.onFailure(100);
  TEST_ASSERT_EQUAL_UINT32(200, b.currentMs);
  TEST_ASSERT_EQUAL_UINT32(300, b.nextMs);

  b.onFailure(300);
  TEST_ASSERT_EQUAL_UINT32(400, b.currentMs);
  TEST_ASSERT_EQUAL_UINT32(700, b.nextMs);

  b.onFailure(700);
  TEST_ASSERT_EQUAL_UINT32(800, b.currentMs);
  TEST_ASSERT_EQUAL_UINT32(1500, b.nextMs);

  b.onSuccess(1500, 1000);
  TEST_ASSERT_EQUAL_UINT32(100, b.currentMs);
  TEST_ASSERT_EQUAL_UINT32(2500, b.nextMs);
}

void test_failure_hysteresis() {
  FailureHysteresis f;
  f.threshold = 3;
  f.reset();
  TEST_ASSERT_FALSE(f.recordFailure());
  TEST_ASSERT_FALSE(f.recordFailure());
  TEST_ASSERT_TRUE(f.recordFailure());
  f.recordSuccess();
  TEST_ASSERT_FALSE(f.recordFailure());
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_debounce);
  RUN_TEST(test_backoff);
  RUN_TEST(test_failure_hysteresis);
  return UNITY_END();
}

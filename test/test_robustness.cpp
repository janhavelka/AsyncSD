#include <unity.h>

#include "InternalLogic.h"

using AsyncSD::internal::CopyWritePool;
using AsyncSD::internal::CopyWriteSlot;
using AsyncSD::internal::incrementDroppedResults;
using AsyncSD::internal::shouldAbortShutdown;
using AsyncSD::internal::shouldStall;

void setUp() {}
void tearDown() {}

void test_stall_detector() {
  TEST_ASSERT_FALSE(shouldStall(1000, 0, 5000, true));
  TEST_ASSERT_FALSE(shouldStall(1000, 900, 0, true));
  TEST_ASSERT_FALSE(shouldStall(1000, 900, 200, false));
  TEST_ASSERT_TRUE(shouldStall(5000, 1000, 3000, true));
}

void test_shutdown_policy() {
  TEST_ASSERT_FALSE(shouldAbortShutdown(false, false));
  TEST_ASSERT_FALSE(shouldAbortShutdown(false, true));
  TEST_ASSERT_FALSE(shouldAbortShutdown(true, false));
  TEST_ASSERT_TRUE(shouldAbortShutdown(true, true));
}

void test_copy_write_pool() {
  CopyWriteSlot slots[2]{};
  uint8_t storage[2 * 8]{};
  CopyWritePool pool;
  pool.init(slots, storage, 2, 8);

  uint8_t data1[4] = {1, 2, 3, 4};
  uint8_t data2[4] = {5, 6, 7, 8};
  uint8_t idx = 0xFF;
  void* buf = nullptr;

  TEST_ASSERT_TRUE(pool.allocate(4, data1, &idx, &buf));
  TEST_ASSERT_EQUAL_UINT8(0, idx);
  TEST_ASSERT_NOT_NULL(buf);

  uint8_t idx2 = 0xFF;
  void* buf2 = nullptr;
  TEST_ASSERT_TRUE(pool.allocate(4, data2, &idx2, &buf2));
  TEST_ASSERT_EQUAL_UINT8(1, idx2);

  uint8_t idx3 = 0xFF;
  void* buf3 = nullptr;
  TEST_ASSERT_FALSE(pool.allocate(4, data2, &idx3, &buf3));

  pool.release(idx);
  TEST_ASSERT_TRUE(pool.allocate(4, data2, &idx3, &buf3));
  TEST_ASSERT_EQUAL_UINT8(0, idx3);

  uint8_t idx4 = 0xFF;
  void* buf4 = nullptr;
  TEST_ASSERT_FALSE(pool.allocate(16, data2, &idx4, &buf4));
}

void test_dropped_results_counter() {
  std::atomic<uint32_t> counter{0};
  TEST_ASSERT_EQUAL_UINT32(1, incrementDroppedResults(counter));
  TEST_ASSERT_EQUAL_UINT32(2, incrementDroppedResults(counter));
  TEST_ASSERT_EQUAL_UINT32(2, counter.load());
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_stall_detector);
  RUN_TEST(test_shutdown_policy);
  RUN_TEST(test_copy_write_pool);
  RUN_TEST(test_dropped_results_counter);
  return UNITY_END();
}


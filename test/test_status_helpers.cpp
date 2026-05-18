#include <unity.h>

#include "AsyncSD/Status.h"

void test_status_helpers() {
  TEST_ASSERT_TRUE(AsyncSD::ok(AsyncSD::ErrorCode::Ok));
  TEST_ASSERT_FALSE(AsyncSD::ok(AsyncSD::ErrorCode::Timeout));

  TEST_ASSERT_TRUE(AsyncSD::inProgress(AsyncSD::SdStatus::Initializing));
  TEST_ASSERT_TRUE(AsyncSD::inProgress(AsyncSD::SdStatus::Mounting));
  TEST_ASSERT_TRUE(AsyncSD::inProgress(AsyncSD::SdStatus::Busy));
  TEST_ASSERT_FALSE(AsyncSD::inProgress(AsyncSD::SdStatus::Ready));
  TEST_ASSERT_FALSE(AsyncSD::inProgress(AsyncSD::SdStatus::Fault));

  AsyncSD::RequestResult result{};
  TEST_ASSERT_TRUE(result.ok());
  result.code = AsyncSD::ErrorCode::IoError;
  TEST_ASSERT_FALSE(result.ok());

  AsyncSD::ErrorInfo info{};
  TEST_ASSERT_TRUE(info.ok());
  info.code = AsyncSD::ErrorCode::NotReady;
  TEST_ASSERT_FALSE(info.ok());
}

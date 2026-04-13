#include <unity.h>

#include "Arduino.h"
#include "AsyncSD/AsyncSD.h"
#include "Presence.h"
#include "SPI.h"
#include "SdFat.h"

using AsyncSD::internal::Backoff;
using AsyncSD::internal::Debounce;
using AsyncSD::internal::FailureHysteresis;

namespace {

AsyncSD::SdCardConfig makeConfig() {
  AsyncSD::SdCardConfig cfg;
  cfg.transport = AsyncSD::TransportType::Spi;
  cfg.spi = &SPI;
  cfg.pinCs = 10;
  cfg.pinMosi = 11;
  cfg.pinMiso = 12;
  cfg.pinSck = 13;
  cfg.cdPin = 21;
  cfg.autoInitSpi = false;
  cfg.autoMount = false;
  cfg.useWorkerTask = false;
  cfg.cdPollMs = 5;
  cfg.cdDebounceMs = 20;
  cfg.mountTimeoutMs = 50;
  cfg.opTimeoutMs = 50;
  cfg.ioTimeoutMs = 50;
  cfg.lockTimeoutMs = 10;
  cfg.workerStallMs = 0;
  cfg.requestQueueDepth = 8;
  cfg.resultQueueDepth = 8;
  cfg.maxOpenFiles = 4;
  cfg.maxPathLength = 64;
  return cfg;
}

bool runUntilResult(AsyncSD::SdCardManager& sd, AsyncSD::RequestId id,
                    AsyncSD::RequestResult* out, uint32_t maxSteps = 64) {
  if (id == AsyncSD::INVALID_REQUEST_ID || !out) {
    return false;
  }
  for (uint32_t i = 0; i < maxSteps; ++i) {
    sd.workerStep(2000);
    if (sd.getResult(id, out)) {
      return true;
    }
  }
  return false;
}

void requireMount(AsyncSD::SdCardManager& sd) {
  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id = sd.requestMount();
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));
}

}  // namespace

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

void test_cd_presence_snapshot_initial_state() {
  arduino_stub::reset();
  sdfat_stub::reset();
  arduino_stub::setDigitalReadValue(0);

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));

  const AsyncSD::PresenceInfo presence = sd.presenceInfo();
  TEST_ASSERT_TRUE(presence.cdConfigured);
  TEST_ASSERT_TRUE(presence.cdActiveLow);
  TEST_ASSERT_FALSE(presence.cdInterruptEnabled);
  TEST_ASSERT_FALSE(presence.cdRawLevelHigh);
  TEST_ASSERT_TRUE(presence.cardPresent);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::SdStatus::CardInserted),
                        static_cast<int>(sd.status()));

  sd.end();
}

void test_cd_presence_snapshot_updates_after_debounce() {
  arduino_stub::reset();
  sdfat_stub::reset();
  arduino_stub::setDigitalReadValue(1);

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));

  AsyncSD::PresenceInfo presence = sd.presenceInfo();
  TEST_ASSERT_TRUE(presence.cdConfigured);
  TEST_ASSERT_TRUE(presence.cdRawLevelHigh);
  TEST_ASSERT_FALSE(presence.cardPresent);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::SdStatus::NoCard),
                        static_cast<int>(sd.status()));

  arduino_stub::advanceMs(5);
  arduino_stub::setDigitalReadValue(0);
  sd.workerStep(2000);

  presence = sd.presenceInfo();
  TEST_ASSERT_FALSE(presence.cdRawLevelHigh);
  TEST_ASSERT_FALSE(presence.cardPresent);

  arduino_stub::advanceMs(20);
  sd.workerStep(2000);

  presence = sd.presenceInfo();
  TEST_ASSERT_FALSE(presence.cdRawLevelHigh);
  TEST_ASSERT_TRUE(presence.cardPresent);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::SdStatus::CardInserted),
                        static_cast<int>(sd.status()));

  sd.end();
}

void test_info_result_includes_presence_snapshot() {
  arduino_stub::reset();
  sdfat_stub::reset();
  arduino_stub::setDigitalReadValue(0);

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));
  requireMount(sd);

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id = sd.requestInfo();
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::RequestType::Info),
                        static_cast<int>(res.type));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));
  TEST_ASSERT_TRUE(res.presenceInfo.cdConfigured);
  TEST_ASSERT_FALSE(res.presenceInfo.cdRawLevelHigh);
  TEST_ASSERT_TRUE(res.presenceInfo.cardPresent);

  sd.end();
}

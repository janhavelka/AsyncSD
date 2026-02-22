/**
 * @file test_queue_safety.cpp
 * @brief Tests for result queue safety and misc robustness fixes.
 */

#include <unity.h>

#include <string.h>

#include "Arduino.h"
#include "AsyncSD/AsyncSD.h"
#include "SPI.h"
#include "SdFat.h"

namespace {

AsyncSD::SdCardConfig makeConfig() {
  AsyncSD::SdCardConfig cfg;
  cfg.transport = AsyncSD::TransportType::Spi;
  cfg.spi = &SPI;
  cfg.pinCs = 10;
  cfg.pinMosi = 11;
  cfg.pinMiso = 12;
  cfg.pinSck = 13;
  cfg.autoInitSpi = false;
  cfg.autoMount = false;
  cfg.useWorkerTask = false;
  cfg.mountTimeoutMs = 50;
  cfg.opTimeoutMs = 50;
  cfg.ioTimeoutMs = 50;
  cfg.lockTimeoutMs = 10;
  cfg.workerStallMs = 0;
  cfg.requestQueueDepth = 8;
  cfg.resultQueueDepth = 4;
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

void requireCreateFile(AsyncSD::SdCardManager& sd, const char* path) {
  AsyncSD::RequestResult openRes{};
  const AsyncSD::RequestId openId =
      sd.requestOpen(path,
                     AsyncSD::OpenMode::Write | AsyncSD::OpenMode::Create |
                         AsyncSD::OpenMode::Truncate);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, openId);
  TEST_ASSERT_TRUE(runUntilResult(sd, openId, &openRes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(openRes.code));
  TEST_ASSERT_NOT_EQUAL_UINT16(AsyncSD::INVALID_FILE_HANDLE, openRes.handle);

  AsyncSD::RequestResult closeRes{};
  const AsyncSD::RequestId closeId = sd.requestClose(openRes.handle);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, closeId);
  TEST_ASSERT_TRUE(runUntilResult(sd, closeId, &closeRes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(closeRes.code));
}

}  // namespace

/// @brief Verify that getResult (by ID) followed by new results doesn't lose data.
/// This is the regression test for the result-queue slot-overwrite bug.
void test_result_queue_getresult_no_overwrite() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.resultQueueDepth = 4;
  cfg.requestQueueDepth = 8;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);

  // Create 4 files
  requireCreateFile(sd, "/f0.txt");
  requireCreateFile(sd, "/f1.txt");
  requireCreateFile(sd, "/f2.txt");
  requireCreateFile(sd, "/f3.txt");

  // Enqueue 4 stat requests to fill the result queue
  AsyncSD::RequestId ids[4];
  ids[0] = sd.requestStat("/f0.txt");
  ids[1] = sd.requestStat("/f1.txt");
  ids[2] = sd.requestStat("/f2.txt");
  ids[3] = sd.requestStat("/f3.txt");
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, ids[i]);
  }

  // Process all 4 requests
  for (uint32_t step = 0; step < 16; ++step) {
    sd.workerStep(2000);
  }

  // Consume result #1 (middle of the queue) by ID, creating a hole
  AsyncSD::RequestResult mid{};
  TEST_ASSERT_TRUE(sd.getResult(ids[1], &mid));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(mid.code));

  // Now enqueue another stat request — this should fill the hole, not overwrite
  requireCreateFile(sd, "/f4.txt");
  AsyncSD::RequestId newId = sd.requestStat("/f4.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, newId);

  for (uint32_t step = 0; step < 4; ++step) {
    sd.workerStep(2000);
  }

  // All original results (except id[1] already consumed) should still be retrievable
  AsyncSD::RequestResult r0{}, r2{}, r3{}, rNew{};
  TEST_ASSERT_TRUE(sd.getResult(ids[0], &r0));
  TEST_ASSERT_EQUAL_UINT32(ids[0], r0.id);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(r0.code));

  TEST_ASSERT_TRUE(sd.getResult(ids[2], &r2));
  TEST_ASSERT_EQUAL_UINT32(ids[2], r2.id);

  TEST_ASSERT_TRUE(sd.getResult(ids[3], &r3));
  TEST_ASSERT_EQUAL_UINT32(ids[3], r3.id);

  TEST_ASSERT_TRUE(sd.getResult(newId, &rNew));
  TEST_ASSERT_EQUAL_UINT32(newId, rNew.id);

  TEST_ASSERT_EQUAL_UINT32(0, sd.getDroppedResults());

  sd.end();
}

/// @brief Verify popResult works correctly after getResult creates holes.
void test_result_queue_pop_after_get() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.resultQueueDepth = 4;
  cfg.requestQueueDepth = 8;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);

  requireCreateFile(sd, "/a.txt");
  requireCreateFile(sd, "/b.txt");
  requireCreateFile(sd, "/c.txt");

  AsyncSD::RequestId idA = sd.requestStat("/a.txt");
  AsyncSD::RequestId idB = sd.requestStat("/b.txt");
  AsyncSD::RequestId idC = sd.requestStat("/c.txt");

  for (uint32_t step = 0; step < 12; ++step) {
    sd.workerStep(2000);
  }

  // Consume middle result by ID
  AsyncSD::RequestResult mid{};
  TEST_ASSERT_TRUE(sd.getResult(idB, &mid));
  TEST_ASSERT_EQUAL_UINT32(idB, mid.id);

  // Pop remaining results — order may vary due to ring-buffer wrapping,
  // but both idA and idC must appear exactly once with no data loss.
  AsyncSD::RequestResult first{}, second{};
  TEST_ASSERT_TRUE(sd.popResult(&first));
  TEST_ASSERT_TRUE(sd.popResult(&second));
  TEST_ASSERT_TRUE((first.id == idA && second.id == idC) ||
                   (first.id == idC && second.id == idA));

  // Queue should be empty
  AsyncSD::RequestResult empty{};
  TEST_ASSERT_FALSE(sd.popResult(&empty));

  sd.end();
}

/// @brief Verify that request IDs are correct when returned from requestXxx().
void test_request_id_correctness() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));
  requireMount(sd);

  requireCreateFile(sd, "/id_test.txt");

  // Enqueue several different request types and verify IDs are unique and valid
  AsyncSD::RequestId id1 = sd.requestStat("/id_test.txt");
  AsyncSD::RequestId id2 = sd.requestStat("/id_test.txt");
  AsyncSD::RequestId id3 = sd.requestMkdir("/id_dir");

  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id1);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id2);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id3);
  TEST_ASSERT_NOT_EQUAL_UINT32(id1, id2);
  TEST_ASSERT_NOT_EQUAL_UINT32(id2, id3);
  TEST_ASSERT_NOT_EQUAL_UINT32(id1, id3);

  // Process and verify each result matches its request ID
  for (uint32_t step = 0; step < 16; ++step) {
    sd.workerStep(2000);
  }

  AsyncSD::RequestResult r1{}, r2{}, r3{};
  TEST_ASSERT_TRUE(sd.getResult(id1, &r1));
  TEST_ASSERT_EQUAL_UINT32(id1, r1.id);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::RequestType::Stat),
                        static_cast<int>(r1.type));

  TEST_ASSERT_TRUE(sd.getResult(id2, &r2));
  TEST_ASSERT_EQUAL_UINT32(id2, r2.id);

  TEST_ASSERT_TRUE(sd.getResult(id3, &r3));
  TEST_ASSERT_EQUAL_UINT32(id3, r3.id);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::RequestType::Mkdir),
                        static_cast<int>(r3.type));

  sd.end();
}

/// @brief Verify ListDir respects the deadline and doesn't loop unboundedly.
void test_listdir_bounded() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.opTimeoutMs = 5;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);

  // Create some files
  for (int i = 0; i < 8; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "/file%d.txt", i);
    requireCreateFile(sd, name);
  }

  AsyncSD::DirEntry entries[16]{};
  AsyncSD::RequestId id = sd.requestListDir("/", entries, 16);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);

  // Even with a tight timeout, we should get a result (possibly Timeout, possibly Ok)
  AsyncSD::RequestResult res{};
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  // Either Ok (completed in time) or Timeout (deadline hit)
  TEST_ASSERT_TRUE(res.code == AsyncSD::ErrorCode::Ok ||
                   res.code == AsyncSD::ErrorCode::Timeout);

  sd.end();
}

#include <unity.h>

#include <errno.h>
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

void test_rename_success() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  const AsyncSD::SdCardConfig cfg = makeConfig();
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);
  requireCreateFile(sd, "/a.txt");

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id = sd.requestRename("/a.txt", "/b.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::RequestType::Rename),
                        static_cast<int>(res.type));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));
  TEST_ASSERT_FALSE(sdfat_stub::pathExists("/a.txt"));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/b.txt"));

  sd.end();
}

void test_rename_missing_source_returns_not_found() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));
  requireMount(sd);

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id = sd.requestRename("/missing.txt", "/b.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::NotFound),
                        static_cast<int>(res.code));

  sd.end();
}

void test_rename_existing_destination_fail_mode() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));
  requireMount(sd);
  requireCreateFile(sd, "/a.txt");
  requireCreateFile(sd, "/b.txt");

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id =
      sd.requestRename("/a.txt", "/b.txt", AsyncSD::RenameMode::FailIfExists);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::AlreadyExists),
                        static_cast<int>(res.code));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/a.txt"));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/b.txt"));

  sd.end();
}

void test_rename_existing_destination_replace_mode() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));
  requireMount(sd);
  requireCreateFile(sd, "/a.txt");
  requireCreateFile(sd, "/b.txt");

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id =
      sd.requestRename("/a.txt", "/b.txt", AsyncSD::RenameMode::ReplaceIfExists);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));
  TEST_ASSERT_FALSE(sdfat_stub::pathExists("/a.txt"));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/b.txt"));

  sd.end();
}

void test_rename_same_path_is_noop_success() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));
  requireMount(sd);
  requireCreateFile(sd, "/same.txt");

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id =
      sd.requestRename("/same.txt", "/same.txt", AsyncSD::RenameMode::FailIfExists);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/same.txt"));

  sd.end();
}

void test_rename_failure_maps_not_found_from_filesystem() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(makeConfig()));
  requireMount(sd);
  requireCreateFile(sd, "/a.txt");

  sdfat_stub::setForcedRenameErrno(ENOENT);
  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id = sd.requestRename("/a.txt", "/b.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::NotFound),
                        static_cast<int>(res.code));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/a.txt"));
  TEST_ASSERT_FALSE(sdfat_stub::pathExists("/b.txt"));

  sd.end();
}

void test_rename_path_normalization_and_length_limits() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.mountPoint = "/sd";
  cfg.maxPathLength = 24;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);

  AsyncSD::RequestResult mkdirRes{};
  const AsyncSD::RequestId mkdirId = sd.requestMkdir("/sd/dir");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, mkdirId);
  TEST_ASSERT_TRUE(runUntilResult(sd, mkdirId, &mkdirRes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(mkdirRes.code));

  requireCreateFile(sd, "/sd/dir/a.txt");

  AsyncSD::RequestResult renameRes{};
  const AsyncSD::RequestId renameId =
      sd.requestRename("/sd/dir/a.txt", "/sd/dir/b.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, renameId);
  TEST_ASSERT_TRUE(runUntilResult(sd, renameId, &renameRes));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(renameRes.code));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/dir/b.txt"));

  char tooLong[40];
  tooLong[0] = '/';
  for (size_t i = 1; i + 1 < sizeof(tooLong); ++i) {
    tooLong[i] = 'x';
  }
  tooLong[sizeof(tooLong) - 1] = '\0';

  const AsyncSD::RequestId tooLongId = sd.requestRename("/sd/dir/b.txt", tooLong);
  TEST_ASSERT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, tooLongId);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::PathTooLong),
                        static_cast<int>(sd.lastError()));

  sd.end();
}

void test_rename_mountpoint_strip_allows_long_input_when_stripped_fits() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.mountPoint = "/sd";
  cfg.maxPathLength = 16;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);

  // Full input exceeds maxPathLength, but stripped path fits.
  requireCreateFile(sd, "/sd/abcdefghijkl");

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id =
      sd.requestRename("/sd/abcdefghijkl", "/sd/mnopqrstuvwx");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/mnopqrstuvwx"));

  sd.end();
}

void test_rename_queue_full_returns_busy() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.requestQueueDepth = 1;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);
  requireCreateFile(sd, "/a.txt");

  const AsyncSD::RequestId id1 = sd.requestRename("/a.txt", "/b.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id1);

  const AsyncSD::RequestId id2 = sd.requestRename("/a.txt", "/c.txt");
  TEST_ASSERT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id2);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Busy),
                        static_cast<int>(sd.lastError()));

  AsyncSD::RequestResult res{};
  TEST_ASSERT_TRUE(runUntilResult(sd, id1, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));

  sd.end();
}

void test_rename_timeout_behavior() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.opTimeoutMs = 5;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);
  requireCreateFile(sd, "/a.txt");

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id = sd.requestRename("/a.txt", "/b.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);

  arduino_stub::advanceMs(10);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Timeout),
                        static_cast<int>(res.code));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/a.txt"));
  TEST_ASSERT_FALSE(sdfat_stub::pathExists("/b.txt"));

  sd.end();
}

void test_rename_large_timeout_not_immediate_timeout() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.opTimeoutMs = UINT32_MAX;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);
  requireCreateFile(sd, "/a.txt");

  AsyncSD::RequestResult res{};
  const AsyncSD::RequestId id = sd.requestRename("/a.txt", "/b.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
  TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                        static_cast<int>(res.code));
  TEST_ASSERT_TRUE(sdfat_stub::pathExists("/b.txt"));

  sd.end();
}

void test_rename_result_overflow_telemetry() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardConfig cfg = makeConfig();
  cfg.requestQueueDepth = 4;
  cfg.resultQueueDepth = 1;

  AsyncSD::SdCardManager sd;
  TEST_ASSERT_TRUE(sd.begin(cfg));
  requireMount(sd);
  requireCreateFile(sd, "/a0.txt");
  requireCreateFile(sd, "/a1.txt");
  requireCreateFile(sd, "/a2.txt");

  const AsyncSD::RequestId id0 = sd.requestRename("/a0.txt", "/b0.txt");
  const AsyncSD::RequestId id1 = sd.requestRename("/a1.txt", "/b1.txt");
  const AsyncSD::RequestId id2 = sd.requestRename("/a2.txt", "/b2.txt");
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id0);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id1);
  TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id2);

  for (uint32_t i = 0; i < 16; ++i) {
    sd.workerStep(2000);
  }

  TEST_ASSERT_EQUAL_UINT32(2, sd.getDroppedResults());
  const AsyncSD::ErrorInfo info = sd.lastErrorInfo();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::Operation::ResultEnqueue),
                        static_cast<int>(info.op));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Busy),
                        static_cast<int>(info.code));

  sd.end();
}

void test_rename_reinit_lifecycle() {
  arduino_stub::reset();
  sdfat_stub::reset();

  AsyncSD::SdCardManager sd;
  const AsyncSD::SdCardConfig cfg = makeConfig();

  for (uint8_t cycle = 0; cycle < 2; ++cycle) {
    TEST_ASSERT_TRUE(sd.begin(cfg));
    requireMount(sd);

    const char* src = (cycle == 0) ? "/cycle0.txt" : "/cycle1.txt";
    const char* dst = (cycle == 0) ? "/cycle0_done.txt" : "/cycle1_done.txt";

    requireCreateFile(sd, src);
    AsyncSD::RequestResult res{};
    const AsyncSD::RequestId id = sd.requestRename(src, dst);
    TEST_ASSERT_NOT_EQUAL_UINT32(AsyncSD::INVALID_REQUEST_ID, id);
    TEST_ASSERT_TRUE(runUntilResult(sd, id, &res));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AsyncSD::ErrorCode::Ok),
                          static_cast<int>(res.code));
    TEST_ASSERT_TRUE(sdfat_stub::pathExists(dst));

    sd.end();
  }
}

/**
 * @file main.cpp
 * @brief SPI CLI control example for AsyncSD.
 *
 * Commands:
 *   help
 *   status
 *   health
 *   info
 *   mount
 *   unmount
 *   open <path> <mode>
 *   close <handle>
 *   read <handle> <offset> <len>
 *   write <handle> <offset|append> <data>
 *   writecopy <handle> <offset|append> <data>
 *   sync <handle>
 *   mkdir <path>
 *   rm <path>
 *   stat <path>
 *   stress <count>
 */

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "AsyncSD/AsyncSD.h"
#include "AsyncSD/Version.h"
#include "examples/common/BoardPins.h"
#include "examples/common/Log.h"

static AsyncSD::SdCardManager g_sd;

struct PendingBuffer {
  AsyncSD::RequestId id = AsyncSD::INVALID_REQUEST_ID;
  uint8_t data[256]{};
  uint32_t len = 0;
  bool inUse = false;
  bool isRead = false;
};

static PendingBuffer g_buffers[4];

enum class StressStage : uint8_t { Idle, Opening, Writing, Syncing, Closing };

struct StressState {
  StressStage stage = StressStage::Idle;
  bool active = false;
  uint32_t remaining = 0;
  uint32_t counter = 0;
  AsyncSD::FileHandle handle = AsyncSD::INVALID_FILE_HANDLE;
  AsyncSD::RequestId pendingId = AsyncSD::INVALID_REQUEST_ID;
  char buffer[64]{};
};

static StressState g_stress;

static const char* statusToStr(AsyncSD::SdStatus st) {
  switch (st) {
    case AsyncSD::SdStatus::Disabled:
      return "Disabled";
    case AsyncSD::SdStatus::NoCard:
      return "NoCard";
    case AsyncSD::SdStatus::CardInserted:
      return "CardInserted";
    case AsyncSD::SdStatus::Initializing:
      return "Initializing";
    case AsyncSD::SdStatus::Mounting:
      return "Mounting";
    case AsyncSD::SdStatus::Ready:
      return "Ready";
    case AsyncSD::SdStatus::Busy:
      return "Busy";
    case AsyncSD::SdStatus::Removed:
      return "Removed";
    case AsyncSD::SdStatus::Error:
      return "Error";
    case AsyncSD::SdStatus::Fault:
      return "Fault";
    default:
      return "Unknown";
  }
}

static const char* fsTypeToStr(AsyncSD::FsType fs) {
  switch (fs) {
    case AsyncSD::FsType::Fat12:
      return "FAT12";
    case AsyncSD::FsType::Fat16:
      return "FAT16";
    case AsyncSD::FsType::Fat32:
      return "FAT32";
    case AsyncSD::FsType::ExFat:
      return "exFAT";
    default:
      return "Unknown";
  }
}

static const char* cardTypeToStr(AsyncSD::CardType type) {
  switch (type) {
    case AsyncSD::CardType::Sd1:
      return "SD1";
    case AsyncSD::CardType::Sd2:
      return "SD2";
    case AsyncSD::CardType::SdHC:
      return "SDHC/SDXC";
    default:
      return "Unknown";
  }
}

static void printHex(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (data[i] < 16) {
      Serial.print('0');
    }
    Serial.print(data[i], HEX);
  }
}

static PendingBuffer* allocBuffer(bool isRead) {
  for (auto& buf : g_buffers) {
    if (!buf.inUse) {
      buf.inUse = true;
      buf.isRead = isRead;
      buf.id = AsyncSD::INVALID_REQUEST_ID;
      buf.len = 0;
      return &buf;
    }
  }
  return nullptr;
}

static PendingBuffer* findBuffer(AsyncSD::RequestId id) {
  for (auto& buf : g_buffers) {
    if (buf.inUse && buf.id == id) {
      return &buf;
    }
  }
  return nullptr;
}

static void releaseBuffer(AsyncSD::RequestId id) {
  PendingBuffer* buf = findBuffer(id);
  if (buf) {
    buf->inUse = false;
  }
}

static AsyncSD::OpenMode parseMode(const char* modeStr) {
  if (!modeStr || modeStr[0] == '\0') {
    return AsyncSD::OpenMode::Read;
  }
  AsyncSD::OpenMode mode = AsyncSD::OpenMode::None;
  if (strchr(modeStr, 'r')) {
    mode = mode | AsyncSD::OpenMode::Read;
  }
  if (strchr(modeStr, 'w')) {
    mode = mode | AsyncSD::OpenMode::Write;
  }
  if (strchr(modeStr, 'a')) {
    mode = mode | AsyncSD::OpenMode::Append;
  }
  if (strchr(modeStr, 'c')) {
    mode = mode | AsyncSD::OpenMode::Create;
  }
  if (strchr(modeStr, 't')) {
    mode = mode | AsyncSD::OpenMode::Truncate;
  }
  if (strchr(modeStr, 'x')) {
    mode = mode | AsyncSD::OpenMode::Exclusive;
  }
  if (mode == AsyncSD::OpenMode::None) {
    mode = AsyncSD::OpenMode::Read;
  }
  return mode;
}

static void printHelp() {
  Serial.println();
  Serial.println(F("=== AsyncSD CLI ==="));
  Serial.print(F("Version: "));
  Serial.println(AsyncSD::VERSION);
  Serial.print(F("Built:   "));
  Serial.println(AsyncSD::BUILD_TIMESTAMP);
  Serial.println(F("Commands:"));
  Serial.println(F("  help"));
  Serial.println(F("  status"));
  Serial.println(F("  health"));
  Serial.println(F("  info"));
  Serial.println(F("  mount"));
  Serial.println(F("  unmount"));
  Serial.println(F("  open <path> <mode>"));
  Serial.println(F("  close <handle>"));
  Serial.println(F("  read <handle> <offset> <len>"));
  Serial.println(F("  write <handle> <offset|append> <data>"));
  Serial.println(F("  writecopy <handle> <offset|append> <data>"));
  Serial.println(F("  sync <handle>"));
  Serial.println(F("  mkdir <path>"));
  Serial.println(F("  rm <path>"));
  Serial.println(F("  stat <path>"));
  Serial.println(F("  stress <count>"));
  Serial.println(F("Mode flags: r w a c t x (read/write/append/create/truncate/exclusive)"));
  Serial.println();
}

static void printStatus() {
  const AsyncSD::SdStatus st = g_sd.status();
  const AsyncSD::FsInfo fs = g_sd.fsInfo();
  Serial.print(F("Status: "));
  Serial.println(statusToStr(st));
  Serial.print(F("FS: "));
  Serial.println(fsTypeToStr(fs.fsType));
  Serial.printf("Capacity bytes: %llu\n",
                static_cast<unsigned long long>(fs.capacityBytes));
  if (fs.usedBytesValid) {
    Serial.printf("Used bytes: %llu\n",
                  static_cast<unsigned long long>(fs.usedBytes));
  }
  if (fs.freeBytesValid) {
    Serial.printf("Free bytes: %llu\n",
                  static_cast<unsigned long long>(fs.freeBytes));
  }
  if (fs.usedBytesValid && fs.capacityBytes > 0) {
    const uint32_t pct =
        static_cast<uint32_t>((fs.usedBytes * 100ULL) / fs.capacityBytes);
    Serial.printf("Used percent: %lu%%\n", static_cast<unsigned long>(pct));
  }
}

static void printHealth() {
  const AsyncSD::ErrorInfo info = g_sd.lastErrorInfo();
  const AsyncSD::WorkerHealth health = g_sd.getWorkerHealth();
  Serial.print(F("Last error code: "));
  Serial.println(static_cast<int>(info.code));
  Serial.print(F("Op: "));
  Serial.println(static_cast<int>(info.op));
  Serial.print(F("Detail: "));
  Serial.println(info.detail);
  Serial.print(F("Timestamp: "));
  Serial.println(info.timestampMs);
  if (info.path) {
    Serial.print(F("Path: "));
    Serial.println(info.path);
  }
  Serial.print(F("Last progress ms: "));
  Serial.println(health.lastProgressMs);
  Serial.print(F("Last IO ms: "));
  Serial.println(health.lastSuccessfulIoMs);
  Serial.print(F("Consecutive failures: "));
  Serial.println(health.consecutiveFailures);
  Serial.print(F("Req queue depth: "));
  Serial.println(health.queueDepthRequests);
  Serial.print(F("Res queue depth: "));
  Serial.println(health.queueDepthResults);
  Serial.print(F("Stall events: "));
  Serial.println(health.stallEvents);
  Serial.print(F("Dropped results: "));
  Serial.println(g_sd.getDroppedResults());
}

static void printInfo(const AsyncSD::FsInfo& fs, const AsyncSD::CardInfo& card) {
  Serial.println(F("=== Card Info ==="));
  Serial.print(F("Type: "));
  Serial.println(cardTypeToStr(card.type));
  Serial.printf("Card capacity bytes: %llu\n",
                static_cast<unsigned long long>(card.capacityBytes));
  if (card.cardStatusValid) {
    Serial.print(F("Card status: 0x"));
    Serial.println(card.cardStatus, HEX);
  }
  if (card.ocrValid) {
    Serial.print(F("OCR: 0x"));
    Serial.println(card.ocr, HEX);
  }
  if (card.cidValid) {
    Serial.print(F("CID: "));
    printHex(card.cid, sizeof(card.cid));
    Serial.println();
  }
  if (card.csdValid) {
    Serial.print(F("CSD: "));
    printHex(card.csd, sizeof(card.csd));
    Serial.println();
  }
  if (card.scrValid) {
    Serial.print(F("SCR: "));
    printHex(card.scr, sizeof(card.scr));
    Serial.println();
  }
  if (card.sdsValid) {
    Serial.print(F("SDS: "));
    printHex(card.sds, sizeof(card.sds));
    Serial.println();
  }

  Serial.println(F("=== Filesystem Info ==="));
  Serial.print(F("FS type: "));
  Serial.println(fsTypeToStr(fs.fsType));
  Serial.printf("FS capacity bytes: %llu\n",
                static_cast<unsigned long long>(fs.capacityBytes));
  Serial.printf("Cluster count: %lu\n", static_cast<unsigned long>(fs.clusterCount));
  Serial.printf("Sectors/cluster: %lu\n", static_cast<unsigned long>(fs.sectorsPerCluster));
  Serial.printf("Bytes/cluster: %lu\n", static_cast<unsigned long>(fs.bytesPerCluster));
  if (fs.freeClustersValid) {
    Serial.printf("Free clusters: %lu\n",
                  static_cast<unsigned long>(fs.freeClusters));
  }
  if (fs.usedBytesValid) {
    Serial.printf("Used bytes: %llu\n",
                  static_cast<unsigned long long>(fs.usedBytes));
  }
  if (fs.freeBytesValid) {
    Serial.printf("Free bytes: %llu\n",
                  static_cast<unsigned long long>(fs.freeBytes));
  }
  if (fs.usedBytesValid && fs.capacityBytes > 0) {
    const uint32_t pct =
        static_cast<uint32_t>((fs.usedBytes * 100ULL) / fs.capacityBytes);
    Serial.printf("Used percent: %lu%%\n", static_cast<unsigned long>(pct));
  }
}

static bool parseU32(const char* s, uint32_t* out) {
  if (!s || !out) {
    return false;
  }
  char* end = nullptr;
  const unsigned long v = strtoul(s, &end, 10);
  if (end == s) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

static void startStress(uint32_t count) {
  if (g_stress.active) {
    LOGE("Stress already running");
    return;
  }
  g_stress.active = true;
  g_stress.stage = StressStage::Opening;
  g_stress.remaining = count;
  g_stress.counter = 0;
  g_stress.handle = AsyncSD::INVALID_FILE_HANDLE;
  g_stress.pendingId = g_sd.requestOpen("/stress.txt",
                                        AsyncSD::OpenMode::Write |
                                            AsyncSD::OpenMode::Create |
                                            AsyncSD::OpenMode::Append);
  LOGI("Stress started, request id=%lu", static_cast<unsigned long>(g_stress.pendingId));
}

static void enqueueStressWrite() {
  const int n = snprintf(g_stress.buffer, sizeof(g_stress.buffer),
                         "stress line %lu\n", static_cast<unsigned long>(g_stress.counter++));
  if (n <= 0) {
    g_stress.active = false;
    return;
  }
  g_stress.pendingId =
      g_sd.requestWrite(g_stress.handle, AsyncSD::APPEND_OFFSET,
                        g_stress.buffer, static_cast<size_t>(n));
}

static void handleStressResult(const AsyncSD::RequestResult& res) {
  if (!g_stress.active || res.id != g_stress.pendingId) {
    return;
  }
  if (res.code != AsyncSD::ErrorCode::Ok) {
    LOGE("Stress failed, code=%d", static_cast<int>(res.code));
    g_stress.active = false;
    g_stress.stage = StressStage::Idle;
    return;
  }

  switch (g_stress.stage) {
    case StressStage::Opening:
      g_stress.handle = res.handle;
      g_stress.stage = StressStage::Writing;
      enqueueStressWrite();
      break;
    case StressStage::Writing:
      if (g_stress.remaining > 0) {
        g_stress.remaining--;
      }
      if (g_stress.remaining == 0) {
        g_stress.stage = StressStage::Syncing;
        g_stress.pendingId = g_sd.requestSync(g_stress.handle);
      } else {
        enqueueStressWrite();
      }
      break;
    case StressStage::Syncing:
      g_stress.stage = StressStage::Closing;
      g_stress.pendingId = g_sd.requestClose(g_stress.handle);
      break;
    case StressStage::Closing:
      g_stress.active = false;
      g_stress.stage = StressStage::Idle;
      LOGI("Stress completed");
      break;
    default:
      break;
  }
}

static void handleResult(const AsyncSD::RequestResult& res) {
  handleStressResult(res);

  PendingBuffer* buf = findBuffer(res.id);
  if (buf) {
    if (buf->isRead) {
      Serial.print(F("Read bytes: "));
      Serial.println(res.bytesProcessed);
      Serial.write(buf->data, res.bytesProcessed);
      Serial.println();
    } else {
      Serial.print(F("Write bytes: "));
      Serial.println(res.bytesProcessed);
    }
    releaseBuffer(res.id);
  }

  Serial.print(F("Result id="));
  Serial.print(res.id);
  Serial.print(F(" type="));
  Serial.print(static_cast<int>(res.type));
  Serial.print(F(" code="));
  Serial.print(static_cast<int>(res.code));
  if (res.type == AsyncSD::RequestType::Open) {
    Serial.print(F(" handle="));
    Serial.print(res.handle);
  }
  if (res.type == AsyncSD::RequestType::Info) {
    Serial.println();
    printInfo(res.fsInfo, res.cardInfo);
  }
  Serial.println();
}

static void processLine(char* line) {
  char* cmd = strtok(line, " ");
  if (!cmd) {
    return;
  }

  if (strcmp(cmd, "help") == 0) {
    printHelp();
  } else if (strcmp(cmd, "status") == 0) {
    printStatus();
  } else if (strcmp(cmd, "health") == 0) {
    printHealth();
  } else if (strcmp(cmd, "info") == 0) {
    g_sd.requestInfo();
  } else if (strcmp(cmd, "mount") == 0) {
    g_sd.requestMount();
  } else if (strcmp(cmd, "unmount") == 0) {
    g_sd.requestUnmount();
  } else if (strcmp(cmd, "open") == 0) {
    char* path = strtok(nullptr, " ");
    char* mode = strtok(nullptr, " ");
    g_sd.requestOpen(path, parseMode(mode));
  } else if (strcmp(cmd, "close") == 0) {
    char* arg = strtok(nullptr, " ");
    uint32_t handle = 0;
    if (parseU32(arg, &handle)) {
      g_sd.requestClose(static_cast<AsyncSD::FileHandle>(handle));
    }
  } else if (strcmp(cmd, "read") == 0) {
    char* h = strtok(nullptr, " ");
    char* off = strtok(nullptr, " ");
    char* len = strtok(nullptr, " ");
    uint32_t handle = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    if (parseU32(h, &handle) && parseU32(off, &offset) && parseU32(len, &length)) {
      PendingBuffer* buf = allocBuffer(true);
      if (!buf) {
        LOGE("No free buffer");
        return;
      }
      if (length > sizeof(buf->data)) {
        length = sizeof(buf->data);
      }
      const AsyncSD::RequestId id =
          g_sd.requestRead(static_cast<AsyncSD::FileHandle>(handle), offset,
                           buf->data, length);
      buf->id = id;
      buf->len = length;
    }
  } else if (strcmp(cmd, "write") == 0) {
    char* h = strtok(nullptr, " ");
    char* off = strtok(nullptr, " ");
    char* data = strtok(nullptr, "");
    uint32_t handle = 0;
    if (!parseU32(h, &handle) || !off || !data) {
      return;
    }
    uint64_t offset = 0;
    if (strcmp(off, "append") == 0) {
      offset = AsyncSD::APPEND_OFFSET;
    } else {
      uint32_t temp = 0;
      if (!parseU32(off, &temp)) {
        return;
      }
      offset = temp;
    }
    PendingBuffer* buf = allocBuffer(false);
    if (!buf) {
      LOGE("No free buffer");
      return;
    }
    size_t len = strnlen(data, sizeof(buf->data));
    memcpy(buf->data, data, len);
    buf->len = static_cast<uint32_t>(len);
    const AsyncSD::RequestId id =
        g_sd.requestWrite(static_cast<AsyncSD::FileHandle>(handle), offset,
                          buf->data, len);
    buf->id = id;
  } else if (strcmp(cmd, "writecopy") == 0) {
    char* h = strtok(nullptr, " ");
    char* off = strtok(nullptr, " ");
    char* data = strtok(nullptr, "");
    uint32_t handle = 0;
    if (!parseU32(h, &handle) || !off || !data) {
      return;
    }
    uint64_t offset = 0;
    if (strcmp(off, "append") == 0) {
      offset = AsyncSD::APPEND_OFFSET;
    } else {
      uint32_t temp = 0;
      if (!parseU32(off, &temp)) {
        return;
      }
      offset = temp;
    }
    const size_t len = strnlen(data, 256);
    g_sd.requestWriteCopy(static_cast<AsyncSD::FileHandle>(handle), offset,
                          data, len);
  } else if (strcmp(cmd, "sync") == 0) {
    char* h = strtok(nullptr, " ");
    uint32_t handle = 0;
    if (parseU32(h, &handle)) {
      g_sd.requestSync(static_cast<AsyncSD::FileHandle>(handle));
    }
  } else if (strcmp(cmd, "mkdir") == 0) {
    char* path = strtok(nullptr, "");
    g_sd.requestMkdir(path);
  } else if (strcmp(cmd, "rm") == 0) {
    char* path = strtok(nullptr, "");
    g_sd.requestRemove(path);
  } else if (strcmp(cmd, "stat") == 0) {
    char* path = strtok(nullptr, "");
    g_sd.requestStat(path);
  } else if (strcmp(cmd, "stress") == 0) {
    char* cnt = strtok(nullptr, " ");
    uint32_t count = 0;
    if (parseU32(cnt, &count) && count > 0) {
      startStress(count);
    }
  } else {
    LOGE("Unknown command");
  }
}

void setup() {
  log_begin(115200);
  delay(100);

  AsyncSD::SdCardConfig cfg;
  cfg.pinCs = pins::SD_CS;
  cfg.pinMosi = pins::SPI_MOSI;
  cfg.pinMiso = pins::SPI_MISO;
  cfg.pinSck = pins::SPI_SCK;
  cfg.cdPin = pins::SD_CD;
  cfg.autoMount = true;
  cfg.useWorkerTask = true;

  if (!g_sd.begin(cfg)) {
    LOGE("AsyncSD begin failed");
  }

  printHelp();
  Serial.println(F("Ready."));
}

void loop() {
  g_sd.poll();

  AsyncSD::RequestResult res{};
  while (g_sd.popResult(&res)) {
    handleResult(res);
  }

  static char lineBuf[128]{};
  static size_t lineLen = 0;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      processLine(lineBuf);
      lineLen = 0;
      continue;
    }
    if (lineLen + 1 < sizeof(lineBuf)) {
      lineBuf[lineLen++] = c;
    }
  }
}

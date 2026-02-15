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
 *   rename <from> <to> [replace]
 *   stat <path>
 *   ls [path]
 *   touch <path>
 *   cat <path> [maxlen]
 *   stress <count>
 */

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AsyncSD/AsyncSD.h"
#include "AsyncSD/Version.h"
#include "examples/common/BoardPins.h"
#include "examples/common/Log.h"

static AsyncSD::SdCardManager g_sd;

// --- ListDir support ---
static constexpr uint16_t LS_MAX_ENTRIES = 64;
static AsyncSD::DirEntry g_dirEntries[LS_MAX_ENTRIES];
static AsyncSD::RequestId g_lsRequestId = AsyncSD::INVALID_REQUEST_ID;

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

// --- Cat support ---
enum class CatStage : uint8_t { Idle, Opening, Reading, Closing };

struct CatState {
  CatStage stage = CatStage::Idle;
  bool active = false;
  AsyncSD::FileHandle handle = AsyncSD::INVALID_FILE_HANDLE;
  AsyncSD::RequestId pendingId = AsyncSD::INVALID_REQUEST_ID;
  uint8_t buffer[256]{};
  uint32_t maxBytes = 512;
  uint32_t totalRead = 0;
};

static CatState g_cat;

static const char* errorCodeToStr(AsyncSD::ErrorCode c) {
  switch (c) {
    case AsyncSD::ErrorCode::Ok:              return "Ok";
    case AsyncSD::ErrorCode::Timeout:         return "Timeout";
    case AsyncSD::ErrorCode::Busy:            return "Busy";
    case AsyncSD::ErrorCode::NoMem:           return "NoMem";
    case AsyncSD::ErrorCode::Fault:           return "Fault";
    case AsyncSD::ErrorCode::BusNotAvailable: return "BusNotAvailable";
    case AsyncSD::ErrorCode::CardInitFailed:  return "CardInitFailed";
    case AsyncSD::ErrorCode::MountFailed:     return "MountFailed";
    case AsyncSD::ErrorCode::FsUnsupported:   return "FsUnsupported";
    case AsyncSD::ErrorCode::FsCorrupt:       return "FsCorrupt";
    case AsyncSD::ErrorCode::IoError:         return "IoError";
    case AsyncSD::ErrorCode::ReadOnly:        return "ReadOnly";
    case AsyncSD::ErrorCode::WriteProtect:    return "WriteProtect";
    case AsyncSD::ErrorCode::NoSpaceLeft:     return "NoSpaceLeft";
    case AsyncSD::ErrorCode::FileTooLarge:    return "FileTooLarge";
    case AsyncSD::ErrorCode::TooManyOpenFiles:return "TooManyOpenFiles";
    case AsyncSD::ErrorCode::PathTooLong:     return "PathTooLong";
    case AsyncSD::ErrorCode::NameTooLong:     return "NameTooLong";
    case AsyncSD::ErrorCode::InvalidArgument: return "InvalidArgument";
    case AsyncSD::ErrorCode::InvalidContext:  return "InvalidContext";
    case AsyncSD::ErrorCode::NotReady:        return "NotReady";
    case AsyncSD::ErrorCode::NotInitialized:  return "NotInitialized";
    case AsyncSD::ErrorCode::Unsupported:     return "Unsupported";
    case AsyncSD::ErrorCode::InternalError:   return "InternalError";
    case AsyncSD::ErrorCode::NotFound:        return "NotFound";
    case AsyncSD::ErrorCode::AlreadyExists:   return "AlreadyExists";
    default:                                  return "Unknown";
  }
}

static const char* requestTypeToStr(AsyncSD::RequestType t) {
  switch (t) {
    case AsyncSD::RequestType::Mount:    return "Mount";
    case AsyncSD::RequestType::Unmount:  return "Unmount";
    case AsyncSD::RequestType::Info:     return "Info";
    case AsyncSD::RequestType::Open:     return "Open";
    case AsyncSD::RequestType::Close:    return "Close";
    case AsyncSD::RequestType::Read:     return "Read";
    case AsyncSD::RequestType::Write:    return "Write";
    case AsyncSD::RequestType::Sync:     return "Sync";
    case AsyncSD::RequestType::Mkdir:    return "Mkdir";
    case AsyncSD::RequestType::Remove:   return "Remove";
    case AsyncSD::RequestType::Stat:     return "Stat";
    case AsyncSD::RequestType::Rename:   return "Rename";
    case AsyncSD::RequestType::ListDir:  return "ListDir";
    default:                             return "Unknown";
  }
}

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

static AsyncSD::RenameMode parseRenameMode(const char* modeStr) {
  if (!modeStr || modeStr[0] == '\0') {
    return AsyncSD::RenameMode::FailIfExists;
  }
  if (strcmp(modeStr, "replace") == 0 || strcmp(modeStr, "1") == 0) {
    return AsyncSD::RenameMode::ReplaceIfExists;
  }
  return AsyncSD::RenameMode::FailIfExists;
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
  Serial.println(F("  rename <from> <to> [replace]"));
  Serial.println(F("  stat <path>"));
  Serial.println(F("  ls [path]"));
  Serial.println(F("  touch <path>"));
  Serial.println(F("  cat <path> [maxlen]"));
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
    LOGE("Stress failed: %s (detail=%ld)",
         errorCodeToStr(res.code), static_cast<long>(res.detail));
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

static void handleCatResult(const AsyncSD::RequestResult& res) {
  if (!g_cat.active || res.id != g_cat.pendingId) {
    return;
  }
  if (res.code != AsyncSD::ErrorCode::Ok) {
    LOGE("Cat failed: %s (detail=%ld)",
         errorCodeToStr(res.code), static_cast<long>(res.detail));
    g_cat.active = false;
    g_cat.stage = CatStage::Idle;
    return;
  }

  switch (g_cat.stage) {
    case CatStage::Opening:
      g_cat.handle = res.handle;
      g_cat.stage = CatStage::Reading;
      {
        uint32_t chunk = g_cat.maxBytes - g_cat.totalRead;
        if (chunk > sizeof(g_cat.buffer)) {
          chunk = sizeof(g_cat.buffer);
        }
        g_cat.pendingId = g_sd.requestRead(g_cat.handle, g_cat.totalRead,
                                           g_cat.buffer, chunk);
      }
      break;
    case CatStage::Reading:
      if (res.bytesProcessed > 0) {
        Serial.write(g_cat.buffer, res.bytesProcessed);
        g_cat.totalRead += res.bytesProcessed;
      }
      if (res.bytesProcessed == 0 || g_cat.totalRead >= g_cat.maxBytes) {
        Serial.println();
        LOGI("Cat done, %lu bytes", static_cast<unsigned long>(g_cat.totalRead));
        g_cat.stage = CatStage::Closing;
        g_cat.pendingId = g_sd.requestClose(g_cat.handle);
      } else {
        uint32_t chunk = g_cat.maxBytes - g_cat.totalRead;
        if (chunk > sizeof(g_cat.buffer)) {
          chunk = sizeof(g_cat.buffer);
        }
        g_cat.pendingId = g_sd.requestRead(g_cat.handle, g_cat.totalRead,
                                           g_cat.buffer, chunk);
      }
      break;
    case CatStage::Closing:
      g_cat.active = false;
      g_cat.stage = CatStage::Idle;
      break;
    default:
      break;
  }
}

static void handleResult(const AsyncSD::RequestResult& res) {
  handleStressResult(res);
  handleCatResult(res);

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
  Serial.print(requestTypeToStr(res.type));
  Serial.print(F(" code="));
  Serial.print(errorCodeToStr(res.code));
  if (res.detail != 0) {
    Serial.print(F(" detail="));
    Serial.print(res.detail);
  }
  if (res.type == AsyncSD::RequestType::Open) {
    Serial.print(F(" handle="));
    Serial.print(res.handle);
  }
  Serial.println();

  if (res.type == AsyncSD::RequestType::Info) {
    printInfo(res.fsInfo, res.cardInfo);
  }

  if (res.type == AsyncSD::RequestType::Stat && res.code == AsyncSD::ErrorCode::Ok) {
    Serial.print(F("  dir="));
    Serial.print(res.stat.isDir ? "yes" : "no");
    Serial.print(F(" size="));
    Serial.println(static_cast<unsigned long long>(res.stat.size));
  }

  if (res.type == AsyncSD::RequestType::ListDir && res.id == g_lsRequestId) {
    g_lsRequestId = AsyncSD::INVALID_REQUEST_ID;
    if (res.code == AsyncSD::ErrorCode::Ok) {
      const uint32_t count = res.bytesProcessed;
      Serial.printf("  %lu entries:\n", static_cast<unsigned long>(count));
      for (uint32_t i = 0; i < count; ++i) {
        const AsyncSD::DirEntry& de = g_dirEntries[i];
        if (de.isDir) {
          Serial.printf("  [DIR]  %s\n", de.name);
        } else {
          Serial.printf("  %8llu  %s\n", static_cast<unsigned long long>(de.size), de.name);
        }
      }
    }
  }
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
  } else if (strcmp(cmd, "rename") == 0) {
    char* fromPath = strtok(nullptr, " ");
    char* toPath = strtok(nullptr, " ");
    char* mode = strtok(nullptr, " ");
    if (!fromPath || !toPath) {
      return;
    }
    g_sd.requestRename(fromPath, toPath, parseRenameMode(mode));
  } else if (strcmp(cmd, "stat") == 0) {
    char* path = strtok(nullptr, "");
    g_sd.requestStat(path);
  } else if (strcmp(cmd, "ls") == 0) {
    char* path = strtok(nullptr, "");
    if (!path || path[0] == '\0') {
      path = const_cast<char*>("/");
    }
    g_lsRequestId = g_sd.requestListDir(path, g_dirEntries, LS_MAX_ENTRIES);
    if (g_lsRequestId == AsyncSD::INVALID_REQUEST_ID) {
      LOGE("ls request failed");
    }
  } else if (strcmp(cmd, "touch") == 0) {
    char* path = strtok(nullptr, "");
    if (path && path[0] != '\0') {
      g_sd.requestOpen(path, AsyncSD::OpenMode::Write | AsyncSD::OpenMode::Create);
    }
  } else if (strcmp(cmd, "cat") == 0) {
    char* path = strtok(nullptr, " ");
    char* maxArg = strtok(nullptr, " ");
    if (!path || path[0] == '\0') {
      LOGE("Usage: cat <path> [maxlen]");
      return;
    }
    if (g_cat.active) {
      LOGE("Cat already running");
      return;
    }
    g_cat.active = true;
    g_cat.stage = CatStage::Opening;
    g_cat.totalRead = 0;
    g_cat.maxBytes = 512;
    if (maxArg) {
      uint32_t m = 0;
      if (parseU32(maxArg, &m) && m > 0) {
        g_cat.maxBytes = m;
      }
    }
    g_cat.pendingId = g_sd.requestOpen(path, AsyncSD::OpenMode::Read);
    LOGI("Cat %s (max %lu bytes)", path, static_cast<unsigned long>(g_cat.maxBytes));
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
  cfg.autoInitSpi = true;
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

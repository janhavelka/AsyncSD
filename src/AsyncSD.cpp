/**
 * @file AsyncSD.cpp
 * @brief AsyncSD implementation.
 */

#include "AsyncSD/AsyncSD.h"

#include <Arduino.h>
#include <SdFat.h>
#include <atomic>
#include <errno.h>
#include <new>
#include <string.h>

#include "InternalLogic.h"
#include "Presence.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace AsyncSD {

struct Internal;

class ISdTransport {
 public:
  virtual ~ISdTransport() = default;
  virtual ErrorCode begin(Internal* st, const SdCardConfig& cfg) = 0;
  virtual void end(Internal* st) = 0;
  virtual bool probe(Internal* st, const SdCardConfig& cfg) = 0;
};

class SpiTransport : public ISdTransport {
 public:
  ErrorCode begin(Internal* st, const SdCardConfig& cfg) override;
  void end(Internal* st) override;
  bool probe(Internal* st, const SdCardConfig& cfg) override;
};

class SdmmcTransport : public ISdTransport {
 public:
  ErrorCode begin(Internal* /*st*/, const SdCardConfig& /*cfg*/) override {
    return ErrorCode::Unsupported;
  }
  void end(Internal* /*st*/) override {}
  bool probe(Internal* /*st*/, const SdCardConfig& /*cfg*/) override { return false; }
};

struct Request {
  RequestId id = INVALID_REQUEST_ID;
  RequestType type = RequestType::Mount;
  ResultCallback callback = nullptr;
  void* user = nullptr;

  uint32_t enqueueMs = 0;
  uint32_t deadlineMs = 0;

  // Common parameters
  FileHandle handle = INVALID_FILE_HANDLE;
  OpenMode openMode = OpenMode::None;
  RenameMode renameMode = RenameMode::FailIfExists;

  // Buffer I/O
  void* buffer = nullptr;
  uint32_t length = 0;
  uint32_t processed = 0;
  uint64_t offset = 0;
  uint8_t copySlot = 0xFF;

  // Path handling
  char* fromPath = nullptr;
  uint16_t fromPathLen = 0;
  char* toPath = nullptr;
  uint16_t toPathLen = 0;

  bool started = false;
};

struct RequestSlot {
  Request req{};
  bool inUse = false;
};

struct ResultSlot {
  RequestResult result{};
  bool inUse = false;
};

struct FileSlot {
  FsFile file{};
  bool inUse = false;
};

struct HealthState {
  std::atomic<uint32_t> lastProgressMs{0};
  std::atomic<uint32_t> lastSuccessfulIoMs{0};
  std::atomic<uint32_t> lastErrorMs{0};
  std::atomic<uint32_t> consecutiveFailures{0};
  std::atomic<uint32_t> queueDepthRequests{0};
  std::atomic<uint32_t> queueDepthResults{0};
  std::atomic<uint32_t> stallEvents{0};
  std::atomic<uint8_t> currentStatus{static_cast<uint8_t>(SdStatus::Disabled)};
  std::atomic<uint16_t> lastErrorCode{static_cast<uint16_t>(ErrorCode::Ok)};
  std::atomic<bool> stallActive{false};
  std::atomic<uint32_t> droppedResults{0};
};

class DefaultSpiBusGuard : public ISpiBusGuard {
 public:
  DefaultSpiBusGuard() { _mutex = xSemaphoreCreateMutex(); }

  ~DefaultSpiBusGuard() override {
    if (_mutex) {
      vSemaphoreDelete(_mutex);
      _mutex = nullptr;
    }
  }

  bool lock(uint32_t timeoutMs) override {
    if (!_mutex) {
      return false;
    }
    return xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  }

  void unlock() override {
    if (_mutex) {
      xSemaphoreGive(_mutex);
    }
  }

 private:
  SemaphoreHandle_t _mutex = nullptr;
};

struct Internal {
  SdStatus status = SdStatus::Disabled;
  ErrorCode lastError = ErrorCode::Ok;
  ErrorInfo lastErrorInfo{};
  FsInfo fsInfo{};
  CardInfo cardInfo{};
  HealthState health{};
  SdCardConfig runtimeConfig{};

  bool initialized = false;
  bool mounted = false;
  std::atomic<bool> workerRunning{false};
  std::atomic<bool> stopWorker{false};
  bool spiInitialized = false;
  bool shutdownFailed = false;
  std::atomic<bool> inCallback{false};

  ISpiBusGuard* guard = nullptr;
  bool guardOwned = false;
  ISdTransport* transport = nullptr;
  bool transportOwned = false;

  SdFs* sd = nullptr;
  FileSlot* files = nullptr;
  uint8_t maxOpenFiles = 0;

  RequestSlot* reqQueue = nullptr;
  uint8_t reqHead = 0;
  uint8_t reqTail = 0;
  uint8_t reqCount = 0;
  uint8_t reqDepth = 0;

  ResultSlot* resQueue = nullptr;
  uint8_t resHead = 0;
  uint8_t resTail = 0;
  uint8_t resCount = 0;
  uint8_t resDepth = 0;

  char* pathPool = nullptr;
  uint16_t pathLen = 0;
  uint16_t pathStride = 0;

  char* errorPath = nullptr;
  uint16_t errorPathLen = 0;

  internal::CopyWriteSlot* copySlots = nullptr;
  uint8_t* copySlotPool = nullptr;
  uint8_t copySlotCount = 0;
  uint16_t copySlotSize = 0;
  internal::CopyWritePool copyPool{};

  RequestId nextRequestId = 1;

  // Presence detection
  internal::Debounce cdDebounce{};
  internal::Backoff backoff{};
  internal::FailureHysteresis probeFailures{};

  bool cdEnabled = false;
  bool cdInterruptEnabled = false;
  volatile bool cdInterruptFlag = false;
  bool cardPresent = false;

  uint32_t lastCdPollMs = 0;
  uint32_t lastProbeMs = 0;

  bool pendingAutoMount = false;
  bool pendingAutoUnmount = false;

  TaskHandle_t task = nullptr;

  portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
};

static bool allocateCopySlot(Internal* st, uint32_t length, const void* data,
                             uint8_t* outSlot, void** outBuffer);
static void releaseCopySlot(Internal* st, const Request& req);
static ErrorCode performUnmount(Internal* st, const SdCardConfig& cfg);
static void workerStepCore(Internal* st, const SdCardConfig& cfg, uint32_t budgetUs);
static void workerTaskEntry(void* arg);

static inline bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

static constexpr uint32_t MAX_TIME_WINDOW_MS = 0x7FFFFFFFU;

static inline uint32_t clampTimeWindowMs(uint32_t valueMs) {
  return (valueMs > MAX_TIME_WINDOW_MS) ? MAX_TIME_WINDOW_MS : valueMs;
}

static inline bool budgetExceeded(uint32_t startUs, uint32_t budgetUs) {
  if (budgetUs == 0) {
    return false;
  }
  return static_cast<uint32_t>(micros() - startUs) >= budgetUs;
}

static uint32_t selectTimeoutMs(const SdCardConfig& cfg, RequestType type) {
  switch (type) {
    case RequestType::Mount:
    case RequestType::Unmount:
      return cfg.mountTimeoutMs;
    case RequestType::Read:
    case RequestType::Write:
    case RequestType::Sync:
      return cfg.ioTimeoutMs;
    default:
      return cfg.opTimeoutMs;
  }
}

static Operation opFromRequest(RequestType type) {
  switch (type) {
    case RequestType::Mount:
      return Operation::Mount;
    case RequestType::Unmount:
      return Operation::Unmount;
    case RequestType::Info:
      return Operation::Info;
    case RequestType::Open:
      return Operation::Open;
    case RequestType::Close:
      return Operation::Close;
    case RequestType::Read:
      return Operation::Read;
    case RequestType::Write:
      return Operation::Write;
    case RequestType::Sync:
      return Operation::Sync;
    case RequestType::Mkdir:
      return Operation::Mkdir;
    case RequestType::Remove:
      return Operation::Remove;
    case RequestType::Stat:
      return Operation::Stat;
    case RequestType::Rename:
      return Operation::Rename;
    default:
      return Operation::None;
  }
}

static FsType mapFsType(uint8_t fatType) {
  if (fatType == FAT_TYPE_EXFAT) {
    return FsType::ExFat;
  }
  switch (fatType) {
    case 12:
      return FsType::Fat12;
    case 16:
      return FsType::Fat16;
    case 32:
      return FsType::Fat32;
    default:
      return FsType::Unknown;
  }
}

static CardType mapCardType(uint8_t type) {
  switch (type) {
    case SD_CARD_TYPE_SD1:
      return CardType::Sd1;
    case SD_CARD_TYPE_SD2:
      return CardType::Sd2;
    case SD_CARD_TYPE_SDHC:
      return CardType::SdHC;
    default:
      return CardType::Unknown;
  }
}

static bool boundedLength(const char* str, size_t maxChars, size_t* outLen) {
  if (!str || !outLen || maxChars == 0) {
    return false;
  }
  for (size_t i = 0; i < maxChars; ++i) {
    if (str[i] == '\0') {
      *outLen = i;
      return true;
    }
  }
  return false;
}

static void copyPath(char* dst, uint16_t dstLen, const char* src) {
  if (!dst || dstLen == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  const size_t maxCopy = static_cast<size_t>(dstLen - 1);
  size_t i = 0;
  for (; i < maxCopy && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

static bool matchesMountPointPrefix(const char* src, const char* mountPoint, size_t mountLen) {
  if (!src || !mountPoint || mountLen == 0) {
    return false;
  }
  for (size_t i = 0; i < mountLen; ++i) {
    const char c = src[i];
    if (c == '\0' || c != mountPoint[i]) {
      return false;
    }
  }
  const char next = src[mountLen];
  return next == '\0' || next == '/';
}

static bool normalizePath(const SdCardConfig& cfg, const char* in, char* out, uint16_t outLen) {
  if (!in || !out || outLen < 2) {
    return false;
  }

  const size_t maxChars = static_cast<size_t>(outLen);
  const char* src = in;

  const char* mp = cfg.mountPoint;
  if (mp && mp[0] != '\0') {
    size_t mpLen = 0;
    if (!boundedLength(mp, maxChars, &mpLen)) {
      return false;
    }
    if (mpLen > 0 && matchesMountPointPrefix(src, mp, mpLen)) {
      src += mpLen;
      if (*src == '/') {
        src++;
      }
    }
  }

  if (*src == '\0') {
    out[0] = '/';
    out[1] = '\0';
    return true;
  }

  size_t srcLen = 0;
  if (!boundedLength(src, maxChars, &srcLen)) {
    return false;
  }
  if (srcLen >= outLen) {
    return false;
  }

  memcpy(out, src, srcLen + 1);
  return true;
}

static uint8_t toSdFatFlags(OpenMode mode) {
  uint8_t flags = 0;
  const bool read = hasFlag(mode, OpenMode::Read);
  const bool write = hasFlag(mode, OpenMode::Write);

  if (read && write) {
    flags |= O_RDWR;
  } else if (write) {
    flags |= O_WRITE;
  } else {
    flags |= O_READ;
  }

  if (hasFlag(mode, OpenMode::Create)) {
    flags |= O_CREAT;
  }
  if (hasFlag(mode, OpenMode::Truncate)) {
    flags |= O_TRUNC;
  }
  if (hasFlag(mode, OpenMode::Append)) {
    flags |= O_APPEND;
  }
  if (hasFlag(mode, OpenMode::Exclusive)) {
    flags |= O_EXCL;
  }

  return flags;
}

static int32_t fsFailureDetail(Internal* st) {
  const int32_t err = errno;
  if (err != 0) {
    return err;
  }
  if (st && st->sd) {
    return st->sd->sdErrorCode();
  }
  return 0;
}

static ErrorCode mapRenameFailure(int32_t detail) {
  switch (detail) {
    case ENOENT:
      return ErrorCode::NotFound;
    case EEXIST:
      return ErrorCode::AlreadyExists;
    default:
      return ErrorCode::IoError;
  }
}

static void setStatus(Internal* st, SdStatus status) {
  if (!st) {
    return;
  }
  portENTER_CRITICAL(&st->stateMux);
  st->status = status;
  portEXIT_CRITICAL(&st->stateMux);
  st->health.currentStatus.store(static_cast<uint8_t>(status), std::memory_order_relaxed);
}

static void setFsInfo(Internal* st, const FsInfo& info) {
  if (!st) {
    return;
  }
  portENTER_CRITICAL(&st->stateMux);
  st->fsInfo = info;
  portEXIT_CRITICAL(&st->stateMux);
}

static void setCardInfo(Internal* st, const CardInfo& info) {
  if (!st) {
    return;
  }
  portENTER_CRITICAL(&st->stateMux);
  st->cardInfo = info;
  portEXIT_CRITICAL(&st->stateMux);
}

static void setLastError(Internal* st, ErrorCode code, Operation op, int32_t detail,
                         const char* path, uint32_t bytesReq, uint32_t bytesDone) {
  if (!st) {
    return;
  }

  const uint32_t nowMs = millis();
  portENTER_CRITICAL(&st->stateMux);
  st->lastError = code;
  st->lastErrorInfo.code = code;
  st->lastErrorInfo.op = op;
  st->lastErrorInfo.detail = detail;
  st->lastErrorInfo.timestampMs = nowMs;
  st->lastErrorInfo.bytesRequested = bytesReq;
  st->lastErrorInfo.bytesProcessed = bytesDone;

  if (st->errorPath && st->errorPathLen > 0) {
    copyPath(st->errorPath, st->errorPathLen, path);
    st->lastErrorInfo.path = st->errorPath;
  } else {
    st->lastErrorInfo.path = nullptr;
  }
  portEXIT_CRITICAL(&st->stateMux);

  st->health.lastErrorCode.store(static_cast<uint16_t>(code), std::memory_order_relaxed);
  st->health.lastErrorMs.store(nowMs, std::memory_order_relaxed);
}

static void recordProgress(Internal* st, uint32_t nowMs) {
  if (!st) {
    return;
  }
  st->health.lastProgressMs.store(nowMs, std::memory_order_relaxed);
}

static void recordSuccess(Internal* st) {
  if (!st) {
    return;
  }
  st->health.consecutiveFailures.store(0, std::memory_order_relaxed);
}

static void recordFailure(Internal* st) {
  if (!st) {
    return;
  }
  st->health.consecutiveFailures.fetch_add(1, std::memory_order_relaxed);
}

static void recordIoSuccess(Internal* st, uint32_t nowMs) {
  if (!st) {
    return;
  }
  st->health.lastSuccessfulIoMs.store(nowMs, std::memory_order_relaxed);
}

static bool lockBus(Internal* st, const SdCardConfig& cfg) {
  if (!st || !st->guard) {
    return false;
  }
  if (!st->guard->lock(cfg.lockTimeoutMs)) {
    return false;
  }
  st->guard->beginTransaction();
  return true;
}

static void unlockBus(Internal* st) {
  if (!st || !st->guard) {
    return;
  }
  st->guard->endTransaction();
  st->guard->unlock();
}

ErrorCode SpiTransport::begin(Internal* st, const SdCardConfig& cfg) {
  if (!st || !st->sd) {
    return ErrorCode::InternalError;
  }
  if (!lockBus(st, cfg)) {
    return ErrorCode::BusNotAvailable;
  }

  if (cfg.autoInitSpi && !st->spiInitialized) {
    st->spiInitialized = true;
    cfg.spi->begin(static_cast<int8_t>(cfg.pinSck),
                   static_cast<int8_t>(cfg.pinMiso),
                   static_cast<int8_t>(cfg.pinMosi),
                   static_cast<int8_t>(cfg.pinCs));
  }

  const uint8_t options = cfg.spiShared ? SHARED_SPI : DEDICATED_SPI;
  SdSpiConfig spiConfig(static_cast<uint8_t>(cfg.pinCs), options, cfg.spiFrequencyHz, cfg.spi);

  const bool ok = st->sd->begin(spiConfig);
  unlockBus(st);
  return ok ? ErrorCode::Ok : ErrorCode::CardInitFailed;
}

void SpiTransport::end(Internal* st) {
  if (!st || !st->sd) {
    return;
  }
  st->sd->end();
}

bool SpiTransport::probe(Internal* st, const SdCardConfig& cfg) {
  if (!st || !st->sd) {
    return false;
  }
  if (!lockBus(st, cfg)) {
    return false;
  }
  FsFile root;
  const bool ok = root.open("/", O_RDONLY);
  if (ok) {
    root.close();
  }
  unlockBus(st);
  return ok;
}

static void cdIsr(void* arg) {
  Internal* st = reinterpret_cast<Internal*>(arg);
  if (st) {
    st->cdInterruptFlag = true;
  }
}

static void workerTaskEntry(void* arg) {
  Internal* st = static_cast<Internal*>(arg);
  if (!st) {
    vTaskDelete(nullptr);
    return;
  }

  st->workerRunning.store(true, std::memory_order_relaxed);
  while (!st->stopWorker.load(std::memory_order_relaxed)) {
    workerStepCore(st, st->runtimeConfig, st->runtimeConfig.workerBudgetUs);
    if (st->runtimeConfig.workerIdleMs > 0) {
      vTaskDelay(pdMS_TO_TICKS(st->runtimeConfig.workerIdleMs));
    } else {
      taskYIELD();
    }
  }
  st->workerRunning.store(false, std::memory_order_relaxed);
  vTaskDelete(nullptr);
}

SdCardManager::SdCardManager() { _internal = new (std::nothrow) Internal(); }

SdCardManager::~SdCardManager() {
  end();
  if (_internal) {
    if (_internal->workerRunning.load(std::memory_order_relaxed)) {
      // Safe leak to avoid use-after-free if worker is still running.
      _internal = nullptr;
      return;
    }
    delete _internal;
    _internal = nullptr;
  }
}

bool SdCardManager::begin(const SdCardConfig& config, ISpiBusGuard* guard) {
  if (!_internal) {
    return false;
  }
  if (_internal->initialized) {
    setLastError(_internal, ErrorCode::Busy, Operation::Begin, 0, nullptr, 0, 0);
    return false;
  }

  SdCardConfig validated = config;
  validated.lockTimeoutMs = clampTimeWindowMs(validated.lockTimeoutMs);
  validated.mountTimeoutMs = clampTimeWindowMs(validated.mountTimeoutMs);
  validated.opTimeoutMs = clampTimeWindowMs(validated.opTimeoutMs);
  validated.ioTimeoutMs = clampTimeWindowMs(validated.ioTimeoutMs);
  validated.cdDebounceMs = clampTimeWindowMs(validated.cdDebounceMs);
  validated.cdPollMs = clampTimeWindowMs(validated.cdPollMs);
  validated.probeIntervalMs = clampTimeWindowMs(validated.probeIntervalMs);
  validated.probeBackoffMinMs = clampTimeWindowMs(validated.probeBackoffMinMs);
  validated.probeBackoffMaxMs = clampTimeWindowMs(validated.probeBackoffMaxMs);
  validated.workerIdleMs = clampTimeWindowMs(validated.workerIdleMs);
  validated.workerStallMs = clampTimeWindowMs(validated.workerStallMs);
  validated.shutdownTimeoutMs = clampTimeWindowMs(validated.shutdownTimeoutMs);

  if (validated.probeFailThreshold == 0) {
    validated.probeFailThreshold = 1;
  }
  if (validated.probeBackoffMinMs == 0) {
    validated.probeBackoffMinMs = 1;
  }
  if (validated.probeBackoffMaxMs < validated.probeBackoffMinMs) {
    validated.probeBackoffMaxMs = validated.probeBackoffMinMs;
  }

  // Basic validation
  if (validated.requestQueueDepth == 0 || validated.resultQueueDepth == 0 ||
      validated.maxOpenFiles == 0 || validated.maxPathLength < 2 ||
      validated.maxPathLength > (UINT16_MAX / 2U) ||
      validated.ioChunkBytes == 0 ||
      (validated.transport == TransportType::Spi && validated.spiFrequencyHz == 0) ||
      (validated.useWorkerTask &&
       validated.workerStackBytes < sizeof(StackType_t))) {
    setLastError(_internal, ErrorCode::InvalidArgument, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  size_t mountPointLen = 0;
  if (validated.mountPoint &&
      !boundedLength(validated.mountPoint, static_cast<size_t>(validated.maxPathLength),
                     &mountPointLen)) {
    setLastError(_internal, ErrorCode::InvalidArgument, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }
  (void)mountPointLen;

  if (validated.transport == TransportType::Spi) {
    if (!validated.spi || validated.pinCs < 0) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Begin, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Fault);
      return false;
    }
  }

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
  if (validated.transport == TransportType::Sdmmc) {
    setLastError(_internal, ErrorCode::Unsupported, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }
#endif

  _config = validated;
  _internal->runtimeConfig = _config;

  // Reset runtime state so repeated begin/end cycles stay deterministic.
  _internal->status = SdStatus::Disabled;
  _internal->lastError = ErrorCode::Ok;
  _internal->lastErrorInfo = ErrorInfo{};
  _internal->fsInfo = FsInfo{};
  _internal->cardInfo = CardInfo{};
  _internal->mounted = false;
  _internal->workerRunning.store(false, std::memory_order_relaxed);
  _internal->stopWorker.store(false, std::memory_order_relaxed);
  _internal->spiInitialized = false;
  _internal->shutdownFailed = false;
  _internal->inCallback.store(false, std::memory_order_relaxed);
  _internal->guard = nullptr;
  _internal->guardOwned = false;
  _internal->transport = nullptr;
  _internal->transportOwned = false;
  _internal->sd = nullptr;
  _internal->files = nullptr;
  _internal->reqQueue = nullptr;
  _internal->resQueue = nullptr;
  _internal->pathPool = nullptr;
  _internal->pathLen = 0;
  _internal->pathStride = 0;
  _internal->errorPath = nullptr;
  _internal->errorPathLen = 0;
  _internal->copySlots = nullptr;
  _internal->copySlotPool = nullptr;
  _internal->copySlotCount = 0;
  _internal->copySlotSize = 0;
  _internal->maxOpenFiles = 0;
  _internal->reqHead = 0;
  _internal->reqTail = 0;
  _internal->reqCount = 0;
  _internal->reqDepth = 0;
  _internal->resHead = 0;
  _internal->resTail = 0;
  _internal->resCount = 0;
  _internal->resDepth = 0;
  _internal->nextRequestId = 1;
  _internal->cdEnabled = false;
  _internal->cdInterruptEnabled = false;
  _internal->cdInterruptFlag = false;
  _internal->cardPresent = false;
  _internal->lastCdPollMs = 0;
  _internal->lastProbeMs = 0;
  _internal->pendingAutoMount = false;
  _internal->pendingAutoUnmount = false;
  _internal->task = nullptr;

  // Allocate resources
  _internal->sd = new (std::nothrow) SdFs();
  if (!_internal->sd) {
    setLastError(_internal, ErrorCode::NoMem, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  _internal->files = new (std::nothrow) FileSlot[_config.maxOpenFiles];
  _internal->reqQueue = new (std::nothrow) RequestSlot[_config.requestQueueDepth];
  _internal->resQueue = new (std::nothrow) ResultSlot[_config.resultQueueDepth];
  const size_t pathStride = static_cast<size_t>(_config.maxPathLength) * 2U;
  _internal->pathPool =
      new (std::nothrow) char[static_cast<size_t>(_config.requestQueueDepth) * pathStride];
  _internal->errorPath = new (std::nothrow) char[_config.maxPathLength];
  if (_config.copyWriteSlots > 0 && _config.maxCopyWriteBytes > 0) {
    _internal->copySlots =
        new (std::nothrow) internal::CopyWriteSlot[_config.copyWriteSlots];
    _internal->copySlotPool =
        new (std::nothrow) uint8_t[_config.copyWriteSlots * _config.maxCopyWriteBytes];
  }

  if (!_internal->files || !_internal->reqQueue || !_internal->resQueue ||
      !_internal->pathPool || !_internal->errorPath ||
      ((_config.copyWriteSlots > 0 && _config.maxCopyWriteBytes > 0) &&
       (!_internal->copySlots || !_internal->copySlotPool))) {
    end();
    setLastError(_internal, ErrorCode::NoMem, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  _internal->maxOpenFiles = _config.maxOpenFiles;
  _internal->reqDepth = _config.requestQueueDepth;
  _internal->resDepth = _config.resultQueueDepth;
  _internal->pathLen = _config.maxPathLength;
  _internal->pathStride = static_cast<uint16_t>(_config.maxPathLength * 2U);
  _internal->errorPathLen = _config.maxPathLength;
  _internal->copySlotCount = _config.copyWriteSlots;
  _internal->copySlotSize = _config.maxCopyWriteBytes;

  for (uint8_t i = 0; i < _internal->reqDepth; ++i) {
    _internal->reqQueue[i].inUse = false;
    char* slotPathBase = _internal->pathPool + (i * _internal->pathStride);
    _internal->reqQueue[i].req.fromPath = slotPathBase;
    _internal->reqQueue[i].req.fromPathLen = _internal->pathLen;
    _internal->reqQueue[i].req.toPath = slotPathBase + _internal->pathLen;
    _internal->reqQueue[i].req.toPathLen = _internal->pathLen;
  }

  for (uint8_t i = 0; i < _internal->resDepth; ++i) {
    _internal->resQueue[i].inUse = false;
  }

  for (uint8_t i = 0; i < _internal->maxOpenFiles; ++i) {
    _internal->files[i].inUse = false;
  }

  _internal->copyPool.init(_internal->copySlots, _internal->copySlotPool,
                           _internal->copySlotCount, _internal->copySlotSize);

  _internal->guard = guard;
  _internal->guardOwned = false;
  if (!_internal->guard) {
    _internal->guard = new (std::nothrow) DefaultSpiBusGuard();
    _internal->guardOwned = true;
  }
  if (!_internal->guard) {
    end();
    setLastError(_internal, ErrorCode::NoMem, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  _internal->transport = nullptr;
  _internal->transportOwned = true;
  if (_config.transport == TransportType::Spi) {
    _internal->transport = new (std::nothrow) SpiTransport();
  } else {
    _internal->transport = new (std::nothrow) SdmmcTransport();
  }
  if (!_internal->transport) {
    end();
    setLastError(_internal, ErrorCode::NoMem, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  // CD pin setup
  if (_config.cdPin >= 0) {
    _internal->cdEnabled = true;
    const uint8_t mode = _config.cdPullup && _config.cdActiveLow ? INPUT_PULLUP : INPUT;
    pinMode(static_cast<uint8_t>(_config.cdPin), mode);
    const bool raw = digitalRead(static_cast<uint8_t>(_config.cdPin)) != 0;
    const bool present = _config.cdActiveLow ? !raw : raw;
    _internal->cdDebounce.debounceMs = _config.cdDebounceMs;
    _internal->cdDebounce.reset(present, millis());
    _internal->cardPresent = present;
    _internal->pendingAutoMount = _config.autoMount && _internal->cardPresent;
    if (_config.cdUseInterrupt) {
      _internal->cdInterruptEnabled = true;
      attachInterruptArg(static_cast<uint8_t>(_config.cdPin), cdIsr, _internal, CHANGE);
    }
  }

  // No-CD backoff defaults
  _internal->backoff.minMs = _config.probeBackoffMinMs;
  _internal->backoff.maxMs = _config.probeBackoffMaxMs;
  _internal->backoff.reset(millis());
  _internal->probeFailures.threshold = _config.probeFailThreshold;
  _internal->probeFailures.reset();

  _internal->initialized = true;
  setStatus(_internal, _internal->cdEnabled ? (_internal->cardPresent ? SdStatus::CardInserted
                                                                      : SdStatus::NoCard)
                                            : SdStatus::NoCard);
  const uint32_t nowMs = millis();
  _internal->health.lastProgressMs.store(nowMs, std::memory_order_relaxed);
  _internal->health.lastSuccessfulIoMs.store(0, std::memory_order_relaxed);
  _internal->health.lastErrorMs.store(0, std::memory_order_relaxed);
  _internal->health.consecutiveFailures.store(0, std::memory_order_relaxed);
  _internal->health.queueDepthRequests.store(0, std::memory_order_relaxed);
  _internal->health.queueDepthResults.store(0, std::memory_order_relaxed);
  _internal->health.stallEvents.store(0, std::memory_order_relaxed);
  _internal->health.droppedResults.store(0, std::memory_order_relaxed);
  _internal->health.lastErrorCode.store(static_cast<uint16_t>(ErrorCode::Ok),
                                        std::memory_order_relaxed);
  _internal->health.stallActive.store(false, std::memory_order_relaxed);

  if (_config.useWorkerTask) {
    _internal->stopWorker.store(false, std::memory_order_relaxed);
    _internal->workerRunning.store(false, std::memory_order_relaxed);
    _internal->task = nullptr;
    const uint32_t stackDepth =
        static_cast<uint32_t>(_config.workerStackBytes / sizeof(StackType_t));
    const BaseType_t ok =
        (_config.workerCore >= 0)
            ? xTaskCreatePinnedToCore(
                  workerTaskEntry,
                  "AsyncSD",
                  stackDepth,
                  _internal,
                  _config.workerPriority,
                  &_internal->task,
                  _config.workerCore)
            : xTaskCreate(
                  workerTaskEntry,
                  "AsyncSD",
                  stackDepth,
                  _internal,
                  _config.workerPriority,
                  &_internal->task);

    if (ok != pdPASS) {
      end();
      setLastError(_internal, ErrorCode::InternalError, Operation::Begin, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Fault);
      return false;
    }
  }

  return true;
}

void SdCardManager::end() {
  if (!_internal) {
    return;
  }

  if (_internal->shutdownFailed &&
      _internal->workerRunning.load(std::memory_order_relaxed)) {
    return;
  }

  if (_internal->initialized && _internal->cdEnabled && _internal->cdInterruptEnabled) {
    detachInterrupt(static_cast<uint8_t>(_config.cdPin));
    _internal->cdInterruptEnabled = false;
  }

  if (_internal->task) {
    _internal->stopWorker.store(true, std::memory_order_relaxed);
    if (xTaskGetCurrentTaskHandle() == _internal->task) {
      // Called from worker context; signal stop and avoid teardown.
      setLastError(_internal, ErrorCode::InvalidContext, Operation::Shutdown, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Fault);
      return;
    }
    const uint32_t startMs = millis();
    while (_internal->workerRunning.load(std::memory_order_relaxed) &&
           !deadlineReached(millis(), startMs + _config.shutdownTimeoutMs)) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    const bool workerStillRunning =
        _internal->workerRunning.load(std::memory_order_relaxed);
    const bool timedOut = workerStillRunning;
    if (internal::shouldAbortShutdown(workerStillRunning, timedOut)) {
      setLastError(_internal, ErrorCode::Timeout, Operation::Shutdown, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Fault);
      _internal->health.stallActive.store(true, std::memory_order_relaxed);
      _internal->shutdownFailed = true;
      if (_internal->guard) {
        _internal->guard->onWorkerStuckOrKilled();
      }
      return;
    }
    _internal->task = nullptr;
  }

  if (_internal->mounted) {
    const ErrorCode unmountCode = performUnmount(_internal, _config);
    if (unmountCode != ErrorCode::Ok) {
      setLastError(_internal, unmountCode, Operation::Unmount, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Fault);
    }
  }

  if (_internal->guardOwned && _internal->guard) {
    delete _internal->guard;
  }
  _internal->guard = nullptr;
  _internal->guardOwned = false;
  if (_internal->transportOwned && _internal->transport) {
    delete _internal->transport;
  }
  _internal->transport = nullptr;
  _internal->transportOwned = false;

  delete[] _internal->files;
  delete[] _internal->reqQueue;
  delete[] _internal->resQueue;
  delete[] _internal->pathPool;
  portENTER_CRITICAL(&_internal->stateMux);
  _internal->lastErrorInfo.path = nullptr;
  portEXIT_CRITICAL(&_internal->stateMux);
  delete[] _internal->errorPath;
  delete[] _internal->copySlots;
  delete[] _internal->copySlotPool;
  delete _internal->sd;

  _internal->files = nullptr;
  _internal->reqQueue = nullptr;
  _internal->resQueue = nullptr;
  _internal->pathPool = nullptr;
  _internal->errorPath = nullptr;
  _internal->copySlots = nullptr;
  _internal->copySlotPool = nullptr;
  _internal->sd = nullptr;

  _internal->initialized = false;
  _internal->mounted = false;
  _internal->workerRunning.store(false, std::memory_order_relaxed);
  _internal->stopWorker.store(false, std::memory_order_relaxed);
  _internal->spiInitialized = false;
  _internal->shutdownFailed = false;
  _internal->inCallback.store(false, std::memory_order_relaxed);
  _internal->maxOpenFiles = 0;
  _internal->reqHead = 0;
  _internal->reqTail = 0;
  _internal->reqCount = 0;
  _internal->reqDepth = 0;
  _internal->resHead = 0;
  _internal->resTail = 0;
  _internal->resCount = 0;
  _internal->resDepth = 0;
  _internal->pathLen = 0;
  _internal->pathStride = 0;
  _internal->errorPathLen = 0;
  _internal->copySlotCount = 0;
  _internal->copySlotSize = 0;
  _internal->nextRequestId = 1;
  _internal->cdEnabled = false;
  _internal->cdInterruptEnabled = false;
  _internal->cdInterruptFlag = false;
  _internal->cardPresent = false;
  _internal->lastCdPollMs = 0;
  _internal->lastProbeMs = 0;
  _internal->pendingAutoMount = false;
  _internal->pendingAutoUnmount = false;
  _internal->task = nullptr;
  _internal->runtimeConfig = SdCardConfig{};
  setStatus(_internal, SdStatus::Disabled);
  setFsInfo(_internal, FsInfo{});
  setCardInfo(_internal, CardInfo{});
  _internal->health.stallActive.store(false, std::memory_order_relaxed);
  _internal->health.queueDepthRequests.store(0, std::memory_order_relaxed);
  _internal->health.queueDepthResults.store(0, std::memory_order_relaxed);
  _internal->health.droppedResults.store(0, std::memory_order_relaxed);
}

void SdCardManager::poll() {
  if (!_internal) {
    return;
  }
  if (_config.useWorkerTask && _internal->task &&
      xTaskGetCurrentTaskHandle() != _internal->task) {
    return;
  }
  workerStep(_config.workerBudgetUs);
}

SdStatus SdCardManager::status() const {
  if (!_internal) {
    return SdStatus::Fault;
  }
  portENTER_CRITICAL(&_internal->stateMux);
  const SdStatus st = _internal->status;
  portEXIT_CRITICAL(&_internal->stateMux);
  return st;
}

ErrorCode SdCardManager::lastError() const {
  if (!_internal) {
    return ErrorCode::InternalError;
  }
  portENTER_CRITICAL(&_internal->stateMux);
  const ErrorCode code = _internal->lastError;
  portEXIT_CRITICAL(&_internal->stateMux);
  return code;
}

ErrorInfo SdCardManager::lastErrorInfo() const {
  ErrorInfo info{};
  if (!_internal) {
    info.code = ErrorCode::InternalError;
    return info;
  }
  portENTER_CRITICAL(&_internal->stateMux);
  info = _internal->lastErrorInfo;
  portEXIT_CRITICAL(&_internal->stateMux);
  return info;
}

WorkerHealth SdCardManager::getWorkerHealth() const {
  WorkerHealth health{};
  if (!_internal) {
    health.currentStatus = SdStatus::Disabled;
    health.lastErrorCode = ErrorCode::InternalError;
    return health;
  }
  health.lastProgressMs =
      _internal->health.lastProgressMs.load(std::memory_order_relaxed);
  health.lastSuccessfulIoMs =
      _internal->health.lastSuccessfulIoMs.load(std::memory_order_relaxed);
  health.lastErrorMs =
      _internal->health.lastErrorMs.load(std::memory_order_relaxed);
  health.consecutiveFailures =
      _internal->health.consecutiveFailures.load(std::memory_order_relaxed);
  health.queueDepthRequests =
      _internal->health.queueDepthRequests.load(std::memory_order_relaxed);
  health.queueDepthResults =
      _internal->health.queueDepthResults.load(std::memory_order_relaxed);
  health.stallEvents =
      _internal->health.stallEvents.load(std::memory_order_relaxed);
  health.currentStatus =
      static_cast<SdStatus>(_internal->health.currentStatus.load(std::memory_order_relaxed));
  health.lastErrorCode =
      static_cast<ErrorCode>(_internal->health.lastErrorCode.load(std::memory_order_relaxed));
  return health;
}

FsInfo SdCardManager::fsInfo() const {
  FsInfo info{};
  if (!_internal) {
    return info;
  }
  portENTER_CRITICAL(&_internal->stateMux);
  info = _internal->fsInfo;
  portEXIT_CRITICAL(&_internal->stateMux);
  return info;
}

CardInfo SdCardManager::cardInfo() const {
  CardInfo info{};
  if (!_internal) {
    return info;
  }
  portENTER_CRITICAL(&_internal->stateMux);
  info = _internal->cardInfo;
  portEXIT_CRITICAL(&_internal->stateMux);
  return info;
}

uint32_t SdCardManager::getDroppedResults() const {
  if (!_internal) {
    return 0;
  }
  return _internal->health.droppedResults.load(std::memory_order_relaxed);
}

bool SdCardManager::isReady() const {
  if (!_internal) {
    return false;
  }
  return status() == SdStatus::Ready;
}

static bool enqueueInternal(Internal* st, const SdCardConfig& cfg, RequestType type,
                            const char* fromPath, const char* toPath, FileHandle handle,
                            OpenMode mode, RenameMode renameMode, uint64_t offset,
                            void* buffer, uint32_t length, uint8_t copySlot,
                            ResultCallback cb, void* user) {
  const char* errPath = fromPath ? fromPath : toPath;
  if (!st || !st->initialized) {
    setLastError(st, ErrorCode::NotInitialized, Operation::Enqueue, 0, errPath, length, 0);
    return false;
  }
  if (st->health.stallActive.load(std::memory_order_relaxed)) {
    setLastError(st, ErrorCode::Fault, Operation::Enqueue, 0, errPath, length, 0);
    return false;
  }
  if (st->inCallback.load(std::memory_order_relaxed)) {
    setLastError(st, ErrorCode::InvalidContext, Operation::Enqueue, 0, errPath, length, 0);
    return false;
  }

  portENTER_CRITICAL(&st->queueMux);
  if (st->reqCount >= st->reqDepth) {
    portEXIT_CRITICAL(&st->queueMux);
    setLastError(st, ErrorCode::Busy, Operation::Enqueue, 0, errPath, length, 0);
    return false;
  }

  RequestSlot& slot = st->reqQueue[st->reqTail];
  slot.inUse = true;

  Request& req = slot.req;
  char* fromPathBuf = req.fromPath;
  uint16_t fromPathLen = req.fromPathLen;
  char* toPathBuf = req.toPath;
  uint16_t toPathLen = req.toPathLen;
  req = Request{};
  req.fromPath = fromPathBuf;
  req.fromPathLen = fromPathLen;
  req.toPath = toPathBuf;
  req.toPathLen = toPathLen;
  if (req.fromPath && req.fromPathLen > 0) {
    req.fromPath[0] = '\0';
  }
  if (req.toPath && req.toPathLen > 0) {
    req.toPath[0] = '\0';
  }
  req.type = type;
  req.callback = cb;
  req.user = user;
  req.enqueueMs = millis();
  req.deadlineMs = req.enqueueMs + selectTimeoutMs(cfg, type);
  req.handle = handle;
  req.openMode = mode;
  req.renameMode = renameMode;
  req.offset = offset;
  req.buffer = buffer;
  req.length = length;
  req.processed = 0;
  req.started = false;
  req.copySlot = copySlot;

  req.id = st->nextRequestId++;
  if (req.id == INVALID_REQUEST_ID) {
    req.id = st->nextRequestId++;
  }

  if (req.fromPath && fromPath) {
    if (!normalizePath(cfg, fromPath, req.fromPath, req.fromPathLen)) {
      slot.inUse = false;
      portEXIT_CRITICAL(&st->queueMux);
      setLastError(st, ErrorCode::PathTooLong, Operation::Enqueue, 0, fromPath, length, 0);
      return false;
    }
  } else if (fromPath) {
    // path buffer not available
    slot.inUse = false;
    portEXIT_CRITICAL(&st->queueMux);
    setLastError(st, ErrorCode::InternalError, Operation::Enqueue, 0, fromPath, length, 0);
    return false;
  }

  if (req.toPath && toPath) {
    if (!normalizePath(cfg, toPath, req.toPath, req.toPathLen)) {
      slot.inUse = false;
      portEXIT_CRITICAL(&st->queueMux);
      setLastError(st, ErrorCode::PathTooLong, Operation::Enqueue, 0, toPath, length, 0);
      return false;
    }
  } else if (toPath) {
    // secondary path buffer not available
    slot.inUse = false;
    portEXIT_CRITICAL(&st->queueMux);
    setLastError(st, ErrorCode::InternalError, Operation::Enqueue, 0, toPath, length, 0);
    return false;
  }

  st->reqTail = static_cast<uint8_t>((st->reqTail + 1) % st->reqDepth);
  st->reqCount++;
  const uint8_t depth = st->reqCount;
  portEXIT_CRITICAL(&st->queueMux);
  st->health.queueDepthRequests.store(depth, std::memory_order_relaxed);
  return true;
}

RequestId SdCardManager::requestMount(ResultCallback cb, void* user) {
  if (!_internal) {
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Mount, nullptr, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestUnmount(ResultCallback cb, void* user) {
  if (!_internal) {
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Unmount, nullptr, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestInfo(ResultCallback cb, void* user) {
  if (!_internal) {
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Info, nullptr, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestOpen(const char* path, OpenMode mode, ResultCallback cb,
                                     void* user) {
  if (!_internal || !path) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Open, 0, path, 0, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Open, path, nullptr,
                         INVALID_FILE_HANDLE, mode, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestClose(FileHandle handle, ResultCallback cb, void* user) {
  if (!_internal || handle == INVALID_FILE_HANDLE) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Close, 0, nullptr, 0, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Close, nullptr, nullptr,
                         handle, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestRead(FileHandle handle, uint64_t offset, void* dst,
                                     size_t len, ResultCallback cb, void* user) {
  const uint32_t requestedBytes =
      (len > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(len);
  if (!_internal || handle == INVALID_FILE_HANDLE || !dst || len == 0 ||
      len > UINT32_MAX || offset == APPEND_OFFSET) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Read, 0, nullptr,
                   requestedBytes, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Read, nullptr, nullptr,
                         handle, OpenMode::None, RenameMode::FailIfExists,
                         offset, dst, static_cast<uint32_t>(len), 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestWrite(FileHandle handle, uint64_t offset, const void* src,
                                      size_t len, ResultCallback cb, void* user) {
  const uint32_t requestedBytes =
      (len > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(len);
  if (!_internal || handle == INVALID_FILE_HANDLE || !src || len == 0 ||
      len > UINT32_MAX) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Write, 0, nullptr,
                   requestedBytes, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Write, nullptr, nullptr,
                         handle, OpenMode::None, RenameMode::FailIfExists,
                         offset, const_cast<void*>(src), static_cast<uint32_t>(len),
                         0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestWriteCopy(FileHandle handle, uint64_t offset, const void* src,
                                          size_t len, ResultCallback cb, void* user) {
  if (!_internal) {
    return INVALID_REQUEST_ID;
  }
  const uint32_t requestedBytes =
      (len > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(len);
  if (!_internal->initialized) {
    setLastError(_internal, ErrorCode::NotInitialized, Operation::Enqueue, 0, nullptr,
                 requestedBytes, 0);
    return INVALID_REQUEST_ID;
  }
  if (_internal->inCallback.load(std::memory_order_relaxed)) {
    setLastError(_internal, ErrorCode::InvalidContext, Operation::Enqueue, 0, nullptr,
                 requestedBytes, 0);
    return INVALID_REQUEST_ID;
  }
  if (handle == INVALID_FILE_HANDLE || !src || len == 0 || len > UINT32_MAX) {
    setLastError(_internal, ErrorCode::InvalidArgument, Operation::Write, 0, nullptr,
                 requestedBytes, 0);
    return INVALID_REQUEST_ID;
  }
  if (_config.copyWriteSlots == 0 || _config.maxCopyWriteBytes == 0 ||
      len > _config.maxCopyWriteBytes) {
    setLastError(_internal, ErrorCode::InvalidArgument, Operation::Write, 0, nullptr,
                 requestedBytes, 0);
    return INVALID_REQUEST_ID;
  }
  if (_internal->health.stallActive.load(std::memory_order_relaxed)) {
    setLastError(_internal, ErrorCode::Fault, Operation::Enqueue, 0, nullptr,
                 requestedBytes, 0);
    return INVALID_REQUEST_ID;
  }

  uint8_t slot = 0xFF;
  void* buffer = nullptr;
  if (!allocateCopySlot(_internal, static_cast<uint32_t>(len), src, &slot, &buffer)) {
    setLastError(_internal, ErrorCode::Busy, Operation::Enqueue, 0, nullptr,
                 requestedBytes, 0);
    return INVALID_REQUEST_ID;
  }

  const bool ok = enqueueInternal(_internal, _config, RequestType::Write, nullptr, nullptr,
                                  handle, OpenMode::None, RenameMode::FailIfExists,
                                  offset, buffer, static_cast<uint32_t>(len),
                                  slot, cb, user);
  if (!ok) {
    Request tmp{};
    tmp.copySlot = slot;
    releaseCopySlot(_internal, tmp);
    return INVALID_REQUEST_ID;
  }
  return _internal->nextRequestId - 1;
}

RequestId SdCardManager::requestSync(FileHandle handle, ResultCallback cb, void* user) {
  if (!_internal || handle == INVALID_FILE_HANDLE) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Sync, 0, nullptr, 0, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Sync, nullptr, nullptr,
                         handle, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestMkdir(const char* path, ResultCallback cb, void* user) {
  if (!_internal || !path) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Mkdir, 0, path, 0, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Mkdir, path, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestRemove(const char* path, ResultCallback cb, void* user) {
  if (!_internal || !path) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Remove, 0, path, 0, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Remove, path, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestRename(const char* fromPath, const char* toPath,
                                       RenameMode mode, ResultCallback cb, void* user) {
  if (!_internal || !fromPath || !toPath) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Rename, 0,
                   fromPath ? fromPath : toPath, 0, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Rename, fromPath, toPath,
                         INVALID_FILE_HANDLE, OpenMode::None, mode,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestStat(const char* path, ResultCallback cb, void* user) {
  if (!_internal || !path) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Stat, 0, path, 0, 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Stat, path, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, RenameMode::FailIfExists,
                         0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

bool SdCardManager::getResult(RequestId id, RequestResult* out) {
  if (!_internal || !out || id == INVALID_REQUEST_ID) {
    return false;
  }
  bool found = false;
  portENTER_CRITICAL(&_internal->queueMux);
  if (_internal->resCount == 0) {
    portEXIT_CRITICAL(&_internal->queueMux);
    return false;
  }
  bool anyInUse = false;
  for (uint8_t i = 0; i < _internal->resDepth; ++i) {
    ResultSlot& slot = _internal->resQueue[i];
    if (slot.inUse) {
      anyInUse = true;
    }
    if (slot.inUse && slot.result.id == id) {
      *out = slot.result;
      slot.inUse = false;
      if (_internal->resCount > 0) {
        _internal->resCount--;
      }
      const uint8_t depth = _internal->resCount;
      portEXIT_CRITICAL(&_internal->queueMux);
      _internal->health.queueDepthResults.store(depth, std::memory_order_relaxed);
      found = true;
      return true;
    }
  }
  if (!anyInUse) {
    _internal->resHead = 0;
    _internal->resTail = 0;
    _internal->resCount = 0;
    portEXIT_CRITICAL(&_internal->queueMux);
    _internal->health.queueDepthResults.store(0, std::memory_order_relaxed);
    return false;
  }
  portEXIT_CRITICAL(&_internal->queueMux);
  return found;
}

bool SdCardManager::popResult(RequestResult* out) {
  if (!_internal || !out) {
    return false;
  }
  portENTER_CRITICAL(&_internal->queueMux);
  if (_internal->resCount == 0) {
    portEXIT_CRITICAL(&_internal->queueMux);
    return false;
  }

  uint8_t idx = _internal->resHead;
  for (uint8_t attempts = 0; attempts < _internal->resDepth; ++attempts) {
    ResultSlot& slot = _internal->resQueue[idx];
    if (slot.inUse) {
      *out = slot.result;
      slot.inUse = false;
      if (_internal->resCount > 0) {
        _internal->resCount--;
      }
      const uint8_t depth = _internal->resCount;
      _internal->resHead = static_cast<uint8_t>((idx + 1) % _internal->resDepth);
      portEXIT_CRITICAL(&_internal->queueMux);
      _internal->health.queueDepthResults.store(depth, std::memory_order_relaxed);
      return true;
    }
    idx = static_cast<uint8_t>((idx + 1) % _internal->resDepth);
  }

  _internal->resHead = 0;
  _internal->resTail = 0;
  _internal->resCount = 0;
  portEXIT_CRITICAL(&_internal->queueMux);
  _internal->health.queueDepthResults.store(0, std::memory_order_relaxed);
  return false;
}

static void enqueueResult(Internal* st, const SdCardConfig& cfg, const Request& req,
                          ErrorCode code, int32_t detail, uint32_t bytesDone,
                          FileHandle handle, const FileStat& stat) {
  if (!st) {
    return;
  }

  RequestResult result{};
  result.id = req.id;
  result.type = req.type;
  result.code = code;
  result.detail = detail;
  result.timestampMs = millis();
  result.bytesRequested = req.length;
  result.bytesProcessed = bytesDone;
  result.handle = handle;
  result.stat = stat;
  portENTER_CRITICAL(&st->stateMux);
  result.fsInfo = st->fsInfo;
  result.cardInfo = st->cardInfo;
  portEXIT_CRITICAL(&st->stateMux);

  if (cfg.enableWorkerCallbacks) {
    st->inCallback.store(true, std::memory_order_relaxed);
    if (req.callback) {
      req.callback(result, req.user);
    }
    if (cfg.onResult) {
      cfg.onResult(result, cfg.onResultUser);
    }
    st->inCallback.store(false, std::memory_order_relaxed);
  }

  portENTER_CRITICAL(&st->queueMux);
  if (st->resCount >= st->resDepth) {
    portEXIT_CRITICAL(&st->queueMux);
    internal::incrementDroppedResults(st->health.droppedResults);
    setLastError(st, ErrorCode::Busy, Operation::ResultEnqueue, 0, req.fromPath, req.length,
                 bytesDone);
    return;
  }

  ResultSlot& slot = st->resQueue[st->resTail];
  slot.inUse = true;
  slot.result = result;
  st->resTail = static_cast<uint8_t>((st->resTail + 1) % st->resDepth);
  st->resCount++;
  const uint8_t depth = st->resCount;
  portEXIT_CRITICAL(&st->queueMux);
  st->health.queueDepthResults.store(depth, std::memory_order_relaxed);
}

static FileSlot* getFileSlot(Internal* st, FileHandle handle) {
  if (!st || handle == INVALID_FILE_HANDLE) {
    return nullptr;
  }
  const uint16_t index = static_cast<uint16_t>(handle - 1);
  if (index >= st->maxOpenFiles) {
    return nullptr;
  }
  if (!st->files[index].inUse) {
    return nullptr;
  }
  return &st->files[index];
}

static FileHandle allocateFileSlot(Internal* st) {
  if (!st) {
    return INVALID_FILE_HANDLE;
  }
  for (uint8_t i = 0; i < st->maxOpenFiles; ++i) {
    if (!st->files[i].inUse) {
      st->files[i].inUse = true;
      return static_cast<FileHandle>(i + 1);
    }
  }
  return INVALID_FILE_HANDLE;
}

static bool allocateCopySlot(Internal* st, uint32_t length, const void* data,
                             uint8_t* outSlot, void** outBuffer) {
  if (!st || !outSlot || !outBuffer || st->copySlotCount == 0 ||
      st->copySlotSize == 0) {
    return false;
  }
  portENTER_CRITICAL(&st->queueMux);
  const bool ok = st->copyPool.allocate(length, data, outSlot, outBuffer);
  portEXIT_CRITICAL(&st->queueMux);
  return ok;
}

static void releaseCopySlot(Internal* st, const Request& req) {
  if (!st || req.copySlot == 0xFF || req.copySlot >= st->copySlotCount) {
    return;
  }
  portENTER_CRITICAL(&st->queueMux);
  st->copyPool.release(req.copySlot);
  portEXIT_CRITICAL(&st->queueMux);
}

static void closeAllFiles(Internal* st) {
  if (!st) {
    return;
  }
  for (uint8_t i = 0; i < st->maxOpenFiles; ++i) {
    if (st->files[i].inUse) {
      st->files[i].file.close();
      st->files[i].inUse = false;
    }
  }
}

static void markAllFilesClosed(Internal* st) {
  if (!st) {
    return;
  }
  for (uint8_t i = 0; i < st->maxOpenFiles; ++i) {
    if (st->files[i].inUse) {
      st->files[i].inUse = false;
    }
  }
}

static FsInfo buildFsInfo(Internal* st, bool includeUsage) {
  FsInfo info{};
  if (!st || !st->sd) {
    return info;
  }
  info.fsType = mapFsType(st->sd->fatType());
  info.sectorsPerCluster = static_cast<uint32_t>(st->sd->sectorsPerCluster());
  info.bytesPerCluster = static_cast<uint32_t>(st->sd->bytesPerCluster());
  info.clusterCount = static_cast<uint32_t>(st->sd->clusterCount());
  if (info.clusterCount > 0 && info.sectorsPerCluster > 0) {
    info.capacityBytes =
        static_cast<uint64_t>(info.clusterCount) *
        static_cast<uint64_t>(info.sectorsPerCluster) * 512ULL;
  }

  if (includeUsage) {
    const int32_t freeClusters = st->sd->freeClusterCount();
    if (freeClusters >= 0 && info.sectorsPerCluster > 0) {
      info.freeClusters = static_cast<uint32_t>(freeClusters);
      info.freeClustersValid = true;
      info.freeBytes =
          static_cast<uint64_t>(info.freeClusters) *
          static_cast<uint64_t>(info.sectorsPerCluster) * 512ULL;
      info.freeBytesValid = true;
      if (info.capacityBytes >= info.freeBytes) {
        info.usedBytes = info.capacityBytes - info.freeBytes;
        info.usedBytesValid = true;
      }
    }
  }

  return info;
}

static ErrorCode performMount(Internal* st, const SdCardConfig& cfg) {
  if (!st || !st->sd) {
    return ErrorCode::InternalError;
  }
  if (st->mounted) {
    return ErrorCode::Ok;
  }

  setStatus(st, SdStatus::Mounting);

  if (!st->transport) {
    st->mounted = false;
    setFsInfo(st, FsInfo{});
    setCardInfo(st, CardInfo{});
    return ErrorCode::InternalError;
  }
  const ErrorCode initCode = st->transport->begin(st, cfg);
  if (initCode != ErrorCode::Ok) {
    st->mounted = false;
    setFsInfo(st, FsInfo{});
    setCardInfo(st, CardInfo{});
    return initCode;
  }

  FsInfo info = buildFsInfo(st, false);
  if (info.fsType == FsType::Unknown) {
    if (lockBus(st, cfg)) {
      st->transport->end(st);
      unlockBus(st);
    }
    st->mounted = false;
    setFsInfo(st, FsInfo{});
    setCardInfo(st, CardInfo{});
    return ErrorCode::FsUnsupported;
  }

  setFsInfo(st, info);

  st->mounted = true;
  return ErrorCode::Ok;
}

static ErrorCode performUnmount(Internal* st, const SdCardConfig& cfg) {
  if (!st || !st->sd) {
    return ErrorCode::InternalError;
  }
  if (!st->mounted) {
    return ErrorCode::Ok;
  }

  if (!st->transport) {
    return ErrorCode::InternalError;
  }

  const bool locked = lockBus(st, cfg);
  if (!locked) {
    // Best-effort logical close without touching the bus.
    markAllFilesClosed(st);
    st->mounted = false;
    setFsInfo(st, FsInfo{});
    setCardInfo(st, CardInfo{});
    return ErrorCode::BusNotAvailable;
  }

  closeAllFiles(st);
  st->transport->end(st);
  unlockBus(st);
  st->mounted = false;
  setFsInfo(st, FsInfo{});
  setCardInfo(st, CardInfo{});
  return ErrorCode::Ok;
}

static bool probeCard(Internal* st, const SdCardConfig& cfg) {
  if (!st || !st->sd || !st->mounted || !st->transport) {
    return false;
  }
  return st->transport->probe(st, cfg);
}

static void failAllPending(Internal* st, const SdCardConfig& cfg, ErrorCode code) {
  if (!st) {
    return;
  }
  while (true) {
    Request req{};
    bool found = false;
    portENTER_CRITICAL(&st->queueMux);
    for (uint8_t i = 0; i < st->reqDepth; ++i) {
      RequestSlot& slot = st->reqQueue[i];
      if (slot.inUse) {
        req = slot.req;
        slot.inUse = false;
        if (st->reqCount > 0) {
          st->reqCount--;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      st->reqHead = 0;
      st->reqTail = 0;
      st->reqCount = 0;
      portEXIT_CRITICAL(&st->queueMux);
      st->health.queueDepthRequests.store(0, std::memory_order_relaxed);
      break;
    }
    portEXIT_CRITICAL(&st->queueMux);
    releaseCopySlot(st, req);
    enqueueResult(st, cfg, req, code, 0, 0, INVALID_FILE_HANDLE, FileStat{});
  }
}

static void updateCdPresence(Internal* st, const SdCardConfig& cfg, uint32_t nowMs) {
  if (!st || !st->cdEnabled) {
    return;
  }

  bool shouldSample = false;
  if (st->cdInterruptEnabled) {
    if (st->cdInterruptFlag) {
      st->cdInterruptFlag = false;
      shouldSample = true;
    }
  }

  if (!shouldSample && deadlineReached(nowMs, st->lastCdPollMs + cfg.cdPollMs)) {
    shouldSample = true;
  }

  if (!shouldSample) {
    return;
  }

  st->lastCdPollMs = nowMs;
  const bool raw = digitalRead(static_cast<uint8_t>(cfg.cdPin)) != 0;
  const bool sampleNow = cfg.cdActiveLow ? !raw : raw;

  if (st->cdDebounce.update(sampleNow, nowMs)) {
    st->cardPresent = st->cdDebounce.stable;
    setStatus(st, st->cardPresent ? SdStatus::CardInserted : SdStatus::NoCard);
    if (!st->cardPresent) {
      st->pendingAutoUnmount = true;
    } else {
      st->pendingAutoMount = cfg.autoMount;
    }
  }
}

static void workerStepCore(Internal* st, const SdCardConfig& cfg, uint32_t budgetUs) {
  if (!st || !st->initialized) {
    return;
  }

  const uint32_t startUs = micros();
  const uint32_t nowMs = millis();

  if (st->health.stallActive.load(std::memory_order_relaxed)) {
    return;
  }

  updateCdPresence(st, cfg, nowMs);

  // Auto-mount/unmount decisions
  if (!st->cdEnabled) {
    if (!st->mounted && cfg.autoMount && st->backoff.shouldFire(nowMs)) {
      st->pendingAutoMount = true;
    }

    if (st->mounted &&
        deadlineReached(nowMs, st->lastProbeMs + cfg.probeIntervalMs)) {
      const bool ok = probeCard(st, cfg);
      st->lastProbeMs = nowMs;
      recordProgress(st, nowMs);
      if (ok) {
        st->probeFailures.recordSuccess();
      } else if (st->probeFailures.recordFailure()) {
        st->pendingAutoUnmount = true;
        st->probeFailures.reset();
      }
    }
  }

  uint8_t pendingRequests = 0;
  portENTER_CRITICAL(&st->queueMux);
  pendingRequests = st->reqCount;
  portEXIT_CRITICAL(&st->queueMux);

  const bool hasPendingWork = (pendingRequests > 0) || st->pendingAutoMount ||
                              st->pendingAutoUnmount;
  if (!hasPendingWork) {
    recordProgress(st, nowMs);
  }

  if (cfg.workerStallMs > 0 && hasPendingWork) {
    const uint32_t lastProgress =
        st->health.lastProgressMs.load(std::memory_order_relaxed);
    if (internal::shouldStall(nowMs, lastProgress, cfg.workerStallMs, hasPendingWork)) {
      bool expected = false;
      if (st->health.stallActive.compare_exchange_strong(
              expected, true, std::memory_order_relaxed)) {
        st->health.stallEvents.fetch_add(1, std::memory_order_relaxed);
        setLastError(st, ErrorCode::Fault, Operation::None, 0, nullptr, 0, 0);
        setStatus(st, SdStatus::Fault);
        recordFailure(st);
        st->pendingAutoMount = false;
        st->pendingAutoUnmount = false;
        failAllPending(st, cfg, ErrorCode::Fault);
        (void)performUnmount(st, cfg);
      }
      return;
    }
  }

  if (st->pendingAutoUnmount) {
    st->pendingAutoUnmount = false;
    ErrorCode unmountCode = performUnmount(st, cfg);
    if (unmountCode != ErrorCode::Ok) {
      setLastError(st, unmountCode, Operation::Unmount, 0, nullptr, 0, 0);
      setStatus(st, SdStatus::Error);
      recordFailure(st);
      recordProgress(st, nowMs);
      st->pendingAutoUnmount = true;
      return;
    }
    setStatus(st, st->cdEnabled ? (st->cardPresent ? SdStatus::CardInserted
                                                    : SdStatus::NoCard)
                                : SdStatus::NoCard);
    recordSuccess(st);
    recordProgress(st, nowMs);
    failAllPending(st, cfg, ErrorCode::NotReady);
    if (budgetExceeded(startUs, budgetUs)) {
      return;
    }
  }

  if (st->pendingAutoMount) {
    st->pendingAutoMount = false;
    const ErrorCode mountCode = performMount(st, cfg);
    if (mountCode == ErrorCode::Ok) {
      setStatus(st, SdStatus::Ready);
      st->backoff.onSuccess(nowMs, cfg.probeIntervalMs);
      recordSuccess(st);
    } else {
      setLastError(st, mountCode, Operation::Mount, 0, nullptr, 0, 0);
      setStatus(st, SdStatus::Error);
      st->backoff.onFailure(nowMs);
      recordFailure(st);
    }
    recordProgress(st, nowMs);
    if (budgetExceeded(startUs, budgetUs)) {
      return;
    }
  }

  // Process request queue head
  portENTER_CRITICAL(&st->queueMux);
  if (st->reqCount == 0) {
    portEXIT_CRITICAL(&st->queueMux);
    return;
  }

  if (!st->reqQueue[st->reqHead].inUse) {
    bool found = false;
    uint8_t idx = st->reqHead;
    for (uint8_t i = 0; i < st->reqDepth; ++i) {
      if (st->reqQueue[idx].inUse) {
        st->reqHead = idx;
        found = true;
        break;
      }
      idx = static_cast<uint8_t>((idx + 1) % st->reqDepth);
    }
    if (!found) {
      st->reqHead = 0;
      st->reqTail = 0;
      st->reqCount = 0;
      portEXIT_CRITICAL(&st->queueMux);
      st->health.queueDepthRequests.store(0, std::memory_order_relaxed);
      recordProgress(st, nowMs);
      return;
    }
  }

  RequestSlot& slot = st->reqQueue[st->reqHead];
  if (!slot.inUse) {
    portEXIT_CRITICAL(&st->queueMux);
    return;
  }
  Request& req = slot.req;
  portEXIT_CRITICAL(&st->queueMux);

  const uint32_t nowReqMs = millis();
  if (deadlineReached(nowReqMs, req.deadlineMs)) {
    releaseCopySlot(st, req);
    enqueueResult(st, cfg, req, ErrorCode::Timeout, 0, req.processed,
                  INVALID_FILE_HANDLE, FileStat{});
    setLastError(st, ErrorCode::Timeout, opFromRequest(req.type), 0, req.fromPath,
                 req.length, req.processed);
    portENTER_CRITICAL(&st->queueMux);
    slot.inUse = false;
    st->reqHead = static_cast<uint8_t>((st->reqHead + 1) % st->reqDepth);
    if (st->reqCount > 0) {
      st->reqCount--;
    }
    const uint8_t depth = st->reqCount;
    portEXIT_CRITICAL(&st->queueMux);
    st->health.queueDepthRequests.store(depth, std::memory_order_relaxed);
    recordFailure(st);
    recordProgress(st, nowReqMs);
    return;
  }

  ErrorCode code = ErrorCode::Ok;
  int32_t detail = 0;
  uint32_t bytesDone = req.processed;
  FileHandle outHandle = INVALID_FILE_HANDLE;
  FileStat stat{};
  const char* errorPath = req.fromPath;

  const bool isIoRequest = (req.type != RequestType::Mount && req.type != RequestType::Unmount);
  if (isIoRequest && st->mounted) {
    setStatus(st, SdStatus::Busy);
  }

  switch (req.type) {
    case RequestType::Mount: {
      code = performMount(st, cfg);
      if (code == ErrorCode::Ok) {
        setStatus(st, SdStatus::Ready);
      } else {
        setStatus(st, SdStatus::Error);
      }
      break;
    }
    case RequestType::Unmount: {
      code = performUnmount(st, cfg);
      if (code == ErrorCode::Ok) {
        setStatus(st, st->cdEnabled ? (st->cardPresent ? SdStatus::CardInserted
                                                        : SdStatus::NoCard)
                                    : SdStatus::NoCard);
      } else {
        setStatus(st, SdStatus::Error);
      }
      break;
    }
    case RequestType::Info: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      CardInfo cardInfo{};
      FsInfo fsInfo{};
      SdCard* card = st->sd ? st->sd->card() : nullptr;
      if (!card) {
        code = ErrorCode::InternalError;
        unlockBus(st);
        break;
      }

      bool ioError = false;
      bool timedOut = false;
      cardInfo.type = mapCardType(card->type());
      cardInfo.sectorCount = static_cast<uint64_t>(card->sectorCount());
      cardInfo.capacityBytes = cardInfo.sectorCount * 512ULL;
#if HAS_SDIO_CLASS
      const uint32_t statusVal = card->status();
      if (statusVal != 0xFFFFFFFFUL) {
        cardInfo.cardStatus = statusVal;
        cardInfo.cardStatusValid = true;
      }
#endif

      uint32_t ocr = 0;
      if (card->readOCR(&ocr)) {
        cardInfo.ocr = ocr;
        cardInfo.ocrValid = true;
      } else {
        ioError = true;
      }

      if (!deadlineReached(millis(), req.deadlineMs)) {
        cid_t cid{};
        if (card->readCID(&cid)) {
          memcpy(cardInfo.cid, &cid, sizeof(cid));
          cardInfo.cidValid = true;
        } else {
          ioError = true;
        }
      } else {
        timedOut = true;
      }

      if (!timedOut && !deadlineReached(millis(), req.deadlineMs)) {
        csd_t csd{};
        if (card->readCSD(&csd)) {
          memcpy(cardInfo.csd, &csd, sizeof(csd));
          cardInfo.csdValid = true;
        } else {
          ioError = true;
        }
      } else if (!timedOut) {
        timedOut = true;
      }

      if (!timedOut && !deadlineReached(millis(), req.deadlineMs)) {
        scr_t scr{};
        if (card->readSCR(&scr)) {
          memcpy(cardInfo.scr, &scr, sizeof(scr));
          cardInfo.scrValid = true;
        } else {
          ioError = true;
        }
      } else if (!timedOut) {
        timedOut = true;
      }

      if (!timedOut && !deadlineReached(millis(), req.deadlineMs)) {
        sds_t sds{};
        if (card->readSDS(&sds)) {
          memcpy(cardInfo.sds, &sds, sizeof(sds));
          cardInfo.sdsValid = true;
        }
      } else if (!timedOut) {
        timedOut = true;
      }

      if (timedOut || deadlineReached(millis(), req.deadlineMs)) {
        code = ErrorCode::Timeout;
      } else if (ioError) {
        code = ErrorCode::IoError;
        detail = st->sd ? st->sd->sdErrorCode() : 0;
      }

      fsInfo = buildFsInfo(st, true);
      setFsInfo(st, fsInfo);
      setCardInfo(st, cardInfo);
      unlockBus(st);
      break;
    }
    case RequestType::Open: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      const uint8_t flags = toSdFatFlags(req.openMode);
      const FileHandle handle = allocateFileSlot(st);
      if (handle == INVALID_FILE_HANDLE) {
        code = ErrorCode::TooManyOpenFiles;
        unlockBus(st);
        break;
      }
      FileSlot* slotFile = getFileSlot(st, handle);
      if (!slotFile || !slotFile->file.open(req.fromPath, flags)) {
        if (slotFile) {
          slotFile->inUse = false;
        }
        detail = fsFailureDetail(st);
        if (detail == EEXIST) {
          code = ErrorCode::AlreadyExists;
          errorPath = req.fromPath;
        } else if (detail == ENOENT) {
          code = ErrorCode::NotFound;
        } else {
          code = ErrorCode::IoError;
        }
        unlockBus(st);
        break;
      }
      unlockBus(st);
      outHandle = handle;
      break;
    }
    case RequestType::Close: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(st, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      slotFile->file.close();
      slotFile->inUse = false;
      unlockBus(st);
      break;
    }
    case RequestType::Read: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(st, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!req.started && req.offset != APPEND_OFFSET) {
        if (!slotFile->file.seekSet(req.offset)) {
          detail = fsFailureDetail(st);
          code = ErrorCode::IoError;
          unlockBus(st);
          break;
        }
      }
      req.started = true;
      const uint32_t remaining = req.length - req.processed;
      const uint32_t chunk =
          remaining > cfg.ioChunkBytes ? cfg.ioChunkBytes : remaining;
      const int32_t readCount =
          slotFile->file.read(static_cast<uint8_t*>(req.buffer) + req.processed, chunk);
      if (readCount < 0) {
        detail = fsFailureDetail(st);
        code = ErrorCode::IoError;
      } else {
        req.processed += static_cast<uint32_t>(readCount);
        bytesDone = req.processed;
      }
      unlockBus(st);
      const uint32_t ioMs = millis();
      if (code == ErrorCode::Ok && readCount > 0) {
        recordIoSuccess(st, ioMs);
      }
      if (code == ErrorCode::Ok && req.processed < req.length && readCount > 0 &&
          !budgetExceeded(startUs, budgetUs)) {
        recordProgress(st, ioMs);
        return;
      }
      break;
    }
    case RequestType::Write: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(st, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!req.started) {
        if (req.offset == APPEND_OFFSET) {
          if (!slotFile->file.seekEnd()) {
            detail = fsFailureDetail(st);
            code = ErrorCode::IoError;
            unlockBus(st);
            break;
          }
        } else {
          if (!slotFile->file.seekSet(req.offset)) {
            detail = fsFailureDetail(st);
            code = ErrorCode::IoError;
            unlockBus(st);
            break;
          }
        }
      }
      req.started = true;
      if (st->fsInfo.fsType == FsType::Fat32) {
        const uint64_t fileSize = slotFile->file.fileSize();
        const uint64_t base =
            (req.offset == APPEND_OFFSET) ? fileSize : req.offset;
        const uint64_t target = (fileSize > base) ? fileSize : base;
        if (target + static_cast<uint64_t>(req.length) > 0xFFFFFFFFULL) {
          code = ErrorCode::FileTooLarge;
          unlockBus(st);
          break;
        }
      }
      const uint32_t remaining = req.length - req.processed;
      const uint32_t chunk =
          remaining > cfg.ioChunkBytes ? cfg.ioChunkBytes : remaining;
      const size_t written =
          slotFile->file.write(static_cast<const uint8_t*>(req.buffer) + req.processed, chunk);
      if (written == 0) {
        detail = fsFailureDetail(st);
        code = (detail == ENOSPC) ? ErrorCode::NoSpaceLeft : ErrorCode::IoError;
      } else {
        req.processed += static_cast<uint32_t>(written);
        bytesDone = req.processed;
      }
      unlockBus(st);
      const uint32_t ioMs = millis();
      if (code == ErrorCode::Ok && written > 0) {
        recordIoSuccess(st, ioMs);
      }
      if (code == ErrorCode::Ok && req.processed < req.length &&
          !budgetExceeded(startUs, budgetUs)) {
        recordProgress(st, ioMs);
        return;
      }
      break;
    }
    case RequestType::Sync: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(st, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!slotFile->file.sync()) {
        detail = fsFailureDetail(st);
        code = ErrorCode::IoError;
      } else {
        recordIoSuccess(st, millis());
      }
      unlockBus(st);
      break;
    }
    case RequestType::Mkdir: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!st->sd->mkdir(req.fromPath, true)) {
        detail = fsFailureDetail(st);
        code = (detail == EEXIST) ? ErrorCode::AlreadyExists : ErrorCode::IoError;
      }
      unlockBus(st);
      break;
    }
    case RequestType::Remove: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!st->sd->remove(req.fromPath)) {
        if (!st->sd->rmdir(req.fromPath)) {
          detail = fsFailureDetail(st);
          code = (detail == ENOENT) ? ErrorCode::NotFound : ErrorCode::IoError;
        }
      }
      unlockBus(st);
      break;
    }
    case RequestType::Rename: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (deadlineReached(millis(), req.deadlineMs)) {
        code = ErrorCode::Timeout;
        unlockBus(st);
        break;
      }
      if (!st->sd->exists(req.fromPath)) {
        detail = ENOENT;
        code = ErrorCode::NotFound;
        unlockBus(st);
        break;
      }
      if (strcmp(req.fromPath, req.toPath) == 0) {
        unlockBus(st);
        break;
      }
      const bool dstExists = st->sd->exists(req.toPath);
      if (req.renameMode == RenameMode::FailIfExists && dstExists) {
        detail = EEXIST;
        errorPath = req.toPath;
        code = ErrorCode::AlreadyExists;
        unlockBus(st);
        break;
      }
      if (req.renameMode == RenameMode::ReplaceIfExists && dstExists) {
        if (!st->sd->remove(req.toPath) && !st->sd->rmdir(req.toPath)) {
          detail = fsFailureDetail(st);
          errorPath = req.toPath;
          code = ErrorCode::IoError;
          unlockBus(st);
          break;
        }
      }
      if (deadlineReached(millis(), req.deadlineMs)) {
        code = ErrorCode::Timeout;
        unlockBus(st);
        break;
      }
      if (!st->sd->rename(req.fromPath, req.toPath)) {
        detail = fsFailureDetail(st);
        code = mapRenameFailure(detail);
        if (code == ErrorCode::AlreadyExists) {
          errorPath = req.toPath;
        }
        unlockBus(st);
        break;
      }
      unlockBus(st);
      break;
    }
    case RequestType::Stat: {
      if (!st->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(st, cfg)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      FsFile f;
      if (!f.open(req.fromPath, O_RDONLY)) {
        detail = fsFailureDetail(st);
        code = (detail == ENOENT) ? ErrorCode::NotFound : ErrorCode::IoError;
        unlockBus(st);
        break;
      }
      stat.size = f.fileSize();
      stat.isDir = f.isDir();
      f.close();
      unlockBus(st);
      break;
    }
    default:
      code = ErrorCode::InternalError;
      break;
  }

  if (isIoRequest) {
    if (code == ErrorCode::Ok) {
      if (st->mounted) {
        setStatus(st, SdStatus::Ready);
      }
    } else if (code == ErrorCode::NotReady) {
      setStatus(st, st->cdEnabled ? (st->cardPresent ? SdStatus::CardInserted
                                                      : SdStatus::NoCard)
                                  : SdStatus::NoCard);
    } else {
      setStatus(st, SdStatus::Error);
    }
  }

  releaseCopySlot(st, req);
  enqueueResult(st, cfg, req, code, detail, bytesDone, outHandle, stat);
  if (code != ErrorCode::Ok) {
    setLastError(st, code, opFromRequest(req.type), detail, errorPath, req.length,
                 bytesDone);
    if (!st->cdEnabled && (code == ErrorCode::IoError || code == ErrorCode::NotReady)) {
      if (st->probeFailures.recordFailure()) {
        st->pendingAutoUnmount = true;
        st->probeFailures.reset();
      }
    }
    recordFailure(st);
  } else {
    recordSuccess(st);
  }

  recordProgress(st, millis());
  portENTER_CRITICAL(&st->queueMux);
  slot.inUse = false;
  st->reqHead = static_cast<uint8_t>((st->reqHead + 1) % st->reqDepth);
  if (st->reqCount > 0) {
    st->reqCount--;
  }
  const uint8_t depth = st->reqCount;
  portEXIT_CRITICAL(&st->queueMux);
  st->health.queueDepthRequests.store(depth, std::memory_order_relaxed);
}

void SdCardManager::workerStep(uint32_t budgetUs) {
  if (!_internal || !_internal->initialized) {
    return;
  }
  if (_config.useWorkerTask && _internal->task &&
      xTaskGetCurrentTaskHandle() != _internal->task) {
    return;
  }
  workerStepCore(_internal, _config, budgetUs);
}

}  // namespace AsyncSD

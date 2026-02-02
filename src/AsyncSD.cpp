/**
 * @file AsyncSD.cpp
 * @brief AsyncSD implementation.
 */

#include "AsyncSD/AsyncSD.h"

#include <Arduino.h>
#include <SdFat.h>
#include <atomic>
#include <errno.h>
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

  // Buffer I/O
  void* buffer = nullptr;
  uint32_t length = 0;
  uint32_t processed = 0;
  uint64_t offset = 0;
  uint8_t copySlot = 0xFF;

  // Path handling
  char* path = nullptr;
  uint16_t pathLen = 0;

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

  bool initialized = false;
  bool mounted = false;
  bool workerRunning = false;
  bool stopWorker = false;
  bool spiInitialized = false;
  bool shutdownFailed = false;

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

static inline bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
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

static bool normalizePath(const SdCardConfig& cfg, const char* in, char* out, uint16_t outLen) {
  if (!in || !out || outLen < 2) {
    return false;
  }

  const char* src = in;
  const char* mp = cfg.mountPoint;
  if (mp && mp[0] != '\0') {
    const size_t mpLen = strlen(mp);
    if (mpLen > 0 && strncmp(src, mp, mpLen) == 0) {
      if (src[mpLen] == '\0' || src[mpLen] == '/') {
        src += mpLen;
        if (*src == '/') {
          src++;
        }
      }
    }
  }

  if (*src == '\0') {
    src = "/";
  }

  const size_t srcLen = strlen(src);
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

SdCardManager::SdCardManager() { _internal = new Internal(); }

SdCardManager::~SdCardManager() {
  end();
  delete _internal;
  _internal = nullptr;
}

bool SdCardManager::begin(const SdCardConfig& config, ISpiBusGuard* guard) {
  if (!_internal) {
    return false;
  }
  if (_internal->initialized) {
    setLastError(_internal, ErrorCode::Busy, Operation::Begin, 0, nullptr, 0, 0);
    return false;
  }

  // Basic validation
  if (config.requestQueueDepth == 0 || config.resultQueueDepth == 0 ||
      config.maxOpenFiles == 0 || config.maxPathLength < 2 ||
      config.ioChunkBytes == 0) {
    setLastError(_internal, ErrorCode::InvalidArgument, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  if (config.transport == TransportType::Spi) {
    if (!config.spi || config.pinCs < 0) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Begin, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Fault);
      return false;
    }
  }

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
  if (config.transport == TransportType::Sdmmc) {
    setLastError(_internal, ErrorCode::Unsupported, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }
#endif

  _config = config;

  // Allocate resources
  _internal->sd = new SdFs();
  if (!_internal->sd) {
    setLastError(_internal, ErrorCode::InternalError, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  _internal->files = new FileSlot[_config.maxOpenFiles];
  _internal->reqQueue = new RequestSlot[_config.requestQueueDepth];
  _internal->resQueue = new ResultSlot[_config.resultQueueDepth];
  _internal->pathPool = new char[_config.requestQueueDepth * _config.maxPathLength];
  _internal->errorPath = new char[_config.maxPathLength];
  if (_config.copyWriteSlots > 0 && _config.maxCopyWriteBytes > 0) {
  _internal->copySlots = new internal::CopyWriteSlot[_config.copyWriteSlots];
    _internal->copySlotPool =
        new uint8_t[_config.copyWriteSlots * _config.maxCopyWriteBytes];
  }

  if (!_internal->files || !_internal->reqQueue || !_internal->resQueue ||
      !_internal->pathPool || !_internal->errorPath ||
      ((_config.copyWriteSlots > 0 && _config.maxCopyWriteBytes > 0) &&
       (!_internal->copySlots || !_internal->copySlotPool))) {
    end();
    setLastError(_internal, ErrorCode::InternalError, Operation::Begin, 0, nullptr, 0, 0);
    setStatus(_internal, SdStatus::Fault);
    return false;
  }

  _internal->maxOpenFiles = _config.maxOpenFiles;
  _internal->reqDepth = _config.requestQueueDepth;
  _internal->resDepth = _config.resultQueueDepth;
  _internal->pathLen = _config.maxPathLength;
  _internal->errorPathLen = _config.maxPathLength;
  _internal->copySlotCount = _config.copyWriteSlots;
  _internal->copySlotSize = _config.maxCopyWriteBytes;

  for (uint8_t i = 0; i < _internal->reqDepth; ++i) {
    _internal->reqQueue[i].inUse = false;
    _internal->reqQueue[i].req.path = _internal->pathPool + (i * _internal->pathLen);
    _internal->reqQueue[i].req.pathLen = _internal->pathLen;
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
    _internal->guard = new DefaultSpiBusGuard();
    _internal->guardOwned = true;
  }

  _internal->transport = nullptr;
  _internal->transportOwned = true;
  if (_config.transport == TransportType::Spi) {
    _internal->transport = new SpiTransport();
  } else {
    _internal->transport = new SdmmcTransport();
  }
  if (!_internal->transport) {
    end();
    setLastError(_internal, ErrorCode::InternalError, Operation::Begin, 0, nullptr, 0, 0);
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
    _internal->stopWorker = false;
    _internal->workerRunning = false;
    const BaseType_t ok =
        (_config.workerCore >= 0)
            ? xTaskCreatePinnedToCore(
                  [](void* arg) {
                    SdCardManager* self = static_cast<SdCardManager*>(arg);
                    if (!self) {
                      vTaskDelete(nullptr);
                      return;
                    }
                    Internal* st = self->_internal;
                    if (!st) {
                      vTaskDelete(nullptr);
                      return;
                    }
                    st->workerRunning = true;
                    while (!st->stopWorker) {
                      self->workerStep(self->_config.workerBudgetUs);
                      if (self->_config.workerIdleMs > 0) {
                        vTaskDelay(pdMS_TO_TICKS(self->_config.workerIdleMs));
                      } else {
                        taskYIELD();
                      }
                    }
                    st->workerRunning = false;
                    vTaskDelete(nullptr);
                  },
                  "AsyncSD",
                  _config.workerStackBytes / sizeof(StackType_t),
                  this,
                  _config.workerPriority,
                  &_internal->task,
                  _config.workerCore)
            : xTaskCreate(
                  [](void* arg) {
                    SdCardManager* self = static_cast<SdCardManager*>(arg);
                    if (!self) {
                      vTaskDelete(nullptr);
                      return;
                    }
                    Internal* st = self->_internal;
                    if (!st) {
                      vTaskDelete(nullptr);
                      return;
                    }
                    st->workerRunning = true;
                    while (!st->stopWorker) {
                      self->workerStep(self->_config.workerBudgetUs);
                      if (self->_config.workerIdleMs > 0) {
                        vTaskDelay(pdMS_TO_TICKS(self->_config.workerIdleMs));
                      } else {
                        taskYIELD();
                      }
                    }
                    st->workerRunning = false;
                    vTaskDelete(nullptr);
                  },
                  "AsyncSD",
                  _config.workerStackBytes / sizeof(StackType_t),
                  this,
                  _config.workerPriority,
                  &_internal->task);

    if (ok != pdPASS) {
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

  if (_internal->shutdownFailed && _internal->workerRunning) {
    return;
  }

  if (_internal->initialized && _internal->cdEnabled && _internal->cdInterruptEnabled) {
    detachInterrupt(static_cast<uint8_t>(_config.cdPin));
    _internal->cdInterruptEnabled = false;
  }

  if (_internal->task) {
    _internal->stopWorker = true;
    if (xTaskGetCurrentTaskHandle() == _internal->task) {
      // Called from worker context; let the task exit on its own.
      return;
    }
    const uint32_t startMs = millis();
    while (_internal->workerRunning &&
           !deadlineReached(millis(), startMs + _config.shutdownTimeoutMs)) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    const bool timedOut = _internal->workerRunning;
    if (internal::shouldAbortShutdown(_internal->workerRunning, timedOut)) {
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
  _internal->shutdownFailed = false;
  setStatus(_internal, SdStatus::Disabled);
  _internal->health.stallActive.store(false, std::memory_order_relaxed);
  _internal->health.queueDepthRequests.store(0, std::memory_order_relaxed);
  _internal->health.queueDepthResults.store(0, std::memory_order_relaxed);
  _internal->health.droppedResults.store(0, std::memory_order_relaxed);
}

void SdCardManager::poll() {
  if (!_internal) {
    return;
  }
  if (_config.useWorkerTask && _internal->workerRunning) {
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
                            const char* path, FileHandle handle, OpenMode mode,
                            uint64_t offset, void* buffer, uint32_t length,
                            uint8_t copySlot, ResultCallback cb, void* user) {
  if (!st || !st->initialized) {
    setLastError(st, ErrorCode::NotInitialized, Operation::Enqueue, 0, path, length, 0);
    return false;
  }
  if (st->health.stallActive.load(std::memory_order_relaxed)) {
    setLastError(st, ErrorCode::Fault, Operation::Enqueue, 0, path, length, 0);
    return false;
  }

  portENTER_CRITICAL(&st->queueMux);
  if (st->reqCount >= st->reqDepth) {
    portEXIT_CRITICAL(&st->queueMux);
    setLastError(st, ErrorCode::Busy, Operation::Enqueue, 0, path, length, 0);
    return false;
  }

  RequestSlot& slot = st->reqQueue[st->reqTail];
  slot.inUse = true;

  Request& req = slot.req;
  char* pathBuf = req.path;
  uint16_t pathLen = req.pathLen;
  req = Request{};
  req.path = pathBuf;
  req.pathLen = pathLen;
  if (req.path && req.pathLen > 0) {
    req.path[0] = '\0';
  }
  req.type = type;
  req.callback = cb;
  req.user = user;
  req.enqueueMs = millis();
  req.deadlineMs = req.enqueueMs + selectTimeoutMs(cfg, type);
  req.handle = handle;
  req.openMode = mode;
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

  if (req.path && path) {
    if (!normalizePath(cfg, path, req.path, req.pathLen)) {
      slot.inUse = false;
      portEXIT_CRITICAL(&st->queueMux);
      setLastError(st, ErrorCode::PathTooLong, Operation::Enqueue, 0, path, length, 0);
      return false;
    }
  } else if (path) {
    // path buffer not available
    slot.inUse = false;
    portEXIT_CRITICAL(&st->queueMux);
    setLastError(st, ErrorCode::InternalError, Operation::Enqueue, 0, path, length, 0);
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
  return enqueueInternal(_internal, _config, RequestType::Mount, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestUnmount(ResultCallback cb, void* user) {
  if (!_internal) {
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Unmount, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestInfo(ResultCallback cb, void* user) {
  if (!_internal) {
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Info, nullptr,
                         INVALID_FILE_HANDLE, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
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
  return enqueueInternal(_internal, _config, RequestType::Open, path,
                         INVALID_FILE_HANDLE, mode, 0, nullptr, 0, 0xFF, cb, user)
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
  return enqueueInternal(_internal, _config, RequestType::Close, nullptr,
                         handle, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestRead(FileHandle handle, uint64_t offset, void* dst,
                                     size_t len, ResultCallback cb, void* user) {
  if (!_internal || handle == INVALID_FILE_HANDLE || !dst || len == 0 ||
      len > UINT32_MAX) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Read, 0, nullptr,
                   static_cast<uint32_t>(len), 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Read, nullptr,
                         handle, OpenMode::None, offset, dst,
                         static_cast<uint32_t>(len), 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestWrite(FileHandle handle, uint64_t offset, const void* src,
                                      size_t len, ResultCallback cb, void* user) {
  if (!_internal || handle == INVALID_FILE_HANDLE || !src || len == 0 ||
      len > UINT32_MAX) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Write, 0, nullptr,
                   static_cast<uint32_t>(len), 0);
    }
    return INVALID_REQUEST_ID;
  }
  return enqueueInternal(_internal, _config, RequestType::Write, nullptr,
                         handle, OpenMode::None, offset,
                         const_cast<void*>(src), static_cast<uint32_t>(len), 0xFF, cb, user)
             ? _internal->nextRequestId - 1
             : INVALID_REQUEST_ID;
}

RequestId SdCardManager::requestWriteCopy(FileHandle handle, uint64_t offset, const void* src,
                                          size_t len, ResultCallback cb, void* user) {
  if (!_internal || handle == INVALID_FILE_HANDLE || !src || len == 0 ||
      len > UINT32_MAX) {
    if (_internal) {
      setLastError(_internal, ErrorCode::InvalidArgument, Operation::Write, 0, nullptr,
                   static_cast<uint32_t>(len), 0);
    }
    return INVALID_REQUEST_ID;
  }
  if (_config.copyWriteSlots == 0 || _config.maxCopyWriteBytes == 0 ||
      len > _config.maxCopyWriteBytes) {
    setLastError(_internal, ErrorCode::InvalidArgument, Operation::Write, 0, nullptr,
                 static_cast<uint32_t>(len), 0);
    return INVALID_REQUEST_ID;
  }
  if (_internal->health.stallActive.load(std::memory_order_relaxed)) {
    setLastError(_internal, ErrorCode::Fault, Operation::Enqueue, 0, nullptr,
                 static_cast<uint32_t>(len), 0);
    return INVALID_REQUEST_ID;
  }

  uint8_t slot = 0xFF;
  void* buffer = nullptr;
  if (!allocateCopySlot(_internal, static_cast<uint32_t>(len), src, &slot, &buffer)) {
    setLastError(_internal, ErrorCode::Busy, Operation::Enqueue, 0, nullptr,
                 static_cast<uint32_t>(len), 0);
    return INVALID_REQUEST_ID;
  }

  const bool ok = enqueueInternal(_internal, _config, RequestType::Write, nullptr,
                                  handle, OpenMode::None, offset,
                                  buffer, static_cast<uint32_t>(len), slot, cb, user);
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
  return enqueueInternal(_internal, _config, RequestType::Sync, nullptr,
                         handle, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
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
  return enqueueInternal(_internal, _config, RequestType::Mkdir, path,
                         INVALID_FILE_HANDLE, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
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
  return enqueueInternal(_internal, _config, RequestType::Remove, path,
                         INVALID_FILE_HANDLE, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
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
  return enqueueInternal(_internal, _config, RequestType::Stat, path,
                         INVALID_FILE_HANDLE, OpenMode::None, 0, nullptr, 0, 0xFF, cb, user)
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
  for (uint8_t i = 0; i < _internal->resDepth; ++i) {
    ResultSlot& slot = _internal->resQueue[i];
    if (slot.inUse && slot.result.id == id) {
      *out = slot.result;
      slot.inUse = false;
      _internal->resCount--;
      const uint8_t depth = _internal->resCount;
      portEXIT_CRITICAL(&_internal->queueMux);
      _internal->health.queueDepthResults.store(depth, std::memory_order_relaxed);
      found = true;
      return true;
    }
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
      _internal->resCount--;
      const uint8_t depth = _internal->resCount;
      _internal->resHead = static_cast<uint8_t>((idx + 1) % _internal->resDepth);
      portEXIT_CRITICAL(&_internal->queueMux);
      _internal->health.queueDepthResults.store(depth, std::memory_order_relaxed);
      return true;
    }
    idx = static_cast<uint8_t>((idx + 1) % _internal->resDepth);
  }

  portEXIT_CRITICAL(&_internal->queueMux);
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

  if (req.callback) {
    req.callback(result, req.user);
  }
  if (cfg.onResult) {
    cfg.onResult(result, cfg.onResultUser);
  }

  portENTER_CRITICAL(&st->queueMux);
  if (st->resCount >= st->resDepth) {
    portEXIT_CRITICAL(&st->queueMux);
    internal::incrementDroppedResults(st->health.droppedResults);
    setLastError(st, ErrorCode::Busy, Operation::ResultEnqueue, 0, req.path, req.length,
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
    return ErrorCode::InternalError;
  }
  const ErrorCode initCode = st->transport->begin(st, cfg);
  if (initCode != ErrorCode::Ok) {
    return initCode;
  }

  FsInfo info = buildFsInfo(st, false);
  if (info.fsType == FsType::Unknown) {
    st->transport->end(st);
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

void SdCardManager::workerStep(uint32_t budgetUs) {
  if (!_internal || !_internal->initialized) {
    return;
  }

  if (_config.useWorkerTask && _internal->workerRunning &&
      xTaskGetCurrentTaskHandle() != _internal->task) {
    return;
  }

  const uint32_t startUs = micros();
  const uint32_t nowMs = millis();

  if (_internal->health.stallActive.load(std::memory_order_relaxed)) {
    return;
  }

  updateCdPresence(_internal, _config, nowMs);

  // Auto-mount/unmount decisions
  if (!_internal->cdEnabled) {
    if (!_internal->mounted && _config.autoMount && _internal->backoff.shouldFire(nowMs)) {
      _internal->pendingAutoMount = true;
    }

    if (_internal->mounted &&
        deadlineReached(nowMs, _internal->lastProbeMs + _config.probeIntervalMs)) {
      const bool ok = probeCard(_internal, _config);
      _internal->lastProbeMs = nowMs;
      recordProgress(_internal, nowMs);
      if (ok) {
        _internal->probeFailures.recordSuccess();
      } else if (_internal->probeFailures.recordFailure()) {
        _internal->pendingAutoUnmount = true;
        _internal->probeFailures.reset();
      }
    }
  }

  uint8_t pendingRequests = 0;
  portENTER_CRITICAL(&_internal->queueMux);
  pendingRequests = _internal->reqCount;
  portEXIT_CRITICAL(&_internal->queueMux);

  const bool hasPendingWork = (pendingRequests > 0) || _internal->pendingAutoMount ||
                              _internal->pendingAutoUnmount;
  if (!hasPendingWork) {
    recordProgress(_internal, nowMs);
  }

  if (_config.workerStallMs > 0 && hasPendingWork) {
    const uint32_t lastProgress =
        _internal->health.lastProgressMs.load(std::memory_order_relaxed);
    if (internal::shouldStall(nowMs, lastProgress, _config.workerStallMs, hasPendingWork)) {
      bool expected = false;
      if (_internal->health.stallActive.compare_exchange_strong(
              expected, true, std::memory_order_relaxed)) {
        _internal->health.stallEvents.fetch_add(1, std::memory_order_relaxed);
        setLastError(_internal, ErrorCode::Fault, Operation::None, 0, nullptr, 0, 0);
        setStatus(_internal, SdStatus::Fault);
        recordFailure(_internal);
        _internal->pendingAutoMount = false;
        _internal->pendingAutoUnmount = false;
        failAllPending(_internal, _config, ErrorCode::Fault);
        (void)performUnmount(_internal, _config);
      }
      return;
    }
  }

  if (_internal->pendingAutoUnmount) {
    _internal->pendingAutoUnmount = false;
    ErrorCode unmountCode = performUnmount(_internal, _config);
    if (unmountCode != ErrorCode::Ok) {
      setLastError(_internal, unmountCode, Operation::Unmount, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Error);
      recordFailure(_internal);
      recordProgress(_internal, nowMs);
      _internal->pendingAutoUnmount = true;
      return;
    }
    setStatus(_internal, _internal->cdEnabled ? (_internal->cardPresent ? SdStatus::CardInserted
                                                                        : SdStatus::NoCard)
                                              : SdStatus::NoCard);
    recordSuccess(_internal);
    recordProgress(_internal, nowMs);
    failAllPending(_internal, _config, ErrorCode::NotReady);
    if (budgetExceeded(startUs, budgetUs)) {
      return;
    }
  }

  if (_internal->pendingAutoMount) {
    _internal->pendingAutoMount = false;
    const ErrorCode st = performMount(_internal, _config);
    if (st == ErrorCode::Ok) {
      setStatus(_internal, SdStatus::Ready);
      _internal->backoff.onSuccess(nowMs, _config.probeIntervalMs);
      recordSuccess(_internal);
    } else {
      setLastError(_internal, st, Operation::Mount, 0, nullptr, 0, 0);
      setStatus(_internal, SdStatus::Error);
      _internal->backoff.onFailure(nowMs);
      recordFailure(_internal);
    }
    recordProgress(_internal, nowMs);
    if (budgetExceeded(startUs, budgetUs)) {
      return;
    }
  }

  // Process request queue head
  portENTER_CRITICAL(&_internal->queueMux);
  if (_internal->reqCount == 0) {
    portEXIT_CRITICAL(&_internal->queueMux);
    return;
  }

  RequestSlot& slot = _internal->reqQueue[_internal->reqHead];
  if (!slot.inUse) {
    portEXIT_CRITICAL(&_internal->queueMux);
    return;
  }
  Request& req = slot.req;
  portEXIT_CRITICAL(&_internal->queueMux);

  const uint32_t nowReqMs = millis();
  if (deadlineReached(nowReqMs, req.deadlineMs)) {
    releaseCopySlot(_internal, req);
    enqueueResult(_internal, _config, req, ErrorCode::Timeout, 0, req.processed,
                  INVALID_FILE_HANDLE, FileStat{});
    setLastError(_internal, ErrorCode::Timeout, opFromRequest(req.type), 0, req.path,
                 req.length, req.processed);
    portENTER_CRITICAL(&_internal->queueMux);
    slot.inUse = false;
    _internal->reqHead = static_cast<uint8_t>((_internal->reqHead + 1) % _internal->reqDepth);
    _internal->reqCount--;
    const uint8_t depth = _internal->reqCount;
    portEXIT_CRITICAL(&_internal->queueMux);
    _internal->health.queueDepthRequests.store(depth, std::memory_order_relaxed);
    recordFailure(_internal);
    recordProgress(_internal, nowReqMs);
    return;
  }

  ErrorCode code = ErrorCode::Ok;
  int32_t detail = 0;
  uint32_t bytesDone = req.processed;
  FileHandle outHandle = INVALID_FILE_HANDLE;
  FileStat stat{};

  const bool isIoRequest = (req.type != RequestType::Mount && req.type != RequestType::Unmount);
  if (isIoRequest && _internal->mounted) {
    setStatus(_internal, SdStatus::Busy);
  }

  switch (req.type) {
    case RequestType::Mount: {
      code = performMount(_internal, _config);
      if (code == ErrorCode::Ok) {
        setStatus(_internal, SdStatus::Ready);
      } else {
        setStatus(_internal, SdStatus::Error);
      }
      break;
    }
    case RequestType::Unmount: {
      code = performUnmount(_internal, _config);
      if (code == ErrorCode::Ok) {
        setStatus(_internal, _internal->cdEnabled ? (_internal->cardPresent ? SdStatus::CardInserted
                                                                            : SdStatus::NoCard)
                                                  : SdStatus::NoCard);
      } else {
        setStatus(_internal, SdStatus::Error);
      }
      break;
    }
    case RequestType::Info: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      CardInfo cardInfo{};
      FsInfo fsInfo{};
      SdCard* card = _internal->sd ? _internal->sd->card() : nullptr;
      if (!card) {
        code = ErrorCode::InternalError;
        unlockBus(_internal);
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
        detail = _internal->sd ? _internal->sd->sdErrorCode() : 0;
      }

      fsInfo = buildFsInfo(_internal, true);
      setFsInfo(_internal, fsInfo);
      setCardInfo(_internal, cardInfo);
      unlockBus(_internal);
      break;
    }
    case RequestType::Open: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      const uint8_t flags = toSdFatFlags(req.openMode);
      const FileHandle handle = allocateFileSlot(_internal);
      if (handle == INVALID_FILE_HANDLE) {
        code = ErrorCode::TooManyOpenFiles;
        unlockBus(_internal);
        break;
      }
      FileSlot* slotFile = getFileSlot(_internal, handle);
      if (!slotFile || !slotFile->file.open(req.path, flags)) {
        if (slotFile) {
          slotFile->inUse = false;
        }
        detail = errno;
        code = ErrorCode::IoError;
        unlockBus(_internal);
        break;
      }
      unlockBus(_internal);
      outHandle = handle;
      break;
    }
    case RequestType::Close: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(_internal, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      slotFile->file.close();
      slotFile->inUse = false;
      unlockBus(_internal);
      break;
    }
    case RequestType::Read: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(_internal, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!req.started && req.offset != APPEND_OFFSET) {
        if (!slotFile->file.seekSet(req.offset)) {
          code = ErrorCode::IoError;
          unlockBus(_internal);
          break;
        }
      }
      req.started = true;
      const uint32_t remaining = req.length - req.processed;
      const uint32_t chunk =
          remaining > _config.ioChunkBytes ? _config.ioChunkBytes : remaining;
      const int32_t readCount =
          slotFile->file.read(static_cast<uint8_t*>(req.buffer) + req.processed, chunk);
      if (readCount < 0) {
        detail = errno;
        code = ErrorCode::IoError;
      } else {
        req.processed += static_cast<uint32_t>(readCount);
        bytesDone = req.processed;
      }
      unlockBus(_internal);
      const uint32_t ioMs = millis();
      if (code == ErrorCode::Ok && readCount > 0) {
        recordIoSuccess(_internal, ioMs);
      }
      if (code == ErrorCode::Ok && req.processed < req.length && readCount > 0 &&
          !budgetExceeded(startUs, budgetUs)) {
        recordProgress(_internal, ioMs);
        return;
      }
      break;
    }
    case RequestType::Write: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(_internal, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!req.started) {
        if (req.offset == APPEND_OFFSET) {
          if (!slotFile->file.seekEnd()) {
            code = ErrorCode::IoError;
            unlockBus(_internal);
            break;
          }
        } else {
          if (!slotFile->file.seekSet(req.offset)) {
            code = ErrorCode::IoError;
            unlockBus(_internal);
            break;
          }
        }
      }
      req.started = true;
      if (_internal->fsInfo.fsType == FsType::Fat32) {
        const uint64_t fileSize = slotFile->file.fileSize();
        const uint64_t base =
            (req.offset == APPEND_OFFSET) ? fileSize : req.offset;
        const uint64_t target = (fileSize > base) ? fileSize : base;
        if (target + static_cast<uint64_t>(req.length) > 0xFFFFFFFFULL) {
          code = ErrorCode::FileTooLarge;
          unlockBus(_internal);
          break;
        }
      }
      const uint32_t remaining = req.length - req.processed;
      const uint32_t chunk =
          remaining > _config.ioChunkBytes ? _config.ioChunkBytes : remaining;
      const size_t written =
          slotFile->file.write(static_cast<const uint8_t*>(req.buffer) + req.processed, chunk);
      if (written == 0) {
        detail = errno;
        code = (errno == ENOSPC) ? ErrorCode::NoSpaceLeft : ErrorCode::IoError;
      } else {
        req.processed += static_cast<uint32_t>(written);
        bytesDone = req.processed;
      }
      unlockBus(_internal);
      const uint32_t ioMs = millis();
      if (code == ErrorCode::Ok && written > 0) {
        recordIoSuccess(_internal, ioMs);
      }
      if (code == ErrorCode::Ok && req.processed < req.length &&
          !budgetExceeded(startUs, budgetUs)) {
        recordProgress(_internal, ioMs);
        return;
      }
      break;
    }
    case RequestType::Sync: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      FileSlot* slotFile = getFileSlot(_internal, req.handle);
      if (!slotFile) {
        code = ErrorCode::InvalidArgument;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!slotFile->file.sync()) {
        code = ErrorCode::IoError;
      } else {
        recordIoSuccess(_internal, millis());
      }
      unlockBus(_internal);
      break;
    }
    case RequestType::Mkdir: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!_internal->sd->mkdir(req.path, true)) {
        code = ErrorCode::IoError;
      }
      unlockBus(_internal);
      break;
    }
    case RequestType::Remove: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      if (!_internal->sd->remove(req.path)) {
        if (!_internal->sd->rmdir(req.path)) {
          code = ErrorCode::IoError;
        }
      }
      unlockBus(_internal);
      break;
    }
    case RequestType::Stat: {
      if (!_internal->mounted) {
        code = ErrorCode::NotReady;
        break;
      }
      if (!lockBus(_internal, _config)) {
        code = ErrorCode::BusNotAvailable;
        break;
      }
      FsFile f;
      if (!f.open(req.path, O_RDONLY)) {
        code = ErrorCode::IoError;
        unlockBus(_internal);
        break;
      }
      stat.size = f.fileSize();
      stat.isDir = f.isDir();
      f.close();
      unlockBus(_internal);
      break;
    }
    default:
      code = ErrorCode::InternalError;
      break;
  }

  if (isIoRequest) {
    if (code == ErrorCode::Ok) {
      if (_internal->mounted) {
        setStatus(_internal, SdStatus::Ready);
      }
    } else if (code == ErrorCode::NotReady) {
      setStatus(_internal, _internal->cdEnabled ? (_internal->cardPresent ? SdStatus::CardInserted
                                                                          : SdStatus::NoCard)
                                                : SdStatus::NoCard);
    } else {
      setStatus(_internal, SdStatus::Error);
    }
  }

  releaseCopySlot(_internal, req);
  enqueueResult(_internal, _config, req, code, detail, bytesDone, outHandle, stat);
  if (code != ErrorCode::Ok) {
    setLastError(_internal, code, opFromRequest(req.type), detail, req.path, req.length,
                 bytesDone);
    if (!_internal->cdEnabled && (code == ErrorCode::IoError || code == ErrorCode::NotReady)) {
      if (_internal->probeFailures.recordFailure()) {
        _internal->pendingAutoUnmount = true;
        _internal->probeFailures.reset();
      }
    }
    recordFailure(_internal);
  } else {
    recordSuccess(_internal);
  }

  recordProgress(_internal, millis());
  portENTER_CRITICAL(&_internal->queueMux);
  slot.inUse = false;
  _internal->reqHead = static_cast<uint8_t>((_internal->reqHead + 1) % _internal->reqDepth);
  _internal->reqCount--;
  const uint8_t depth = _internal->reqCount;
  portEXIT_CRITICAL(&_internal->queueMux);
  _internal->health.queueDepthRequests.store(depth, std::memory_order_relaxed);
}

}  // namespace AsyncSD

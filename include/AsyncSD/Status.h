/**
 * @file Status.h
 * @brief Status and error types for AsyncSD.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace AsyncSD {

/// @brief Request identifier type. 0 is invalid.
using RequestId = uint32_t;

/// @brief File handle type. 0 is invalid.
using FileHandle = uint16_t;

/// @brief Invalid request identifier constant.
static constexpr RequestId INVALID_REQUEST_ID = 0;

/// @brief Invalid file handle constant.
static constexpr FileHandle INVALID_FILE_HANDLE = 0;

/// @brief High-level SD card status.
enum class SdStatus : uint8_t {
  Disabled = 0,
  NoCard,
  CardInserted,
  Initializing,
  Mounting,
  Ready,
  Busy,
  Removed,
  Error,
  Fault
};

/// @brief Error code enumeration for AsyncSD.
enum class ErrorCode : uint16_t {
  Ok = 0,
  Timeout,
  Busy,
  NoMem,
  Fault,
  BusNotAvailable,
  CardInitFailed,
  MountFailed,
  FsUnsupported,
  FsCorrupt,
  IoError,
  ReadOnly,
  WriteProtect,
  NoSpaceLeft,
  FileTooLarge,
  TooManyOpenFiles,
  PathTooLong,
  NameTooLong,
  InvalidArgument,
  InvalidContext,
  NotReady,
  NotInitialized,
  Unsupported,
  InternalError,
  NotFound,
  AlreadyExists
};

/// @brief Operation enum for structured error reporting.
enum class Operation : uint8_t {
  None = 0,
  Begin,
  End,
  Shutdown,
  Mount,
  Unmount,
  Info,
  Open,
  Close,
  Read,
  Write,
  Sync,
  Mkdir,
  Remove,
  Stat,
  Probe,
  Lock,
  Enqueue,
  ResultEnqueue,
  Rename
};

/// @brief Filesystem type.
enum class FsType : uint8_t {
  Unknown = 0,
  Fat12,
  Fat16,
  Fat32,
  ExFat
};

/// @brief SD card type.
enum class CardType : uint8_t {
  Unknown = 0,
  Sd1,
  Sd2,
  SdHC
};

/// @brief Request type enum.
enum class RequestType : uint8_t {
  Mount = 0,
  Unmount,
  Info,
  Open,
  Close,
  Read,
  Write,
  Sync,
  Mkdir,
  Remove,
  Stat,
  Rename
};

/// @brief Structured error info snapshot.
struct ErrorInfo {
  /// @brief Error code.
  ErrorCode code = ErrorCode::Ok;

  /// @brief Operation that failed.
  Operation op = Operation::None;

  /// @brief Raw underlying error detail, if available.
  int32_t detail = 0;

  /// @brief Timestamp of error (millis).
  uint32_t timestampMs = 0;

  /// @brief Bytes requested (if applicable).
  uint32_t bytesRequested = 0;

  /// @brief Bytes processed (if applicable).
  uint32_t bytesProcessed = 0;

  /// @brief Path involved in the failure (may be truncated).
  /// @note Pointer is valid until the next error update.
  const char* path = nullptr;
};

/// @brief File stat information returned by requestStat.
struct FileStat {
  /// @brief File size in bytes.
  uint64_t size = 0;

  /// @brief True if entry is a directory.
  bool isDir = false;
};

/// @brief Filesystem info snapshot.
struct FsInfo {
  /// @brief Filesystem type.
  FsType fsType = FsType::Unknown;

  /// @brief Filesystem capacity in bytes.
  uint64_t capacityBytes = 0;

  /// @brief Used bytes (if available).
  uint64_t usedBytes = 0;

  /// @brief Free bytes (if available).
  uint64_t freeBytes = 0;

  /// @brief Cluster count.
  uint32_t clusterCount = 0;

  /// @brief Free clusters (if available).
  uint32_t freeClusters = 0;

  /// @brief Sectors per cluster.
  uint32_t sectorsPerCluster = 0;

  /// @brief Bytes per cluster.
  uint32_t bytesPerCluster = 0;

  /// @brief True if usedBytes is valid.
  bool usedBytesValid = false;

  /// @brief True if freeBytes is valid.
  bool freeBytesValid = false;

  /// @brief True if freeClusters is valid.
  bool freeClustersValid = false;
};

/// @brief SD card information snapshot.
struct CardInfo {
  /// @brief Card type (SD1/SD2/SDHC).
  CardType type = CardType::Unknown;

  /// @brief Total card sectors (512-byte units).
  uint64_t sectorCount = 0;

  /// @brief Total card capacity in bytes.
  uint64_t capacityBytes = 0;

  /// @brief Raw card status register.
  uint32_t cardStatus = 0;

  /// @brief Raw OCR register.
  uint32_t ocr = 0;

  /// @brief CID register bytes (16).
  uint8_t cid[16]{};

  /// @brief CSD register bytes (16).
  uint8_t csd[16]{};

  /// @brief SCR register bytes (8).
  uint8_t scr[8]{};

  /// @brief SD Status register bytes (64).
  uint8_t sds[64]{};

  /// @brief True if cardStatus is valid.
  bool cardStatusValid = false;

  /// @brief True if ocr is valid.
  bool ocrValid = false;

  /// @brief True if cid is valid.
  bool cidValid = false;

  /// @brief True if csd is valid.
  bool csdValid = false;

  /// @brief True if scr is valid.
  bool scrValid = false;

  /// @brief True if sds is valid.
  bool sdsValid = false;
};

/// @brief Worker health metrics snapshot.
struct WorkerHealth {
  /// @brief Timestamp of last worker progress (millis).
  uint32_t lastProgressMs = 0;

  /// @brief Timestamp of last successful read/write/sync (millis).
  uint32_t lastSuccessfulIoMs = 0;

  /// @brief Timestamp of last error (millis).
  uint32_t lastErrorMs = 0;

  /// @brief Consecutive failure counter.
  uint32_t consecutiveFailures = 0;

  /// @brief Current request queue depth.
  uint32_t queueDepthRequests = 0;

  /// @brief Current result queue depth.
  uint32_t queueDepthResults = 0;

  /// @brief Number of detected worker stalls.
  uint32_t stallEvents = 0;

  /// @brief Current worker status.
  SdStatus currentStatus = SdStatus::Disabled;

  /// @brief Last error code observed by the worker.
  ErrorCode lastErrorCode = ErrorCode::Ok;
};

/// @brief Result for an asynchronous request.
struct RequestResult {
  /// @brief Request id.
  RequestId id = INVALID_REQUEST_ID;

  /// @brief Request type.
  RequestType type = RequestType::Mount;

  /// @brief Result code.
  ErrorCode code = ErrorCode::Ok;

  /// @brief Raw underlying error detail, if available.
  int32_t detail = 0;

  /// @brief Completion timestamp (millis).
  uint32_t timestampMs = 0;

  /// @brief Bytes requested (if applicable).
  uint32_t bytesRequested = 0;

  /// @brief Bytes processed (if applicable).
  uint32_t bytesProcessed = 0;

  /// @brief File handle (for open requests).
  FileHandle handle = INVALID_FILE_HANDLE;

  /// @brief File stat result (for stat requests).
  FileStat stat{};

  /// @brief Filesystem info snapshot (for info requests).
  FsInfo fsInfo{};

  /// @brief Card info snapshot (for info requests).
  CardInfo cardInfo{};
};

/// @brief Result callback type (invoked in worker context).
using ResultCallback = void (*)(const RequestResult& result, void* user);

}  // namespace AsyncSD

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
  NotReady,
  NotInitialized,
  Unsupported,
  InternalError
};

/// @brief Operation enum for structured error reporting.
enum class Operation : uint8_t {
  None = 0,
  Begin,
  End,
  Mount,
  Unmount,
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
  Enqueue
};

/// @brief Filesystem type.
enum class FsType : uint8_t {
  Unknown = 0,
  Fat12,
  Fat16,
  Fat32,
  ExFat
};

/// @brief Request type enum.
enum class RequestType : uint8_t {
  Mount = 0,
  Unmount,
  Open,
  Close,
  Read,
  Write,
  Sync,
  Mkdir,
  Remove,
  Stat
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

  /// @brief Total capacity in bytes.
  uint64_t capacityBytes = 0;

  /// @brief Used bytes (if available).
  uint64_t usedBytes = 0;

  /// @brief True if usedBytes is valid.
  bool usedBytesValid = false;
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
};

}  // namespace AsyncSD

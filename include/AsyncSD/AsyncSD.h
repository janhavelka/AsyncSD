/**
 * @file AsyncSD.h
 * @brief Public API for AsyncSD SD card manager.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "AsyncSD/Config.h"
#include "AsyncSD/Status.h"

namespace AsyncSD {

struct Internal;

/// @brief File open mode flags.
enum class OpenMode : uint8_t {
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,
  Create = 1 << 2,
  Truncate = 1 << 3,
  Append = 1 << 4,
  Exclusive = 1 << 5
};

/// @brief Bitwise OR for OpenMode.
inline OpenMode operator|(OpenMode a, OpenMode b) {
  return static_cast<OpenMode>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/// @brief Bitwise AND for OpenMode.
inline OpenMode operator&(OpenMode a, OpenMode b) {
  return static_cast<OpenMode>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/// @brief Check if a flag is present.
inline bool hasFlag(OpenMode value, OpenMode flag) {
  return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

/// @brief Special offset to append to end of file.
static constexpr uint64_t APPEND_OFFSET = UINT64_MAX;

/// @brief Result callback type (invoked in worker context).
using ResultCallback = void (*)(const RequestResult& result, void* user);

/**
 * @brief SPI bus guard interface for shared bus ownership.
 *
 * The library never owns the SPI bus. Provide a guard that integrates
 * with your application's SPI locking strategy.
 */
class ISpiBusGuard {
 public:
  virtual ~ISpiBusGuard() = default;

  /// @brief Attempt to lock the bus within timeout.
  /// @param timeoutMs Maximum time to wait for lock.
  /// @return true if lock acquired, false otherwise.
  virtual bool lock(uint32_t timeoutMs) = 0;

  /// @brief Unlock the bus.
  virtual void unlock() = 0;

  /// @brief Optional hook called after lock acquisition.
  virtual void beginTransaction() {}

  /// @brief Optional hook called before unlocking.
  virtual void endTransaction() {}
};

/**
 * @brief Async SD card manager for ESP32-S2/S3.
 *
 * All public APIs are nonblocking. Blocking SD operations execute in
 * a worker task or via explicit workerStep() calls.
 *
 * @note Not thread-safe for public API calls. Call from one task.
 */
class SdCardManager {
 public:
  SdCardManager();
  ~SdCardManager();
  SdCardManager(const SdCardManager&) = delete;
  SdCardManager& operator=(const SdCardManager&) = delete;

  /**
   * @brief Initialize with configuration and optional SPI guard.
   * @param config Configuration parameters.
   * @param guard SPI bus guard (nullable).
   * @return true on success, false if configuration invalid.
   * @note Allocates internal buffers and may start a worker task.
   */
  bool begin(const SdCardConfig& config, ISpiBusGuard* guard = nullptr);

  /**
   * @brief Stop the library and release resources.
   * @note Safe to call multiple times.
   */
  void end();

  /**
   * @brief Worker step for externally-driven operation.
   * @param budgetUs Max time budget for this step (microseconds).
   * @note Use when useWorkerTask=false.
   */
  void workerStep(uint32_t budgetUs);

  /**
   * @brief Optional poll hook (lightweight). No-op when worker task is running.
   */
  void poll();

  /**
   * @brief Get current status.
   */
  SdStatus status() const;

  /**
   * @brief Get last error code.
   */
  ErrorCode lastError() const;

  /**
   * @brief Get structured error info snapshot.
   */
  ErrorInfo lastErrorInfo() const;

  /**
   * @brief Get worker health metrics snapshot.
   * @note Snapshot uses atomics and may be slightly stale.
   */
  WorkerHealth getWorkerHealth() const;

  /**
   * @brief Get filesystem info snapshot.
   */
  FsInfo fsInfo() const;

  /**
   * @brief Check if filesystem is mounted and ready.
   */
  bool isReady() const;

  /**
   * @brief Get active configuration.
   */
  const SdCardConfig& config() const { return _config; }

  // ---------------------------
  // Asynchronous request API
  // ---------------------------

  /**
   * @brief Enqueue a mount request.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   * @note Nonblocking. Uses bounded request queue.
   */
  RequestId requestMount(ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue an unmount request.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   * @note Nonblocking. Uses bounded request queue.
   */
  RequestId requestUnmount(ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue an open request.
   * @param path File path (may include mountPoint prefix).
   * @param mode OpenMode flags.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   * @note Nonblocking. File handle returned in RequestResult.
   */
  RequestId requestOpen(const char* path, OpenMode mode,
                        ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue a close request.
   * @param handle File handle from requestOpen.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   * @note Nonblocking.
   */
  RequestId requestClose(FileHandle handle,
                         ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue a read request.
   * @param handle File handle.
   * @param offset Byte offset to seek before reading.
   * @param dst Destination buffer (must remain valid until result).
   * @param len Number of bytes to read.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   * @note Nonblocking. Read is chunked internally.
   */
  RequestId requestRead(FileHandle handle, uint64_t offset, void* dst, size_t len,
                        ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue a write request.
   * @param handle File handle.
   * @param offset Byte offset to seek before writing, or APPEND_OFFSET.
   * @param src Source buffer (must remain valid until result).
   * @param len Number of bytes to write.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   * @note Nonblocking. Write is chunked internally.
   */
  RequestId requestWrite(FileHandle handle, uint64_t offset, const void* src, size_t len,
                         ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue a sync (flush) request.
   * @param handle File handle.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   */
  RequestId requestSync(FileHandle handle,
                        ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue a mkdir request.
   * @param path Directory path.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   */
  RequestId requestMkdir(const char* path,
                         ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue a remove request (file or directory).
   * @param path Path to remove.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   */
  RequestId requestRemove(const char* path,
                          ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Enqueue a stat request.
   * @param path Path to stat.
   * @param cb Optional callback invoked on completion (worker context).
   * @param user User context pointer passed to callback.
   * @return RequestId or INVALID_REQUEST_ID if enqueue failed.
   */
  RequestId requestStat(const char* path,
                        ResultCallback cb = nullptr, void* user = nullptr);

  /**
   * @brief Retrieve a completed result by request id.
   * @param id Request id.
   * @param out Output result structure.
   * @return true if found and copied.
   */
  bool getResult(RequestId id, RequestResult* out);

  /**
   * @brief Pop the oldest completed result.
   * @param out Output result structure.
   * @return true if a result was popped.
   */
  bool popResult(RequestResult* out);

 private:
  Internal* _internal = nullptr;
  SdCardConfig _config{};
};

}  // namespace AsyncSD

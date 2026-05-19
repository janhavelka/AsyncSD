/**
 * @file Config.h
 * @brief Configuration structure for AsyncSD.
 */

#pragma once

#include <stdint.h>

#if defined(ARDUINO)
#include <SPI.h>
#else
class SPIClass;
#endif

#include "AsyncSD/Status.h"

namespace AsyncSD {

/// @brief Backend implementation selected at compile/runtime boundary.
enum class Backend : uint8_t {
  /// @brief Arduino + SdFat v2 backend. This is the current production backend.
  ARDUINO_SDFAT = 0,

  /// @brief ESP-IDF VFS/POSIX backend. Contract is public; implementation is pending.
  IDF_VFS
};

/// @brief Transport selection for SD access.
enum class TransportType : uint8_t {
  Spi = 0,
  Sdmmc
};

/**
 * @brief ESP-IDF VFS adapter callbacks.
 *
 * The IDF backend does not own SDMMC/SDSPI host setup, GPIO, or FatFS mounting.
 * Applications provide an already configured VFS mount point and optional
 * bounded lock/mount/status callbacks.
 */
struct IdfVfsAdapter {
  /// @brief Mounted VFS path, for example "/sdcard".
  const char* mountPoint = "/sdcard";

  /// @brief Opaque application context passed to callbacks.
  void* user = nullptr;

  /// @brief Optional callback returning whether the VFS path is mounted.
  bool (*isMounted)(void* user) = nullptr;

  /// @brief Optional callback returning card presence when known.
  bool (*isPresent)(void* user) = nullptr;

  /// @brief Optional monotonic millisecond clock callback.
  uint32_t (*nowMs)(void* user) = nullptr;

  /// @brief Optional bounded lock callback for shared VFS/card access.
  ErrorCode (*lock)(uint32_t timeoutMs, void* user) = nullptr;

  /// @brief Optional unlock callback paired with lock().
  void (*unlock)(void* user) = nullptr;

  /// @brief Optional bounded application-owned mount callback.
  ErrorCode (*mount)(uint32_t timeoutMs, void* user) = nullptr;

  /// @brief Optional bounded application-owned unmount callback.
  ErrorCode (*unmount)(uint32_t timeoutMs, void* user) = nullptr;
};

/**
 * @brief Configuration for AsyncSD initialization.
 *
 * All hardware-specific parameters (pins, SPI instance) are injected here.
 * The library never hardcodes pins or owns the SPI bus.
 */
struct SdCardConfig {
  // ---------------------------
  // Transport
  // ---------------------------

  /// @brief Backend implementation. Arduino builds default to SdFat; pure IDF defaults to VFS.
#if defined(ESP_PLATFORM) && !defined(ARDUINO)
  Backend backend = Backend::IDF_VFS;
#else
  Backend backend = Backend::ARDUINO_SDFAT;
#endif

  /// @brief Transport type (SPI now, SDMMC reserved for ESP32-S3).
  TransportType transport = TransportType::Spi;

  /// @brief Mount point prefix for user paths (logical only).
  /// @note Used to strip prefix if present. Defaults to "/sd".
  const char* mountPoint = "/sd";

  /// @brief ESP-IDF VFS adapter. Used only when backend == Backend::IDF_VFS.
  IdfVfsAdapter idfVfs{};

  // ---------------------------
  // SPI configuration
  // ---------------------------

  /// @brief SPI bus instance (default: &SPI).
#if defined(ARDUINO)
  SPIClass* spi = &SPI;
#else
  SPIClass* spi = nullptr;
#endif

  /// @brief SPI chip select pin (required for SPI transport).
  /// @note Set to -1 to disable SPI transport.
  int pinCs = 10;

  /// @brief SPI MOSI pin. Used only if autoInitSpi is true.
  int pinMosi = 11;

  /// @brief SPI MISO pin. Used only if autoInitSpi is true.
  int pinMiso = 13;

  /// @brief SPI SCK pin. Used only if autoInitSpi is true.
  int pinSck = 12;

  /// @brief If true, library calls spi->begin(...). Default false (app owns bus).
  bool autoInitSpi = false;

  /// @brief SPI clock frequency in Hz.
  uint32_t spiFrequencyHz = 25000000;

  /// @brief SPI mode (0-3). Default SPI_MODE0.
#if defined(SPI_MODE0)
  uint8_t spiMode = SPI_MODE0;
#else
  uint8_t spiMode = 0;
#endif

  /// @brief True to configure SdFat for shared SPI (recommended).
  bool spiShared = true;

  // ---------------------------
  // Card Detect (CD)
  // ---------------------------

  /// @brief Card detect pin. Set to -1 to disable CD functionality.
  int cdPin = -1;

  /// @brief True if CD is active-low (most sockets). Default true.
  bool cdActiveLow = true;

  /// @brief Enable CD interrupt (ISR sets a flag only). Default false.
  bool cdUseInterrupt = false;

  /// @brief Enable internal pullup for CD pin when cdActiveLow is true.
  bool cdPullup = true;

  /// @brief CD debounce window in milliseconds.
  uint32_t cdDebounceMs = 50;

  /// @brief CD poll period when interrupts are disabled (ms).
  uint32_t cdPollMs = 20;

  // ---------------------------
  // Timeouts and limits
  // ---------------------------

  /// @brief SPI bus lock timeout in milliseconds.
  uint32_t lockTimeoutMs = 50;

  /// @brief Mount/unmount timeout in milliseconds.
  uint32_t mountTimeoutMs = 5000;

  /// @brief Generic file operation timeout in milliseconds.
  uint32_t opTimeoutMs = 3000;

  /// @brief Read/write I/O timeout in milliseconds.
  uint32_t ioTimeoutMs = 2000;

  /// @brief Max bytes per read/write chunk (bounded worker step).
  uint32_t ioChunkBytes = 512;

  // ---------------------------
  // No-CD probe policy
  // ---------------------------

  /// @brief Probe interval when mounted and no CD pin (ms).
  uint32_t probeIntervalMs = 2000;

  /// @brief Initial probe backoff when not mounted (ms).
  uint32_t probeBackoffMinMs = 250;

  /// @brief Maximum probe backoff when not mounted (ms).
  uint32_t probeBackoffMaxMs = 10000;

  /// @brief Consecutive probe failures before declaring removal.
  uint8_t probeFailThreshold = 3;

  // ---------------------------
  // Worker task configuration
  // ---------------------------

  /// @brief Use library-owned worker task. Default true.
  bool useWorkerTask = true;

  /// @brief Worker task stack size in bytes.
  uint16_t workerStackBytes = 4096;

  /// @brief Worker task priority.
  uint8_t workerPriority = 1;

  /// @brief Worker task core pinning (-1 = no pinning).
  int8_t workerCore = -1;

  /// @brief Worker task idle delay when no work (ms).
  uint32_t workerIdleMs = 5;

  /// @brief Worker step time budget in microseconds.
  uint32_t workerBudgetUs = 2000;

  /// @brief Worker stall detection threshold (ms). Set to 0 to disable.
  uint32_t workerStallMs = 5000;

  /// @brief Timeout waiting for worker shutdown (ms).
  uint32_t shutdownTimeoutMs = 250;

  // ---------------------------
  // Queues and resource limits
  // ---------------------------

  /// @brief Request queue depth (bounded).
  uint8_t requestQueueDepth = 8;

  /// @brief Result queue depth (bounded).
  uint8_t resultQueueDepth = 8;

  /// @brief Maximum number of open files.
  uint8_t maxOpenFiles = 4;

  /// @brief Maximum path length stored in requests (including NUL).
  uint16_t maxPathLength = 96;

  /// @brief Maximum bytes to copy for requestWriteCopy().
  uint16_t maxCopyWriteBytes = 512;

  /// @brief Copy-write slot count for requestWriteCopy().
  uint8_t copyWriteSlots = 2;

  // ---------------------------
  // Behavior flags
  // ---------------------------

  /// @brief Automatically mount when a card is detected.
  bool autoMount = true;

  /// @brief Enable invoking callbacks from worker context. Default false.
  bool enableWorkerCallbacks = false;

  /// @brief Optional global result callback (worker context).
  /// @note Only used when enableWorkerCallbacks is true.
  ResultCallback onResult = nullptr;

  /// @brief User context for onResult callback.
  void* onResultUser = nullptr;
};

}  // namespace AsyncSD

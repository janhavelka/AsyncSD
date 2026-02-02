/**
 * @file Config.h
 * @brief Configuration structure for AsyncSD.
 */

#pragma once

#include <stdint.h>

#include <SPI.h>

namespace AsyncSD {

/// @brief Transport selection for SD access.
enum class TransportType : uint8_t {
  Spi = 0,
  Sdmmc
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

  /// @brief Transport type (SPI now, SDMMC reserved for ESP32-S3).
  TransportType transport = TransportType::Spi;

  /// @brief Mount point prefix for user paths (logical only).
  /// @note Used to strip prefix if present. Defaults to "/sd".
  const char* mountPoint = "/sd";

  // ---------------------------
  // SPI configuration
  // ---------------------------

  /// @brief SPI bus instance (default: &SPI).
  SPIClass* spi = &SPI;

  /// @brief SPI chip select pin (required for SPI transport).
  /// @note Set to -1 to disable SPI transport.
  int pinCs = -1;

  /// @brief SPI MOSI pin. Used only if autoInitSpi is true.
  int pinMosi = -1;

  /// @brief SPI MISO pin. Used only if autoInitSpi is true.
  int pinMiso = -1;

  /// @brief SPI SCK pin. Used only if autoInitSpi is true.
  int pinSck = -1;

  /// @brief If true, library calls spi->begin(...). Default false (app owns bus).
  bool autoInitSpi = false;

  /// @brief SPI clock frequency in Hz.
  uint32_t spiFrequencyHz = 25000000;

  /// @brief SPI mode (0-3). Default SPI_MODE0.
  uint8_t spiMode = SPI_MODE0;

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

  // ---------------------------
  // Behavior flags
  // ---------------------------

  /// @brief Automatically mount when a card is detected.
  bool autoMount = true;
};

}  // namespace AsyncSD

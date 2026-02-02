/**
 * @file BoardPins.h
 * @brief Example default pin mapping for ESP32-S2 / ESP32-S3 reference hardware.
 *
 * These are convenience defaults for reference hardware only.
 * NOT part of the library API. Override for your hardware.
 */

#pragma once

namespace pins {

// ====================================================================
// EXAMPLE DEFAULT PIN MAPPING - ESP32-S2 / ESP32-S3 REFERENCE HARDWARE
// ====================================================================
// These pins are NOT library defaults. They are example-only values.
// Override them for your board by creating your own BoardPins.h or
// passing explicit values to Config structs in your application.
// ====================================================================

/// @brief SPI MOSI pin. Example default for ESP32-S2/S3.
static constexpr int SPI_MOSI = 11;

/// @brief SPI SCK pin. Example default for ESP32-S2/S3.
static constexpr int SPI_SCK = 12;

/// @brief SPI MISO pin. Example default for ESP32-S2/S3.
static constexpr int SPI_MISO = 13;

/// @brief SD card CS pin. Example default for ESP32-S2/S3.
static constexpr int SD_CS = 10;

/// @brief SD card CD (card-detect) pin. Set to -1 if not wired.
static constexpr int SD_CD = -1;

/// @brief LED pin. Example default for ESP32-S3 (GPIO48).
static constexpr int LED = 48;

}  // namespace pins

/**
 * @file BoardPins.h
 * @brief Example default pin mapping for ESP32-S2 / ESP32-S3 reference hardware.
 *
 * These are convenience defaults for our reference designs only.
 * NOT part of the library API. Override for your hardware.
 *
 * @warning The library itself is pin-agnostic. All pins are passed via Config.
 *          These defaults are provided for examples only.
 */

#pragma once

#include <stdint.h>

namespace pins {

// ====================================================================
// EXAMPLE DEFAULT PIN MAPPING - ESP32-S2 / ESP32-S3 REFERENCE HARDWARE
// ====================================================================
// These pins are NOT library defaults. They are example-only values.
// Override them for your board by creating your own BoardPins.h or
// passing explicit values to Config structs in your application.
// ====================================================================

/// @brief I2C SDA pin (data line). Example default for ESP32-S2/S3.
/// Override for your hardware.
static constexpr int SDA = 8;

/// @brief I2C SCL pin (clock line). Example default for ESP32-S2/S3.
/// Override for your hardware.
static constexpr int SCL = 9;

/// @brief SPI MOSI pin (master out, slave in). Example default for ESP32-S2/S3.
/// Override for your hardware.
static constexpr int SPI_MOSI = 11;

/// @brief SPI SCK pin (serial clock). Example default for ESP32-S2/S3.
/// Override for your hardware.
static constexpr int SPI_SCK = 12;

/// @brief SPI MISO pin (master in, slave out). Example default for ESP32-S2/S3.
/// Override for your hardware.
static constexpr int SPI_MISO = 13;

/// @brief LED pin. Example default for ESP32-S3 (RGB LED on GPIO48).
/// Override for your hardware. Set to -1 to disable.
static constexpr int LED = 48;

}  // namespace pins

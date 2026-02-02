/**
 * @file Config.h
 * @brief Configuration structure for YourLib.
 *
 * All hardware-specific parameters (pins, interfaces) are injected via this
 * struct. The library never hardcodes pin values.
 */

#pragma once

#include <stdint.h>

namespace YourLibrary {

/**
 * @brief Configuration for YourLib initialization.
 *
 * Pass to YourLib::begin() to configure the library. All pin values default
 * to -1 (disabled). Set only the pins your application uses.
 *
 * @note Pin values are board-specific. Define them in your application or
 *       example code, not in the library.
 */
struct Config {
  /// @brief GPIO pin for LED output. Set to -1 to disable LED functionality.
  /// @note Application-provided. Library does not define pin defaults.
  int ledPin = -1;

  /// @brief GPIO pin for UART RX. Set to -1 if UART is not used.
  /// @note Application-provided. Library does not define pin defaults.
  int uartRxPin = -1;

  /// @brief GPIO pin for UART TX. Set to -1 if UART is not used.
  /// @note Application-provided. Library does not define pin defaults.
  int uartTxPin = -1;

  /// @brief Interval in milliseconds for periodic tick actions.
  /// @note Must be > 0. Validated in begin().
  uint32_t intervalMs = 1000;
};

}  // namespace YourLibrary

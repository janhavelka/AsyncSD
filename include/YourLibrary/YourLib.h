/**
 * @file YourLib.h
 * @brief Main library class for YourLib.
 *
 * Provides a non-blocking, cooperative library with begin/tick/end lifecycle.
 * All configuration is injected; no hardcoded pins or hidden state.
 */

#pragma once

#include <stdint.h>

#include "YourLibrary/Config.h"
#include "YourLibrary/Status.h"

namespace YourLibrary {

/**
 * @brief Main library class implementing begin/tick/end lifecycle.
 *
 * Usage:
 * @code
 * YourLibrary::YourLib lib;
 *
 * void setup() {
 *   YourLibrary::Config config;
 *   config.ledPin = 48;
 *   config.intervalMs = 1000;
 *   auto st = lib.begin(config);
 *   if (!st.ok()) { // handle error }
 * }
 *
 * void loop() {
 *   lib.tick(millis());
 * }
 * @endcode
 *
 * @note This class is not thread-safe. Call all methods from the same
 *       task/thread (typically Arduino loop()).
 * @note Do not call from ISRs.
 */
class YourLib {
 public:
  /**
   * @brief Initialize the library with the given configuration.
   *
   * Must be called before tick(). Can be called again after end() to
   * reinitialize with different settings.
   *
   * @param config Configuration struct with pins and parameters.
   * @return Status Ok on success, or error with details.
   *
   * @note Validates config.intervalMs > 0.
   * @note If ledPin >= 0, configures GPIO as output.
   */
  Status begin(const Config& config);

  /**
   * @brief Stop the library and release resources.
   *
   * Safe to call multiple times. After end(), isInitialized() returns false.
   * Call begin() to restart.
   */
  void end();

  /**
   * @brief Cooperative update function. Call from loop().
   *
   * Performs periodic actions based on configured interval. Returns
   * immediately if not enough time has elapsed.
   *
   * @param now_ms Current time in milliseconds (typically from millis()).
   *
   * @note Non-blocking. Safe to call every loop iteration.
   * @note Does nothing if begin() was not called or end() was called.
   * @note Handles millis() wraparound correctly (occurs after ~49.7 days).
   */
  void tick(uint32_t now_ms);

  /**
   * @brief Check if library is currently initialized.
   * @return true if begin() succeeded and end() has not been called.
   */
  bool isInitialized() const { return _initialized; }

  /**
   * @brief Get current configuration.
   * @return Reference to active configuration struct.
   * @note Only valid if isInitialized() returns true.
   */
  const Config& getConfig() const { return _config; }

  /**
   * @brief Get scheduled time for next tick action.
   * @return Timestamp in milliseconds when next action will occur.
   * @note Useful for debugging or synchronization with other timers.
   */
  uint32_t getNextTickMs() const { return _nextMs; }

  /**
   * @brief Get configured interval in milliseconds.
   * @return Configured intervalMs from begin().
   */
  uint32_t getIntervalMs() const { return _config.intervalMs; }

  /**
   * @brief Get tick count since begin().
   * @return Number of internal ticks executed.
   */
  uint32_t getTickCount() const { return _tickCount; }

  /**
   * @brief Get timestamp of last executed tick.
   * @return Timestamp in milliseconds when last tick work was performed.
   * @note Returns 0 if no ticks have been executed yet.
   */
  uint32_t getLastTickMs() const { return _lastTickMs; }

 private:
  Config _config{};
  bool _initialized = false;
  uint32_t _nextMs = 0;
  uint32_t _tickCount = 0;
  uint32_t _lastTickMs = 0;
};

}  // namespace YourLibrary

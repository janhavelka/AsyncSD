/**
 * @file YourLib.cpp
 * @brief Implementation of YourLib.
 */

#include "YourLibrary/YourLib.h"

#include <Arduino.h>

namespace YourLibrary {

Status YourLib::begin(const Config& config) {
  // Validate configuration
  if (config.intervalMs == 0) {
    return Status(Err::INVALID_CONFIG, 0, "intervalMs must be > 0");
  }

  _config = config;
  _initialized = true;
  _nextMs = 0;
  _tickCount = 0;
  _lastTickMs = 0;

  // Configure LED pin if provided
  if (_config.ledPin >= 0) {
    pinMode(static_cast<uint8_t>(_config.ledPin), OUTPUT);
    digitalWrite(static_cast<uint8_t>(_config.ledPin), LOW);
  }

  return Ok();
}

void YourLib::end() {
  if (!_initialized) {
    return;
  }

  _initialized = false;

  // Turn off LED if configured
  if (_config.ledPin >= 0) {
    digitalWrite(static_cast<uint8_t>(_config.ledPin), LOW);
  }
}

void YourLib::tick(uint32_t now_ms) {
  if (!_initialized) {
    return;
  }

  // Wrap-safe deadline check using signed delta
  if (static_cast<int32_t>(now_ms - _nextMs) >= 0) {
    _nextMs = now_ms + _config.intervalMs;
    _tickCount++;
    _lastTickMs = now_ms;

    // Example action: toggle LED if configured
    if (_config.ledPin >= 0) {
      const uint8_t pin = static_cast<uint8_t>(_config.ledPin);
      const int current = digitalRead(pin);
      digitalWrite(pin, current ? LOW : HIGH);
    }
  }
}

}  // namespace YourLibrary

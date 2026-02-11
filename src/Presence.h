/**
 * @file Presence.h
 * @brief Internal helpers for debounce and backoff policies.
 */

#pragma once

#include <stdint.h>

namespace AsyncSD {
namespace internal {

/// @brief Simple debounce helper.
struct Debounce {
  uint32_t debounceMs = 0;
  bool stable = false;
  bool lastSample = false;
  uint32_t lastChangeMs = 0;

  void reset(bool initialState, uint32_t nowMs) {
    stable = initialState;
    lastSample = initialState;
    lastChangeMs = nowMs;
  }

  /// @brief Update with a new sample.
  /// @return true if stable state changed.
  bool update(bool sample, uint32_t nowMs) {
    if (sample != lastSample) {
      lastSample = sample;
      lastChangeMs = nowMs;
    }

    if (sample != stable) {
      if (static_cast<uint32_t>(nowMs - lastChangeMs) >= debounceMs) {
        stable = sample;
        return true;
      }
    }
    return false;
  }
};

/// @brief Exponential backoff helper.
struct Backoff {
  uint32_t minMs = 0;
  uint32_t maxMs = 0;
  uint32_t currentMs = 0;
  uint32_t nextMs = 0;

  void reset(uint32_t nowMs) {
    currentMs = minMs;
    nextMs = nowMs + currentMs;
  }

  bool shouldFire(uint32_t nowMs) const {
    return static_cast<int32_t>(nowMs - nextMs) >= 0;
  }

  void onFailure(uint32_t nowMs) {
    if (currentMs == 0) {
      currentMs = minMs;
    } else {
      const uint32_t next =
          (currentMs > (UINT32_MAX / 2U)) ? UINT32_MAX : (currentMs * 2U);
      currentMs = (next > maxMs) ? maxMs : next;
    }
    nextMs = nowMs + currentMs;
  }

  void onSuccess(uint32_t nowMs, uint32_t intervalMs) {
    currentMs = minMs;
    nextMs = nowMs + intervalMs;
  }
};

/// @brief Failure hysteresis counter.
struct FailureHysteresis {
  uint8_t threshold = 1;
  uint8_t count = 0;

  void reset() { count = 0; }

  bool recordFailure() {
    if (count < 0xFF) {
      count++;
    }
    return count >= threshold;
  }

  void recordSuccess() { count = 0; }
};

}  // namespace internal
}  // namespace AsyncSD

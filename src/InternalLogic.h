/**
 * @file InternalLogic.h
 * @brief Internal helper logic for AsyncSD (host-testable).
 */

#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace AsyncSD {
namespace internal {

/// @brief Copy-write slot for requestWriteCopy().
struct CopyWriteSlot {
  uint8_t* data = nullptr;
  uint16_t length = 0;
  bool inUse = false;
};

/// @brief Copy-write pool helper (no locking).
struct CopyWritePool {
  CopyWriteSlot* slots = nullptr;
  uint8_t* storage = nullptr;
  uint8_t slotCount = 0;
  uint16_t slotSize = 0;

  void init(CopyWriteSlot* slotsIn, uint8_t* storageIn, uint8_t count, uint16_t size) {
    slots = slotsIn;
    storage = storageIn;
    slotCount = count;
    slotSize = size;
    if (!slots || !storage || slotCount == 0 || slotSize == 0) {
      return;
    }
    for (uint8_t i = 0; i < slotCount; ++i) {
      slots[i].inUse = false;
      slots[i].length = 0;
      slots[i].data = storage + (i * slotSize);
    }
  }

  bool allocate(uint32_t length, const void* data, uint8_t* outSlot, void** outBuffer) {
    if (!slots || !storage || !outSlot || !outBuffer || slotCount == 0 || slotSize == 0) {
      return false;
    }
    if (length == 0 || length > slotSize) {
      return false;
    }
    for (uint8_t i = 0; i < slotCount; ++i) {
      if (!slots[i].inUse) {
        slots[i].inUse = true;
        slots[i].length = static_cast<uint16_t>(length);
        if (data && slots[i].data) {
          memcpy(slots[i].data, data, length);
        }
        *outSlot = i;
        *outBuffer = slots[i].data;
        return true;
      }
    }
    return false;
  }

  void release(uint8_t slot) {
    if (!slots || slot >= slotCount) {
      return;
    }
    slots[slot].inUse = false;
    slots[slot].length = 0;
  }
};

/// @brief Stall detection helper.
inline bool shouldStall(uint32_t nowMs, uint32_t lastProgressMs, uint32_t stallMs,
                        bool hasPendingWork) {
  if (!hasPendingWork || stallMs == 0 || lastProgressMs == 0) {
    return false;
  }
  return static_cast<uint32_t>(nowMs - lastProgressMs) >= stallMs;
}

/// @brief Shutdown policy helper.
inline bool shouldAbortShutdown(bool workerRunning, bool timedOut) {
  return workerRunning && timedOut;
}

/// @brief Increment dropped results counter.
inline uint32_t incrementDroppedResults(std::atomic<uint32_t>& counter) {
  return counter.fetch_add(1, std::memory_order_relaxed) + 1U;
}

}  // namespace internal
}  // namespace AsyncSD

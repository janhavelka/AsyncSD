#include "Arduino.h"

#include <atomic>

#include "SPI.h"

namespace {

std::atomic<uint64_t> g_nowUs{0};
std::atomic<int> g_digitalReadValue{1};

}  // namespace

SPIClass SPI;

extern "C" {

uint32_t millis(void) {
  return static_cast<uint32_t>(g_nowUs.load(std::memory_order_relaxed) / 1000ULL);
}

uint32_t micros(void) {
  return static_cast<uint32_t>(g_nowUs.load(std::memory_order_relaxed));
}

void delay(uint32_t ms) {
  g_nowUs.fetch_add(static_cast<uint64_t>(ms) * 1000ULL, std::memory_order_relaxed);
}

void pinMode(uint8_t /*pin*/, uint8_t /*mode*/) {}

int digitalRead(uint8_t /*pin*/) {
  return g_digitalReadValue.load(std::memory_order_relaxed);
}

void attachInterruptArg(uint8_t /*pin*/, void (* /*handler*/)(void*),
                        void* /*arg*/, int /*mode*/) {}

void detachInterrupt(uint8_t /*pin*/) {}

}  // extern "C"

namespace arduino_stub {

void reset() {
  g_nowUs.store(0, std::memory_order_relaxed);
  g_digitalReadValue.store(1, std::memory_order_relaxed);
}

void setTimeMs(uint32_t ms) {
  g_nowUs.store(static_cast<uint64_t>(ms) * 1000ULL, std::memory_order_relaxed);
}

void advanceMs(uint32_t ms) {
  g_nowUs.fetch_add(static_cast<uint64_t>(ms) * 1000ULL, std::memory_order_relaxed);
}

void setDigitalReadValue(int value) {
  g_digitalReadValue.store(value, std::memory_order_relaxed);
}

}  // namespace arduino_stub

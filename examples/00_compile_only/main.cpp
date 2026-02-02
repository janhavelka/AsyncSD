/**
 * @file main.cpp
 * @brief Minimal compile-only skeleton demonstrating library lifecycle.
 *
 * This example verifies that the library compiles correctly.
 * It shows the minimal required usage pattern:
 *   1. Create instance
 *   2. Configure and call begin()
 *   3. Call tick() in loop
 *   4. Optionally call end() to stop
 *
 * No serial output; just blinks LED if configured.
 */

#include <Arduino.h>

#include "examples/common/BoardPins.h"
#include "YourLibrary/YourLib.h"

static YourLibrary::YourLib g_lib;
static bool g_initialized = false;

void setup() {
  // Configure the library
  YourLibrary::Config config;
  config.ledPin = pins::LED;
  config.intervalMs = 500;  // 500ms blink

  // Initialize - check status in real applications
  const YourLibrary::Status st = g_lib.begin(config);
  g_initialized = st.ok();
}

void loop() {
  if (!g_initialized) {
    // Initialization failed; do nothing (or handle error)
    return;
  }

  // Cooperative tick - call every loop iteration
  g_lib.tick(millis());
}

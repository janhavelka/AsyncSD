/**
 * @file main.cpp
 * @brief Interactive CLI example for YourLib.
 *
 * Demonstrates library lifecycle with serial commands.
 * Type 'help' for available commands.
 */

#include <Arduino.h>

#include "examples/common/BoardPins.h"
#include "examples/common/Log.h"
#include "YourLibrary/Version.h"
#include "YourLibrary/YourLib.h"

static YourLibrary::YourLib g_lib;

/**
 * @brief Non-blocking line reader from Serial.
 * @return Complete line (without newline) or empty string if incomplete.
 */
static String read_line() {
  static String buffer;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      String result = buffer;
      buffer = "";
      return result;
    }
    if (buffer.length() < 64) {  // Limit buffer size
      buffer += c;
    }
  }
  return "";
}

/**
 * @brief Print available commands.
 */
static void print_help() {
  Serial.println();
  Serial.println(F("=== YourLib CLI ==="));
  Serial.print(F("Version: "));
  Serial.println(YourLibrary::VERSION);
  Serial.print(F("Built:   "));
  Serial.println(YourLibrary::BUILD_TIMESTAMP);
  Serial.print(F("Commit:  "));
  Serial.print(YourLibrary::GIT_COMMIT);
  Serial.print(F(" ("));
  Serial.print(YourLibrary::GIT_STATUS);
  Serial.println(F(")"));
  Serial.println(F("Commands:"));
  Serial.println(F("  help          - Show this help"));
  Serial.println(F("  start [ms]    - Start with interval (default: 1000ms)"));
  Serial.println(F("  stop          - Stop the library"));
  Serial.println(F("  status        - Show current state"));
  Serial.println();
}

/**
 * @brief Handle 'start' command.
 * @param arg Optional interval argument (e.g., "500").
 */
static void cmd_start(const String& arg) {
  if (g_lib.isInitialized()) {
    LOGI("Already running. Use 'stop' first.");
    return;
  }

  uint32_t interval = 1000;
  if (arg.length() > 0) {
    const long val = arg.toInt();
    if (val > 0) {
      interval = static_cast<uint32_t>(val);
    }
  }

  YourLibrary::Config config;
  config.intervalMs = interval;
  config.ledPin = pins::LED;

  const YourLibrary::Status st = g_lib.begin(config);
  if (!st.ok()) {
    LOGE("begin() failed: %s (code=%d)", st.msg, static_cast<int>(st.code));
  } else {
    LOGI("Started. interval=%lu ms, led_pin=%d",
         static_cast<unsigned long>(interval), pins::LED);
  }
}

/**
 * @brief Handle 'stop' command.
 */
static void cmd_stop() {
  if (!g_lib.isInitialized()) {
    LOGI("Not running.");
    return;
  }
  g_lib.end();
  LOGI("Stopped.");
}

/**
 * @brief Handle 'status' command.
 */
static void cmd_status() {
  LOGI("Running: %s", g_lib.isInitialized() ? "yes" : "no");
}

/**
 * @brief Process a single command line.
 * @param line The command line to process.
 */
static void process_command(const String& line) {
  if (line == "help") {
    print_help();
  } else if (line == "start") {
    cmd_start("");
  } else if (line.startsWith("start ")) {
    cmd_start(line.substring(6));
  } else if (line == "stop") {
    cmd_stop();
  } else if (line == "status") {
    cmd_status();
  } else {
    LOGE("Unknown command. Type 'help' for usage.");
  }
}

void setup() {
  log_begin(115200);
  delay(100);  // Allow USB CDC to initialize
  print_help();
  Serial.println(F("Ready. Type a command:"));
}

void loop() {
  // Non-blocking tick
  g_lib.tick(millis());

  // Non-blocking command processing
  const String line = read_line();
  if (line.length() > 0) {
    process_command(line);
  }
}

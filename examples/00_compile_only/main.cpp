/**
 * @file main.cpp
 * @brief Minimal compile-only skeleton for AsyncSD.
 */

#include <Arduino.h>

#include "examples/common/BoardPins.h"
#include "AsyncSD/AsyncSD.h"

static AsyncSD::SdCardManager g_sd;

void setup() {
  AsyncSD::SdCardConfig cfg;
  cfg.pinCs = pins::SD_CS;
  cfg.pinMosi = pins::SPI_MOSI;
  cfg.pinMiso = pins::SPI_MISO;
  cfg.pinSck = pins::SPI_SCK;
  cfg.cdPin = pins::SD_CD;
  cfg.autoMount = false;
  cfg.useWorkerTask = false;
  (void)g_sd.begin(cfg);
}

void loop() {
  // External worker mode for compile-only verification
  g_sd.workerStep(1000);
  delay(1);
}

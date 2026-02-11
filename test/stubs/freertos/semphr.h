#pragma once

#include "freertos/FreeRTOS.h"

using SemaphoreHandle_t = void*;

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  return reinterpret_cast<SemaphoreHandle_t>(1);
}

inline void vSemaphoreDelete(SemaphoreHandle_t /*mutex*/) {}

inline int xSemaphoreTake(SemaphoreHandle_t /*mutex*/, TickType_t /*timeout*/) {
  return pdTRUE;
}

inline int xSemaphoreGive(SemaphoreHandle_t /*mutex*/) {
  return pdTRUE;
}

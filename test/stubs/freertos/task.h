#pragma once

#include "freertos/FreeRTOS.h"

using TaskHandle_t = void*;

inline int xTaskCreate(void (* /*taskCode*/)(void*), const char* /*name*/,
                       uint32_t /*stackDepth*/, void* /*params*/,
                       uint32_t /*priority*/, TaskHandle_t* outHandle) {
  if (outHandle) {
    *outHandle = reinterpret_cast<TaskHandle_t>(1);
  }
  return pdPASS;
}

inline int xTaskCreatePinnedToCore(void (* /*taskCode*/)(void*), const char* /*name*/,
                                   uint32_t /*stackDepth*/, void* /*params*/,
                                   uint32_t /*priority*/, TaskHandle_t* outHandle,
                                   int32_t /*coreId*/) {
  if (outHandle) {
    *outHandle = reinterpret_cast<TaskHandle_t>(1);
  }
  return pdPASS;
}

inline TaskHandle_t xTaskGetCurrentTaskHandle() {
  return nullptr;
}

inline void vTaskDelete(TaskHandle_t /*task*/) {}

inline void vTaskDelay(TickType_t /*ticks*/) {}

inline void taskYIELD() {}

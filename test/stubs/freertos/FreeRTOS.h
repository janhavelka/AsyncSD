#pragma once

#include <stdint.h>

using TickType_t = uint32_t;
using StackType_t = uint32_t;
using BaseType_t = int32_t;

#ifndef pdTRUE
#define pdTRUE 1
#endif

#ifndef pdFALSE
#define pdFALSE 0
#endif

#ifndef pdPASS
#define pdPASS 1
#endif

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif

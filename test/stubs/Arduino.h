#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t millis(void);
uint32_t micros(void);
void delay(uint32_t ms);
void pinMode(uint8_t pin, uint8_t mode);
int digitalRead(uint8_t pin);
void attachInterruptArg(uint8_t pin, void (*handler)(void*), void* arg, int mode);
void detachInterrupt(uint8_t pin);

#ifdef __cplusplus
}

namespace arduino_stub {
void reset();
void setTimeMs(uint32_t ms);
void advanceMs(uint32_t ms);
void setDigitalReadValue(int value);
}  // namespace arduino_stub
#endif

#ifndef INPUT
#define INPUT 0x00
#endif

#ifndef INPUT_PULLUP
#define INPUT_PULLUP 0x02
#endif

#ifndef CHANGE
#define CHANGE 0x01
#endif

#pragma once

#include <stdint.h>

class SPIClass {
 public:
  void begin(int8_t /*sck*/, int8_t /*miso*/, int8_t /*mosi*/, int8_t /*ss*/) {}
};

extern SPIClass SPI;

#ifndef SPI_MODE0
#define SPI_MODE0 0
#endif

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#ifdef O_RDONLY
#undef O_RDONLY
#endif
#define O_RDONLY 0x01

#ifdef O_READ
#undef O_READ
#endif
#define O_READ O_RDONLY

#ifdef O_WRITE
#undef O_WRITE
#endif
#define O_WRITE 0x02

#ifdef O_RDWR
#undef O_RDWR
#endif
#define O_RDWR (O_READ | O_WRITE)

#ifdef O_CREAT
#undef O_CREAT
#endif
#define O_CREAT 0x04

#ifdef O_TRUNC
#undef O_TRUNC
#endif
#define O_TRUNC 0x08

#ifdef O_APPEND
#undef O_APPEND
#endif
#define O_APPEND 0x10

#ifdef O_EXCL
#undef O_EXCL
#endif
#define O_EXCL 0x20

#ifndef FAT_TYPE_EXFAT
#define FAT_TYPE_EXFAT 64
#endif

#ifndef SD_CARD_TYPE_SD1
#define SD_CARD_TYPE_SD1 1
#endif

#ifndef SD_CARD_TYPE_SD2
#define SD_CARD_TYPE_SD2 2
#endif

#ifndef SD_CARD_TYPE_SDHC
#define SD_CARD_TYPE_SDHC 3
#endif

#ifndef SHARED_SPI
#define SHARED_SPI 0x01
#endif

#ifndef DEDICATED_SPI
#define DEDICATED_SPI 0x02
#endif

#ifndef HAS_SDIO_CLASS
#define HAS_SDIO_CLASS 0
#endif

struct cid_t {
  uint8_t raw[16]{};
};

struct csd_t {
  uint8_t raw[16]{};
};

struct scr_t {
  uint8_t raw[8]{};
};

struct sds_t {
  uint8_t raw[64]{};
};

class SPIClass;

class SdSpiConfig {
 public:
  SdSpiConfig(uint8_t csPin, uint8_t options, uint32_t maxSpeed, SPIClass* spi)
      : csPin(csPin), options(options), maxSpeed(maxSpeed), spi(spi) {}

  uint8_t csPin;
  uint8_t options;
  uint32_t maxSpeed;
  SPIClass* spi;
};

class SdCard {
 public:
  uint8_t type() const;
  uint32_t sectorCount() const;
  uint32_t status() const;
  bool readOCR(uint32_t* out) const;
  bool readCID(cid_t* out) const;
  bool readCSD(csd_t* out) const;
  bool readSCR(scr_t* out) const;
  bool readSDS(sds_t* out) const;
};

class SdFs;

class FsFile {
 public:
  FsFile() = default;

  bool open(const char* path, uint8_t flags);
  void close();
  int32_t read(uint8_t* dst, uint32_t len);
  size_t write(const uint8_t* src, uint32_t len);
  bool seekSet(uint64_t pos);
  bool seekEnd();
  bool sync();
  uint64_t fileSize() const;
  bool isDir() const;

 private:
  std::string _path;
  uint64_t _pos = 0;
  bool _open = false;
  bool _readable = false;
  bool _writable = false;
};

class SdFs {
 public:
  bool begin(const SdSpiConfig& cfg);
  void end();
  SdCard* card();

  uint8_t fatType() const;
  uint32_t sectorsPerCluster() const;
  uint32_t bytesPerCluster() const;
  uint32_t clusterCount() const;
  int32_t freeClusterCount() const;

  bool exists(const char* path) const;
  bool mkdir(const char* path, bool recursive);
  bool remove(const char* path);
  bool rmdir(const char* path);
  bool rename(const char* fromPath, const char* toPath);

  int32_t sdErrorCode() const;

 private:
  SdCard _card{};
};

namespace sdfat_stub {

void reset();
void setRenameDelayMs(uint32_t delayMs);
void setForcedRenameErrno(int err);
bool pathExists(const char* path);

}  // namespace sdfat_stub

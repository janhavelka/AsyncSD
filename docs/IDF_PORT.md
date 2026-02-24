# AsyncSD — ESP-IDF Migration Prompt

> **Library**: AsyncSD (async SD card logging with FreeRTOS task)
> **Current version**: 1.2.0 → **Target**: 2.0.0
> **Namespace**: `AsyncSD`
> **Include path**: `#include "AsyncSD/AsyncSD.h"` (verify actual — `Config.h` has SPI.h in public header)
> **Difficulty**: Hard — SdFat dependency, SPI.h in public header, fundamental storage API change

---

## Pre-Migration

```bash
git tag v1.2.0   # freeze Arduino-era version
```

---

## Current State — Arduino Dependencies (exact)

| API | Count | Location |
|-----|-------|----------|
| `#include <Arduino.h>` | present | |
| `#include <SPI.h>` | 1 | **PUBLIC HEADER** `Config.h` |
| `SPIClass*` | in `Config` struct | **Public API** |
| `SdFat` library | entire storage backend | Dependency |
| `millis()` | 20 | Throughout |
| `micros()` | 2 | |
| `pinMode()` | 1 | SD card detect |
| `digitalRead()` | 2 | SD card detect |

FreeRTOS task architecture already present (async write queue, background flush task).

**Critical**: `SPI.h` is in the **public header** `Config.h`. The `Config` struct takes `SPIClass*`. The entire storage layer uses `SdFat`. This is a fundamental API/dependency change.

---

## Architecture Change: Replace SdFat with ESP-IDF VFS/FATFS

### Old (Arduino)

```cpp
// Config.h (public):
#include <SPI.h>
#include <SdFat.h>

struct Config {
    SPIClass* spi;
    int csPin;
    int cdPin;       // card-detect
    uint32_t spiFreq;
    // ...
};
```

Library internally uses `SdFat` for all file operations: `open()`, `write()`, `close()`, `sync()`, etc.

### New (ESP-IDF) — VFS/FATFS

ESP-IDF provides SD card support through the VFS layer. The caller mounts the SD card and gives the library a mount-point path. The library uses standard POSIX file I/O (`fopen`, `fwrite`, `fclose`, `fsync`).

```cpp
// New Config (public):
#include <cstdint>

namespace AsyncSD {

struct Config {
    const char* mountPoint = "/sdcard";  // VFS mount path
    gpio_num_t cdPin = GPIO_NUM_NC;      // card-detect (or NC if unused)
    bool cdActiveLow = true;
    // ... existing queue/task config fields
};

}
```

### Storage layer replacement

Replace all `SdFat` calls with POSIX:

| SdFat | POSIX |
|-------|-------|
| `file.open(path, O_WRITE \| O_CREAT)` | `fopen(path, "a")` |
| `file.write(buf, len)` | `fwrite(buf, 1, len, fp)` |
| `file.sync()` | `fflush(fp); fsync(fileno(fp))` |
| `file.close()` | `fclose(fp)` |
| `sd.exists(path)` | `access(path, F_OK) == 0` |
| `sd.mkdir(path)` | `mkdir(path, 0755)` |
| `file.size()` | `fseek(fp, 0, SEEK_END); ftell(fp)` |

The SD card SPI initialization and VFS mounting is the **caller's responsibility**, not the library's. This makes the library transport-agnostic (could work with SDMMC, SPI SD, or even SPIFFS/LittleFS).

### Card-detect pin

If `cdPin != GPIO_NUM_NC`, use ESP-IDF GPIO:

```cpp
#include "driver/gpio.h"
// pinMode(cdPin, INPUT_PULLUP) → gpio_config with pull-up
// digitalRead(cdPin) → gpio_get_level(cdPin)
```

---

## Steps

### 1. Remove `SdFat` dependency entirely

- Remove `#include <SdFat.h>` from all files
- Remove `#include <SPI.h>` from public header
- Remove `SPIClass*` from Config

### 2. Replace Config struct

Remove SPI-related fields. Add `mountPoint` (const char*). Keep queue/task configuration fields.

### 3. Replace all file operations with POSIX

Add includes:

```cpp
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/unistd.h>
```

Replace every SdFat file call with the POSIX equivalent (see table above).

### 4. Replace card-detect GPIO

```cpp
#include "driver/gpio.h"
// 1× pinMode() + 2× digitalRead() for card-detect pin
```

### 5. Replace timing APIs

```cpp
#include "esp_timer.h"

static inline uint32_t nowMs() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline uint32_t nowUs() {
    return (uint32_t)(esp_timer_get_time());
}
```

- Replace 20× `millis()` → `nowMs()`
- Replace 2× `micros()` → `nowUs()`

### 6. Remove `#include <Arduino.h>`

### 7. Keep FreeRTOS task architecture

The existing async write queue + background flush task is framework-agnostic. Keep it.

### 8. Add `CMakeLists.txt` (library root)

```cmake
idf_component_register(
    SRCS "src/AsyncSD.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer fatfs vfs
)
```

Adjust `SRCS` to match actual source files.

### 9. Add `idf_component.yml` (library root)

```yaml
version: "2.0.0"
description: "Async SD card logger with FreeRTOS write queue"
targets:
  - esp32s2
  - esp32s3
dependencies:
  idf: ">=5.0"
```

### 10. Version bump

- `library.json` → `2.0.0`
- `Version.h` (if present) → `2.0.0`

### 11. Replace Arduino example with ESP-IDF example

Create `examples/espidf_basic/main/main.cpp`:

```cpp
#include <cstdio>
#include "AsyncSD/AsyncSD.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* MOUNT = "/sdcard";

extern "C" void app_main() {
    // --- Caller mounts SD card via VFS ---
    esp_vfs_fat_sdmmc_mount_config_t mountCfg{};
    mountCfg.format_if_mount_failed = false;
    mountCfg.max_files = 5;
    mountCfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t busCfg{};
    busCfg.mosi_io_num = GPIO_NUM_35;
    busCfg.miso_io_num = GPIO_NUM_37;
    busCfg.sclk_io_num = GPIO_NUM_36;
    busCfg.quadwp_io_num = -1;
    busCfg.quadhd_io_num = -1;
    busCfg.max_transfer_sz = 4096;
    spi_bus_initialize((spi_host_device_t)host.slot, &busCfg, SPI_DMA_CH_AUTO);

    sdspi_device_config_t slotCfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slotCfg.gpio_cs = GPIO_NUM_34;
    slotCfg.host_id = (spi_host_device_t)host.slot;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slotCfg, &mountCfg, &card);
    if (err != ESP_OK) {
        printf("SD mount failed: %s\n", esp_err_to_name(err));
        return;
    }

    // --- Library uses mounted VFS path ---
    AsyncSD::Config cfg{};
    cfg.mountPoint = MOUNT;

    AsyncSD::Logger logger;
    auto st = logger.begin(cfg);
    if (st.err != AsyncSD::Err::Ok) {
        printf("begin() failed: %s\n", st.msg);
    }

    while (true) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        logger.tick(now);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

Create `examples/espidf_basic/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
    REQUIRES driver esp_timer fatfs vfs sdmmc)
```

Create `examples/espidf_basic/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(asyncsd_espidf_basic)
```

---

## Verification

```bash
cd examples/espidf_basic && idf.py set-target esp32s2 && idf.py build
```

- [ ] `idf.py build` succeeds
- [ ] Zero `#include <Arduino.h>`, `#include <SPI.h>`, `#include <SdFat.h>` anywhere
- [ ] Zero `SPIClass`, `SdFat`, `millis()`, `micros()`, `pinMode()`, `digitalRead()` remaining
- [ ] Library uses POSIX file I/O only (fopen/fwrite/fclose/fsync)
- [ ] SD card mounting is caller's responsibility, not library's
- [ ] FreeRTOS task architecture preserved
- [ ] Version bumped to 2.0.0
- [ ] `git tag v2.0.0`

# AsyncSD ESP-IDF v6.0.1 Port Audit

Scope: future ESP-IDF support for the current AsyncSD library while keeping the
Arduino/PlatformIO/SdFat path compatible. This is an implementation brief, not a
completed port.

Arduino-ESP32/PlatformIO builds are regression checks only. They do not prove
pure ESP-IDF v6.0.1 compatibility, and the current library metadata/source path
is Arduino/SdFat-only.

## Current State

- The library is Arduino-only. `include/AsyncSD/Config.h` includes `<SPI.h>` and
  exposes `SPIClass*` in `SdCardConfig`.
- `src/AsyncSD.cpp` depends on Arduino APIs (`millis`, `micros`, GPIO helpers,
  `SPIClass`) and SdFat v2 types (`SdFs`, `FsFile`, `SdSpiConfig`, card APIs).
- The public model is already close to IDF expectations: `SdCardManager` owns a
  bounded request queue, can run from a worker task or external `workerStep`,
  and reports structured request results.
- `TransportType::Sdmmc` exists but is a stub; SPI/SdFat is the only working
  backend.
- Examples are Arduino sketches under `examples/01_spi_cli_control` with
  board-specific pins isolated in `examples/common/BoardPins.h`.
- `platformio.ini` and `library.json` declare Arduino framework usage and SdFat
  dependency only.

## Blockers

1. Public headers are not ESP-IDF compilable because of the unconditional
   Arduino `SPI.h` include and `SPIClass*` field.
2. The implementation is tightly coupled to SdFat file/card objects; ESP-IDF
   should use VFS/POSIX over FatFS rather than porting SdFat.
3. Mount ownership must be defined. The library rules say AsyncSD must not own
   the SPI bus; IDF SDMMC/SDSPI host setup must stay in the application or in an
   injected adapter.
4. Card info, filesystem info, and presence reporting currently come from SdFat
   and Arduino GPIO. These need IDF-specific adapters or graceful "unknown"
   fields.
5. The SDMMC backend is a placeholder and must not be advertised as ready until
   there is a real IDF VFS/FatFS path.

## Exact Files/APIs To Change

- `include/AsyncSD/Config.h`
  - Guard Arduino-only includes and fields with `#if defined(ARDUINO)`.
  - Add an IDF-safe backend selector such as `Backend::ArduinoSdFat` and
    `Backend::IdfVfs`.
  - Keep `mountPoint`, queue, timeout, worker, and callback fields common.
  - Add IDF-only or generic adapter fields for mounted VFS access and optional
    card-present/card-info callbacks.
- `include/AsyncSD/AsyncSD.h`
  - Keep the existing request API and `begin(const SdCardConfig&, ISpiBusGuard*)`
    shape for Arduino compatibility.
  - Avoid exposing `sdmmc_card_t`, `FILE*`, or ESP-IDF headers in the common
    public API unless the field is behind `ESP_PLATFORM`.
- `include/AsyncSD/Status.h`
  - Append-only if new errors are needed, for example `VFS_NOT_MOUNTED`,
    `MOUNT_FAILED`, `UNMOUNT_FAILED`, or `POSIX_ERROR`.
- `src/AsyncSD.cpp`
  - Split SdFat-specific code into an Arduino backend and add an IDF VFS backend.
  - Replace direct `FsFile` storage with an internal backend file handle variant
    (`FsFile` for Arduino, `FILE*` or fd for IDF).
  - Use POSIX/VFS calls for IDF: `fopen`, `fread`, `fwrite`, `fflush`, `fsync`,
    `fclose`, `stat`, `mkdir`, `rename`, `unlink`, `opendir`, `readdir`.
  - Replace Arduino time/GPIO calls in IDF builds with `esp_timer_get_time`,
    FreeRTOS tick helpers, and optional GPIO/card-detect callbacks.
- `src/InternalLogic.h` and `src/Presence.h`
  - Keep reusable queue/state helpers, but make presence detection backend driven.
- Add, during implementation, root `CMakeLists.txt`, `idf_component.yml`, and
  IDF examples. Do not change `platformio.ini` for the documentation-only audit.

## Architecture Preserving Arduino Compatibility

Use a compile-time backend split:

- Arduino backend: current SdFat implementation, current examples, current
  `SPIClass*` behavior, and current PlatformIO metadata stay intact.
- ESP-IDF backend: no SdFat, no Arduino headers, file operations happen through
  VFS/POSIX on a mount point that the application owns or an injected mount
  adapter owns.
- Common layer: request queue, worker task/external worker mode, request IDs,
  callbacks, status/result mapping, timeouts, and public operation names.

Recommended IDF mount policy:

- Minimum supported port: application mounts SD card with ESP-IDF, passes
  `mountPoint` to AsyncSD, and AsyncSD performs asynchronous VFS file operations.
- Optional later feature: injected mount/unmount callbacks so applications can
  let AsyncSD request mount state changes without AsyncSD configuring pins,
  SDMMC slots, SPI hosts, or card-detect GPIO.

The first supported IDF surface should be the app-owned VFS contract above.
Do not mix that with component-owned SDSPI/SDMMC mount helpers in the first
port; mount helpers can be added later as a separate adapter once the VFS
backend is stable.

Expected field parity for the first VFS backend:

| Existing public field | IDF VFS handling |
| --- | --- |
| request status/result | Preserve exactly; map `errno`/`esp_err_t` into detail fields. |
| mounted/present | From adapter callbacks; unknown when callbacks are absent. |
| card type/capacity | Optional adapter-supplied data; otherwise report unsupported/unknown. |
| filesystem free/used | Use FatFS/VFS stats when available; otherwise report unknown without failing file I/O. |
| open file handles | Backend-owned `FILE*` or file descriptor; never exposed in common public API. |
| sync/flush | Use `fflush` plus `fsync(fileno(file))` where available and map both failures. |

## Adapter Contract

The IDF backend should not call `spi_bus_initialize`, `sdspi_host_init_device`,
`sdmmc_host_init_slot`, or GPIO setup directly unless those calls live in an
example-owned adapter. The library receives already configured services:

```cpp
struct IdfVfsAdapter {
  const char* mountPoint;              // e.g. "/sdcard"
  void* user;
  bool (*isMounted)(void* user);
  bool (*isPresent)(void* user);       // optional, may be null
  uint32_t (*nowMs)(void* user);       // optional, esp_timer_get_time()/1000
  SdStatus (*lock)(uint32_t timeoutMs, void* user);   // optional
  void (*unlock)(void* user);                         // optional
  SdStatus (*mount)(uint32_t timeoutMs, void* user);  // optional
  SdStatus (*unmount)(uint32_t timeoutMs, void* user);// optional
};
```

Status mapping must preserve the existing request result contract. `errno` and
`esp_err_t` values go into the existing detail field; user-facing messages remain
static strings.

## CMake/Component Plan

For the AsyncSD component with VFS-only operation:

```cmake
idf_component_register(
  SRCS "src/AsyncSD.cpp"
  INCLUDE_DIRS "include"
  REQUIRES freertos esp_timer
)
```

If the component itself includes FatFS/VFS mount helpers, add:

```cmake
REQUIRES freertos esp_timer fatfs vfs sdmmc
```

For SDSPI/SDMMC examples, the example `main` component should require the host
drivers it uses:

```cmake
REQUIRES fatfs vfs sdmmc esp_driver_spi esp_driver_sdspi esp_driver_sdmmc esp_driver_gpio
```

Use ESP-IDF v6 headers:

- VFS/FatFS: `esp_vfs_fat.h`
- SD protocol: `sdmmc_cmd.h`
- SDSPI host: `driver/sdspi_host.h` from `esp_driver_sdspi`
- SDMMC host: `driver/sdmmc_host.h` from `esp_driver_sdmmc`
- SPI host for SDSPI setup: `driver/spi_master.h` from `esp_driver_spi`
- GPIO card detect in examples: `driver/gpio.h` from `esp_driver_gpio`

## Examples

Arduino example remains `examples/01_spi_cli_control` and should continue to
configure `SdCardConfig::spi`, pins, and `autoInitSpi` exactly as today.

IDF SDSPI example should be a separate project, for example
`examples/idf_sdspi_basic`, with this ownership model:

```cpp
extern "C" void app_main(void) {
  // Example owns SPI bus, SDSPI device, FatFS mount, card-detect GPIO, and unmount.
  // AsyncSD only receives "/sdcard" and performs queued VFS file operations.
}
```

IDF SDMMC example should mirror it with `SDMMC_HOST_DEFAULT` and
`SDMMC_SLOT_CONFIG_DEFAULT`, but still pass only a mounted VFS path/adapter to
AsyncSD.

## Test And Validation Plan

- Native tests: keep current queue/status tests and add backend-neutral tests for
  path normalization, mount-state behavior, errno mapping, and request ordering.
- IDF build test: compile the component for ESP32-S2 and ESP32-S3 with
  `ESP_PLATFORM` and no Arduino headers available.
- IDF fake VFS test: use a host or target test adapter that maps operations to a
  temp directory and injects `errno` failures.
- Hardware tests: SDSPI mount/read/write/list/remove and SDMMC mount/read/write
  on ESP32-S2/S3, including card removal, remount, and worker shutdown.
- Concurrency tests: two queued file operations, queue full, timeout path, and
  shutdown while worker is idle and while an operation is in flight.

## IDF v6.0.1 Hazards

- Legacy umbrella `driver` dependencies are not enough. Use split components
  such as `esp_driver_spi`, `esp_driver_sdspi`, `esp_driver_sdmmc`, and
  `esp_driver_gpio`.
- `esp_vfs_fat_sdspi_mount` and `esp_vfs_fat_sdmmc_mount` can block. Run them in
  the worker or application setup, not from a short public API path.
- FatFS/VFS path prefixes matter. Store normalized absolute paths under the
  configured mount point and reject traversal outside it.
- FatFS `max_files`, long filename, format-on-fail, and exFAT options are
  Kconfig-dependent. Document required sdkconfig options in the examples.
- Do not share a mounted SD card from multiple tasks without a clear lock. The
  AsyncSD worker should be the only owner of open file handles.
- SDSPI bus sharing requires an application-level guard. Keep `ISpiBusGuard` for
  Arduino and provide an equivalent IDF adapter lock.
- File flush semantics differ: `fflush` is not the same as media sync; use
  `fsync(fileno(file))` where available and map failures.
- Card-detect GPIO is board glue. Do not configure or assume a CD pin in the
  library.

## Ordered Checklist

1. Add `docs/IDF_PORT.md` acceptance criteria to the implementation issue.
2. Guard Arduino-only public includes and fields in `Config.h`.
3. Introduce the IDF VFS backend type without changing existing Arduino defaults.
4. Split `src/AsyncSD.cpp` into common request handling plus backend operations.
5. Implement VFS/POSIX file operations and map `errno` to `SdStatus`.
6. Add optional mounted/present/lock/time callbacks for IDF.
7. Add component `CMakeLists.txt` and `idf_component.yml`.
8. Add IDF SDSPI and SDMMC examples that own host setup and FatFS mount.
9. Add native fake-backend tests and IDF compile tests.
10. Run Arduino PlatformIO examples/tests to prove existing behavior was not
    broken.
11. Run ESP-IDF v6.0.1 builds for ESP32-S2 and ESP32-S3.
12. Document any unsupported card-info fields returned by the VFS backend.

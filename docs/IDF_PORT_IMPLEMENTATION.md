# AsyncSD ESP-IDF Port Implementation Notes

Date: 2026-05-19

## Implemented In This Pass

- Added `Backend::ARDUINO_SDFAT` and `Backend::IDF_VFS` to make backend
  selection explicit without changing the Arduino default.
- Added `IdfVfsAdapter` with app-owned mount, presence, clock, lock, mount, and
  unmount callbacks.
- Guarded Arduino `SPI.h` usage in the public config header so pure ESP-IDF
  consumers can include `AsyncSD/AsyncSD.h`.
- Added a pure ESP-IDF compilation path in `src/AsyncSD.cpp` that returns
  `ErrorCode::Unsupported` instead of pulling in Arduino or SdFat.
- Added root ESP-IDF component metadata.
- Added `examples/idf_vfs_contract` to demonstrate the current IDF contract and
  unsupported runtime behavior.
- Added `scripts/check_idf_example_contract.py` to statically verify that the
  IDF contract example uses native IDF APIs and no Arduino compatibility facade.

## Current Runtime Matrix

| Build mode | Backend | Runtime status |
| --- | --- | --- |
| Arduino / PlatformIO | `Backend::ARDUINO_SDFAT` | Supported production path |
| Native tests | `Backend::ARDUINO_SDFAT` with stubs | Supported regression path |
| Pure ESP-IDF | `Backend::IDF_VFS` | API/component prep only; `begin()` returns `Unsupported` |

## Remaining Blockers

1. Split the monolithic `src/AsyncSD.cpp` worker into backend-neutral request
   handling plus backend file operations.
2. Implement the IDF VFS file handle table with `FILE*` or file descriptors.
3. Implement POSIX operations for open, close, read, write, sync, mkdir, remove,
   rename, stat, and listdir.
4. Map `errno` values into existing `ErrorCode` and `RequestResult::detail`.
5. Replace Arduino `millis()`, `micros()`, GPIO sampling, and FreeRTOS task
   assumptions in the common layer with injected backend/platform hooks.
6. Add fake VFS tests for request ordering, path normalization, mount-state
   handling, timeout paths, and `errno` mapping.
7. Add hardware IDF examples that own SDSPI/SDMMC host setup and FatFS mount.
8. Run ESP-IDF v6.0.1 component builds for ESP32-S2 and ESP32-S3 once IDF is
   available in CI or the local environment.

## Do Not Claim Yet

- Do not advertise ESP-IDF file I/O support.
- Do not claim SDMMC support.
- Do not let the component own SPI/SDSPI/SDMMC bus setup in the first real VFS
  backend.

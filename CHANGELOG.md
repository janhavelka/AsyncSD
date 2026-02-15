# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Added `requestListDir(path, entries, maxEntries, cb, user)` async directory listing API
- Added `DirEntry` struct (name, size, isDir) and `ListDir` to `RequestType`/`Operation` enums
- Added CLI commands: `ls [path]`, `touch <path>`, `cat <path> [maxlen]`
- Added `errorCodeToStr()` and `requestTypeToStr()` helpers to CLI example
- Added named error codes and detail field to CLI result output

### Changed
- `performMount()` now forces unmount-then-remount when already mounted, enabling clean re-initialization after card re-insertion
- Info request error detail now uses packed `(sdErrorData << 8) | sdErrorCode` for richer diagnostics

### Fixed
- Fixed `FsFile::open()` flag truncation on ESP32: `toSdFatFlags()` returned `uint8_t` but ESP32 fcntl flags (`O_CREAT=0x200`, `O_TRUNC=0x400`, `O_EXCL=0x800`) exceed 8 bits — changed to `int`
- Fixed `FsFile::open()` using static `FsVolume::m_cwv` pointer (2-arg form) which was not reliably set — changed all 5 call sites to explicit 3-arg `open(volume, path, flags)` form
- Fixed `requestListDir()` returning 0 entries: `bytesDone` was captured before the switch statement but `ListDir` case wrote count to `req.processed` without updating `bytesDone`
- Fixed stale `FsInfo`/`CardInfo` displayed after card ejection: Info handler was caching `buildFsInfo()` results even on I/O error; now clears caches on failure
- Fixed stale cached info persisting between probe failures: `FsInfo`/`CardInfo` now cleared immediately on first I/O or probe failure, not only after unmount
- Fixed `mkdir` failing on already-existing directories: now checks existence before attempting create

## [1.1.0] - 2026-02-11

### Added
- Added `requestRename(fromPath, toPath, mode)` async API with `RenameMode` destination policy
- Added `RequestType::Rename` and `Operation::Rename`
- Added `ErrorCode::NotFound` and `ErrorCode::AlreadyExists` for path-existence failures
- Added CLI example support for `rename` command in `01_spi_cli_control`
- Added native host test coverage for rename request flows, queue saturation, timeout behavior, result overflow telemetry, and reinit lifecycle

### Changed
- Hardened `begin()` validation/sanitization for path bounds, worker stack sizing, and backoff thresholds
- Worker task entry now runs from internal state/config only (no `SdCardManager*` dereference from task loop)
- Extended internal request path storage to hold bounded source/destination paths for two-path operations
- Rename worker flow now uses explicit source/destination existence checks and replace-if-exists behavior
- Clamped configuration time windows to a signed-safe range to prevent immediate timeout/stall behavior on extreme values
- Updated native test configuration to build library + stubs only (`src/**` and `test/stubs/**`)

### Fixed
- Fixed potential use-after-free when the manager object is destroyed while the worker task is still running
- Fixed stale runtime state across repeated `begin()`/`end()` cycles (queue indices, CD flags, pending automount/unmount, and task state)
- Fixed stale `lastErrorInfo.path` pointer lifetime across teardown by clearing path before buffer free
- Fixed unbounded path normalization scans by using bounded string handling
- Fixed request counter/result counter underflow guards to prevent queue corruption on unexpected state drift
- Fixed unsupported-filesystem mount cleanup to respect SPI bus locking during transport shutdown
- Fixed `requestWriteCopy()` pre-begin/callback-context validation paths to return deterministic errors
- Fixed `requestRead()` to reject `APPEND_OFFSET` as invalid input
- Fixed rename error mapping to surface deterministic `NotFound`/`AlreadyExists` outcomes
- Fixed rename same-path handling to complete as a no-op success
- Fixed rename filesystem `ENOENT` fallback mapping to `NotFound` for race-safe semantics
- Fixed mount-point path normalization so long prefixed inputs are accepted when stripped paths fit configured limits
- Fixed queue counter drift handling by self-healing request/result queue heads when counters and slots diverge
- Fixed missing filesystem error detail propagation for multiple I/O operations (`open/read/write/sync/mkdir/remove/stat`)

## [1.0.0] - 2026-02-02

### Added
- AsyncSD nonblocking SD card manager with worker task or external worker step
- SdFat v2 (`SdFs` + `FsFile`) support for FAT32 + exFAT
- SPI transport with bus guard and SDMMC stub
- CD pin debounce + no-CD probe/backoff logic
- Bounded async request queue and result handling with deadlines and chunked I/O
- Worker health metrics and soft stall watchdog
- Card/filesystem info snapshots (capacity/usage and raw registers)
- Bounded write-copy pool (`requestWriteCopy`)
- CLI example (`01_spi_cli_control`)
- Host tests for debounce/backoff and robustness helpers

### Changed
- Worker callbacks disabled by default (optional via `enableWorkerCallbacks`)
- Shutdown timeout faults and safe-leaks resources (no task kill)
- Result queue overflow increments dropped counter and records `ResultEnqueue`
- `begin()` uses nothrow allocations and fails gracefully on OOM

### Removed
- Compile-only examples

[Unreleased]: https://github.com/YOUR_USERNAME/AsyncSD/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/YOUR_USERNAME/AsyncSD/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/YOUR_USERNAME/AsyncSD/releases/tag/v1.0.0

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Nothing yet

### Changed
- Nothing yet

### Fixed
- Nothing yet

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

[Unreleased]: https://github.com/YOUR_USERNAME/AsyncSD/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/YOUR_USERNAME/AsyncSD/releases/tag/v1.0.0

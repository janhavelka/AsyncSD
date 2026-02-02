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

## [0.1.0] - 2026-02-02

### Added
- AsyncSD nonblocking SD card manager with worker task or external worker step
- SdFat v2 (`SdFs` + `FsFile`) support for FAT32 + exFAT
- SPI transport with bus guard and SDMMC stub
- CD pin debounce + no-CD probe/backoff logic
- Bounded async request queue and result handling
- CLI example (`01_spi_cli_control`) and compile-only example
- Host tests for debounce/backoff logic

[Unreleased]: https://github.com/YOUR_USERNAME/AsyncSD/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/YOUR_USERNAME/AsyncSD/releases/tag/v0.1.0

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial library template structure
- `begin(Config) -> Status`, `tick(now_ms)`, `end()` API pattern
- Status/Err type with static error messages
- Config injection pattern (no hardcoded pins)
- Basic CLI example (`01_basic_bringup_cli`)
- Compile-only skeleton example (`00_compile_only`)
- GitHub Actions CI for ESP32-S2 and ESP32-S3
- Doxygen-style documentation in public headers

### Changed
- Nothing yet

### Deprecated
- Nothing yet

### Removed
- Nothing yet

### Fixed
- Nothing yet

### Security
- Nothing yet

## [0.1.0] - 2026-01-10

### Added
- Initial release with template structure
- ESP32-S2 and ESP32-S3 support

[Unreleased]: https://github.com/YOUR_USERNAME/esp32-platformio-library-template/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/YOUR_USERNAME/esp32-platformio-library-template/releases/tag/v0.1.0

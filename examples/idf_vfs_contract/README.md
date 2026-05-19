# AsyncSD ESP-IDF VFS Contract Example

This example verifies the ESP-IDF-safe public configuration surface.

It does not mount a card and it does not perform file I/O yet. The current
ESP-IDF component path returns `ErrorCode::Unsupported` from `begin()` until the
VFS/POSIX backend is implemented.

The intended ownership model is:

- The application initializes SDSPI or SDMMC.
- The application mounts FatFS/VFS at a path such as `/sdcard`.
- AsyncSD receives only the mounted VFS path plus bounded callbacks in
  `SdCardConfig::idfVfs`.

Build from this directory after installing ESP-IDF v6:

```bash
idf.py set-target esp32s3
idf.py build
```

#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

REQUIRED_FILES = [
    "CMakeLists.txt",
    "idf_component.yml",
    "examples/idf_vfs_contract/CMakeLists.txt",
    "examples/idf_vfs_contract/main/CMakeLists.txt",
    "examples/idf_vfs_contract/main/main.cpp",
]

REQUIRED_MAIN_TOKENS = [
    'extern "C" void app_main(void)',
    "esp_timer_get_time",
    "ESP_LOGI",
    "ESP_LOGW",
    "AsyncSD::Backend::IDF_VFS",
    "cfg.idfVfs.mountPoint",
    "cfg.idfVfs.nowMs",
    "cfg.idfVfs.isMounted",
    "sd.begin(cfg)",
]

FORBIDDEN_IDF_TOKENS = [
    "Arduino.h",
    "SPI.h",
    "SdFat.h",
    "Serial",
    "SPIClass",
    "IdfArduinoCompat",
]


def fail(msg: str) -> None:
    print(f"IDF example contract FAILED: {msg}")
    raise SystemExit(1)


def require_token(text: str, token: str, label: str) -> None:
    if token not in text:
        fail(f"{label} missing token '{token}'")


def main() -> int:
    for rel in REQUIRED_FILES:
        if not (ROOT / rel).exists():
            fail(f"missing {rel}")

    main_cpp = (ROOT / "examples/idf_vfs_contract/main/main.cpp").read_text(
        encoding="utf-8", errors="replace"
    )
    for token in REQUIRED_MAIN_TOKENS:
        require_token(main_cpp, token, "IDF VFS contract example")
    for token in FORBIDDEN_IDF_TOKENS:
        if token in main_cpp:
            fail(f"IDF example must not use Arduino compatibility token '{token}'")

    cmake = (ROOT / "examples/idf_vfs_contract/main/CMakeLists.txt").read_text(
        encoding="utf-8", errors="replace"
    )
    if re.search(r"\bAsyncSD\b", cmake) is None:
        fail("IDF example CMake missing AsyncSD dependency")

    manifest = (ROOT / "idf_component.yml").read_text(encoding="utf-8", errors="replace")
    for token in ("esp32s2", "esp32s3", "idf:"):
        require_token(manifest, token, "idf_component.yml")

    print("IDF example contract PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

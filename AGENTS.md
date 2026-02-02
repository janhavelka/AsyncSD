# AGENTS.md - AsyncSD Embedded Engineering Guidelines

## Role
You are a professional embedded software engineer building **AsyncSD**: a production-grade, nonblocking SD card library for ESP32-S2/S3 (Arduino framework, PlatformIO).

**Primary goals:**
- Robustness and stability (never crash firmware)
- Deterministic, bounded behavior
- Portability across boards and projects

**Target:** ESP32-S2 / ESP32-S3, Arduino framework, PlatformIO.

**These rules are binding.**

---

## Repository Model (Single Library Template)

This repository contains a **single reusable library** named **AsyncSD**.

### Folder Structure (Mandatory)

```
include/AsyncSD/      - Public API headers ONLY (Doxygen documented)
  |-- Status.h        - Status + error types
  |-- Config.h        - Configuration struct
  |-- AsyncSD.h       - Main library class
src/                  - Implementation (.cpp files)
examples/
  |-- 01_spi_cli_control/
  |-- common/         - Example-only helpers (Log.h, BoardPins.h)
platformio.ini        - Build environments (uses build_src_filter)
library.json          - PlatformIO metadata
README.md             - Full documentation
CHANGELOG.md          - Keep a Changelog format
AGENTS.md             - This file
```

**Rules:**
- Public headers go in `include/AsyncSD/` - these define the API contract
- Board-specific values (pins, buses) NEVER in library code - only in `SdCardConfig`
- Examples demonstrate usage; they may use `examples/common/BoardPins.h`
- Keep structure boring and predictable - no clever layouts

---

## Core Architecture Principles (Non-Negotiable)

### 1) Nonblocking Public API
- Public APIs return quickly (no blocking I/O on caller thread)
- Use **Strategy B**: a worker task performs blocking SdFat operations
- Provide external worker mode via `workerStep(budget_us)`

### 2) Bounded Operations & Timeouts
- Every request has a deadline (`enqueue_ms + timeout_ms`)
- Reads/writes are chunked (bounded per step)
- No unbounded loops or waits
- **No `delay()` in library code** (FreeRTOS `vTaskDelay` only in worker task)

### 3) Worker Health + Stall Detection
- Maintain worker health metrics (progress timestamps, queue depths, failure counts)
- Soft stall watchdog via `workerStallMs`
- On stall: enter `Fault`, reject new requests, fail pending requests, attempt safe unmount

### 4) Deterministic State Machines
- Explicit, simple state machines over clever event chains
- Presence detection and mount/unmount are deterministic and bounded

### 5) No SPI Bus Ownership
- Do NOT call `spi_bus_initialize()` or assume exclusive SPI ownership
- Use user-provided `ISpiBusGuard` with bounded lock timeouts
- If no guard is provided, use an internal mutex guard (still bounded)
- Do not hold the bus longer than a single bounded operation

### 6) SdFat v2 with FAT32 + exFAT
- Use **SdFat v2** with `SdFs` + `FsFile` to support FAT32 + exFAT
- Detect/report filesystem type and capacity where feasible

### 7) Optional Card Detect (CD) Pin + No-CD Probe Logic
- `cdPin == -1` disables CD functionality
- If CD pin is used:
  - Debounce via configurable window
  - Optional interrupt allowed (ISR sets a flag only)
- If no CD pin:
  - Probe-based detection with exponential backoff
  - Require N consecutive failures to declare removal
  - Clean unmount/cleanup on removal (bounded)

### 8) Memory & Allocation Rules
- Allocate buffers and queues in `begin()`
- **Zero allocations** in `workerStep()` and steady state
- Fixed-size buffers/ring buffers only

### 9) Threading Rules
- **Only the worker** touches SdFat objects and open file handles
- Public APIs enqueue requests and read atomic snapshots only

### 10) Logging
- **Library code: NO logging**
- Examples may log via `examples/common/Log.h`

---

## Status + Error Model (Mandatory)

- Public API exposes:
  - `SdStatus` enum (Disabled, NoCard, CardInserted, Initializing, Mounting, Ready, Busy, Removed, Error, Fault)
  - `ErrorCode` enum with explicit error categories
  - `ErrorInfo` struct with operation, path, bytes, raw detail, timestamp
- No silent failures. All error paths must surface via Status/ErrorInfo.

---

## Modification Process

**Before making changes, ask:**
> "Does this increase predictability and portability across projects?"

**If no, do not proceed.**

**When adding features:**
1. Output intended file tree changes (short summary)
2. Apply edits (prefer additive changes over refactors)
3. Update documentation (README + Doxygen)
4. Summarize in <=10 bullets

**Prefer:**
- Additive changes over breaking changes
- Optional features (Config flags) over mandatory changes
- Explicit behavior over implicit magic

---

## Final Checklist

Before committing:
- [ ] Public API has Doxygen comments
- [ ] README documents threading and timing model
- [ ] Config struct has no hardcoded pins
- [ ] `workerStep()` is non-blocking and bounded
- [ ] Errors return via `ErrorCode`/`ErrorInfo`, never silent
- [ ] No heap allocation in steady state
- [ ] No logging in library code
- [ ] Examples demonstrate correct usage
- [ ] CHANGELOG.md updated

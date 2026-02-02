# AsyncSD

AsyncSD is a **nonblocking, bounded SD card library** for **ESP32-S2/S3** using the Arduino framework and PlatformIO. It performs **blocking SdFat operations in a worker task** (or an external worker step), so public APIs return quickly and predictably.

**Key features**
- SdFat v2 (`SdFs` + `FsFile`) with **FAT32 + exFAT** support
- Optional **CD pin** with debounce and interrupt flagging
- **No-CD** probe logic with backoff + hysteresis
- **Bounded** async request queue and result handling
- SPI **bus sharing** via user-provided guard

---

## Quickstart (PlatformIO)

```bash
# Build CLI example for ESP32-S3
pio run -e spi_cli_esp32s3

# Upload + monitor
pio run -e spi_cli_esp32s3 -t upload
pio device monitor -e spi_cli_esp32s3
```

## Supported Targets

| Board               | Environment         | Notes               |
| ------------------- | ------------------- | ------------------- |
| ESP32-S3-DevKitC-1  | `spi_cli_esp32s3`   | SPI mode            |
| ESP32-S2-Saola-1    | `spi_cli_esp32s2`   | SPI mode            |

**Note:** SDMMC transport is a stub (returns `Unsupported`) and reserved for future ESP32-S3 support.

## Dependency

AsyncSD depends on **SdFat v2**. PlatformIO installs it automatically via `library.json` / `platformio.ini`.

`platformio.ini` sets:
- `-DSDFAT_FILE_TYPE=3` to enable `SdFs`/`FsFile` (FAT32 + exFAT)

---

## Versioning

`library.json` is the single source of truth. A pre-build script generates:

```
include/AsyncSD/Version.h
```

**Usage:**
```cpp
#include "AsyncSD/Version.h"

Serial.println(AsyncSD::VERSION);
Serial.println(AsyncSD::VERSION_FULL);
```

---

## API Overview

```cpp
#include "AsyncSD/AsyncSD.h"

AsyncSD::SdCardManager sd;

void setup() {
  AsyncSD::SdCardConfig cfg;
  cfg.pinCs = 10;
  cfg.pinMosi = 11;
  cfg.pinMiso = 13;
  cfg.pinSck = 12;
  cfg.cdPin = -1;          // No CD pin
  cfg.autoMount = true;
  cfg.useWorkerTask = true; // default

  sd.begin(cfg);
}

void loop() {
  // Worker runs in background (no-op if worker task enabled)
  sd.poll();

  AsyncSD::RequestResult res;
  while (sd.popResult(&res)) {
    // Handle completed requests
  }
}
```

---

## Threading, Timing, Memory, Ownership

**Threading Model**
- Worker task by default (`useWorkerTask=true`)
- Optional external worker via `workerStep(budget_us)`
- Public APIs are nonblocking and enqueue requests only

**Timing**
- `workerStep()` is bounded by `workerBudgetUs`
- Reads/writes are chunked (`ioChunkBytes`)
- All requests have deadlines

**Resource Ownership**
- SPI pins are provided via `SdCardConfig`
- The library **does not own the SPI bus**
- Use `ISpiBusGuard` to integrate with your bus lock strategy

**Memory**
- Allocation in `begin()` only
- **Zero allocations** during steady-state operation

**Error Handling**
- `ErrorCode` + `ErrorInfo` returned via results and `lastErrorInfo()`
- No silent failures

**Worker Health**
- `getWorkerHealth()` provides timestamps, queue depths, failure counters, and status snapshots
- Soft stall watchdog: if work is pending and `millis() - lastProgressMs > workerStallMs`,
  the worker enters `Fault`, rejects new requests (`ErrorCode::Fault`), fails pending requests,
  and attempts a safe unmount

---

## Config Highlights

```cpp
AsyncSD::SdCardConfig cfg;
cfg.pinCs = 10;
cfg.pinMosi = 11;
cfg.pinMiso = 13;
cfg.pinSck = 12;
cfg.cdPin = -1;                 // disable CD
cfg.autoMount = true;
cfg.requestQueueDepth = 8;
cfg.maxOpenFiles = 4;
cfg.maxPathLength = 96;
cfg.ioChunkBytes = 512;
cfg.workerBudgetUs = 2000;
cfg.workerStallMs = 5000;
cfg.maxCopyWriteBytes = 512;
cfg.copyWriteSlots = 2;
```

See `include/AsyncSD/Config.h` for full field list and Doxygen notes.

---

## Status + Error Model

**SdStatus** (examples):
- `NoCard`, `CardInserted`, `Mounting`, `Ready`, `Busy`, `Error`, `Fault`

**ErrorCode** (examples):
- `Timeout`, `BusNotAvailable`, `CardInitFailed`, `MountFailed`, `IoError`, `NoSpaceLeft`, `InvalidArgument`

**ErrorInfo**
- operation (`Operation` enum)
- path (if available)
- bytes requested / processed
- timestamp (`millis`)

---

## Card + Filesystem Info

Use `requestInfo()` to refresh card and filesystem details asynchronously:

```cpp
auto id = sd.requestInfo();
AsyncSD::RequestResult res;
if (sd.getResult(id, &res)) {
  // res.fsInfo and res.cardInfo contain snapshots
}
```

`cardInfo()` and `fsInfo()` return the latest snapshots, including:
- Card type, OCR, CID/CSD/SCR/SDS raw registers
- Filesystem capacity, used/free bytes, cluster geometry

---

## Presence Detection

**With CD pin**
- Debounced input
- Optional interrupt (ISR sets flag only)
 - Set `cdPin = -1` to disable CD support

**Without CD pin**
- Probe-based detection with exponential backoff
- `probeFailThreshold` consecutive failures => removal
- Safe unmount on removal

---

## SPI Bus Sharing

AsyncSD never initializes the SPI bus unless `autoInitSpi=true`.  
Provide a guard to coordinate shared access:

```cpp
class MyGuard : public AsyncSD::ISpiBusGuard {
 public:
  bool lock(uint32_t timeoutMs) override { /* ... */ }
  void unlock() override { /* ... */ }
};
```

If no guard is provided, AsyncSD creates an internal mutex guard.

---

## Async Request/Result API

All operations are **queued** and completed by the worker.

```cpp
auto id = sd.requestOpen("/log.txt",
                         AsyncSD::OpenMode::Write | AsyncSD::OpenMode::Create);

AsyncSD::RequestResult res;
if (sd.getResult(id, &res)) {
  // res.handle contains file handle on success
}
```

**Buffer lifetime:** For `requestRead`/`requestWrite`, the caller must keep the buffer valid until the result is received.

**Safer writes:** `requestWriteCopy()` copies data into a bounded internal pool for transient buffers
(stack data, ISR-produced buffers, etc.). Copy size is limited by `maxCopyWriteBytes` and pool size
by `copyWriteSlots`.

**Append writes:** Use `AsyncSD::APPEND_OFFSET` as the offset to append to the end of a file.

**Result overflow telemetry:** If the result queue overflows, `getDroppedResults()` increments and
`lastErrorInfo()` reports `ResultEnqueue` with `ErrorCode::Busy`.

**Optional global result callback:** Set `SdCardConfig::onResult` to receive every result in worker
context. The callback must be fast and nonblocking, and must not call into AsyncSD in a way that
could deadlock.

---

## Examples

| Example                | Description                                |
| ---------------------- | ------------------------------------------ |
| `01_spi_cli_control`   | Interactive CLI to exercise the full API   |

---

## Testing

Host-only tests (debounce/backoff logic):

```bash
pio test -e native_test
```

---

## Project Structure

```
include/AsyncSD/     # Public headers
  |-- Status.h
  |-- Config.h
  |-- AsyncSD.h
src/                # Implementation
examples/           # Example apps
```

---

## License

MIT License. See `LICENSE`.

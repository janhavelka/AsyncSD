# AsyncSD Robustness Audit

Date: 2026-02-02  
Auditor: Codex (automated review requested by maintainer)  
Scope: Full codebase review for failure, stall, block, or crash scenarios.  
Target: ESP32-S2/S3, Arduino framework, PlatformIO, SdFat v2 (SdFs/FsFile).

## Executive Summary

AsyncSD implements a nonblocking SD card manager using a worker task and bounded queues. The public API only enqueues work and never performs blocking I/O on the caller thread. The worker is the sole owner of SdFat objects and file handles.

**Critical fixes applied during this audit:**
1. Prevented use-after-free on `end()` by force-stopping the worker task if it does not stop within timeout.
2. Added SPI bus locking for unmount to avoid bus collisions.
3. Fixed FAT32 file size limit check for overwrite and append cases.
4. Added explicit guard against oversized `size_t` reads/writes (uint32_t truncation).
5. Guarded against double `begin()` without `end()`.

Residual risks primarily stem from SdFat internals possibly blocking in the worker task and from bus guard behavior supplied by the application. These are documented below.

---

## Architecture Overview

**Core components**
- `AsyncSD::SdCardManager` (public API)
- `AsyncSD::SdCardConfig` (configuration, all hardware injected)
- `SdFs` + `FsFile` (SdFat v2 for FAT32 + exFAT)
- `ISdTransport` abstraction:
  - `SpiTransport` (fully implemented)
  - `SdmmcTransport` (stub, returns `Unsupported`)
- Bounded request queue and bounded result queue
- Presence detection:
  - CD pin with debounce and optional interrupt flag
  - No-CD probe with exponential backoff and failure hysteresis

**Threading model**
- Default: library-owned worker task
- Optional: external worker via `workerStep(budget_us)`
- Public APIs are nonblocking and not thread-safe (single caller task expected)
- Worker is the only thread touching SdFat or file handles

---

## Boundedness and Timing

**Guaranteed bounded**
- Public APIs: always return quickly; only enqueue work
- Queue size: fixed (`requestQueueDepth`, `resultQueueDepth`)
- Worker step: `workerBudgetUs` bound; read/write chunk size `ioChunkBytes`
- Lock attempts: bounded by `lockTimeoutMs`
- Requests: absolute deadline (`enqueue_ms + timeout_ms`)

**Potentially unbounded internally**
- SdFat operations can still block inside the worker task in rare cases
  - This does not block the caller thread
  - It can stall the worker task (no progress on queue)

---

## Memory and Allocation

- All allocations happen in `begin()` (queues, path pool, file slots, SdFs).
- No heap allocation in steady state or `workerStep()`.
- Fixed-size path buffers; long paths are rejected with `PathTooLong`.
- Bounded file slots (`maxOpenFiles`).

---

## Failure/Crash Scenarios and Handling

### 1) Double Initialization
**Risk:** repeated `begin()` without `end()` could leak memory or corrupt state.  
**Fix:** added guard in `SdCardManager::begin()` returning `Busy` and no re-init.  
**Files:** `src/AsyncSD.cpp` (`SdCardManager::begin`).

### 2) Worker Task Does Not Stop During `end()`
**Risk:** worker blocked in SdFat could lead to use-after-free when resources are freed.  
**Fix:** `end()` now force-stops the worker task after timeout and proceeds with cleanup to avoid use-after-free.  
**Files:** `src/AsyncSD.cpp` (`SdCardManager::end`, destructor).

### 3) SPI Bus Collision During Unmount
**Risk:** unmount previously called SdFat without bus guard, violating shared bus rules.  
**Fix:** `performUnmount` now locks the bus before closing files and calling transport `end()`. If the lock fails, it performs a best-effort logical close (no bus I/O) and returns `BusNotAvailable`.  
**Files:** `src/AsyncSD.cpp` (`performUnmount`).

### 4) FAT32 Size Limit Check Incorrect
**Risk:** writing at offset lower than current file size could exceed FAT32 limit without detection.  
**Fix:** size check now uses `max(fileSize, offset) + length`.  
**Files:** `src/AsyncSD.cpp` (`RequestType::Write` path).

### 5) Read/Write Length Overflow
**Risk:** `size_t` > `uint32_t` truncation could corrupt behavior.  
**Fix:** reject requests where `len > UINT32_MAX`.  
**Files:** `src/AsyncSD.cpp` (`requestRead`, `requestWrite`).

---

## Scenario Review Matrix (Key Cases)

**Initialization**
- Invalid config (queue depth 0, path length < 2, missing CS pin): `begin()` returns false, `SdStatus::Fault`, `ErrorCode::InvalidArgument`.
- Unsupported transport on non-S3: `begin()` returns false, `ErrorCode::Unsupported`.

**Queueing**
- Request queue full: `request*()` returns `INVALID_REQUEST_ID`, `ErrorCode::Busy`.
- Result queue full: results dropped, `ErrorCode::Busy` recorded.

**Mount/Unmount**
- Mount without card: `CardInitFailed` or `MountFailed`, status `Error`.
- Unsupported filesystem: `FsUnsupported`, status `Error`.
- Unmount while bus locked: `BusNotAvailable`, status `Error`, best-effort logical close.

**I/O**
- Read/write when not mounted: `NotReady`.
- Invalid handle: `InvalidArgument`.
- Too many open files: `TooManyOpenFiles`.
- FAT32 file size overflow: `FileTooLarge`.
- Disk full: `NoSpaceLeft` when `errno == ENOSPC` (SdFat may not always set errno).

**Presence**
- CD pin flapping: debounced, no rapid state changes.
- No-CD mode: exponential backoff for mount retries; removal after `probeFailThreshold` consecutive failures.

**Timeouts**
- Request exceeds deadline: `Timeout`, result returned with partial progress.
- Bus lock timeout: `BusNotAvailable`, request fails quickly.

---

## Additional Robustness Notes

### Queue Full
If request or result queues are full, requests fail immediately with `Busy`. No blocking or heap growth occurs.

### Card Removal During I/O
I/O failures propagate as `IoError` or `NotReady`. For no-CD mode, repeated I/O errors trigger auto-unmount through failure hysteresis.

### Bus Lock Failures
Any operation requiring SPI bus lock returns `BusNotAvailable` if it cannot acquire the lock within `lockTimeoutMs`.

### CD Pin Noise
Debounce uses `cdDebounceMs`, with optional interrupt flag. Stable transitions are required to change presence state.

### No-CD Mode
Auto-mount is retried using exponential backoff. Removal requires `probeFailThreshold` consecutive failures.

---

## Residual Risks (Not Fully Eliminated)

1. **SdFat internal blocking**  
   - SdFat calls can still block within the worker task.
   - This does not block the caller thread but can starve queued work.
   - Mitigation: worker task isolates this, but a stuck worker still stalls throughput.

2. **User-provided SPI guard correctness**  
   - If the guard deadlocks or never releases, operations will fail or stall.
   - Mitigation: guard uses bounded timeout; recommend guard correctness tests.

3. **Worker stop on shutdown**  
   - If worker is blocked, `end()` force-deletes the task after timeout to avoid UAF.
   - This can leave the SPI bus or guard in an unknown state if the worker was mid-transaction.

4. **Result queue overflow**  
   - If results are never drained, future results are dropped with `Busy`.
   - Mitigation: application should poll `popResult()` regularly or use callbacks.

5. **Buffer lifetime for read/write**  
   - Caller must keep buffers valid until the result arrives.
   - Misuse can cause data corruption or crashes outside library control.

---

## Recommendations for Long-Running Systems

1. **Implement watchdog for worker task**  
   Monitor worker progress and trigger recovery if it stalls for too long.

2. **Expose a health metric**  
   Track last successful I/O time and queue depth to detect degradation early.

3. **Add SD card reinit strategy**  
   If `performMount` fails repeatedly, consider escalating to `Fault` with explicit reinit.

4. **Bus Guard Validation**  
   Add a test harness for your SPI guard to verify lock/unlock correctness and timeout behavior.

---

## Test Coverage

Implemented host-only tests for presence helpers:
- `Debounce`
- `Backoff`
- `FailureHysteresis`

**Missing tests (recommended):**
- SPI bus lock/unlock behavior under contention
- Mount/unmount request sequence with concurrent queue load
- Card removal during active read/write
- Result queue overflow behavior

---

## File References

Core implementation:
- `src/AsyncSD.cpp`
- `src/Presence.h`

Public API:
- `include/AsyncSD/AsyncSD.h`
- `include/AsyncSD/Config.h`
- `include/AsyncSD/Status.h`

Examples:
- `examples/01_spi_cli_control/main.cpp`
- `examples/00_compile_only/main.cpp`

Tests:
- `test/native/test_presence.cpp`

---

## Conclusion

AsyncSD is designed around bounded, nonblocking operation with robust error propagation. The critical fixes applied in this audit reduce crash risks (notably the worker shutdown UAF) and improve correctness on FAT32 size checks and unmount bus safety. The largest remaining risk is the possibility of SdFat blocking the worker task, which should be addressed operationally via watchdogs and system-level recovery strategies.

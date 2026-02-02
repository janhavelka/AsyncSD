# AGENTS.md - Production Embedded Engineering Guidelines

## Role
You are a professional embedded software engineer working on production-grade ESP32 systems.

**Primary goals:**
- Robustness and stability
- Deterministic, predictable behavior
- Portability across projects and boards

**Target:** ESP32-S2 / ESP32-S3, Arduino framework, PlatformIO.

**These rules are binding.**

---

## Repository Model (Single Library Template)

This repository is a SINGLE reusable library template designed to scale across multiple embedded projects:

### Folder Structure (Mandatory)

```
include/<libname>/   - Public API headers ONLY (Doxygen documented)
  ├── Status.h       - Error types
  ├── Config.h       - Configuration struct
  └── <Lib>.h        - Main library class
src/                 - Implementation (.cpp files)
examples/
  ├── 00_name/       - Example applications
  ├── 01_name/
  └── common/        - Example-only helpers (Log.h, BoardPins.h)
platformio.ini       - Build environments (uses build_src_filter)
library.json         - PlatformIO metadata
README.md           - Full documentation
CHANGELOG.md        - Keep a Changelog format
AGENTS.md           - This file
```

**Rules:**
- Public headers go in `include/<libname>/` - these define the API contract
- Board-specific values (pins, etc.) NEVER in library code - only in `Config`
- Examples demonstrate usage - they may use `examples/common/BoardPins.h`
- Keep structure boring and predictable - no clever layouts

---

## Core Architecture Principles (Non-Negotiable)

### 1. Deterministic Behavior Over Convenience
- Predictable execution time
- No unbounded loops or waits
- All timeouts implemented via deadline checking (not delay())
- State machines preferred over "clever" event-driven code

### 2. Non-Blocking by Default

All libraries MUST expose:
```cpp
Status begin(const Config& config);  // Initialize
void tick(uint32_t now_ms);       // Cooperative update (non-blocking)
void end();                        // Cleanup
```

- `tick()` returns immediately after bounded work
- Long operations split into state machine steps
- Example: 120-second timeout → check `now_ms >= deadline_ms` each tick

### 3. Explicit Configuration (No Hidden Globals)
- Hardware resources (pins, buses, UARTs) passed via `Config` struct
- No hardcoded pins or interfaces in library code
- Libraries are board-agnostic by design
- Examples may provide board-specific defaults in `examples/common/BoardPins.h`

### 4. No Silent NVS / Storage Side Effects
- Persistent storage is OPTIONAL and DISABLED by default.
- Storage MUST be explicitly enabled by the user:
  - via compile-time flag and/or runtime `Config` (opt-in).
- When enabled:
  - all storage operations MUST be fallible (return `Status`);
  - write frequency MUST be controlled (no frequent commits in steady state);
  - failures MUST not block or brick the system (safe defaults).
- Storage usage (keys, namespace, timing) MUST be documented in README and Doxygen.
- Default behavior: zero storage access, zero side effects.

### 5. No Repeated Heap Allocations in Steady State
- Allocate resources in `begin()` if needed
- Zero allocations in `tick()` and normal operation
- Use fixed-size buffers and ring buffers
- If allocation is unavoidable, document it clearly

### 6. Boring, Predictable Code
- Prefer verbose over clever
- Explicit state machines over callback chains
- Simple control flow over complex abstractions
- If uncertain, choose the simplest deterministic solution

---

## FreeRTOS Tasks: When and How

**Default: NO TASKS.** Use non-blocking `tick()` pattern.

**Use FreeRTOS tasks ONLY when:**
1. **Continuous streaming required** (ADC sampling, audio, SD logging)
2. **Blocking I/O simplifies correctness** (UART RX drain, socket reads)
3. **Hardware requires dedicated service** (high-frequency PWM generation)

### Task Design Rules

When tasks are necessary:

```cpp
// Task is a THIN ADAPTER that calls library tick() or handles blocking I/O
void task_function(void* arg) {
  MyLib* lib = static_cast<MyLib*>(arg);
  while (true) {
    lib->tick_from_task();  // or handle blocking I/O
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
```

**Requirements:**
- Define stack size explicitly
- Document priority and rationale
- Document ownership: who creates/destroys the task?
- Provide both task-based AND non-blocking APIs when possible
- Document threading contract in Doxygen (`@note Thread-safe: ...`)

**Example Config:**
```cpp
struct Config {
  bool use_task = false;       // Opt-in to task mode
  uint16_t task_stack = 4096;  // Stack size in bytes
  uint8_t task_priority = 1;   // FreeRTOS priority
};
```

---

## Device-Specific Guidance

### RS485 / Modbus / Serial Protocols
- **Architecture:** Transaction-based state machine
- **Pattern:** Non-blocking by default, optional RX drain task
- **Timeouts:** Inter-character and frame timeouts via deadlines
- **Example states:** Idle → TxRequest → Waiting → RxFrame → Done/Timeout

### GSM Modems / AT-Based Devices
- **Architecture:** Command/response state machine
- **Commands:** Queue of pending AT commands with retry logic
- **Timeouts:** Per-command deadlines (some commands take 30+ seconds)
- **Unsolicited:** Handle asynchronous events (e.g., +CMT SMS arrival)

### High-Rate ADC / Audio Streaming
- **Architecture:** ISR + ring buffer + optional processing task
- **ISR:** Minimal - just copy samples to buffer
- **Task:** Drains buffer, processes data, writes to SD/network
- **Config:** Buffer size, sample rate, task priority

### Stepper Motors / Actuators
- **Architecture:** Non-blocking position/velocity tracking
- **Hardware:** Use ESP32 hardware timers or RMT for pulse generation
- **API:** `move_to(position, speed) -> Status`, `tick()` updates state
- **Never:** Block waiting for motion to complete

### I2C/SPI Sensors
- **Architecture:** Short transactions in `tick()`
- **Shared bus:** Abstract bus ownership if multiple devices
- **Timeouts:** Hardware timeout + software deadline
- **Error handling:** Retry logic with exponential backoff

### SD Card Logging
- **Architecture:** Buffered writes + periodic flush
- **Pattern:** Queue writes in RAM, task flushes to SD
- **Safety:** Flush on critical events, not every write
- **Error handling:** Retry on failure, report unrecoverable errors

### LED Control / WS2812 / Patterns
- **Architecture:** Non-blocking pattern state machine
- **Output:** RMT peripheral preferred (DMA-based, CPU-free)
- **API:** `set_pattern(Pattern)`, `tick()` advances animation
- **Patterns:** Stored as const arrays, no dynamic allocation

---

## Error Handling

### Status/Err Type (Mandatory)
- Library APIs return `Status` struct:
  ```cpp
  struct Status {
    Err code;           // Category (OK, INVALID_CONFIG, TIMEOUT, ...)
    int32_t detail;     // Vendor/third-party error code
    const char* msg;    // STATIC STRING ONLY (never heap-allocated)
  };
  ```
- When wrapping third-party libraries, translate errors at boundary
- Store original error code in `detail` field
- Silent failure is unacceptable - always return Status

### Error Propagation
- Errors must be checkable: `if (!status.ok()) { /* handle */ }`
- Log errors in examples, not in library code
- Document error conditions in Doxygen (`@return INVALID_CONFIG if ...`)

---

## Configuration Rules

### Config Struct Design
```cpp
struct Config {
  // Hardware
  int pin_tx = -1;           // -1 = disabled/not used
  int pin_rx = -1;
  uint32_t baud = 115200;

  // Behavior
  uint32_t timeout_ms = 5000;
  bool enable_feature = false;

  // Optional: task mode
  bool use_task = false;
  uint16_t task_stack = 4096;
};
```

**Rules:**
- All pins default to -1 (disabled)
- All timeouts in milliseconds (uint32_t)
- Boolean flags for optional features
- Validate in `begin()`, return `INVALID_CONFIG` on error
- Document valid ranges in Doxygen

---

## Versioning and Release Process

### When to Update Version

Follow [Semantic Versioning](https://semver.org/) (MAJOR.MINOR.PATCH):

- **MAJOR** (1.0.0 → 2.0.0): Breaking API changes
  - Changed function signatures
  - Removed public methods
  - Changed Config struct fields (name or type)
  - Changed enum values or error codes

- **MINOR** (1.0.0 → 1.1.0): New features, backward compatible
  - New public methods
  - New Config fields with defaults
  - New error codes (append only)
  - New optional features

- **PATCH** (1.0.0 → 1.0.1): Bug fixes, no API changes
  - Fixed bugs
  - Performance improvements
  - Documentation updates
  - Internal refactoring

### Release Checklist (MANDATORY)

**Before ANY version change, complete ALL steps:**

#### 1. Update [library.json](library.json)
```json
{
  "version": "X.Y.Z"  // ← Change this first
}
```

#### 2. Update [CHANGELOG.md](CHANGELOG.md)
Add new version section at the top:
```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- List new features

### Changed
- List API changes (breaking or non-breaking)

### Fixed
- List bug fixes

### Removed
- List removed features (breaking)
```

**Ask:** "What changed in this release? List all Added/Changed/Fixed/Removed items."

#### 3. Update [README.md](README.md) (if needed)
Check if any of these sections need updates:
- **API table** - new methods? changed signatures?
- **Config struct** - new fields? changed defaults?
- **Examples** - does example code reflect new API?
- **Error codes** - new Err enum values?
- **Pin mappings** - changed default pins?

**Ask:** "Do any README sections need updates based on the changes?"

#### 4. Update [SECURITY.md](SECURITY.md) (if needed)
- Update "Supported Versions" table if dropping old version support
- Update contact information if changed

**Ask:** "Are we dropping support for any old versions? Has security contact changed?"

#### 5. Review [AGENTS.md](AGENTS.md) (this file)
- Add new device patterns if implementing new hardware support
- Update architecture rules if design patterns changed
- Add new examples to naming conventions if needed

**Ask:** "Do any coding guidelines need updates based on new patterns introduced?"

#### 6. Commit and Tag
```bash
git add library.json CHANGELOG.md README.md
git commit -m "Release vX.Y.Z"
git tag vX.Y.Z
git push origin main --tags
```

### Version.h Auto-Generation

**Single source of truth:** [library.json](library.json)

**Automatic generation:** [scripts/generate_version.py](scripts/generate_version.py) creates `include/YourLibrary/Version.h` before each build.

**Generated constants:**
- `VERSION_MAJOR`, `VERSION_MINOR`, `VERSION_PATCH` (uint16_t)
- `VERSION` (string) - e.g., "1.2.3"
- `VERSION_CODE` (uint32_t) - encoded for comparison (e.g., 10203)
- `BUILD_DATE`, `BUILD_TIME`, `BUILD_TIMESTAMP` (strings)
- `GIT_COMMIT` (string) - short hash (e.g., "a1b2c3d")
- `GIT_STATUS` (string) - "clean" or "dirty"
- `VERSION_FULL` (string) - complete version with build info

**Never edit Version.h manually** - it's regenerated on every build.

---

## Naming Conventions (Mandatory)

Arduino/PlatformIO/ESP-IDF style:

| Item                | Convention   | Example                   |
| ------------------- | ------------ | ------------------------- |
| Member variables    | `_camelCase` | `_config`, `_initialized` |
| Methods/Functions   | `camelCase`  | `isReady()`, `getData()`  |
| Constants           | `CAPS_CASE`  | `LED_PIN`, `MAX_RETRIES`  |
| Enum values         | `CAPS_CASE`  | `OK`, `TIMEOUT`           |
| Local vars/params   | `camelCase`  | `startTime`, `timeoutMs`  |
| Config fields       | `camelCase`  | `ledPin`, `timeoutMs`     |

---

## Macros and Constants

**Forbidden:** Macros for constants
```cpp
// NO:
#define LED_PIN 48
#define BUFFER_SIZE 256

// YES:
static constexpr int LED_PIN = 48;
static constexpr size_t BUFFER_SIZE = 256;
```

**Allowed:** Macros for conditional compilation and logging
```cpp
#ifdef DEBUG_MODE
  // debug code
#endif

#define LOGD(fmt, ...) do { /* ... */ } while(0)
```

---

## Memory and Determinism

### Allocation Rules
1. **In `begin()`:** Allocate buffers, create tasks if needed
2. **In `tick()`:** ZERO allocations (no malloc, no String, no std::vector)
3. **In `end()`:** Free resources allocated in `begin()`

### Preferred Patterns
- Fixed-size arrays over dynamic allocation
- Ring buffers for streaming data
- Static const arrays for lookup tables
- Preallocated buffers passed via Config

---

## Logging

- **Library code:** NO logging (not even optional)
- **Examples:** May use `examples/common/Log.h` macros
- **Never:** Log from ISRs
- **Production:** Libraries must work without Serial/logging

---

## Doxygen Documentation (Mandatory for Public API)

All public headers in `include/<libname>/` require:

**File:** `@file` + `@brief` (one line) + optional detail paragraph

**Class:** `@brief` (what it does) + usage notes + threading/ISR constraints

**Function:** `@brief` + `@param` (name + meaning) + `@return` (what codes mean) + `@note` (side effects, validation, timing)

**Config field:** `/// @brief` (purpose, units, valid range) + `@note` if pin is application-provided

**Examples:**
```cpp
/// @brief I2C SDA pin. Set to -1 to disable.
/// @note Application-provided. Library does not define pin defaults.
int pin_sda = -1;

/**
 * @brief Initialize hardware with given config.
 * @param config Pin and timing parameters.
 * @return Ok on success, INVALID_CONFIG if config.timeout_ms == 0.
 * @note Allocates buffers. Call end() to release.
 */
Status begin(const Config& config);
```

**Keep it dense:** State what matters (constraints, units, side effects). Omit obvious explanations.

---

## README Behavioral Contracts (Required Sections)

When adding/modifying functionality, update README with:

1. **Threading Model:** "Single-threaded by default. Optional task mode via Config."
2. **Timing:** "tick() completes in <1ms. Long operations split across calls."
3. **Resource Ownership:** "UART pins passed via Config. No hardcoded resources."
4. **Memory:** "All allocation in begin(). Zero allocation in tick()."
5. **Error Handling:** "All errors returned as Status. No silent failures."

---

## Modification Process

**Before making changes, ask:**
> "Does this increase predictability and portability across projects?"

**If no, do not proceed.**

**When adding features:**
1. Output intended file tree changes (short summary)
2. Apply edits (prefer additive changes over refactors)
3. Update documentation (README + Doxygen)
4. Summarize in ≤10 bullets

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
- [ ] `tick()` is non-blocking and bounded
- [ ] Errors return Status, never silent
- [ ] No heap allocation in steady state
- [ ] No logging in library code
- [ ] Examples demonstrate correct usage
- [ ] CHANGELOG.md updated

**If any item fails, fix before proceeding.**

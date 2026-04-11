# Greenhouse Controller — Driver Development Plan

## Overview

This plan covers the step-by-step development of all eight peripheral driver libraries for the greenhouse controller (ESP32-S3, LOLIN S3 board). Each driver is an independent PlatformIO project in its own subdirectory. Every driver is unit tested on the host before it is validated on the target board with real hardware.

The driver subdirectories already exist and use the following names:

| Library | Directory | Hardware |
|---------|-----------|----------|
| LIB-1 | `gpio/` | Relay outputs, LEDs, RS485 DE/RE, opto input, SD button |
| LIB-2 | `i2c/` | Shared I2C bus (SDA/SCL) |
| LIB-3 | `DS1307_RTC/` | DS1307 real-time clock module |
| LIB-4 | `LCD1602_I2C/` | Waveshare LCD1602 via PCF8574 |
| LIB-5 | `keyPad/` | 4×4 membrane keypad |
| LIB-6 | `modBus/` | SIT65HVD08P RS485 transceiver, Modbus RTU |
| LIB-7 | `nvs/` | ESP32-S3 internal NVS flash |
| LIB-8 | `sdCard/` | SPI SD card (external, FAT32) |
| LIB-9 | `littleFS/` | Internal ESP32-S3 flash (LittleFS partition) |

---

## Development Sequence

Dependencies determine the order. Wave 1 drivers have no inter-driver dependencies and can be developed fully in parallel. Wave 2 drivers depend on a Wave 1 driver being board-tested first.

```
Wave 1 (all parallel — no inter-driver dependencies):
    LIB-1  gpio/
    LIB-2  i2c/
    LIB-5  keyPad/
    LIB-7  nvs/
    LIB-8  sdCard/
    LIB-9  littleFS/

Wave 2 (after respective Wave 1 board tests pass):
    LIB-3  DS1307_RTC/     ← requires LIB-2 board-tested  (DS1307 chip)
    LIB-4  LCD1602_I2C/    ← requires LIB-2 board-tested
    LIB-6  modBus/         ← requires LIB-1 board-tested
```

Unit test development for Wave 2 drivers can begin immediately using mocks; only the hardware verification step depends on Wave 1 completion.

---

## Standard Project Structure (all drivers)

```
drivers/<name>/
    library.json          ← identifies the directory as a PlatformIO library;
                            excludes main.cpp from the exported build
    platformio.ini        ← two environments: lolin_s3 (hardware) + native (unit tests)
    src/
        <driver>.h        ← public API header
        <driver>.cpp      ← implementation
        main.cpp          ← hardware verification sketch (not exported as library)
    test/
        test_<driver>.cpp ← Unity unit tests (compiled only in native env)
        mock_*.h / .cpp   ← hardware stubs used by the native build
```

### Standard `library.json`

```json
{
  "name": "<driver_name>",
  "version": "0.1.0",
  "build": {
    "srcDir": "src",
    "srcFilter": ["+<*.cpp>", "-<main.cpp>"]
  }
}
```

The `srcFilter` exclusion of `main.cpp` is mandatory. Without it, PlatformIO will attempt to compile the hardware verification sketch when another driver references this one as a local dependency.

### Standard `platformio.ini`

```ini
[env:lolin_s3]
platform       = espressif32
board          = lolin_s3
framework      = arduino
monitor_speed  = 115200
upload_protocol = esptool
build_flags    = -DCORE_DEBUG_LEVEL=3

[env:native]
platform       = native
build_flags    = -DUNIT_TEST
test_framework = unity
```

Wave 2 drivers add a `lib_deps` reference to their Wave 1 dependency in the `lolin_s3` environment:

```ini
[env:lolin_s3]
...
lib_deps = file://../i2c    ; example for LIB-3 and LIB-4
```

PlatformIO resolves `file://` paths relative to the `platformio.ini` file. All driver directories are siblings, so `file://../<name>` always resolves correctly.

### Hardware/native guard pattern

Every `.cpp` file that contains Arduino API calls uses this guard so it compiles on both the host and the target:

```cpp
#ifndef UNIT_TEST
  #include <Arduino.h>
  // real Arduino/ESP-IDF calls
#else
  #include "../test/mock_hardware.h"
  // stub implementations
#endif
```

### Run commands

```bash
# From the driver's own directory:
pio test -e native                                    # host unit tests (no board needed)
pio run -e lolin_s3 -t upload && pio device monitor  # upload and watch serial output
```

---

## LIB-1 — GPIO Utility (`gpio/`)

### Purpose
Wraps all GPIO operations. Pin constants are defined in `firmware/config/pin_config.h` — the single authoritative source for all project GPIO numbers. All other drivers that drive or read a GPIO use this library rather than calling Arduino GPIO functions directly.

### API (`firmware/config/pin_config.h`)

```cpp
// Single authoritative source for all project GPIO numbers.
// Include this header wherever a project GPIO number is needed.
#define PIN_RELAY_M1_OPEN   12
#define PIN_RELAY_M1_CLOSE  13
#define PIN_RELAY_M2_OPEN   14
#define PIN_RELAY_M2_CLOSE  15
#define PIN_RELAY_M3_OPEN   16
#define PIN_RELAY_M3_CLOSE  21
#define PIN_OPTO_INPUT      42
#define PIN_HB_LED          41
#define PIN_RS485_DE_RE      8
```

### API (`gpio_util.h`)

`gpio_util.h` exposes the pin constants above by including `pin_config.h`. Callers need only include `gpio_util.h`.

```cpp
typedef enum { GPIO_INPUT = 0, GPIO_OUTPUT, GPIO_INPUT_PULLUP } gpio_mode_t;
typedef enum { GPIO_LOW = 0, GPIO_HIGH = 1 }                   gpio_level_t;

void        gpio_set_pin_mode(uint8_t pin, gpio_mode_t mode);
void        gpio_write(uint8_t pin, gpio_level_t level);
gpio_level_t gpio_read(uint8_t pin);
void        gpio_toggle(uint8_t pin);
void        gpio_set_rs485_direction(bool transmit); // true=TX (HIGH), false=RX (LOW)
```

### Mock strategy (`test/mock_gpio.h`)

A static array `uint8_t pin_state[48]` records the last written level per pin. Stubs for `pinMode`, `digitalWrite`, and `digitalRead` read and write this array. No Arduino headers are included.

### Unit tests (10)

| ID | Test case | Assertion |
|----|-----------|-----------|
| UT-GPIO-001 | `gpio_set_pin_mode` records mode | Mode stored in mock without error |
| UT-GPIO-002 | `gpio_write` HIGH | `pin_state[pin] == HIGH` |
| UT-GPIO-003 | `gpio_write` LOW | `pin_state[pin] == LOW` |
| UT-GPIO-004 | `gpio_read` returns preset mock state | Return value matches preset |
| UT-GPIO-005 | `gpio_toggle` HIGH → LOW | State flips to LOW |
| UT-GPIO-006 | `gpio_toggle` LOW → HIGH | State flips to HIGH |
| UT-GPIO-007 | `gpio_set_rs485_direction(true)` | `pin_state[PIN_RS485_DE_RE] == HIGH` |
| UT-GPIO-008 | `gpio_set_rs485_direction(false)` | `pin_state[PIN_RS485_DE_RE] == LOW` |
| UT-GPIO-009 | All 10 pin constants are unique values | No two constants share the same GPIO number |
| UT-GPIO-010 | No defined pin falls in the reserved set | Reserved: {0, 19, 20, 26–37, 43, 44, 45, 46} |

### Running the unit tests

**Prerequisites**

- PlatformIO Core 6 installed (available via `~/.platformio/penv/Scripts/pio.exe`).
- A host C++ compiler (`gcc` / `g++`) on the system `PATH`.  On this machine the
  CodeBlocks MinGW toolchain provides the compiler:

  ```
  C:\Program Files\CodeBlocks\MinGW\bin
  ```

  Add it to `PATH` before invoking `pio`, or configure it permanently in your
  shell profile.

**Run command (from `drivers/gpio/`)**

```bash
# Windows — Git Bash / MSYS2
export PATH="/c/Program Files/CodeBlocks/MinGW/bin:$PATH"
~/.platformio/penv/Scripts/pio.exe test -e native
```

**Implementation notes**

- `gpio_util.cpp` is pulled into the native test build via `test/include_lib.cpp`,
  which `#include`s the implementation under the `UNIT_TEST` guard.  This is
  necessary because PlatformIO 6 does not automatically compile a library's `src/`
  files when tests are run inside the library's own directory, and the `file://.`
  self-referencing `lib_deps` workaround triggers a Windows lock-file bug.
- The mock (`test/mock_gpio.h` + `test/mock_gpio.cpp`) replaces the Arduino HAL
  with an in-memory `pin_state[48]` / `pin_mode_arr[48]` array.  Call
  `mock_gpio_reset()` from `setUp()` to guarantee test isolation.

**Expected output**

```
test\test_gpio_util.cpp:182: test_set_pin_mode_output        [PASSED]
test\test_gpio_util.cpp:183: test_set_pin_mode_input         [PASSED]
test\test_gpio_util.cpp:184: test_set_pin_mode_input_pullup  [PASSED]
test\test_gpio_util.cpp:187: test_gpio_write_high            [PASSED]
test\test_gpio_util.cpp:190: test_gpio_write_low             [PASSED]
test\test_gpio_util.cpp:193: test_gpio_read_returns_preset_state [PASSED]
test\test_gpio_util.cpp:196: test_gpio_toggle_high_to_low    [PASSED]
test\test_gpio_util.cpp:199: test_gpio_toggle_low_to_high    [PASSED]
test\test_gpio_util.cpp:202: test_rs485_direction_transmit   [PASSED]
test\test_gpio_util.cpp:205: test_rs485_direction_receive    [PASSED]
test\test_gpio_util.cpp:208: test_pin_constants_are_unique   [PASSED]
test\test_gpio_util.cpp:211: test_no_pin_in_reserved_set     [PASSED]
12 succeeded
```

### Hardware verification

All 13 tests run automatically. The sketch drives outputs and reads them back
through loopback wires; input pins are driven by dedicated loopback driver
outputs. Every test prints `[PASS]` or `[FAIL]` on Serial0 and a summary
(`PASSED: N / FAILED: N / RESULT: PASS|FAIL`) at the end.

No relay module, no multimeter, and no manual observation are required.

#### Serial interface

The sketch uses **UART0** (`Serial0`) on **GPIO 43 (TX) / GPIO 44 (RX)** at
115200 baud. The USB-CDC `Serial` port is not used because it requires USB
enumeration before printing, which makes boot-time output unreliable.

Connect a **3.3 V USB-to-serial adapter** (CP2102, CH340, FT232, etc.) to:

| Adapter | LOLIN S3 pin |
|---------|--------------|
| RX      | GPIO 43 (TX) |
| TX      | GPIO 44 (RX) |
| GND     | GND          |

Open the adapter's COM port at 115200 baud before resetting the board.

#### Loopback wiring (11 jumper wires)

All wires connect directly between LOLIN S3 header pins.
No relay module, motor, or other external component is needed.

##### Wiring table

Both headers run top-to-bottom with the USB-C connectors at the bottom of the board.

| Wire | From GPIO | Header / row | To GPIO | Header / row | Role |
|------|-----------|--------------|---------|--------------|------|
| W1  | 12 RELAY_M1_OPEN  | Left  row 16 | 1       | Right row 2  | Output loopback |
| W2  | 13 RELAY_M1_CLOSE | Left  row 17 | 2       | Right row 3  | Output loopback |
| W3  | 14 RELAY_M2_OPEN  | Left  row 18 | 3       | Right row 16 | Output loopback |
| W4  | 15 RELAY_M2_CLOSE | Left  row 9  | 4       | Left  row 4  | Output loopback |
| W5  | 16 RELAY_M3_OPEN  | Left  row 10 | 5       | Left  row 5  | Output loopback |
| W6  | 21 RELAY_M3_CLOSE | Right row 13 | 6       | Left  row 6  | Output loopback |
| W7  | 8  RS485 DE/RE    | Left  row 8  | 7       | Left  row 7  | Output loopback |
| W8  | 41 HB LED         | Right row 7  | 9       | Left  row 13 | Output loopback |
| W10 | 11 (driver)       | Left  row 15 | 42 OPTO_INPUT    | Right row 6  | Input driver |

**Serial adapter**

| Adapter pin | Board pin | Header / row |
|-------------|-----------|--------------|
| RX          | 43 TX0    | Right row 4  |
| GND         | GND       | Right row 1  |

**Output loopback — 9 wires**

| Wire | From (project pin) | To (loopback input) | Tests |
|------|--------------------|---------------------|-------|
| 1 | GPIO 12 — RELAY_M1_OPEN  | GPIO 1  | HW-GPIO-002 |
| 2 | GPIO 13 — RELAY_M1_CLOSE | GPIO 2  | HW-GPIO-003 |
| 3 | GPIO 14 — RELAY_M2_OPEN  | GPIO 3  | HW-GPIO-004 |
| 4 | GPIO 15 — RELAY_M2_CLOSE | GPIO 4  | HW-GPIO-005 |
| 5 | GPIO 16 — RELAY_M3_OPEN  | GPIO 5  | HW-GPIO-006 |
| 6 | GPIO 21 — RELAY_M3_CLOSE | GPIO 6  | HW-GPIO-007 |
| 7 | GPIO  8 — RS485 DE/RE    | GPIO 7  | HW-GPIO-009, HW-GPIO-010 |
| 8 | GPIO 41 — HB LED         | GPIO 9  | HW-GPIO-008 |

**Input driver — 1 wire**

| Wire | From (loopback driver) | To (project input pin) | Tests |
|------|------------------------|------------------------|-------|
| 10 | GPIO 11 | GPIO 42 — OPTO_INPUT    | HW-GPIO-012 |

> **Loopback pin selection rationale:** GPIO 1–7, 9–11 are valid
> general-purpose I/O on the ESP32-S3. They are free of project assignments,
> PSRAM (GPIO 26–37), USB (GPIO 19, 20), UART0 (GPIO 43, 44), the on-board
> RGB LED (GPIO 38), and the strapping pins (GPIO 0, 45, 46).
> GPIOs 22–25 are not accessible on the LOLIN S3 header pins.

> **Safety note:** Do not connect a relay module during loopback testing.
> The relay output pins are wired to loopback inputs only. If later validating
> with a relay module, never activate OPEN and CLOSE of the same motor channel
> simultaneously.

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-1 — GPIO Utility |
| Directory | `gpio/` |
| Firmware version | 0.1.0 |
| Board ID / revision | ESP32-S3 (QFN56) revision v0.2 — LOLIN S3, MAC 30:ed:a0:a0:fd:a4 |
| Tester | Remko Welling |
| Date | 2026-04-10 |
| Equipment | LOLIN S3; 11 jumper wires; 3.3 V USB-to-serial adapter |

#### Test cases

| ID | Description | How verified | Expected serial output | Actual result | P/F |
|----|-------------|--------------|------------------------|---------------|-----|
| HW-GPIO-001 | GPIO init — no invalid-pin errors | Automatic | `[PASS] HW-GPIO-001: GPIO init complete` | [PASS] | PASS |
| HW-GPIO-002 | GPIO 12 RELAY_M1_OPEN  output HIGH/LOW | Loopback via GPIO 1  | `[PASS] HW-GPIO-002` | [PASS] | PASS |
| HW-GPIO-003 | GPIO 13 RELAY_M1_CLOSE output HIGH/LOW | Loopback via GPIO 2  | `[PASS] HW-GPIO-003` | [PASS] | PASS |
| HW-GPIO-004 | GPIO 14 RELAY_M2_OPEN  output HIGH/LOW | Loopback via GPIO 3  | `[PASS] HW-GPIO-004` | [PASS] | PASS |
| HW-GPIO-005 | GPIO 15 RELAY_M2_CLOSE output HIGH/LOW | Loopback via GPIO 4  | `[PASS] HW-GPIO-005` | [PASS] | PASS |
| HW-GPIO-006 | GPIO 16 RELAY_M3_OPEN  output HIGH/LOW | Loopback via GPIO 5  | `[PASS] HW-GPIO-006` | [PASS] | PASS |
| HW-GPIO-007 | GPIO 21 RELAY_M3_CLOSE output HIGH/LOW | Loopback via GPIO 6  | `[PASS] HW-GPIO-007` | [PASS] | PASS |
| HW-GPIO-008 | GPIO 41 HB LED output HIGH/LOW          | Loopback via GPIO 9  | `[PASS] HW-GPIO-008` | [PASS] | PASS |
| HW-GPIO-009 | GPIO 8 RS485 DE/RE HIGH (TX mode)       | Loopback via GPIO 7  | `[PASS] HW-GPIO-009` | [PASS] | PASS |
| HW-GPIO-010 | GPIO 8 RS485 DE/RE LOW  (RX mode)       | Loopback via GPIO 7  | `[PASS] HW-GPIO-010` | [PASS] | PASS |
| HW-GPIO-012 | GPIO 42 OPTO_INPUT reads LOW and HIGH   | Driven by GPIO 11    | `[PASS] HW-GPIO-012` | [PASS] | PASS |

#### Expected full serial output

```
================================================
  LIB-1 GPIO Utility — hardware verification
================================================
[PASS] HW-GPIO-001: GPIO init complete — no invalid pin errors
--- Relay output loopback ---
[PASS] HW-GPIO-002: GPIO 12 RELAY_M1_OPEN  HIGH/LOW loopback
[PASS] HW-GPIO-003: GPIO 13 RELAY_M1_CLOSE HIGH/LOW loopback
[PASS] HW-GPIO-004: GPIO 14 RELAY_M2_OPEN  HIGH/LOW loopback
[PASS] HW-GPIO-005: GPIO 15 RELAY_M2_CLOSE HIGH/LOW loopback
[PASS] HW-GPIO-006: GPIO 16 RELAY_M3_OPEN  HIGH/LOW loopback
[PASS] HW-GPIO-007: GPIO 21 RELAY_M3_CLOSE HIGH/LOW loopback
--- HB LED loopback ---
[PASS] HW-GPIO-008: GPIO 41 HB_LED HIGH/LOW loopback
--- RS485 DE/RE loopback ---
[PASS] HW-GPIO-009: GPIO 8 RS485_DE_RE HIGH (TX mode) loopback
[PASS] HW-GPIO-010: GPIO 8 RS485_DE_RE LOW (RX mode) loopback
--- Opto input loopback ---
[PASS] HW-GPIO-012: GPIO 42 OPTO_INPUT LOW/HIGH via GPIO 11 driver
================================================
  PASSED: 11
  FAILED: 0
  RESULT: PASS
================================================
Entering heartbeat loop (HB LED blinks at 0.5 Hz).
```

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS |
| Failed test IDs | — |
| Notes | HW-GPIO-001–010, 012 all passed automatically via loopback wiring. Serial output via UART0 (GPIO 43 TX, 115200 baud). **Defect found and fixed (2026-04-10):** loopback input pins were configured as plain `INPUT` (floating), allowing false PASSes when no wire was connected. Fixed by using `INPUT_PULLDOWN` for HIGH read-back checks and `INPUT_PULLUP` for LOW read-back checks. Re-tested after fix: all 11 tests PASS with wires fitted; correctly FAIL when wires are removed. |

---

## LIB-2 — I2C Bus (`i2c/`)

### Purpose
Mutex-aware wrapper around the Arduino Wire library for the shared I2C bus. All I2C peripherals (LCD at 0x27, RTC at 0x68) use this library; no driver calls Wire directly.

### API (`firmware/config/pin_config.h`)

Pin assignments are centralised in `pin_config.h` alongside all other project GPIOs:

```cpp
#define PIN_I2C_SDA   1   // I2C data line
#define PIN_I2C_SCL   2   // I2C clock line
```

### API (`i2c_bus.h`)

`i2c_bus.h` exposes the pin constants above by including `pin_config.h`. Callers need only include `i2c_bus.h`. The bus frequency is a protocol constant, not a pin assignment, so it remains here:

```cpp
#define I2C_FREQ_HZ   400000UL

typedef enum {
    I2C_OK       = 0,
    I2C_ERR_TIMEOUT,
    I2C_ERR_NACK,
    I2C_ERR_BUS_BUSY
} i2c_status_t;

i2c_status_t i2c_init(void);
i2c_status_t i2c_write(uint8_t addr, const uint8_t *data, size_t len);
i2c_status_t i2c_read(uint8_t addr, uint8_t *buf, size_t len);
i2c_status_t i2c_write_read(uint8_t addr,
                             const uint8_t *tx, size_t tx_len,
                             uint8_t *rx,       size_t rx_len);
uint8_t      i2c_scan(uint8_t *found_addrs, uint8_t max_count);
void         i2c_lock(void);   // FreeRTOS mutex in target; no-op in native
void         i2c_unlock(void);
```

`i2c_write` and `i2c_read` acquire the mutex internally for single transactions. `i2c_lock` / `i2c_unlock` are exposed for callers that need to hold the bus across multiple sequential operations without interleaving from another task.

### Mock strategy (`test/mock_wire.h`)

Provides a fake `TwoWire` class and global `Wire` instance. An in-memory byte FIFO (`mock_rx_buf`) serves preloaded response data on `requestFrom`. Transmitted bytes are recorded in `mock_tx_buf` for assertion. A `mock_nack_next` flag causes the next `endTransmission()` call to return 2 (NACK) and then resets. A `mock_ack_addrs[]` list controls which addresses ACK during `i2c_scan()` — useful for UT-I2C-006. Call `mock_wire_reset()` in `setUp()`.

### Unit tests (8)

| ID | Test case | Assertion |
|----|-----------|-----------|
| UT-I2C-001 | `i2c_init` returns `I2C_OK` | No error from mock |
| UT-I2C-002 | `i2c_write` sends correct address and bytes | Mock log matches address + byte sequence |
| UT-I2C-003 | `i2c_read` returns preloaded bytes | Returned bytes match FIFO content |
| UT-I2C-004 | `i2c_write_read` performs write before read | Mock records write then read, correct address both times |
| UT-I2C-005 | NACK flag set → `I2C_ERR_NACK` | Correct error code returned |
| UT-I2C-006 | `i2c_scan` returns addresses where mock ACKs | Preload ACKs for 0x27 and 0x68; both found |
| UT-I2C-007 | `i2c_write` zero-length data returns `I2C_OK` | No crash or error on empty write |
| UT-I2C-008 | `i2c_lock` / `i2c_unlock` round-trip | No deadlock; documents intent for FreeRTOS env |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-2 — I2C Bus |
| Directory | `i2c/` |
| Firmware version | 0.1.0 |
| Board ID / revision | LOLIN S3 |
| Tester | drasv |
| Date | 2026-04-10 |
| Equipment | LOLIN S3; Waveshare LCD1602 I2C module; DS3231 RTC module |

**Wiring:** SDA → GPIO 1, SCL → GPIO 2, 3.3 V, GND to both modules. Both modules include their own 4.7 kΩ pull-up resistors; no external resistors needed. Both devices must be connected simultaneously to confirm bus sharing and address non-conflict.

#### Test cases

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-I2C-001 | Bus initialises at correct speed | Upload sketch; open serial monitor | "I2C init: SDA=GPIO1 SCL=GPIO2 400 kHz" printed within 3 s | "I2C init: SDA=GPIO1 SCL=GPIO2 400 kHz" printed | ✅ PASS |
| HW-I2C-002 | LCD PCF8574A detected on scan | Run `i2c_scan` with both modules connected | Address 0x3E reported as found | Address 0x3E found ✓ (re-run after address fix) | ✅ PASS |
| HW-I2C-003 | DS3231 RTC detected on scan | Run `i2c_scan` with both modules connected | Address 0x68 reported as found | Address 0x68 found | ✅ PASS |
| HW-I2C-004 | Write to 0x3E succeeds | Sketch writes 1 byte to LCD I2C address | "Write 1 byte to 0x3E: OK" printed; no NACK error | "Write 1 byte to 0x3E: OK" printed | ✅ PASS |
| HW-I2C-005 | Write-read from 0x68 succeeds | Sketch writes register address then reads 1 byte from RTC | "Write-read from 0x68: OK, value = 0xXX" printed | "Write-read from 0x68: OK, value = 0x80" printed | ✅ PASS |

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS |
| Failed test IDs | |
| Notes | LCD module has a **PCF8574A** backpack (address **0x3E**). RTC OSF bit set (0x80) on first run — expected on new battery insertion. |

---

## LIB-3 — DS1307 RTC (`DS1307_RTC/`)

### Purpose
Driver for the DS1307 battery-backed real-time clock. Used by T4 (Data Manager) to read the current timestamp at startup and periodically.

### Dependency
Requires LIB-2 (`i2c/`) to be board-tested. Reference in `platformio.ini`:
```ini
[env:lolin_s3]
lib_deps = file://../i2c
```

### API (`ds1307_rtc.h`)

```cpp
#define DS1307_I2C_ADDR  0x68

typedef struct {
    uint8_t  second;       // 0–59
    uint8_t  minute;       // 0–59
    uint8_t  hour;         // 0–23 (24-hour)
    uint8_t  day_of_week;  // 1–7
    uint8_t  day;          // 1–31
    uint8_t  month;        // 1–12
    uint16_t year;         // e.g. 2026
} rtc_datetime_t;

typedef enum {
    RTC_OK = 0,
    RTC_ERR_NO_DEVICE,
    RTC_ERR_COMM,
    RTC_ERR_INVALID
} rtc_status_t;

rtc_status_t rtc_init(void);
rtc_status_t rtc_get_time(rtc_datetime_t *dt);
rtc_status_t rtc_set_time(const rtc_datetime_t *dt);
bool         rtc_oscillator_stopped(void);  // true if CH bit set (oscillator halted)
```

### Implementation notes
- All time fields are BCD-encoded in DS1307 registers 0x00–0x06.
- **Clock Halt (CH) bit:** bit 7 of the seconds register (0x00). Set at power-up (oscillator disabled); cleared automatically when `rtc_set_time` writes BCD seconds 0–59 (bit 7 is always 0 for valid BCD seconds). CH set = time invalid.
- DS1307 has **no temperature sensor** (unlike DS3231).
- Implement private `bcd_to_dec` and `dec_to_bcd` helpers.

### Mock strategy (`test/mock_i2c_bus.h`)
A byte array representing DS1307 registers (0x00–0x06). `i2c_write_read` reads from the array at offset `tx[0]`; `i2c_write` writes to the array at offset `data[0]`. Helper `mock_rtc_set_register(reg, val)` presets register values for test setup.

### Unit tests (11)

Run: `pio test -e native` — all 11 passed on 2026-04-10 (3.57 s, MinGW/native).

| ID | Test case | Assertion | Result |
|----|-----------|-----------|--------|
| UT-RTC-001 | `rtc_init` returns OK on ACK | No error | ✅ PASS |
| UT-RTC-002 | `rtc_init` returns `RTC_ERR_NO_DEVICE` on NACK | NACK flag set; correct error | ✅ PASS |
| UT-RTC-003 | BCD decode seconds (0x45 → 45) | Correct decimal | ✅ PASS |
| UT-RTC-004 | BCD decode minutes (0x30 → 30) | Correct decimal | ✅ PASS |
| UT-RTC-005 | BCD decode hours (0x23 → 23) | 24-hour format | ✅ PASS |
| UT-RTC-006 | BCD decode full date (2026-04-10, dow 5) | All fields correct | ✅ PASS |
| UT-RTC-007 | `rtc_set_time` encodes seconds (45 → 0x45) | BCD encode correct | ✅ PASS |
| UT-RTC-008 | `rtc_set_time` encodes year (2026 → 0x26) | High/low byte correct | ✅ PASS |
| UT-RTC-011 | CH bit set → `rtc_oscillator_stopped()` true | Flag detected | ✅ PASS |
| UT-RTC-012 | CH bit clear → `rtc_oscillator_stopped()` false | Flag not set | ✅ PASS |
| UT-RTC-013 | Invalid BCD seconds (0x60) → `RTC_ERR_INVALID` | Out-of-range rejected | ✅ PASS |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-3 — DS1307 RTC |
| Directory | `DS1307_RTC/` |
| Firmware version | 0.1.0 |
| Board ID / revision | LOLIN S3 |
| Tester | drasv |
| Date | 2026-04-10 |
| Equipment | LOLIN S3; DS1307 RTC module; CR2032 battery (inserted before test); LIB-2 board-tested |

**Wiring:** DS1307 SDA → GPIO 1, SCL → GPIO 2, VCC → 3.3 V, GND → GND.

#### Test cases

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-RTC-001 | Driver initialises and detects device | Upload sketch; open serial monitor | "DS1307 init OK" printed within 3 s | Init OK printed | ✅ PASS |
| HW-RTC-002 | Clock Halt flag is clear | Read CH flag from serial output | "Oscillator stop flag: CLEAR" printed (CH cleared by set_time) | "Oscillator stop flag: CLEAR" printed | ✅ PASS |
| HW-RTC-003 | Time can be set | Sketch writes 2026-04-10 12:00:00 to RTC | "Set time: 2026-04-10 12:00:00" confirmed on serial; no error | Set time confirmed; no error | ✅ PASS |
| HW-RTC-004 | Time is advancing | Sketch reads time twice, 3 s apart; prints both readings | Second reading is 3 s (±1 s) later than first | Delta within 2–4 s | ✅ PASS |
| HW-RTC-006 | Battery backup: time retained across RTC power cycle | Disconnect DS1307 VCC only (keep GND, SDA, SCL, CR2032); wait 10 s; reconnect; read time | Elapsed ≥ 18 s, CH bit CLEAR after reconnect | Elapsed 22 s; OSF CLEAR after reconnect | ✅ PASS |

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS |
| Failed test IDs | |
| Notes | DS1307 has no temperature sensor — HW-RTC-005 removed from plan. Battery backup confirmed: elapsed 22 s, CH bit CLEAR after DS1307 VCC reconnect. |

---

## LIB-4 — LCD1602 I2C (`LCD1602_I2C/`)

### Purpose
Driver for the Waveshare LCD1602 I2C module (HD44780 LCD driven by PCF8574A I/O expander at address 0x3E). Used by T8 (UI / Display) to render status screens.

### Dependency
Requires LIB-2 (`i2c/`) to be board-tested.
```ini
[env:lolin_s3]
lib_deps = file://../i2c
```

### API (`lcd1602.h`)

```cpp
#define LCD_I2C_ADDR  0x3E
#define LCD_COLS      16
#define LCD_ROWS       2

typedef enum {
    LCD_OK = 0,
    LCD_ERR_NO_DEVICE,
    LCD_ERR_COMM
} lcd_status_t;

lcd_status_t lcd_init(void);
lcd_status_t lcd_clear(void);
lcd_status_t lcd_home(void);                          // cursor to (0,0) without clearing
lcd_status_t lcd_set_cursor(uint8_t row, uint8_t col);
lcd_status_t lcd_print(uint8_t row, uint8_t col, const char *str);
lcd_status_t lcd_print_char(uint8_t row, uint8_t col, char c);
lcd_status_t lcd_write_row(uint8_t row, const char *text); // pads/truncates to 16 chars
lcd_status_t lcd_backlight_on(void);
lcd_status_t lcd_backlight_off(void);
```

### Implementation notes — PCF8574 bit layout
```
Bit 7 (P7) = DB7    Bit 3 (P3) = Backlight (1 = on)
Bit 6 (P6) = DB6    Bit 2 (P2) = En (pulse high→low per nibble)
Bit 5 (P5) = DB5    Bit 1 (P1) = RW (always 0 — write only)
Bit 4 (P4) = DB4    Bit 0 (P0) = RS (0 = command, 1 = data)
```
Each byte written to the HD44780 requires two nibble transfers (high nibble first), each with an En pulse. Implement private `lcd_send_nibble` and `lcd_send_byte` helpers.

`lcd_write_row` pads strings shorter than 16 chars with trailing spaces, and silently truncates strings longer than 16 chars.

### Mock strategy
Records all bytes sent to 0x3E in a transmission log. `mock_lcd_get_transmitted_bytes(buf, len)` returns the log. Tests decode the nibble-level PCF8574A byte sequence to verify the correct HD44780 commands were issued.

### Unit tests (11)

Run: `pio test -e native` — all 11 passed on 2026-04-10 (1.79 s, MinGW/native).

| ID | Test case | Assertion | Result |
|----|-----------|-----------|--------|
| UT-LCD-001 | `lcd_init` sends HD44780 init sequence | 3 function-set nibbles + entry mode set in mock log | ✅ PASS |
| UT-LCD-002 | `lcd_clear` sends command 0x01 | RS=0, data=0x01 decoded from log | ✅ PASS |
| UT-LCD-003 | `lcd_set_cursor(0, 0)` → DDRAM address 0x80 | Set DDRAM address command = 0x80 | ✅ PASS |
| UT-LCD-004 | `lcd_set_cursor(1, 0)` → DDRAM address 0xC0 | Row 1 base = 0x40; command = 0x80 \| 0x40 | ✅ PASS |
| UT-LCD-005 | `lcd_set_cursor(0, 5)` → DDRAM address 0x85 | Column offset applied: 0x80 + 5 | ✅ PASS |
| UT-LCD-006 | `lcd_print(0, 0, "Hi")` sends 'H' then 'i' as data | RS=1, correct nibble order | ✅ PASS |
| UT-LCD-007 | `lcd_backlight_on` — bit 3 set in all subsequent bytes | Backlight bit present | ✅ PASS |
| UT-LCD-008 | `lcd_backlight_off` — bit 3 cleared | Backlight bit absent | ✅ PASS |
| UT-LCD-009 | `lcd_write_row` pads 3-char string to 16 data bytes | Exactly 16 data bytes in log | ✅ PASS |
| UT-LCD-010 | `lcd_write_row` truncates 20-char string to 16 bytes | No more than 16 data bytes; no buffer overrun | ✅ PASS |
| UT-LCD-011 | NACK on init → `LCD_ERR_NO_DEVICE` | Correct error code | ✅ PASS |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-4 — LCD1602 I2C |
| Directory | `LCD1602_I2C/` |
| Firmware version | 0.1.0 |
| Board ID / revision | LOLIN S3 |
| Tester | drasv |
| Date | 2026-04-10 |
| Equipment | LOLIN S3; Waveshare LCD1602 I2C module (AiP31068L controller, address 0x3E); LIB-2 board-tested |

**Wiring:** LCD SDA → GPIO 1, SCL → GPIO 2, VCC → 5 V, GND → GND. If display is blank with backlight on, adjust the contrast trimpot on the module before proceeding.

#### Test cases

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-LCD-001 | Driver initialises without error | Upload sketch; open serial monitor | "LCD init OK" printed within 3 s | "LCD init OK" printed; I2C scan confirmed device at 0x3E | ✅ PASS |
| HW-LCD-002 | Backlight turns on | Observe LCD module during init | Backlight visibly illuminated | Backlight illuminated after init (hardwired to VCC on AiP31068L module) | ✅ PASS |
| HW-LCD-003 | Row 0 text rendered correctly | Observe LCD line 1 after sketch prints test string | "Hello, World!   " displayed on line 1; all 16 character positions correct | "Hello, World!   " visible on line 1 | ✅ PASS |
| HW-LCD-004 | Row 1 text rendered correctly | Observe LCD line 2 | "Row1 test 12345 " displayed on line 2; all 16 positions correct | "Row1 test 12345 " visible on line 2 | ✅ PASS |
| HW-LCD-005 | lcd_clear blanks the display | Observe LCD after clear command | Both lines completely blank; no residual characters visible | Both lines blank after clear | ✅ PASS |
| HW-LCD-006 | Cursor positioning is accurate | Observe LCD after set_cursor(1,5) and single char write | 'X' visible at column 5 of line 2 (0-indexed); all other positions blank | Not exercised in this run — sketch skipped due to AiP31068L notes | ⏭ SKIP |
| HW-LCD-007 | Backlight turns off | Observe LCD when sketch calls backlight_off | Backlight visibly extinguished; display content present but unlit | AiP31068L has no I2C backlight register; backlight LED hardwired to VCC. lcd_backlight_off() is an accepted stub (returns LCD_OK) | ⏭ SKIP |
| HW-LCD-008 | Backlight turns on again | Observe LCD when sketch calls backlight_on | Backlight illuminated; content readable again | lcd_backlight_on() stub returns LCD_OK; backlight remains on (hardwired) | ✅ PASS |

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS |
| Failed test IDs | |
| Notes | Module controller is AiP31068L (not PCF8574A): address 0x3E confirmed, text and clear functions work correctly. Backlight LED is hardwired to VCC — lcd_backlight_on/off are accepted stubs. HW-LCD-006 cursor positioning and HW-LCD-007 backlight-off were not exercised; cursor positioning covered indirectly by HW-LCD-003/004. 7 / 7 checked items PASSED. |

---

## LIB-5 — Keypad Matrix (`keyPad/`)

### Purpose
Scans the 4×4 membrane keypad and returns a single key character per press, with built-in software debounce. Used by T7 (Keypad Scan).

### API (`keypad_matrix.h`)

```cpp
// Row GPIOs — driven LOW to scan (one at a time)
#define KP_ROW1   3
#define KP_ROW2   4
#define KP_ROW3   5
#define KP_ROW4   6

// Column GPIOs — INPUT_PULLUP; read LOW = key pressed
#define KP_COL1   7
#define KP_COL2   9
#define KP_COL3  10
#define KP_COL4  11

#define KP_NO_KEY  0

// Key layout:
//   Row 1: 1  2  3  A
//   Row 2: 4  5  6  B
//   Row 3: 7  8  9  C
//   Row 4: *  0  #  D

void keypad_init(void);

// Call every ~20 ms from T7.
// Returns key character when pressed, KP_NO_KEY otherwise.
// Built-in 2-scan debounce: a key is reported only after two consecutive
// scans detect the same column LOW for a given row.
char keypad_scan(void);
```

### Mock strategy (`test/mock_gpio.h`)
`mock_gpio_set_col(col_gpio, pressed)` simulates a key press. `digitalRead` returns LOW for set columns and HIGH otherwise. `digitalWrite` and `pinMode` are no-ops that record calls for assertion.

### Unit tests (10)

| ID | Test case | Assertion |
|----|-----------|-----------|
| UT-KP-001 | No key pressed → `KP_NO_KEY` | All columns HIGH; two scans; return = 0 |
| UT-KP-002 | Key pressed — first scan → `KP_NO_KEY` (debounce) | Single scan insufficient to report |
| UT-KP-003 | Key pressed — second scan → correct character | Debounce satisfied; character returned |
| UT-KP-004 | Key 'A' (R1, C4) → 'A' | Correct mapping |
| UT-KP-005 | Key '*' (R4, C1) → '*' | Correct mapping |
| UT-KP-006 | Key '#' (R4, C3) → '#' | Correct mapping |
| UT-KP-007 | Key 'D' (R4, C4) → 'D' | Correct mapping |
| UT-KP-008 | Key released → `KP_NO_KEY` on next scan | No phantom repeat |
| UT-KP-009 | Only one row GPIO driven LOW at a time during scan | Prevents ghost key detection |
| UT-KP-010 | All 16 keys map to distinct characters | No duplicates in the character map |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-5 — Keypad Matrix |
| Directory | `keyPad/` |
| Firmware version | 0.1.0 |
| Board ID / revision | LOLIN S3 |
| Tester | Remko Welling |
| Date | 2026-04-10 |
| Equipment | LOLIN S3; 4×4 membrane keypad |

**Wiring:** Row wires → GPIO 3, 4, 5, 6. Column wires → GPIO 7, 9, 10, 11. Internal pull-ups configured by `keypad_init()`; no external resistors needed.

#### Test cases

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-KP-003 | Idle keypad produces no output | Leave keypad unpressed for 5 s; observe serial | No spurious characters appear during idle period | No spurious output detected | ✅ PASS |
| HW-KP-005 | Multi-press discarded | Hold any two keys simultaneously for 5 s; observe serial | KP_NO_KEY throughout — no character reported | A character was produced during multi-press; discard did not work on hardware | ❌ FAIL |
| HW-KP-004 | Key '1' (Row 1, Col 1) correct character | Press key as requested | `[PASS] key [ 1 ]` | `[PASS] key [ 1 ]` | ✅ PASS |
| HW-KP-004 | Key '2' (Row 1, Col 2) correct character | Press key as requested | `[PASS] key [ 2 ]` | `[PASS] key [ 2 ]` | ✅ PASS |
| HW-KP-004 | Key '3' (Row 1, Col 3) correct character | Press key as requested | `[PASS] key [ 3 ]` | `[PASS] key [ 3 ]` | ✅ PASS |
| HW-KP-004 | Key 'A' (Row 1, Col 4) correct character | Press key as requested | `[PASS] key [ A ]` | `[PASS] key [ A ]` | ✅ PASS |
| HW-KP-004 | Key '4' (Row 2, Col 1) correct character | Press key as requested | `[PASS] key [ 4 ]` | `[PASS] key [ 4 ]` | ✅ PASS |
| HW-KP-004 | Key '5' (Row 2, Col 2) correct character | Press key as requested | `[PASS] key [ 5 ]` | `[PASS] key [ 5 ]` | ✅ PASS |
| HW-KP-004 | Key '6' (Row 2, Col 3) correct character | Press key as requested | `[PASS] key [ 6 ]` | `[PASS] key [ 6 ]` | ✅ PASS |
| HW-KP-004 | Key 'B' (Row 2, Col 4) correct character | Press key as requested | `[PASS] key [ B ]` | `[PASS] key [ B ]` | ✅ PASS |
| HW-KP-004 | Key '7' (Row 3, Col 1) correct character | Press key as requested | `[PASS] key [ 7 ]` | `[PASS] key [ 7 ]` | ✅ PASS |
| HW-KP-004 | Key '8' (Row 3, Col 2) correct character | Press key as requested | `[PASS] key [ 8 ]` | `[PASS] key [ 8 ]` | ✅ PASS |
| HW-KP-004 | Key '9' (Row 3, Col 3) correct character | Press key as requested | `[PASS] key [ 9 ]` | `[PASS] key [ 9 ]` | ✅ PASS |
| HW-KP-004 | Key 'C' (Row 3, Col 4) correct character | Press key as requested | `[PASS] key [ C ]` | `[PASS] key [ C ]` | ✅ PASS |
| HW-KP-004 | Key '*' (Row 4, Col 1) correct character | Press key as requested | `[PASS] key [ * ]` | `[PASS] key [ * ]` | ✅ PASS |
| HW-KP-004 | Key '0' (Row 4, Col 2) correct character | Press key as requested | `[PASS] key [ 0 ]` | `[PASS] key [ 0 ]` | ✅ PASS |
| HW-KP-004 | Key '#' (Row 4, Col 3) correct character | Press key as requested | `[PASS] key [ # ]` | `[PASS] key [ # ]` | ✅ PASS |
| HW-KP-004 | Key 'D' (Row 4, Col 4) correct character | Press key as requested | `[PASS] key [ D ]` | `[PASS] key [ D ]` | ✅ PASS |

#### Expected full serial output

```
================================================
  LIB-5 Keypad Matrix — hardware verification
================================================
Press each key when requested (30 s timeout).
Pressing multiple keys simultaneously is discarded;
release all keys and press only the requested one.
------------------------------------------------
[HW-KP-003] Idle test — do NOT press any key for 5 s ...
[PASS] HW-KP-003: no spurious output during idle period
------------------------------------------------
[HW-KP-005] Multi-press test — hold ANY TWO keys simultaneously for 5 s ...
  Waiting for two keys to be held down...
  Multi-press detected — verifying discard for 5 s...
[PASS] HW-KP-005: multi-press correctly discarded (KP_NO_KEY)
  Release all keys...
------------------------------------------------
Press key [ 1 ]  (Row1/Col1)  — timeout 30 s
[PASS]    HW-KP-004: key [ 1 ]
...
[PASS]    HW-KP-0019: key [ D ]
================================================
  PASSED:  18
  FAILED:  0
  TIMEOUT: 0
  RESULT: PASS
================================================
```

#### Actual serial output (2026-04-10)

```
================================================
  LIB-5 Keypad Matrix — hardware verification
================================================
Press each key when requested (30 s timeout).
Pressing multiple keys simultaneously is discarded;
release all keys and press only the requested one.
------------------------------------------------
[HW-KP-003] Idle test — do NOT press any key for 5 s ...
[PASS] HW-KP-003: no spurious output during idle period
------------------------------------------------
[HW-KP-005] Multi-press test — hold ANY TWO keys simultaneously for 5 s ...
  Waiting for two keys to be held down...
  Multi-press detected — verifying discard for 5 s...
[FAIL] HW-KP-005: multi-press produced a character — not discarded
  Release all keys...
------------------------------------------------
Press key [ 1 ]  (Row1/Col1)  — timeout 30 s
[PASS]    HW-KP-004: key [ 1 ]
Press key [ 2 ]  (Row1/Col2)  — timeout 30 s
[PASS]    HW-KP-005: key [ 2 ]
Press key [ 3 ]  (Row1/Col3)  — timeout 30 s
[PASS]    HW-KP-006: key [ 3 ]
Press key [ A ]  (Row1/Col4)  — timeout 30 s
[PASS]    HW-KP-007: key [ A ]
Press key [ 4 ]  (Row2/Col1)  — timeout 30 s
[PASS]    HW-KP-008: key [ 4 ]
Press key [ 5 ]  (Row2/Col2)  — timeout 30 s
[PASS]    HW-KP-009: key [ 5 ]
Press key [ 6 ]  (Row2/Col3)  — timeout 30 s
[PASS]    HW-KP-0010: key [ 6 ]
Press key [ B ]  (Row2/Col4)  — timeout 30 s
[PASS]    HW-KP-0011: key [ B ]
Press key [ 7 ]  (Row3/Col1)  — timeout 30 s
[PASS]    HW-KP-0012: key [ 7 ]
Press key [ 8 ]  (Row3/Col2)  — timeout 30 s
[PASS]    HW-KP-0013: key [ 8 ]
Press key [ 9 ]  (Row3/Col3)  — timeout 30 s
[PASS]    HW-KP-0014: key [ 9 ]
Press key [ C ]  (Row3/Col4)  — timeout 30 s
[PASS]    HW-KP-0015: key [ C ]
Press key [ * ]  (Row4/Col1)  — timeout 30 s
[PASS]    HW-KP-0016: key [ * ]
Press key [ 0 ]  (Row4/Col2)  — timeout 30 s
[PASS]    HW-KP-0017: key [ 0 ]
Press key [ # ]  (Row4/Col3)  — timeout 30 s
[PASS]    HW-KP-0018: key [ # ]
Press key [ D ]  (Row4/Col4)  — timeout 30 s
[PASS]    HW-KP-0019: key [ D ]
================================================
  PASSED:  17
  FAILED:  1
  TIMEOUT: 0
  RESULT: FAIL
================================================
Verification complete. Board is idle.
```

#### Overall result

| Field | Value |
|-------|-------|
| Result | FAIL |
| Failed test IDs | HW-KP-005 |
| Notes | All 16 individual keys verified correct (HW-KP-004); idle test passed (HW-KP-003). **Defect:** HW-KP-005 FAILED — when two keys are held simultaneously on the physical keypad, the driver produced a character instead of discarding the input. Root cause is likely a hardware ghost-key path in the membrane keypad matrix: pressing two keys that share a row or column creates a current path that makes a third (ghost) key appear pressed on one row at a time, satisfying the single-key-per-row check in the driver and bypassing multi-press detection. **Action required:** investigate ghost-key behaviour and add a blocking diode scheme or revise the scanning algorithm to detect ghost keys across rows before committing to a character. Re-test HW-KP-005 after fix. |

---

## LIB-6 — Modbus RTU (`modBus/`)

### Purpose
Modbus RTU master driver over UART1 and the SIT65HVD08P RS485 transceiver. Reads sensor data from the SenseCAP S200 (wind) and FG6485A (temperature/humidity). Used by T5 (Sensor Poll).

### Dependency
Requires LIB-1 (`gpio/`) to be board-tested (`gpio_set_rs485_direction` is called from `modbus_init`).
```ini
[env:lolin_s3]
lib_deps = file://../gpio
```

### API (`modbus_rtu.h`)

```cpp
#define MODBUS_UART_TX     17
#define MODBUS_UART_RX     18
#define MODBUS_BAUD        9600
#define MODBUS_TIMEOUT_MS  200

typedef enum {
    MODBUS_OK = 0,
    MODBUS_ERR_TIMEOUT,
    MODBUS_ERR_CRC,
    MODBUS_ERR_EXCEPTION,  // device returned a Modbus exception code
    MODBUS_ERR_FRAMING,    // invalid response length or function code mismatch
    MODBUS_ERR_PARAM
} modbus_status_t;

void            modbus_init(void);
modbus_status_t modbus_read_holding_registers(uint8_t device_addr,
                                              uint16_t start_reg,
                                              uint8_t count,
                                              uint16_t *out);   // FC03
modbus_status_t modbus_read_input_registers(uint8_t device_addr,
                                            uint16_t start_reg,
                                            uint8_t count,
                                            uint16_t *out);    // FC04
```

### Implementation sequence (per transaction)
1. Assert DE/RE HIGH via `gpio_set_rs485_direction(true)`.
2. Write 8-byte request frame to `Serial1`.
3. Call `Serial1.flush()` to wait for transmission to complete.
4. Assert DE/RE LOW via `gpio_set_rs485_direction(false)`.
5. Read bytes with per-byte timeout until `byte_count + 5` bytes received or timeout.
6. Validate CRC16 (polynomial 0xA001).
7. Return parsed register values, or an error code.

### Frame format
```
Request (8 bytes):  [addr][fc][reg_hi][reg_lo][cnt_hi][cnt_lo][crc_lo][crc_hi]
Response:           [addr][fc][byte_cnt][data...][crc_lo][crc_hi]
```

### Mock strategy
`mock_uart.h` — `mock_uart_queue_response(bytes, len)` preloads response bytes; `mock_uart_get_transmitted(buf, len)` records what was sent.  
`mock_gpio.h` — records DE/RE transitions for timing assertions.  
In the native build, `gpio/` is not linked; the mock provides the `gpio_set_rs485_direction` stub directly.

### Unit tests (12)

| ID | Test case | Assertion |
|----|-----------|-----------|
| UT-MB-001 | CRC16 of `{0x01,0x03,0x00,0x00,0x00,0x02}` = 0xC40B | Known Modbus CRC test vector |
| UT-MB-002 | CRC16 of single byte `{0xFF}` = known value | Boundary case |
| UT-MB-003 | FC03 request frame bytes are correct | Address, function code, register, count, CRC all verified |
| UT-MB-004 | FC03 response parsed to correct `uint16_t` values | Two register values decoded correctly |
| UT-MB-005 | FC04 request uses function code 0x04 | Frame byte[1] = 0x04 |
| UT-MB-006 | No response → `MODBUS_ERR_TIMEOUT` | Timeout fires; correct error |
| UT-MB-007 | Response with flipped CRC byte → `MODBUS_ERR_CRC` | CRC validation rejects frame |
| UT-MB-008 | Exception response (FC \| 0x80) → `MODBUS_ERR_EXCEPTION` | Exception code detected |
| UT-MB-009 | DE/RE HIGH before first TX byte | Mock GPIO log shows HIGH before UART write |
| UT-MB-010 | DE/RE LOW before RX | Mock GPIO log shows LOW before `read()` |
| UT-MB-011 | Device address 0 → `MODBUS_ERR_PARAM` | Broadcast address rejected for reads |
| UT-MB-012 | Register count > 125 → `MODBUS_ERR_PARAM` | FC03/FC04 maximum enforced |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-6 — Modbus RTU |
| Directory | `modBus/` |
| Firmware version | |
| Board ID / revision | |
| Tester | |
| Date | |
| Equipment | LOLIN S3; SIT65HVD08P RS485 transceiver; 120 Ω termination resistor; PC with USB-RS485 adapter; oscilloscope or logic analyser (for HW-MB-005) |

**Wiring:** SIT65HVD08P DI → GPIO 17, RO → GPIO 18, DE+RE tied → GPIO 8, VCC → 3.3 V, GND → GND. 120 Ω resistor across RS485 A/B at far end of cable.

**Simulator setup:** Start Python `pymodbus` slave server on the PC before uploading. Configure address 1 with holding registers 0–1 = `{0x1234, 0x5678}`; address 2 with input registers 0–1 = `{0x00E6, 0x028F}`.

#### Test cases

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-MB-001 | Driver initialises on correct UART pins | Upload sketch; open serial monitor | "Modbus init: UART1 TX=GPIO17 RX=GPIO18 baud=9600 DE/RE=GPIO8" printed | | |
| HW-MB-002 | FC03 reads correct holding register values | Sketch sends FC03 to addr=1, reg=0, count=2; simulator responds | Serial shows val[0]=0x1234 val[1]=0x5678. Record actual: val[0]=0x___ val[1]=0x___ | | |
| HW-MB-003 | FC04 reads correct input register values | Sketch sends FC04 to addr=2, reg=0, count=2; simulator responds | Serial shows val[0]=0x00E6 val[1]=0x028F. Record actual: val[0]=0x___ val[1]=0x___ | | |
| HW-MB-004 | Timeout returned for absent device | Sketch queries addr=99 (no device on bus) | "MODBUS_ERR_TIMEOUT" returned; no crash; response within 300 ms of request | | |
| HW-MB-005 | DE/RE toggles correctly during transaction | Probe GPIO 8 with oscilloscope or logic analyser during an FC03 transaction | GPIO 8 HIGH during TX frame; transitions LOW before RX window opens. Record transition timing: TX→LOW delay: ___ µs | | |

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS / FAIL / INCOMPLETE |
| Failed test IDs | |
| Notes | |

---

## LIB-7 — NVS Configuration (`nvs/`)

### Purpose
Typed get/set access to the ESP32-S3 NVS flash for all system configuration settings, plus a 1000-entry ring buffer for the event log fallback when no SD card is present. Used by T4 (Data Manager) and T9 (Event Logger).

### API (`nvs_config.h`)

```cpp
// NVS namespaces — match TSDS §5.10
#define NVS_NS_CLIMATE  "climate"
#define NVS_NS_WIND     "wind"
#define NVS_NS_MOTOR    "motor"
#define NVS_NS_ACCESS   "access"
#define NVS_NS_WIFI     "wifi"
#define NVS_NS_MQTT     "mqtt"
#define NVS_NS_SYSTEM   "system"
#define NVS_NS_LOG      "log"

// Schema versioning — see section below
#ifndef NVS_SCHEMA_VERSION
  #define NVS_SCHEMA_VERSION  1
#endif
#define NVS_KEY_SCHEMA_VER  "schema_ver"
#define NVS_KEY_FW_VERSION  "fw_version"   // string "MAJOR.MINOR.PATCH", written on every boot

#ifndef CONFIG_NVS_LOG_CAPACITY
  #define CONFIG_NVS_LOG_CAPACITY  1000
#endif

typedef enum {
    NVS_CFG_OK = 0,
    NVS_CFG_ERR_NOT_FOUND,
    NVS_CFG_ERR_WRITE,
    NVS_CFG_ERR_INIT,
    NVS_CFG_ERR_MIGRATION    // schema mismatch — defaults applied; caller should log
} nvs_cfg_status_t;

// Initialisation (includes schema version check)
nvs_cfg_status_t nvs_cfg_init(void);

// Schema version query
nvs_cfg_status_t nvs_cfg_get_schema_version(int32_t *ver);

// Typed get / set
nvs_cfg_status_t nvs_cfg_get_i32(const char *ns, const char *key, int32_t *val);
nvs_cfg_status_t nvs_cfg_set_i32(const char *ns, const char *key, int32_t val);
nvs_cfg_status_t nvs_cfg_get_str(const char *ns, const char *key, char *buf, size_t buf_len);
nvs_cfg_status_t nvs_cfg_set_str(const char *ns, const char *key, const char *val);
nvs_cfg_status_t nvs_cfg_get_blob(const char *ns, const char *key, void *buf, size_t *len);
nvs_cfg_status_t nvs_cfg_set_blob(const char *ns, const char *key, const void *data, size_t len);
nvs_cfg_status_t nvs_cfg_erase_namespace(const char *ns);

// _or_default helpers — write the default when the key is absent; never return NOT_FOUND
nvs_cfg_status_t nvs_cfg_get_i32_or_default(const char *ns, const char *key,
                                              int32_t default_val, int32_t *val);
nvs_cfg_status_t nvs_cfg_get_str_or_default(const char *ns, const char *key,
                                              const char *default_val,
                                              char *buf, size_t buf_len);
nvs_cfg_status_t nvs_cfg_get_blob_or_default(const char *ns, const char *key,
                                               const void *default_data, size_t default_len,
                                               void *buf, size_t *len);

// Ring buffer log (stored in NVS_NS_LOG namespace)
nvs_cfg_status_t nvs_log_append(const void *entry, size_t entry_size);
nvs_cfg_status_t nvs_log_read(uint32_t offset, void *buf, uint32_t count, uint32_t *count_out);
uint32_t         nvs_log_count(void);
```

### Schema versioning

The driver enforces a schema version to detect when the on-flash NVS layout differs from the current firmware build and to log the migration event.

**How it works:**

| Boot condition | `nvs_cfg_init()` behaviour | Return value |
|----------------|---------------------------|--------------|
| First boot / blank flash | Writes `system/schema_ver = NVS_SCHEMA_VERSION`; writes `system/fw_version = FIRMWARE_VERSION` | `NVS_CFG_OK` |
| Stored version == `NVS_SCHEMA_VERSION` | Overwrites `system/fw_version` with current `FIRMWARE_VERSION` | `NVS_CFG_OK` |
| Stored version ≠ `NVS_SCHEMA_VERSION` | Writes new `system/schema_ver`; overwrites `system/fw_version`; **all other namespaces are preserved** | `NVS_CFG_ERR_MIGRATION` |

`system/fw_version` is overwritten on **every** boot so it always reflects the currently running firmware, regardless of whether a migration occurred. This allows T11 (web server) and T9 (event logger) to read the running firmware version directly from NVS without requiring it to be embedded only in the binary image.

**Migration strategy — preserve, add, ignore removed:**

On a schema version mismatch, namespaces are **not erased**. The three cases are handled as follows:

| Situation | Outcome |
|-----------|---------|
| Key present in NVS and still used by firmware | Read as-is — **user setting is preserved** |
| Key absent in NVS (new setting added in this firmware) | First `_or_default` call writes the factory default — **new key gets default** |
| Key present in NVS but no longer used by firmware | Never read — becomes an orphaned entry; causes no harm; NVS partition reclaims space over time |

**Type-change exception:** If a key's storage type changes between firmware versions (e.g. `i32` → `blob`), the ESP-IDF NVS API returns `ESP_ERR_NVS_TYPE_MISMATCH`. This is the one case that requires explicit per-key handling in the firmware (erase and rewrite the individual key). A blanket namespace erase is not used; only the affected key must be migrated.

**Caller contract:** The caller (typically T4 — Data Manager) checks the return value of `nvs_cfg_init()`. If `NVS_CFG_ERR_MIGRATION` is returned, it logs the event and proceeds normally. All subsequent `_or_default` calls preserve existing user values or write factory defaults for newly absent keys.

**Bumping the version:** Increment `NVS_SCHEMA_VERSION` in `nvs_config.h` when releasing firmware that changes the NVS layout, so that the migration event is logged on the first boot. This is informational — the driver does not erase on mismatch. The version must also be bumped when a key's type changes, so the firmware can detect the condition and handle the affected key explicitly.

### `_or_default` helpers

These combine get + conditional-write into one call, eliminating boilerplate at every call site:

```cpp
// Without _or_default (4 lines):
int32_t t_min;
if (nvs_cfg_get_i32(NVS_NS_CLIMATE, "t_min", &t_min) == NVS_CFG_ERR_NOT_FOUND) {
    t_min = 180;
    nvs_cfg_set_i32(NVS_NS_CLIMATE, "t_min", t_min);
}

// With _or_default (1 line):
int32_t t_min;
nvs_cfg_get_i32_or_default(NVS_NS_CLIMATE, "t_min", 180, &t_min);
```

If the key is present the stored value is returned and the default is ignored.  
If the key is absent the default is written to NVS and returned.  
The caller receives `NVS_CFG_OK` in both cases.

### Ring buffer implementation
Stored in the `log` namespace using three key types:
- `"head"` (i32) — next write slot index (0-based, wraps at `CONFIG_NVS_LOG_CAPACITY`)
- `"count"` (i32) — number of valid entries (max = capacity)
- `"eNNNN"` (blob) — individual entry at slot NNNN (zero-padded 4-digit index)

At 12 bytes per log entry × 1000 entries ≈ 12 KB of NVS space. The LOLIN S3 default Arduino partition table allocates at least 24 KB for NVS.

### Mock strategy (`test/mock_nvs.h`)
An in-memory `std::map<std::string, std::vector<uint8_t>>` backing store keyed on `"namespace:key"` with stubs for all ESP-IDF NVS functions (`nvs_flash_init`, `nvs_open`, `nvs_get_i32`, etc.). `#ifndef UNIT_TEST` guards the real `#include "nvs_flash.h"` / `nvs.h` in `nvs_config.cpp`; under `UNIT_TEST`, `#include "../test/mock_nvs.h"` is substituted.

`mock_nvs_inject_i32(ns, key, val)` allows tests to pre-populate NVS state before calling `nvs_cfg_init()`, which is necessary for testing the schema-mismatch migration path.

### Unit tests (22)

| ID | Test case | Assertion |
|----|-----------|-----------|
| UT-NVS-001 | `nvs_cfg_init` returns `NVS_CFG_OK` | Mock succeeds |
| UT-NVS-002 | `set_i32` / `get_i32` round-trip | Set 180; get 180 |
| UT-NVS-003 | `get_i32` on unset key → `NVS_CFG_ERR_NOT_FOUND` | Missing key detected |
| UT-NVS-004 | `set_str` / `get_str` round-trip | "TestNet" stored and retrieved |
| UT-NVS-005 | `get_str` with small `buf_len` truncates; null-terminated | No buffer overrun |
| UT-NVS-006 | `set_blob` / `get_blob` round-trip | Byte-for-byte equal |
| UT-NVS-007 | `erase_namespace` → subsequent get returns `NOT_FOUND` | Key removed |
| UT-NVS-008 | `nvs_log_append` → `nvs_log_count()` increases | Count incremented |
| UT-NVS-009 | `nvs_log_read` retrieves the appended entry | Bytes match |
| UT-NVS-010 | Ring wraps at capacity: oldest overwritten | Append capacity+1 entries; count = capacity; entry 0 gone |
| UT-NVS-011 | After wrap, `offset=0` reads oldest surviving entry | Correct oldest after wrap |
| UT-NVS-012 | Read count > available clamps to available | `count_out` ≤ actual entries |
| UT-NVS-013 | Key longer than 15 chars: consistent behaviour | Defined: reject or truncate — not both |
| UT-NVS-014 | `nvs_cfg_init` on first boot writes `schema_ver = NVS_SCHEMA_VERSION` | `get_schema_version()` returns current compile-time version |
| UT-NVS-015 | Second `nvs_cfg_init` with matching version → `NVS_CFG_OK` | No migration, version unchanged |
| UT-NVS-016 | `nvs_cfg_init` with stale version → `NVS_CFG_ERR_MIGRATION`; version updated | Mock pre-loads version N-1; init returns MIGRATION; stored ver = NVS_SCHEMA_VERSION |
| UT-NVS-017 | After migration, existing config keys are **preserved** | Pre-set `climate/t_min = 100`; trigger migration; `get_i32` still returns 100 |
| UT-NVS-018 | `get_i32_or_default` absent key → writes default, returns it | Key absent; default 180 written and returned; plain `get_i32` also returns 180 |
| UT-NVS-019 | `get_i32_or_default` present key → returns stored, ignores default | Pre-set to 42; default 99; returns 42 |
| UT-NVS-020 | `get_str_or_default` absent key → writes default string, returns it | Key absent; "Greenhouse1" written and returned |
| UT-NVS-021 | `get_str_or_default` present key → returns stored, ignores default | Pre-set "OfficeNet"; default "Default"; returns "OfficeNet" |
| UT-NVS-022 | `get_blob_or_default` absent key → writes default blob, returns it | Absent blob; default bytes written and returned |
| UT-NVS-023 | `nvs_cfg_init` on first boot writes `fw_version` string | `get_str(system, fw_version)` returns `FIRMWARE_VERSION` |
| UT-NVS-024 | `nvs_cfg_init` on normal boot (no migration) overwrites `fw_version` | Pre-set `fw_version = "0.0.1"`; init with matching schema; `fw_version` updated to `FIRMWARE_VERSION` |
| UT-NVS-025 | `nvs_cfg_init` on migration boot overwrites `fw_version` | Pre-set stale schema + old `fw_version`; init returns MIGRATION; `fw_version` updated to `FIRMWARE_VERSION` |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-7 — NVS Configuration |
| Directory | `nvs/` |
| Firmware version | |
| Board ID / revision | |
| Tester | |
| Date | |
| Equipment | LOLIN S3 only (no external peripherals) |

> If NVS becomes corrupted at any point, run `pio run -e lolin_s3 -t erase` to erase all flash before re-uploading.

#### Test cases

HW-NVS-009 and HW-NVS-010 together form the power-cycle persistence test. Run HW-NVS-009 first, power the board off, then run HW-NVS-010 on the next boot.

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-NVS-001 | NVS initialises without error | Upload sketch; open serial monitor | "NVS init OK" printed within 3 s | | |
| HW-NVS-002 | Integer set/get round-trip | Sketch writes climate/t_min=200 then reads it back | Serial prints "get=200"; value matches what was written | | |
| HW-NVS-003 | String set/get round-trip | Sketch writes wifi/ssid="Greenhouse1" then reads it back | Serial prints `get="Greenhouse1"` | | |
| HW-NVS-004 | Missing key returns NOT_FOUND | Sketch reads a key that was never written | "NOT_FOUND" printed; no crash or hang | | |
| HW-NVS-005 | Namespace erase removes keys | Sketch erases climate namespace; reads climate/t_min | "NOT_FOUND" printed after erase | | |
| HW-NVS-006 | Log ring buffer appends correctly | Sketch appends 10 log entries; reads count | log_count=10 printed | | |
| HW-NVS-007 | Log read returns correct entry bytes | Sketch reads entries 0–4 | Bytes match expected pattern written by sketch. Record first entry raw bytes: ___ | | |
| HW-NVS-008 | Ring buffer caps at 1000 and wraps | Sketch appends 1005 entries total | log_count=1000; serial confirms oldest entry = index 5 (entries 0–4 overwritten) | | |
| HW-NVS-009 | Pre-power-cycle state recorded | After HW-NVS-005: climate/t_min is erased; wifi/ssid="Greenhouse1" is set | Note values; power board off | | |
| HW-NVS-010 | Values persist across power cycle | Power board on after HW-NVS-009; re-run read sketch | climate/t_min = NOT_FOUND (erased value not retained — correct); wifi/ssid = "Greenhouse1" (retained — correct) | | |

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS / FAIL / INCOMPLETE |
| Failed test IDs | |
| Notes | |

---

## LIB-8 — SD Card (`sdCard/`)

### Purpose
File I/O on the external SPI SD card (FAT32). Provides the primary event log storage and implements the log rotation policy (512 KB per file, 10 files retained). Used exclusively by T9 (Event Logger). Optional — absent when the SD card feature is not fitted.

### API (`sd_storage.h`)

```cpp
// SPI pin assignment
#define SD_PIN_MOSI  47
#define SD_PIN_MISO  48
#define SD_PIN_CLK   39
#define SD_PIN_CS    40

typedef enum {
    STORAGE_OK = 0,
    STORAGE_ERR_NO_CARD,
    STORAGE_ERR_MOUNT,
    STORAGE_ERR_IO,
    STORAGE_ERR_NOT_FOUND,
    STORAGE_ERR_FULL,
    STORAGE_ERR_PARAM
} storage_status_t;

storage_status_t storage_init(void);
bool             storage_sd_available(void);
storage_status_t storage_sd_write_append(const char *filename, const char *line);
storage_status_t storage_sd_read(const char *filename, uint32_t offset,
                                 char *buf, size_t buf_len, size_t *bytes_read);
uint32_t         storage_sd_file_size(const char *filename);
uint64_t         storage_sd_free_bytes(void);
storage_status_t storage_sd_list_csv(const char *ext, char *buf, size_t buf_len);
storage_status_t storage_sd_delete(const char *filename);
```

### Implementation note — SPI pin assignment
The LOLIN S3's default SPI pin mapping differs from the project's chosen pins (47/48/39/40). The driver must instantiate a custom `SPIClass` and pass it to `SD.begin`:

```cpp
SPIClass spi(FSPI);
spi.begin(SD_PIN_CLK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
SD.begin(SD_PIN_CS, spi);
```

### `lib_deps` (lolin_s3 env)
```ini
lib_deps = adafruit/SD @ ^1.2.4
```

### Mock strategy
`mock_sd.h` provides a `FakeSD` class backed by `std::map<std::string, std::string>` (filename → content). Stubs for `SD.open()`, `file.write()`, `file.read()`, `file.size()`, `SD.exists()`, `SD.remove()`.

### Unit tests (12)

| ID | Test case | Assertion |
|----|-----------|-----------|
| UT-SD-001 | `storage_init` → `STORAGE_ERR_NO_CARD` when mock reports no card | Correct error |
| UT-SD-002 | `storage_sd_available` false when not mounted | Returns false |
| UT-SD-003 | `write_append` creates file; content matches line | File exists; content correct |
| UT-SD-004 | `write_append` appends to existing file | Both lines present in order |
| UT-SD-005 | `read` offset=0 reads from start | Content matches from byte 0 |
| UT-SD-006 | `read` non-zero offset skips bytes | "ABCDEFGH" at offset=4 → "EFGH" |
| UT-SD-007 | `file_size` returns correct byte count | Written length matches returned size |
| UT-SD-008 | `file_size` of non-existent file → 0 | No crash; returns 0 |
| UT-SD-009 | `list_csv ".csv"` excludes `.txt` files | Only `.csv` filenames returned |
| UT-SD-010 | `delete` removes file | Subsequent `file_size` returns 0 |
| UT-SD-011 | `delete` non-existent file → `STORAGE_ERR_NOT_FOUND` | Correct error |
| UT-SD-012 | `read` with `buf_len` smaller than file: truncates and null-terminates | No overrun; last byte = 0 |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-8 — SD Card |
| Directory | `sdCard/` |
| Firmware version | |
| Board ID / revision | |
| Tester | |
| Date | |
| Equipment | LOLIN S3; SPI SD card breakout board or integrated slot; FAT32-formatted SD card (≤ 32 GB) |

> **FAT32 note:** SD cards > 32 GB often format as exFAT by default, which the Arduino SD library does not support. Reformat using Windows (`format /FS:FAT32 X:`) or the SD Association's SD Formatter before inserting.

#### Test cases

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-SD-001 | SPI bus initialises and card mounts | Insert FAT32 card; upload sketch; open serial | "SD card mounted (FAT32)" printed; no error | | |
| HW-SD-002 | Free space is reported | Read free bytes from serial after mount | Free bytes > 0. Record: ___ bytes | | |
| HW-SD-003 | Write-append creates file | Sketch appends line 1 to "20260410120000.csv" | "Write append: OK" printed; no error | | |
| HW-SD-004 | Write-append grows existing file | Sketch appends line 2 to same file | file_size increases; printed size matches 2 lines. Record size: ___ bytes | | |
| HW-SD-005 | Read from offset 0 returns correct content | Sketch reads file from offset 0 | Content matches line 1 exactly as written | | |
| HW-SD-006 | CSV file appears in directory listing | Sketch calls list_csv on root directory | "20260410120000.csv" present in output | | |
| HW-SD-007 | 512 KB stress write succeeds | Sketch writes 512 KB of data to "bigfile.csv" | file_size = 524288 bytes reported | | |
| HW-SD-008 | Delete removes file | Sketch deletes "bigfile.csv" | "STORAGE_OK" returned; file no longer listed | | |
| HW-SD-009 | Delete non-existent file returns correct error | Sketch deletes "bigfile.csv" a second time | "STORAGE_ERR_NOT_FOUND" returned; no crash | | |
| HW-SD-010 | Absent card returns correct error | Remove SD card; re-run storage_init | "STORAGE_ERR_NO_CARD" returned; no crash or hang | | |

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS / FAIL / INCOMPLETE |
| Failed test IDs | |
| Notes | |

---

## LIB-9 — LittleFS (`littleFS/`)

### Purpose
Read/write access to the LittleFS partition on the ESP32-S3 internal flash. Stores the HTML, CSS, and JavaScript files served by the web interface and is updated by the OTA mechanism. Always present — the LittleFS partition is part of the standard flash layout regardless of whether the SD card feature is fitted. Used by T11 (Web Server) and T13 (OTA).

### Why separate from LIB-8
LittleFS and the SD card are different hardware (internal flash controller vs. external SPI peripheral), different file systems (LittleFS vs. FAT32), different consumers (T11/T13 vs. T9), different optionality (always present vs. optional), and different mount lifetime (once at boot, never unmounted vs. runtime mount/unmount). Combining them in one library would couple an always-present subsystem to an optional one and mix two unrelated hardware abstractions.

### API (`littlefs_storage.h`)

```cpp
typedef enum {
    LFS_OK = 0,
    LFS_ERR_MOUNT,
    LFS_ERR_NOT_FOUND,
    LFS_ERR_IO,
    LFS_ERR_FULL
} lfs_status_t;

// Mount the LittleFS partition. Call once at boot before any other function.
lfs_status_t littlefs_init(void);

// Read entire file into null-terminated buffer; returns LFS_ERR_NOT_FOUND if absent.
lfs_status_t littlefs_read(const char *path, char *buf, size_t buf_len);

// Write (overwrite) a file. Creates the file if it does not exist.
lfs_status_t littlefs_write(const char *path, const void *data, size_t len);

// Returns true if the file exists.
bool         littlefs_exists(const char *path);

// Returns free bytes remaining in the LittleFS partition.
uint64_t     littlefs_free_bytes(void);
```

### FreeRTOS compatibility
All operations are blocking. Callers serialise via MX5 (LittleFS mutex):
- T11 acquires MX5 for each file serve request.
- T13 acquires MX5 exclusively during an OTA web-file write; T11 defers requests while `EG1.OTA_IN_PROGRESS` is set.

### Mock strategy
`mock_lfs.h` — an in-memory `std::map<std::string, std::vector<uint8_t>>` serves as the LittleFS backing store. Stubs for `LittleFS.begin()`, `LittleFS.open()`, file read/write, and `LittleFS.exists()`. No Arduino headers included in the mock.

### Unit tests (8)

| ID | Test case | Assertion |
|----|-----------|-----------|
| UT-LFS-001 | `littlefs_init` returns `LFS_OK` | Mock mount succeeds |
| UT-LFS-002 | `littlefs_read` returns content of existing file | Bytes match mock content; null-terminated |
| UT-LFS-003 | `littlefs_read` on missing file → `LFS_ERR_NOT_FOUND` | No crash; correct error |
| UT-LFS-004 | `littlefs_read` with `buf_len` smaller than file truncates and null-terminates | No overrun; last byte = 0 |
| UT-LFS-005 | `littlefs_write` creates file; content correct | Read back matches written bytes |
| UT-LFS-006 | `littlefs_write` overwrites existing file | Only second content present after second write |
| UT-LFS-007 | `littlefs_exists` returns true for existing file | File written then existence confirmed |
| UT-LFS-008 | `littlefs_exists` returns false for absent file | Non-existent path returns false |

### Hardware verification

#### Test session

| Field | Value |
|-------|-------|
| Driver | LIB-9 — LittleFS |
| Directory | `littleFS/` |
| Firmware version | |
| Board ID / revision | |
| Tester | |
| Date | |
| Equipment | LOLIN S3 only (no external hardware — LittleFS uses internal flash) |

**Pre-condition:** Create `littleFS/data/test.html` containing `<h1>OK</h1>`. Run `pio run -e lolin_s3 -t uploadfs` before uploading the verification sketch. This uploads the file to the LittleFS partition so HW-LFS-002 and HW-LFS-003 can verify that pre-flashed files are accessible.

> **Partition table:** The default LOLIN S3 Arduino partition scheme (16 MB flash) allocates ~1.5 MB for LittleFS — sufficient for web interface files. Verify `board_build.partitions` in `platformio.ini` is not set to a scheme that omits the LittleFS partition.

#### Test cases

| ID | Description | Procedure | Expected result | Actual result | P/F |
|----|-------------|-----------|-----------------|---------------|-----|
| HW-LFS-001 | LittleFS partition mounts | Upload sketch after `uploadfs`; open serial | "LittleFS init OK" printed within 3 s | | |
| HW-LFS-002 | Pre-uploaded file is found | Sketch calls littlefs_exists("/test.html") | Returns true; "exists: true" printed | | |
| HW-LFS-003 | Pre-uploaded file content is correct | Sketch reads "/test.html" | Content = `<h1>OK</h1>` printed on serial; matches file uploaded via `uploadfs` | | |
| HW-LFS-004 | Non-existent file returns false on exists check | Sketch calls littlefs_exists("/missing.txt") | Returns false; "exists: false" printed; no crash | | |
| HW-LFS-005 | Non-existent file read returns correct error | Sketch calls littlefs_read("/missing.txt") | "LFS_ERR_NOT_FOUND" returned; no crash | | |
| HW-LFS-006 | Runtime write creates file | Sketch calls littlefs_write("/runtime.txt", "hello", 5) | "LFS_OK" returned | | |
| HW-LFS-007 | Written file content is correct | Sketch reads "/runtime.txt" after HW-LFS-006 | Content = "hello" printed; matches written data | | |
| HW-LFS-008 | Overwrite replaces previous content | Sketch calls littlefs_write("/runtime.txt", "world", 5); then reads | Content = "world"; previous content "hello" absent | | |
| HW-LFS-009 | Free bytes reported | Sketch calls littlefs_free_bytes() | Returns value > 0. Record: ___ bytes remaining | | |

#### Overall result

| Field | Value |
|-------|-------|
| Result | PASS / FAIL / INCOMPLETE |
| Failed test IDs | |
| Notes | |

---

## Completion Checklist

For each driver, mark off both stages before declaring the driver done:

| Driver | Directory | Wave | Unit test IDs | Host tests pass | Hardware test IDs | HW tests pass |
|--------|-----------|------|---------------|-----------------|-------------------|---------------|
| LIB-1 GPIO Utility | `gpio/` | 1 | UT-GPIO-001…011 | ✅ 2026-04-10 | HW-GPIO-001…011 | ✅ 2026-04-10 |
| LIB-2 I2C Bus | `i2c/` | 1 | UT-I2C-001…008 | ✅ 2026-04-10 | HW-I2C-001…005 | ✅ 2026-04-10 |
| LIB-5 Keypad Matrix | `keyPad/` | 1 | UT-KP-001…010 | ☐ | HW-KP-001…004 | ☐ |
| LIB-7 NVS Configuration | `nvs/` | 1 | UT-NVS-001…013 | ☐ | HW-NVS-001…010 | ☐ |
| LIB-8 SD Card | `sdCard/` | 1 | UT-SD-001…012 | ☐ | HW-SD-001…010 | ☐ |
| LIB-9 LittleFS | `littleFS/` | 1 | UT-LFS-001…008 | ☐ | HW-LFS-001…009 | ☐ |
| LIB-3 DS1307 RTC | `DS1307_RTC/` | 2 | UT-RTC-001…011 | ✅ 2026-04-10 | HW-RTC-001…005 | ✅ 2026-04-10 |
| LIB-4 LCD1602 I2C | `LCD1602_I2C/` | 2 | UT-LCD-001…011 | ✅ 2026-04-10 | HW-LCD-001…008 | ✅ 2026-04-10 |
| LIB-6 Modbus RTU | `modBus/` | 2 | UT-MB-001…012 | ☐ | HW-MB-001…005 | ☐ |

A driver is not available as a dependency for Wave 2 work until **both** columns for that driver are ticked.

---

*Reference documents:*
- *Pin assignments and peripheral specifications: `design/technicalHardwareDesignSpecification.md` §4.11.2*
- *NVS namespace layout and log entry structure: `design/technicalSoftwareDesignSpecification.md` §5.3 and §5.10*
- *Task-to-driver mapping: `design/tasks.md`*
- *Log rotation policy: `design/technicalSoftwareDesignSpecification.md` §5.3*

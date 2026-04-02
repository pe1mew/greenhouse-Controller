# Implementation Plan

## Greenhouse Ventilation Controller

| Field        | Value                                    |
|--------------|------------------------------------------|
| Document     | Implementation Plan                      |
| Project      | Greenhouse Ventilation Controller        |
| Version      | 0.1 (draft)                              |
| Date         | 2026-03-30                               |
| Status       | Draft                                    |
| Related docs | `technicalHardwareDesignSpecification.md`, `technicalSoftwareDesignSpecification.md`, `tasks.md`, `softwareTestPlan.md` |

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Part A — Hardware Implementation](#2-part-a--hardware-implementation)
   - [A1 — Schematic Completion](#a1--schematic-completion)
   - [A2 — PCB Layout](#a2--pcb-layout)
   - [A3 — Fabrication and Assembly](#a3--fabrication-and-assembly)
   - [A4 — Hardware Bring-Up and Verification](#a4--hardware-bring-up-and-verification)
3. [Part B — Software Implementation](#3-part-b--software-implementation)
   - [Phase 1 — Peripheral Libraries](#phase-1--peripheral-libraries)
   - [Phase 2 — FreeRTOS Task Implementation](#phase-2--freertos-task-implementation)
   - [Phase 3 — Integration and System Test](#phase-3--integration-and-system-test)
4. [Dependency Overview](#4-dependency-overview)
5. [Open Issues That Block Implementation](#5-open-issues-that-block-implementation)

---

## 1. Introduction

This document defines the ordered implementation plan for the greenhouse ventilation controller. It is split into two independent tracks:

- **Part A — Hardware**: PCB design, fabrication, assembly, and electrical verification.
- **Part B — Software**: Peripheral driver libraries, unit tests, FreeRTOS task development, and integration.

The two tracks can proceed partially in parallel; hardware bring-up requires a working PCB, while software driver libraries and unit tests can be developed and validated on the host before hardware is available.

### Guiding Principles

- **Libraries first**: Every peripheral is encapsulated in a standalone library before any FreeRTOS task is written. This separates hardware interaction from RTOS machinery.
- **Unit test before integration**: Each library is validated with host-side unit tests before it is used inside a FreeRTOS task. Tests run without hardware (mock/stub the hardware layer) and also on target hardware where practical.
- **RTOS-compatible design**: Library APIs must be safe to call from FreeRTOS tasks. No global state that requires external locking, no blocking delays in ISR context, and no dependencies on `Arduino::delay()` (use `vTaskDelay()` instead).
- **Task order by dependency**: FreeRTOS tasks are implemented in the order that respects their data and signalling dependencies — foundation tasks before application tasks, application tasks before network tasks.

---

## 2. Part A — Hardware Implementation

### A1 — Schematic Completion

**Goal:** A complete, reviewable KiCad schematic with all components, all net connections, and all design decisions resolved.

| Step | Action | Notes |
|------|--------|-------|
| A1.1 | Resolve Open Issue #7: confirm time source (DS3231 assumed) | Affects RTC schematic symbol and power net |
| A1.2 | Resolve Open Issue #1: confirm RRK-3 alarm relay output signal type | Affects opto-coupler input circuit |
| A1.3 | Complete relay output section (6 relays, opto-isolated 5 V drive, screw terminals) | Verify coil current against 5 V rail budget |
| A1.4 | Complete power supply section (AC-DC HLK-10M24, DC-DC 24 V → 5 V, buffer capacitor) | Size buffer cap for ≥ 1 s sustain at full load |
| A1.5 | Complete RS485 transceiver circuit (SIT65HVD08P, DE/RE control, termination) | Include bus termination resistor (120 Ω) |
| A1.6 | Add ESD and TVS protection on all external-facing connectors | RS485, 24 V sensor supply, relay screw terminals |
| A1.7 | Add decoupling capacitors to all ICs following datasheet recommendations | DS3231, SIT65HVD08P, relay driver |
| A1.8 | Add test points on key signals (UART TX/RX, I2C SDA/SCL, 5 V and 3.3 V rails) | Required for bring-up and production test |
| A1.9 | Perform schematic ERC in KiCad; resolve all errors | No unconnected pins without explicit no-connect markers |
| A1.10 | Peer-review schematic against technicalHardwareDesignSpecification.md | Cross-check all GPIO assignments and voltage levels |

**Exit Criteria:** ERC passes with zero errors. All open issues resolved. Schematic reviewed and approved.

---

### A2 — PCB Layout

**Goal:** A complete, DRC-clean PCB layout that fits within the Multicomp Pro MC001110 enclosure (222 × 146 × 55 mm).

| Step | Action | Notes |
|------|--------|-------|
| A2.1 | Define board outline matching enclosure mounting pattern | Confirm standoff positions against enclosure datasheet |
| A2.2 | Import net list from completed schematic | Must be based on approved A1 schematic |
| A2.3 | Place connectors first: screw terminals (relay, sensor, power), USB-C, SD card | Connectors define board edge orientations |
| A2.4 | Place LOLIN S3 module centrally; reserve keep-out zone for WiFi antenna | Antenna must not be shielded by copper planes |
| A2.5 | Place relay module(s) in a block, isolated from logic circuitry | Route high-current paths with wide traces |
| A2.6 | Place I2C peripherals (DS3231, LCD header) close to MCU I2C pins | Keep I2C traces short (< 50 mm) |
| A2.7 | Place RS485 transceiver adjacent to UART pins and RS485 screw terminal | Minimise stub length on differential pair |
| A2.8 | Route power planes (24 V, 5 V, GND) before signal traces | Use poured copper fills; check via stitching for ground plane |
| A2.9 | Route high-current traces (relay coil drive, 24 V input) at ≥ 1 mm width | Calculate trace width from IPC-2221 for operating temperature |
| A2.10 | Route RS485 differential pair as matched length; add 120 Ω termination | Impedance-controlled routing not required at 9600 baud |
| A2.11 | Route I2C and SPI signal traces | Keep away from relay switching traces |
| A2.12 | Add silkscreen labels for all connectors and test points | Include pin numbering and voltage ratings |
| A2.13 | Run DRC; resolve all errors and review all warnings | No unrouted nets; no clearance violations |
| A2.14 | Verify 3D model in KiCad against enclosure dimensions | Check LCD header height against transparent cover |
| A2.15 | Generate fabrication outputs (Gerbers, drill, BOM, pick-and-place) | Follow JLCPCB or equivalent fab house requirements |

**Exit Criteria:** DRC passes with zero errors. 3D fit verified against enclosure. Fabrication files generated and checked.

---

### A3 — Fabrication and Assembly

| Step | Action | Notes |
|------|--------|-------|
| A3.1 | Order PCB (prototype quantity: 5) | Use approved Gerber files from A2 |
| A3.2 | Order all components per BOM | Source from BOM; verify footprints match ordered variants |
| A3.3 | Solder SMD components (DC-DC converter, resistors, capacitors, SIT65HVD08P) | Hot air or reflow; inspect with magnification |
| A3.4 | Solder through-hole components (relay module, screw terminals, DS3231 module) | Mechanical stability required for screw terminals |
| A3.5 | Solder LOLIN S3 module (pin headers or direct SMD pads) | Double-check orientation; module not reversible |
| A3.6 | Install LCD module on standoffs inside enclosure | Connect via ribbon or jumper cable |
| A3.7 | Final assembly into enclosure; dress all cables | Verify cable glands seal correctly for IP67 |

**Exit Criteria:** Assembled board fits in enclosure. Visual inspection complete. No solder bridges or cold joints.

---

### A4 — Hardware Bring-Up and Verification

**Goal:** Verify every hardware subsystem electrically before running firmware.

| Step | Action | Pass Criteria |
|------|--------|---------------|
| A4.1 | Bench power-on: apply 24 VDC (current limited); measure 5 V and 3.3 V rails | 5 V: 4.9–5.1 V; 3.3 V: 3.25–3.35 V; no smoke |
| A4.2 | Measure quiescent current from 24 V rail | Within calculated budget (< 200 mA with no sensors) |
| A4.3 | Flash minimal firmware; verify UART output on serial console | Boot messages appear; no crash |
| A4.4 | Verify I2C bus: scan for DS3231 (0x68) and LCD (0x27) | Both addresses detected |
| A4.5 | Verify DS3231: set time, power-cycle, read back correct time | Time is retained without main power |
| A4.6 | Verify LCD: display test pattern on all 32 character positions | All positions visible through enclosure cover |
| A4.7 | Verify keypad: read all 16 keys individually | Each key generates correct row/column code |
| A4.8 | Verify RS485: connect one Modbus sensor; read a single register | Valid Modbus response received |
| A4.9 | Verify relay outputs: energise each relay individually via firmware | Audible click; continuity measured across contact |
| A4.10 | Verify opto-coupler input: apply 24 V to input; read GPIO | GPIO transitions from low to high (or inverse per schematic) |
| A4.11 | Verify WiFi: connect to AP; ping test | AP visible in scan; HTTP response received |
| A4.12 | Verify SD card (if populated): mount LittleFS; write/read 1 kB file | File contents match after power cycle |
| A4.13 | Verify status LEDs: toggle each LED independently via firmware | All three LEDs illuminate at correct colour |
| A4.14 | Full thermal test at rated load for 30 min | No component exceeds 70 °C case temperature |

**Exit Criteria:** All A4 checks pass. Results documented in a bring-up report.

---

## 3. Part B — Software Implementation

Software is developed in two phases:

- **Phase 1**: Standalone peripheral libraries, each validated with unit tests.
- **Phase 2**: FreeRTOS tasks, implemented in dependency order, each validated by integration tests before the next task is built.

### Phase 1 — Peripheral Libraries

Each library:
- Encapsulates one hardware peripheral.
- Provides a clean C/C++ API with no FreeRTOS calls in the library code itself (use of `vTaskDelay()` in drivers is allowed, but must be documented so callers know the call can block).
- Is covered by unit tests that run on the host (using stubs for Arduino HAL) and on target hardware.
- Is accepted when all unit tests pass on both host and target.

Libraries are listed in order of development priority. Libraries that are dependencies of others are implemented first.

---

#### LIB-1 — GPIO Utility Library

**Description:** Abstracts all digital output and input GPIO operations: relay control (active-low), status LEDs, opto-coupler feedback input, and RS485 DE/RE direction control.

**API surface:**
- `gpio_init()` — configure all GPIO directions and initial states
- `gpio_relay_set(channel, state)` — assert/deassert relay (with active-low inversion)
- `gpio_led_set(led_id, state)` — control LED state
- `gpio_rs485_direction(tx_enable)` — switch RS485 transceiver direction
- `gpio_read_opto()` — read opto-coupler input

**FreeRTOS compatibility:** No RTOS calls; all functions are safe to call from any task context.

**Unit tests:**
- `test_relay_default_all_off` — verify all relays start deasserted after init
- `test_relay_set_assert_deassert` — toggle one relay; verify GPIO output matches
- `test_relay_active_low_inversion` — verify active-low polarity is applied correctly
- `test_led_control` — verify each LED can be turned on and off independently
- `test_rs485_direction_tx` — verify DE/RE pin state in transmit mode
- `test_rs485_direction_rx` — verify DE/RE pin state in receive mode
- `test_opto_read_low` — stub GPIO returns low; verify function returns inactive
- `test_opto_read_high` — stub GPIO returns high; verify function returns active

---

#### LIB-2 — I2C Bus Library

**Description:** Wraps the Arduino Wire library for the shared I2C bus (GPIO 21 SDA, GPIO 22 SCL). Provides mutex-aware access so multiple tasks can safely call I2C peripherals.

**API surface:**
- `i2c_init(sda_pin, scl_pin, freq_hz)` — initialise bus
- `i2c_acquire(timeout_ms)` — take the I2C mutex (MX1); returns bool
- `i2c_release()` — release MX1
- `i2c_write(addr, data, len)` — write bytes to device
- `i2c_read(addr, buf, len)` — read bytes from device
- `i2c_scan()` — return list of responding addresses (used during bring-up)

**FreeRTOS compatibility:** `i2c_acquire()` calls `xSemaphoreTake()`. Must never be called from ISR context.

**Unit tests:**
- `test_i2c_init` — verify no error on init with valid pins
- `test_i2c_acquire_release` — acquire and release mutex; verify state
- `test_i2c_acquire_timeout` — simulate mutex held; verify timeout returns false
- `test_i2c_write_correct_bytes` — stub Wire; verify byte sequence sent
- `test_i2c_read_returns_data` — stub Wire; verify read returns stubbed data
- `test_i2c_scan_finds_address` — stub Wire acknowledge; verify address appears in scan result

---

#### LIB-3 — DS3231 RTC Library

**Description:** Driver for the DS3231 real-time clock over I2C (address 0x68). Provides time read/write and alarm functions. Uses LIB-2 for bus access.

**API surface:**
- `rtc_init()` — initialise; verify DS3231 is present
- `rtc_get_time(datetime_t *dt)` — read current time into struct
- `rtc_set_time(const datetime_t *dt)` — write time to RTC
- `rtc_get_temperature()` — read DS3231 internal temperature (float °C)

**FreeRTOS compatibility:** Calls `i2c_acquire()`; is blocking; must not be called from ISR.

**Unit tests:**
- `test_rtc_init_detects_device` — stub I2C returns ACK at 0x68; verify init succeeds
- `test_rtc_init_absent_device` — stub I2C returns NACK; verify init returns error
- `test_rtc_decode_bcd` — verify BCD → binary decode for hours, minutes, seconds
- `test_rtc_encode_bcd` — verify binary → BCD encode for hours, minutes, seconds
- `test_rtc_get_time_returns_correct_struct` — stub I2C read with known BCD bytes; verify datetime fields
- `test_rtc_set_time_sends_correct_bytes` — call set_time; verify I2C write byte sequence
- `test_rtc_temperature_decode` — stub register values; verify float conversion

---

#### LIB-4 — LCD1602 I2C Library

**Description:** Driver for the Waveshare LCD1602 I2C module (PCF8574 at address 0x27). Provides character and string display. Uses LIB-2 for bus access.

**API surface:**
- `lcd_init()` — initialise display (4-bit mode, 2 rows, backlight on)
- `lcd_clear()` — clear display
- `lcd_set_cursor(row, col)` — position cursor (row 0–1, col 0–15)
- `lcd_print(str)` — write null-terminated string at cursor
- `lcd_print_char(c)` — write single character
- `lcd_backlight(on)` — control backlight

**FreeRTOS compatibility:** Calls `i2c_acquire()`; is blocking; must not be called from ISR.

**Unit tests:**
- `test_lcd_init_sequence` — verify PCF8574 receives correct 4-bit init nibbles
- `test_lcd_clear_sends_command` — verify clear command byte is sent
- `test_lcd_set_cursor_row0` — verify DDRAM address for row 0 positions
- `test_lcd_set_cursor_row1` — verify DDRAM address offset for row 1 (0x40)
- `test_lcd_print_sends_ascii` — write "Hi"; verify PCF8574 receives matching ASCII
- `test_lcd_backlight_on_off` — verify backlight bit in PCF8574 output byte

---

#### LIB-5 — Keypad Matrix Library

**Description:** Driver for the 4×4 membrane keypad (8 GPIO pins: 4 row outputs, 4 column inputs with pull-ups). Implements hardware matrix scan and software debounce.

**API surface:**
- `keypad_init(row_pins[4], col_pins[4])` — configure GPIO directions and pull-ups
- `keypad_scan()` — scan matrix; returns key code or KEY_NONE (call every 20 ms)
- `keypad_get_char(keycode)` — map keycode to character (0–9, A–D, *, #)

**FreeRTOS compatibility:** No RTOS calls; safe to call from any task. Designed for periodic polling from T7.

**Unit tests:**
- `test_keypad_no_key_pressed` — all columns read high; verify returns KEY_NONE
- `test_keypad_key_press_row0_col0` — stub col0 low when row0 driven; verify correct key code
- `test_keypad_key_press_each_key` — parameterised test for all 16 keys
- `test_keypad_debounce_single_transition` — key detected after N consecutive identical scans
- `test_keypad_debounce_rejects_glitch` — single-scan low followed by highs does not trigger key event
- `test_keypad_get_char_mapping` — verify all 16 keycodes map to correct characters

---

#### LIB-6 — Modbus RTU Library

**Description:** Modbus RTU master driver for UART1 (GPIO 17 TX, GPIO 18 RX) via the SIT65HVD08P RS485 transceiver. Implements function codes FC03 (read holding registers) and FC04 (read input registers). Uses LIB-1 for RS485 direction control.

**API surface:**
- `modbus_init(uart_port, baud, tx_pin, rx_pin, de_re_pin)` — configure UART and GPIO
- `modbus_read_registers(slave_id, start_reg, count, buf)` — read `count` registers from slave; returns error code
- `modbus_last_error()` — return last error code (timeout, CRC error, exception code)

**FreeRTOS compatibility:** Uses `uart_read_bytes()` with timeout; is blocking during Modbus transaction. Must be called from a dedicated task (T5) only. UART access is serialised by the caller.

**Unit tests:**
- `test_modbus_crc16_known_values` — verify CRC16 calculation against known Modbus test vectors
- `test_modbus_build_request_fc03` — verify request frame bytes for FC03 at known address/count
- `test_modbus_parse_response_valid` — feed valid response frame; verify register values decoded
- `test_modbus_parse_response_crc_error` — corrupt last byte; verify CRC_ERROR returned
- `test_modbus_parse_response_exception` — feed exception response frame; verify exception code returned
- `test_modbus_timeout` — stub UART returns no data within timeout; verify TIMEOUT error
- `test_modbus_register_count_too_large` — request > 125 registers; verify rejected

---

#### LIB-7 — NVS Configuration Library

**Description:** Wrapper around the ESP32 Non-Volatile Storage (NVS) API for persistent configuration and event log ring buffer storage. Provides typed get/set operations for all system settings.

**API surface:**
- `nvs_config_init()` — open NVS namespace; load defaults if first boot
- `nvs_config_get_u16(key, default_val)` — read uint16 setting
- `nvs_config_set_u16(key, value)` — write uint16 setting
- `nvs_config_get_float(key, default_val)` — read float setting
- `nvs_config_set_float(key, value)` — write float setting
- `nvs_config_get_str(key, buf, max_len)` — read string setting
- `nvs_config_set_str(key, value)` — write string setting
- `nvs_config_reset()` — erase all settings and restore defaults
- `nvs_log_append(entry)` — append log entry to ring buffer
- `nvs_log_read(index, entry)` — read log entry by index

**FreeRTOS compatibility:** NVS API is thread-safe on ESP32; no additional mutex required at library level. Callers serialise via MX4.

**Unit tests:**
- `test_nvs_default_values_on_first_boot` — erase NVS; verify defaults returned
- `test_nvs_round_trip_u16` — write value; read back; verify equal
- `test_nvs_round_trip_float` — write float; read back; verify within epsilon
- `test_nvs_round_trip_string` — write string; read back; verify equal
- `test_nvs_reset_restores_defaults` — write value; reset; verify default returned
- `test_nvs_log_append_and_read` — append 5 entries; read back; verify order and content
- `test_nvs_log_ring_wrap` — fill ring buffer beyond capacity; verify oldest entry overwritten

---

#### LIB-8 — SD Card / LittleFS Library

**Description:** Optional library providing file I/O on the SD card (SPI) and LittleFS (internal flash). Used by T9 (event log) and T11 (web server static files).

**API surface:**
- `storage_init(sd_cs_pin)` — mount SD card (SPI); mount LittleFS; report available space
- `storage_sd_write_append(path, data, len)` — append bytes to file on SD card
- `storage_sd_read(path, buf, max_len)` — read file from SD card
- `littlefs_read(path, buf, max_len)` — read file from LittleFS
- `storage_sd_available()` — returns bool; false if SD card not present or failed to mount

**FreeRTOS compatibility:** File operations are blocking. Callers serialise via MX5 (LittleFS) or a dedicated SD mutex.

**Unit tests:**
- `test_littlefs_read_existing_file` — place known file in LittleFS image; verify read content
- `test_sd_write_append_creates_file` — write to new path; verify file exists after flush
- `test_sd_write_append_grows_file` — append twice; verify file length doubles
- `test_sd_available_false_when_absent` — stub SPI returns no card; verify returns false
- `test_storage_sd_not_blocking_littlefs` — SD unavailable; verify LittleFS reads still succeed

---

### Phase 2 — FreeRTOS Task Implementation

Tasks are implemented in dependency order. A task is not started until all libraries it depends on have passed unit tests and all tasks it depends on have been validated.

The dependency hierarchy is:

```
Group 1 (Foundation) ──► Group 2 (Data Layer) ──► Group 3 (Sensors & Safety)
                                                  ──► Group 4 (Control & Relay)
                                                         ──► Group 5 (UI)
                                                  ──► Group 6 (Network)
```

---

#### Group 1 — Foundation Tasks

These tasks have no inter-task dependencies and can be implemented and tested first.

---

##### TASK-T1 — Watchdog / Heartbeat

**Depends on:** LIB-1 (GPIO Utility)
**Depends on tasks:** None

**Implementation:**
- Implemented as a FreeRTOS software timer callback or a minimal high-priority task.
- Calls `esp_task_wdt_reset()` every 500 ms.
- Calls `gpio_led_set(LED_HEARTBEAT, toggle)` to pulse the amber LED at 1 Hz.
- During startup, pulses LED at 4 Hz until `heartbeat_set_normal()` is called by the main initialisation sequence.

**Acceptance tests:**
- `test_t1_wdt_kick_period` — measure time between WDT kicks; verify within 400–600 ms.
- `test_t1_led_toggle_rate_normal` — verify LED toggles at 1 Hz in normal mode.
- `test_t1_led_toggle_rate_startup` — verify LED toggles at 4 Hz during startup mode.
- `test_t1_starvation_guard` — simulate 600 ms task starvation; verify WDT resets system.

---

##### TASK-T9 — Event Logger

**Depends on:** LIB-7 (NVS), LIB-8 (SD Card / LittleFS)
**Depends on tasks:** None (but queue Q3 must exist)

**Implementation:**
- Waits on queue Q3 for `log_event_t` messages from any task.
- Writes each entry to the NVS ring buffer via `nvs_log_append()`.
- If SD card is available, also appends a CSV line to the log file.
- Decouples all log I/O from real-time tasks.

**Acceptance tests:**
- `test_t9_receives_log_event` — post event to Q3; verify entry appears in NVS log.
- `test_t9_sd_write_when_available` — stub SD available; verify CSV line written.
- `test_t9_sd_skip_when_unavailable` — stub SD absent; verify NVS write still succeeds.
- `test_t9_queue_depth_not_blocking` — post 10 events in rapid succession; verify none dropped.

---

#### Group 2 — Data Layer

##### TASK-T4 — Data Manager

**Depends on:** LIB-7 (NVS Configuration), LIB-3 (DS3231 RTC)
**Depends on tasks:** T1 (must be running to ensure WDT is active before T4 initialises NVS)

**Implementation:**
- Reads all configuration from NVS at startup via LIB-7; populates the in-memory config struct.
- Reads current time from DS3231 via LIB-3 at startup.
- Owns and protects all shared state behind MX2 (sensor readings), MX3 (ring buffer history), and MX4 (configuration).
- Receives new sensor readings from T5 via Q6; updates shared state; sends TN1 to T3 and TN2 to T6.
- Receives configuration update messages from T8 and T11 via Q4; validates values; persists to NVS.
- Exposes accessor functions (`data_get_temperature()`, `data_get_wind_speed()`, `data_get_config()`) that acquire the correct mutex internally.

**Acceptance tests:**
- `test_t4_loads_config_on_startup` — stub NVS with known values; verify data accessors return those values.
- `test_t4_defaults_on_empty_nvs` — clean NVS; verify defaults are used.
- `test_t4_sensor_reading_update` — post reading to Q6; verify data accessors return updated values.
- `test_t4_notifies_t3_on_wind_data` — post wind reading; verify TN1 is sent.
- `test_t4_notifies_t6_on_sensor_data` — post T/RH reading; verify TN2 is sent.
- `test_t4_config_update_persisted` — post config change to Q4; read NVS; verify persisted.
- `test_t4_config_validation_rejects_out_of_range` — post invalid setpoint; verify rejected and NVS unchanged.
- `test_t4_mutex_held_blocks_concurrent_read` — simulate concurrent read during update; verify no race.

---

#### Group 3 — Sensor and Safety

Both T5 and T3 depend on T4 being operational. T3 depends on T5 for data.

---

##### TASK-T5 — Sensor Poll

**Depends on:** LIB-6 (Modbus RTU), LIB-1 (GPIO — RS485 direction)
**Depends on tasks:** T4 (Data Manager — T5 posts readings to Q6 consumed by T4)

**Implementation:**
- Polls SenseCAP S200 (wind speed register, wind direction register) via LIB-6 every 60 s (configurable via T4).
- Polls FG6485A (temperature register, humidity register) via LIB-6 every 60 s.
- On success: builds `sensor_reading_t` and posts to Q6.
- On failure: sets fault flags in EG1 (`SENSOR_FAULT_T` or `SENSOR_FAULT_W`); posts fault log event to Q3.
- Clears fault flag when the next successful reading is received.

**Acceptance tests:**
- `test_t5_reads_wind_sensor_registers` — stub Modbus; verify FC03 request sent to SenseCAP S200 address.
- `test_t5_reads_th_sensor_registers` — stub Modbus; verify FC03 request sent to FG6485A address.
- `test_t5_posts_reading_to_q6` — verify Q6 contains correct decoded reading after successful poll.
- `test_t5_sets_fault_flag_on_modbus_timeout` — stub Modbus timeout; verify EG1.SENSOR_FAULT_W set.
- `test_t5_clears_fault_flag_on_recovery` — fault set; next poll succeeds; verify fault flag cleared.
- `test_t5_respects_poll_interval` — verify T5 does not poll faster than configured interval.

---

##### TASK-T3 — Safety Monitor

**Depends on:** LIB-1 (GPIO), T4 (Data Manager for wind data accessors)
**Depends on tasks:** T4 (running), T5 (produces data that T4 distributes to T3)

**Implementation:**
- Blocks on TN1 (task notification from T4 when new wind data is available).
- On each notification: reads wind speed and direction from T4 via `data_get_wind_speed()` and `data_get_wind_direction()` (mutex-protected).
- If wind speed ≥ v_max threshold OR wind direction is within the configured exclusion zone: posts `CLOSE_ALL` command to Q1; sets EG1.WIND_OVERRIDE.
- Clears WIND_OVERRIDE when wind conditions return to safe values for a configurable hold-off period.
- Active regardless of Automatic/Standby mode — cannot be inhibited.
- Posts all safety events to Q3 for logging.

**Acceptance tests:**
- `test_t3_no_action_below_threshold` — wind = v_max - 1; verify no CLOSE_ALL command.
- `test_t3_triggers_on_speed_threshold` — wind = v_max; verify CLOSE_ALL posted to Q1.
- `test_t3_triggers_on_direction_exclusion` — wind direction in exclusion zone; verify CLOSE_ALL.
- `test_t3_wind_override_flag_set` — trigger safety; verify EG1.WIND_OVERRIDE set.
- `test_t3_hold_off_prevents_premature_clear` — wind drops below threshold; verify flag not cleared until hold-off expires.
- `test_t3_active_in_standby_mode` — system in Standby; verify T3 still triggers safety response.
- `test_t3_logs_safety_event` — trigger safety; verify event posted to Q3.

---

#### Group 4 — Control and Relay

##### TASK-T2 — Relay Controller

**Depends on:** LIB-1 (GPIO — relay outputs, opto input)
**Depends on tasks:** T1 (WDT active), T9 (Event Logger active for command logging)

**Note:** T2 can be implemented before T3, T5, and T6 are complete because it only reacts to commands arriving on Q1. Standalone tests inject commands directly.

**Implementation:**
- Sole owner of all 6 relay GPIO outputs; no other task may call relay GPIO functions directly.
- Receives `actuation_cmd_t` messages from Q1 (producers: T3, T6, T8, T11, T12).
- Per-channel state machine: `CLOSED` → `MOVING` → `OPEN` and reverse.
- Enforces mutual exclusion: OPEN and CLOSE relays for the same channel are never asserted simultaneously. A direction change requires asserting CLOSE first, waiting for travel time, then de-asserting.
- Dwell timer: minimum dwell time in each end state before the next command is accepted.
- Monitors opto-coupler input; on transition: sets EG1.MANUAL_OVERRIDE; sends TN3 to T6; posts log event to Q3.
- Motor travel times: M1 = 21 s, M2 = 21 s, M3 = 171 s.

**Acceptance tests:**
- `test_t2_default_state_all_closed` — startup; verify all relay GPIO deasserted.
- `test_t2_open_command_asserts_open_relay` — post OPEN M1 command; verify OPEN relay GPIO asserted.
- `test_t2_mutual_exclusion_no_simultaneous_open_close` — verify OPEN and CLOSE never simultaneously asserted.
- `test_t2_travel_time_m1` — post OPEN M1; verify relay de-asserts after 21 s.
- `test_t2_travel_time_m3` — post OPEN M3; verify relay de-asserts after 171 s.
- `test_t2_dwell_timer_blocks_rapid_reversal` — OPEN then immediate CLOSE; verify CLOSE deferred until dwell expires.
- `test_t2_close_all_preempts_dwell` — WIND_OVERRIDE CLOSE_ALL; verify all relays closed immediately.
- `test_t2_opto_input_triggers_manual_override` — assert opto input; verify EG1.MANUAL_OVERRIDE set and TN3 sent.
- `test_t2_logs_every_command_execution` — verify Q3 receives log event for each relay transition.

---

##### TASK-T6 — Climate Control

**Depends on:** T4 (sensor data and configuration), T2 (relay commands via Q1), T3 (wind override detection via EG1)
**Depends on tasks:** T2 (running and consuming Q1), T3 (running), T4 (running, TN2 available)

**Implementation:**
- Blocks on TN2 (task notification from T4 when new T/RH data is available).
- Reads current temperature, humidity, and configuration setpoints from T4.
- Evaluates temperature and humidity against setpoints with hysteresis.
- Runs conflict resolution algorithm when temperature and humidity demands conflict (per FR-CC06–CC08).
- Posts OPEN or CLOSE commands for M1, M2, M3 to Q1.
- Inhibited (posts no commands) when EG1.WIND_OVERRIDE, EG1.MANUAL_OVERRIDE, or system is in Standby mode.
- On receiving TN3 (manual override detected): reads EG1.MANUAL_OVERRIDE; stops issuing commands until cleared.

**Acceptance tests:**
- `test_t6_no_action_within_setpoint_band` — T and RH within deadband; verify no command posted.
- `test_t6_opens_window_on_high_temperature` — T > setpoint + hysteresis; verify OPEN command.
- `test_t6_closes_window_on_low_temperature` — T < setpoint - hysteresis; verify CLOSE command.
- `test_t6_opens_window_on_high_humidity` — RH > RH_max setpoint; verify OPEN command.
- `test_t6_conflict_resolution_temperature_wins` — T high and RH low simultaneously; verify temperature demand takes priority per spec.
- `test_t6_inhibited_during_wind_override` — EG1.WIND_OVERRIDE set; verify no commands posted.
- `test_t6_inhibited_during_manual_override` — EG1.MANUAL_OVERRIDE set; verify no commands posted.
- `test_t6_inhibited_in_standby_mode` — system in Standby; verify no commands posted.
- `test_t6_resumes_after_override_cleared` — override cleared; new sensor data arrives; verify commands resume.

---

#### Group 5 — User Interface

Both T7 and T8 can be developed in parallel with Group 4, as they only interact via Q1, Q2, Q4, and Q5.

---

##### TASK-T7 — Keypad Scan

**Depends on:** LIB-5 (Keypad Matrix)
**Depends on tasks:** T8 (must be running to consume Q2; T7 should start after T8)

**Implementation:**
- Implemented as a periodic FreeRTOS software timer callback (20 ms period) or a dedicated task.
- Calls `keypad_scan()` on each tick.
- On key press detected (after debounce): posts `key_event_t` to Q2.
- No key repeat; key held generates one event.

**Acceptance tests:**
- `test_t7_no_event_when_no_key` — scan with no key pressed; verify Q2 empty.
- `test_t7_posts_event_on_key_press` — stub key press; verify Q2 receives key event.
- `test_t7_debounce_suppresses_glitch` — single scan low; verify no event posted.
- `test_t7_no_repeat_on_held_key` — key held for 200 ms; verify exactly one event posted.
- `test_t7_scan_rate_20ms` — measure callback period; verify within 18–22 ms.

---

##### TASK-T8 — UI / Display

**Depends on:** LIB-4 (LCD1602), LIB-2 (I2C), LIB-3 (DS3231), T4 (data accessors), T7 (Q2 key events)
**Depends on tasks:** T4 (running), T9 (running for UI log events), T10 (for network status, but T8 must start before T10)

**Implementation:**
- Wakes every 200 ms to refresh the LCD display with current T, RH, wind speed, window states, mode, and alarms.
- Reads Q2 for key events; processes them through a menu finite state machine.
- Session management: PIN entry required for Farmer and Administrator access.
- Bcrypt PIN hash verification (or equivalent).
- Posts configuration changes to Q4 (consumed by T4).
- Receives network status events from Q5 (posted by T10); updates display.
- Posts all significant UI events to Q3 for logging.

**Acceptance tests:**
- `test_t8_home_screen_renders_temperature` — stub T4 returns known T; verify LCD receives matching string.
- `test_t8_home_screen_renders_window_states` — stub T4 returns OPEN/CLOSED states; verify LCD output.
- `test_t8_menu_navigation_down` — post DOWN key event; verify menu state transitions.
- `test_t8_menu_navigation_enter` — post ENTER key event in menu; verify correct submenu entered.
- `test_t8_pin_entry_accepted` — type correct PIN; verify session elevated.
- `test_t8_pin_entry_rejected` — type wrong PIN; verify session remains at Normal.
- `test_t8_config_change_posted_to_q4` — change setpoint in menu; verify Q4 receives update.
- `test_t8_alarm_shown_on_wind_override` — EG1.WIND_OVERRIDE set; verify alarm message on LCD.
- `test_t8_network_status_shown` — post network connected event to Q5; verify displayed.

---

#### Group 6 — Network Tasks

Network tasks run on Core 0. They depend on Group 2 (Data Manager) but are independent of Groups 4 and 5. They can be developed after T4 is validated.

---

##### TASK-T10 — Network Manager

**Depends on:** ESP-IDF WiFi API
**Depends on tasks:** T4 (data accessors for configuration: SSID, password, AP settings), T9 (event logging)

**Implementation:**
- Starts in Access Point mode if no client credentials are configured.
- Connects as client if credentials are stored in NVS.
- On client connect: triggers NTP synchronisation; updates DS3231 via LIB-3; posts time to T4.
- Posts `net_status_event_t` to Q5 (consumed by T8).
- Reconnects automatically on disconnect.

**Acceptance tests:**
- `test_t10_starts_ap_mode_without_credentials` — empty NVS; verify AP mode started.
- `test_t10_connects_client_with_credentials` — credentials in NVS; verify client connect attempt.
- `test_t10_ntp_sync_on_connect` — stub NTP response; verify DS3231 time set.
- `test_t10_posts_status_to_q5_on_connect` — verify Q5 receives CONNECTED event.
- `test_t10_posts_status_to_q5_on_disconnect` — simulate disconnect; verify Q5 receives DISCONNECTED event.
- `test_t10_reconnects_after_timeout` — simulate disconnect then available again; verify reconnect.

---

##### TASK-T11 — Web Server

**Depends on:** LIB-8 (LittleFS for static files), T4 (data accessors and Q4 for config), T2 (Q1 for manual window commands)
**Depends on tasks:** T10 (WiFi connected), T4 (running)

**Implementation:**
- ESPAsyncWebServer serving HTML/CSS/JS from LittleFS.
- Implements three-state session model: Normal / Farmer / Administrator (session cookie, bcrypt PIN hash).
- Endpoints for sensor data (JSON), window status (JSON), configuration GET/POST, manual window commands, log download.
- Posts configuration changes to Q4; posts manual commands to Q1.

**Acceptance tests:**
- `test_t11_serves_index_html` — GET /; verify 200 response with HTML content.
- `test_t11_serves_sensor_data_json` — GET /api/sensors; verify JSON contains T, RH, wind fields.
- `test_t11_requires_auth_for_config` — unauthenticated POST /config; verify 401 response.
- `test_t11_config_post_accepted_with_session` — authenticated POST; verify Q4 receives update.
- `test_t11_manual_command_requires_farmer_role` — Normal session posts OPEN; verify rejected.
- `test_t11_manual_command_posts_to_q1` — Farmer session posts OPEN M1; verify Q1 receives command.

---

##### TASK-T12 — MQTT Client

**Depends on:** T4 (data accessors), T10 (network)
**Depends on tasks:** T10 (WiFi connected), T4 (running)

**Implementation:**
- Optional feature; enabled via NVS configuration.
- Publishes sensor data and window states to configured MQTT broker on a configurable interval.
- Subscribes to command topics; posts validated commands to Q1.
- Topic structure defined in technicalSoftwareDesignSpecification.md.

**Acceptance tests:**
- `test_t12_disabled_when_not_configured` — MQTT disabled in NVS; verify no connection attempt.
- `test_t12_publishes_sensor_data` — stub MQTT broker; verify publish called with correct topic and payload.
- `test_t12_subscribes_to_command_topic` — verify subscribe called on connect.
- `test_t12_command_received_posts_to_q1` — receive valid OPEN command on topic; verify Q1 receives it.
- `test_t12_ignores_invalid_command_payload` — receive malformed payload; verify Q1 not updated.

---

##### TASK-T13 — OTA Update

**Depends on:** T11 (web server trigger), LIB-8 (LittleFS update)
**Depends on tasks:** T11 (running), T10 (WiFi connected)

**Implementation:**
- Triggered from T11 web UI by Administrator-role session.
- Performs dual-bank firmware OTA using ESP-IDF OTA API.
- On success: reboots into new firmware.
- On failure (up to 3 consecutive): reverts to previous bank.
- Posts OTA status events to Q3; sets EG1.OTA_IN_PROGRESS during update to inhibit other tasks.

**Acceptance tests:**
- `test_t13_requires_admin_role` — Farmer session triggers OTA; verify rejected.
- `test_t13_sets_ota_in_progress_flag` — start OTA; verify EG1.OTA_IN_PROGRESS set.
- `test_t13_clears_flag_on_completion` — OTA completes; verify EG1.OTA_IN_PROGRESS cleared.
- `test_t13_rollback_after_three_failures` — stub OTA to fail; verify rollback after third attempt.

---

### Phase 3 — Integration and System Test

After all tasks are individually validated, the following integration tests verify end-to-end behaviour. These tests align with the acceptance criteria in `softwareTestPlan.md`.

| Test ID | Scenario | Validation |
|---------|----------|------------|
| INT-01 | Startup sequence | All 3 windows close within 215 s; heartbeat LED pulses at 4 Hz then drops to 1 Hz |
| INT-02 | Temperature-driven window opening | T rises above setpoint; T6 posts OPEN; T2 runs relay for correct travel time |
| INT-03 | Wind override | Wind speed breaches threshold mid-opening; T3 posts CLOSE_ALL; windows close; climate control inhibited |
| INT-04 | Wind override clears | Wind drops; hold-off expires; climate control resumes |
| INT-05 | Manual override via RRK-3 | RRK-3 relay de-energises; opto input changes state; T2 detects manual override; T6 inhibited; UI shows alarm |
| INT-06 | Keypad menu — setpoint change | Operator navigates menu; changes T setpoint; T4 persists to NVS; T6 uses new value |
| INT-07 | Web UI — window manual command | Administrator opens M1 via web UI; T11 posts command to Q1; T2 opens window |
| INT-08 | Power interruption | Disconnect 24 V for 5 s; restore; verify DS3231 retains time; NVS retains configuration; system resumes |
| INT-09 | MQTT publish | Broker connected; verify T, RH, wind, window state published every interval |
| INT-10 | OTA update | Upload firmware image via web UI; verify successful flash and boot from new partition |
| INT-11 | Conflict resolution | T high and RH low simultaneously; verify window behaviour follows priority defined in FR-CC06 |
| INT-12 | WDT recovery | Block T1 artificially for > WDT timeout; verify system resets and restarts cleanly |

---

## 4. Dependency Overview

```
Libraries (Phase 1)
├── LIB-1 GPIO Utility
├── LIB-2 I2C Bus
│   ├── LIB-3 DS3231 RTC
│   └── LIB-4 LCD1602
├── LIB-5 Keypad Matrix
├── LIB-6 Modbus RTU  ←── uses LIB-1 (RS485 direction)
├── LIB-7 NVS Config
└── LIB-8 SD / LittleFS

FreeRTOS Tasks (Phase 2)
├── Group 1 (Foundation)
│   ├── T1 Watchdog        ← LIB-1
│   └── T9 Event Logger    ← LIB-7, LIB-8
│
├── Group 2 (Data Layer)
│   └── T4 Data Manager    ← LIB-7, LIB-3 | needs T1
│
├── Group 3 (Sensors & Safety)
│   ├── T5 Sensor Poll     ← LIB-6 | needs T4
│   └── T3 Safety Monitor  ← LIB-1 | needs T4, T5
│
├── Group 4 (Control & Relay)
│   ├── T2 Relay Controller← LIB-1 | needs T1, T9
│   └── T6 Climate Control │ needs T2, T3, T4
│
├── Group 5 (UI)
│   ├── T7 Keypad Scan     ← LIB-5 | needs T8 (Q2 consumer)
│   └── T8 UI / Display    ← LIB-4, LIB-2, LIB-3 | needs T4, T9
│
└── Group 6 (Network)
    ├── T10 Network Manager│ needs T4, T9
    ├── T11 Web Server     ← LIB-8 | needs T10, T4
    ├── T12 MQTT Client    │ needs T10, T4
    └── T13 OTA            │ needs T11, T10
```

---

## 5. Open Issues That Block Implementation

The following open issues from the design documents must be resolved before the affected implementation steps begin.

| Issue | Blocks | Decision Required |
|-------|--------|-------------------|
| Open Issue #1 — RRK-3 alarm relay output signal type | A1 (schematic), LIB-1 unit tests, T2 acceptance tests | Confirm signal polarity (NO/NC) and voltage level of RRK-3 alarm output |
| Open Issue #7 — Time source (DS3231 vs GNSS vs DCF77) | A1 (schematic), LIB-3 development, T4 time initialisation | Confirm DS3231 as primary time source (current design assumption) |

Both issues are expected to be resolved in favour of the current design assumptions (DS3231; opto-coupled feedback input). If a different time source is selected, LIB-3 and the T4 initialisation sequence must be revised accordingly.

---

*End of Implementation Plan v0.1*

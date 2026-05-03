# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [1.3.4] — 2026-05-03

*T2 relay controller integration tests expanded to 13 tests (IT-01–IT-13), two structural test defects fixed, and the full suite verified on hardware — all 13 tests pass.*

### Added
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-10 through IT-13:
  - **IT-10** OPEN travel expiry + dwell enforcement: CMD_OPEN CH1 → waits 26 s for travel timer expiry → CH_OPEN; verifies SRC_T6 CMD_CLOSE blocked during the 3 s dwell window; verifies SRC_T6 CMD_CLOSE accepted after dwell expiry
  - **IT-11** CLOSE→OPEN reversal gap: CMD_OPEN during MOVING_CLOSE → CH_GAP_TO_OPEN → 2 s gap → MOVING_OPEN (symmetric counterpart to IT-04)
  - **IT-12** CMD_RESUME no-op: RESUME acknowledged by T2 with no relay state change
  - **IT-13** Invalid channel discarded: CMD_OPEN ch=0 and ch=4 produce `[W]` log entries and no relay change

### Changed
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-09 structural fix: replaced the two-phase poll (65 s "recal start" assertion + 185 s "recal done" poll) with a single 300 s completion loop; the two-phase approach failed when a second alarm during the guard pushed re-cal start beyond the 65 s window, causing Unity's `longjmp` to skip the completion wait and leave T2 blocked in guard+recal when IT-10 started (Q1 command batch-processing defect — see `firmwareImplementationResults.md` Issue 4)
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 and IT-09 manual step prompts, IT-09 completion message, and end-of-test banner all converted from `Serial.println()` to `ESP_LOGI()`; `Serial.println()` was silently dropped by the USB-CDC driver under test task scheduling conditions (see `firmwareImplementationResults.md` Issue 5), rendering both interactive prompts invisible and causing IT-07 to fail (alarm never connected)
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 prompt updated with explicit instruction to hold jumper still for ≥1 s (75 ms debounce requirement); duration table updated to show 600 s logic analyser capture window
- `firmware/firmwareImplementationResults.md` — test results table completed (13/13 PASS); logic analyser verification data and contact bounce measurement added; Issues 4 and 5 documented; handover state updated

### Verified on hardware
- All 13 integration tests pass (serial output + logic analyser CSV captured and cross-verified)
- Contact bounce measurement: 329 ms bounce on IT-07 jumper insertion correctly filtered by 75 ms debounce; alarm confirmed 83 ms after stable LOW (nominal 75 ms)
- All relay timing within 35 ms of expected values across boot calibration, guard, re-calibration, travel, gap, and dwell transitions

---

## [1.3.3] — 2026-05-03

*60 s guard time introduced after motor alarm clearance before CLOSE_ALL re-calibration starts. An open issue for alarm contact jitter has been added to the implementation plan.*

### Changed
- `firmware/src/relay_controller/relay_controller.cpp` — `handle_alarm_clearance()` now clears `EG1_BIT_MOTOR_ALARM` and logs clearance immediately, then blocks for `ALARM_GUARD_MS = 60 000 ms` (12 × 5 s chunks) before starting re-calibration; re-checks pin at guard expiry and aborts if alarm re-asserted
- `firmware/src/relay_controller/relay_controller.cpp` — added `ALARM_GUARD_MS` and `ALARM_GUARD_CHUNK_MS` constants
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-09 updated: alarm bit check remains at 15 s (bit cleared before guard); new 65 s poll for re-cal start (waits for guard to expire); recal-complete poll unchanged; expected duration updated to ~500 s

### Added
- `firmware/firmwareImplementationPlan.md` — open issue **#1c Alarm contact jitter**: single end-of-guard pin re-check does not detect bounce within the guard window; mitigations listed; decision deferred

### Documentation
- `design/tasks.md`, `design/technicalSoftwareDesignSpecification.md` (×3 locations), `firmware/firmwareImplementationPlan.md` (#1a, #1b, integration test), `firmware/firmwareImplementationResults.md` — all alarm clearance descriptions updated to include the 60 s guard step and abort-on-re-assertion behaviour

---

## [1.3.2] — 2026-05-03

*Alarm polarity corrected throughout: the RRK-3 opto-coupler output is active-low (GPIO42 LOW = alarm active, HIGH = alarm cleared with INPUT_PULLUP), not active-high as previously documented.*

### Fixed
- `firmware/src/relay_controller/relay_controller.cpp` — debounce logic now reads `alarm_signal = (gpio_read(PIN_OPTO_INPUT) == GPIO_LOW)`; onset condition `alarm_signal && !alarm_active`, clearance condition `!alarm_signal && alarm_active`
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-07 docblock and runtime prompt updated: "Connect GPIO42 to GND" (was "3.3 V"); file header "GPIO42→GND" updated
- `design/technicalHardwareDesignSpecification.md` — GPIO table: "active-low — logic LOW when contact is closed (alarm active)"; Open Issue #1: clarified active-low opto-coupler behaviour
- `design/technicalSoftwareDesignSpecification.md` — Motor alarm detection paragraph: "active-low: contact closed → GPIO 42 LOW; contact open → GPIO 42 HIGH"
- `firmware/firmwareImplementationResults.md` — Motor Alarm sequence table: rows corrected to "GPIO42 LOW → alarm asserted" and "GPIO42 HIGH → alarm cleared"

---

## [1.3.1] — 2026-05-03

*Inter-relay gap extended from 100 ms to 2 s to allow back-EMF to dissipate fully before reversing motor direction.*

### Changed
- `firmware/src/relay_controller/relay_controller.cpp` — `RELAY_GAP_MS` increased from `100u` to `2000u`; all inline log strings updated
- `firmware/src/relay_controller/relay_controller.h` — Doxygen updated
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — IT-04 timing updated: mid-gap spot-check at 1150 ms (was 80 ms), final CLOSE-HIGH check at 2650 ms (was 200 ms); all comments updated
- `firmware/firmwareImplementationPlan.md`, `firmware/firmwareImplementationResults.md` — gap value updated throughout
- `design/tasks.md`, `design/technicalSoftwareDesignSpecification.md` — gap value updated

---

## [1.3.0] — 2026-05-03

*Phase 2 firmware implemented and hardware-verified: T2 Relay Controller fully operational with per-channel window FSM, 2 s inter-relay gap enforcement, travel/dwell timers, deferred-ISR motor alarm handling, and a complete on-device Unity integration test suite.*

### Added

#### Firmware — Phase 2: Relay Controller (T2)
- `firmware/src/relay_controller/relay_controller.cpp` — full T2 implementation replacing the Phase 0 stub:
  - Per-channel internal FSM with two transient gap states (`CH_GAP_TO_OPEN`, `CH_GAP_TO_CLOSE`) enforcing a 2 s inter-relay gap on every direction reversal; both relays de-energised during the gap (audible as two distinct clicks spaced ~2 s apart)
  - Travel timers: relay energised for `(travel_mN + 5 s) × 1000 ms`; factory defaults M1/M2 = 26 s, M3 = 176 s; values read from NVS `motor/` namespace at T2 startup via `nvs_cfg_get_i32_or_default()`
  - Dwell timers: `SRC_T3` (Safety Monitor) commands bypass dwell; `SRC_T6` (Climate Control) commands respect it
  - `calib_close_all()`: synchronous blocking CLOSE_ALL at boot and after alarm clearance; de-energises each channel individually at its own travel deadline (M1/M2 at ~26 s, M3 at ~176 s) rather than waiting for the global maximum
  - Deferred-ISR motor alarm: `IRAM_ATTR` ISR on GPIO42 (CHANGE, not suppressed during MOVING); T2 loop confirms after 75 ms debounce by reading live pin state; on assertion: de-energises all 6 relays, sets `EG1_BIT_MOTOR_ALARM`, logs onset; on clearance: clears bit, logs clearance, re-runs `calib_close_all()` (FR-MA01–FR-MA08)
  - Q1 consumer: all commands discarded when `EG1_BIT_MOTOR_ALARM` is set (FR-MA03); 20 ms loop tick
- `firmware/src/relay_controller/relay_controller.h` — complete Doxygen header documenting all T2 responsibilities
- `firmware/test/test_t2_relay/test_t2_relay.cpp` — on-device Unity integration test suite (9 tests):
  - IT-01 NVS factory defaults; IT-02 boot calibration (185 s); IT-03 CMD_OPEN relay energisation; IT-04 direction reversal gap (2 s, audible click … 2 s … click); IT-05 mutual exclusion (OPEN+CLOSE never simultaneously HIGH); IT-06 CLOSE_ALL T3 override; IT-07 alarm onset (interactive, GPIO42 jumper); IT-08 command rejection during alarm; IT-09 alarm clearance + re-calibration (interactive)
  - Minimal heartbeat task (`task_test_heartbeat`) spawned instead of T1 so PIN_HB_LED blinks at 1 Hz during the test run; WDT disabled via `esp_task_wdt_deinit()` (boot calibration exceeds default 5 s timeout)

#### Firmware — test environment
- `firmware/platformio.ini` — `[env:test_t2_relay]` section added; key settings: `test_build_src = yes` (required to compile `src/` during `pio test`), `build_src_filter = +<**> -<main.cpp>` (include all tasks, exclude conflicting `main.cpp`)

### Fixed
- **`calib_close_all()` ran all channels for M3's full 176 s** — rewrote to track per-channel deadlines; M1/M2 relays now de-energise at ~26 s while M3 continues to ~176 s
- **Heartbeat LED static during test run** — test `setup()` now initialises `PIN_HB_LED` as output and spawns a minimal toggle-only heartbeat task

---

## [1.2.0] — 2026-05-03

*Phase 0 firmware implemented and verified on hardware. Firmware implementation plan completed with all gaps and open issues resolved. Design documentation updated for MOTOR_ALARM operating state and C9 scope enforcement.*

### Added

#### Firmware — Phase 0 scaffold (fully verified on hardware)
- `firmware/src/main.cpp` — `setup()`: initialises GPIO, I2C, RTC, NVS, creates all RTOS primitives (Q1–Q6, EG1, MX1–MX5), spawns 12 tasks; `loop()`: self-deletes. T1 Watchdog/Heartbeat fully implemented: 500 ms WDT kick (`esp_task_wdt_add` / `esp_task_wdt_reset`), 1 Hz heartbeat LED toggle (GPIO 41), WS2812B RGB LED (GPIO 38) driven from EG1 state (Red = MOTOR_ALARM, Amber = fault/wind override, Green = normal), day/night brightness with configurable night window (22:00–06:00 default)
- `firmware/src/relay_controller/relay_controller.h/.cpp` — T2 stub
- `firmware/src/safety_monitor/safety_monitor.h/.cpp` — T3 stub
- `firmware/src/data_manager/data_manager.h/.cpp` — T4 stub
- `firmware/src/sensor_poll/sensor_poll.h/.cpp` — T5 stub
- `firmware/src/keypad_scan/keypad_scan.h/.cpp` — T7 stub
- `firmware/src/ui_display/ui_display.h/.cpp` — T8 stub
- `firmware/src/network_manager/network_manager.h/.cpp` — T10 stub
- `firmware/src/web_server/web_server.h/.cpp` — T11 stub
- `firmware/src/mqtt_client/mqtt_client.h/.cpp` — T12 stub
- `firmware/firmwareImplementationResults.md` — Phase 0 implementation results: all six build/boot issues documented with root causes and fixes, verified hardware boot log, verification checklist, Phase 0 → Phase 1 handover state

#### Firmware — pre-Phase-0 modules (already complete before Phase 0 boot)
- `firmware/src/types/app_types.h` — complete shared type system: motor travel constants (M1/M2 = 21 s, M3 = 171 s, 5 s margin), all RTOS handle externs, queue item structs (`window_cmd_t`, `key_event_t`, `log_event_t`, `config_update_t`, `net_status_t`, `sensor_reading_t`), enums (`op_mode_t` with MOTOR_ALARM, `window_state_t`, `log_type_t`, `cmd_source_t` restricted to T3/T6 per C9), EG1 bit definitions (WIND_OVERRIDE, SENSOR_FAULT_T/W, OTA_IN_PROGRESS, MOTOR_ALARM; bit 1 reserved — MANUAL_OVERRIDE not supported by hardware)
- `firmware/src/climate_control/climate_control.h/.cpp` — Gap G resolved: graduated ventilation step table (step 1 = M1, step 2 = M1+M2, step 3 = M1+M2+M3), `vent_step_required_t()` and `vent_step_required_rh()` with close-hysteresis guard, `vent_resolve_conflict()` with three priority modes (CR_TEMP_FIRST, CR_RH_FIRST, CR_DEVIATION)
- `firmware/src/event_logger/event_logger.h/.cpp` — Gap H resolved: `log_post()` drop-oldest Q3 helper (evict-and-retry with portMUX spinlock-protected drop counter), `log_take_dropped_count()` atomic read-and-reset; T9 task stub
- `firmware/src/auth/pin_auth.h/.cpp` — Gap C resolved: PIN hashing via `mbedtls/sha256.h`; SHA-256(16-byte random salt ∥ PIN ASCII); salt stored in NVS `access/pin_salt`; per-role lockout with NVS-persisted expiry timestamps
- `firmware/src/data_manager/sunrise.h/.cpp` — Gap D resolved: NOAA General Solar Position Equations (±2 min accuracy); outputs UTC minutes from midnight; handles polar day/night; zero lat/lon defaults to daytime
- `firmware/firmwareImplementationPlan.md` — complete phased implementation plan (Phases 0–10), all 8 design gaps (A–H) resolved, 6 open issues (#1a/b–#6) resolved, integration test checklist

#### Design documentation
- `design/mocWebUIConciderations.md` — web UI mockup and layout considerations: dashboard, settings page, technical/admin page; responsive mobile-first approach; REST API endpoint mapping

### Changed

#### Design — MOTOR_ALARM operating state (new highest-priority mode)
Resolved Constraint C8: the RRK-3 provides a **single alarm output** that fires only when a motor runs to the **emergency switch** (not on normal manual operation). Manual operation detection via GPIO 42 is not achievable with the current hardware. All affected documents updated:
- `design/functionalRequirementsSpecification.md` — C8 resolved; FR-M08–M11 removed (manual detection not possible); §5.3a added with FR-MA01–FR-MA08 (Motor Alarm requirements: immediate relay de-energisation, highest-priority override, CLOSE_ALL re-calibration on clear, display message, logging); operating modes table updated
- `design/technicalHardwareDesignSpecification.md` — §4.5.2 updated: opto-coupler described as motor emergency stop alarm, not manual override detector
- `design/tasks.md` — T2 function updated (MOTOR_ALARM detection replaces manual override); GPIO 42 ISR description rewritten (not suppressed during MOVING; alarm assert: de-energise all relays + set EG1.MOTOR_ALARM; alarm release: clear flag + CLOSE_ALL re-calibration); EG1 table updated (MANUAL_OVERRIDE removed, MOTOR_ALARM bit 5 added); TN3 task notification removed
- `design/technicalSoftwareDesignSpecification.md` — T2 description updated; manual override detection section replaced with Motor Alarm detection; EG1 table updated; T6 EG1 inhibit flags updated; §5.12 RGB LED Red condition confirmed for MOTOR_ALARM
- `firmware/firmwareImplementationPlan.md` — open issues #1a/#1b updated; Phase 2 T2 GPIO 42 bullet rewritten; Phase 6 T6 EG1 check updated; integration test added

#### Design — C9 scope enforcement (no manual window commands from LCD/web/MQTT)
- `design/functionalRequirementsSpecification.md` — FR-WS05 updated: manual window commands explicitly excluded (C9)
- `design/softwareTestPlan.md` — ST-WI-008 replaced: no longer tests a manual command relay trigger; now verifies the web dashboard contains no window open/close controls
- `design/riskAssessment.md` — manual window via keypad removed from sensor-failure mitigation; MQTT attack chain updated (T12 does not post to Q1; attack surface narrowed to Q4 config updates only)
- `design/implementationPlan.md` — T8 manual window command bullet removed
- `firmware/src/types/app_types.h` — `cmd_source_t` limited to `SRC_T3` and `SRC_T6`; Q1 producer comment updated

#### Firmware configuration
- `firmware/platformio.ini` — `board_build.arduino.memory_type = qio_opi` (LOLIN S3 OPI PSRAM variant); `board_upload.offset_address = 0x20000` (app0 at 0x20000 due to 84 KB NVS); `monitor_dtr = 1` / `monitor_rts = 0`; `lib_extra_dirs = ../drivers`; `lib_ignore = WebServer`; `Adafruit NeoPixel`, `ESPAsyncWebServer`, `AsyncTCP` added to `lib_deps`; `-DCORE_DEBUG_LEVEL=3` and `-DCONFIG_NVS_LOG_CAPACITY=250` added

### Fixed
- **Crash loop on boot** (`rst:0x3 / Saved PC:0x403cdb0a`) — `board_upload.offset_address = 0x20000` added; firmware was being written to `0x10000` while the partition table directed the bootloader to find `app0` at `0x20000`
- **Silent serial output after boot** — all `setup()` diagnostics and T1 heartbeat switched from `Serial.println()` to `ESP_LOGI()`; `Serial.println()` dropped silently when no USB-CDC host had the port open (DTR not asserted), while IDF log calls bypass the DTR check
- **`extern "C"` linkage conflict** in `event_logger.cpp` and `climate_control.cpp` — `extern "C"` blocks removed; all code is C++ throughout
- **`WebServer` LDF conflict** — `lib_ignore = WebServer` added to prevent Arduino's built-in WebServer from being pulled in alongside ESPAsyncWebServer

---

## [1.1.0] — 2026-05-01

*PCB v1.1.0 released following first hardware board test; S200 driver completed; design documentation extended.*

### Added
- `drivers/relay_sequence_test/` — new PlatformIO test project for hardware GPIO verification: sequences all 6 relay outputs (M1/M2/M3 OPEN/CLOSE, GPIO 12–16 and 21), heartbeat LED (GPIO 41), and opto-isolated input (OPTO_INPUT → M1 OPEN follow); includes `README.md` with wiring and usage
- `drivers/s200/` — complete SenseCAP S200 wind sensor driver (LIB-10): `s200.h` / `s200.cpp`, 11 unit tests (UT-S200-001..011), mock Modbus layer, `S200.md` driver documentation
- `hardware/Testing/20250501_HardwareTest.md` — first hardware board test report for PCB v1.1.0: 53 tests across 6 subsystems (voltages, GPIO relays/LEDs/input, 4×4 keyboard, SD card, RTC, LCD); Modbus hardware test pending
- `design/greenhouse_nvs_variables.xlsx` — NVS variable overview spreadsheet covering all namespaces, keys, types, and default values
- `design/hardwareComponentDiagram.puml` + `.png` — hardware component architecture diagram (PlantUML)
- `design/lcd_gui_state_diagram.puml` + `.png` — LCD GUI state diagram (PlantUML)
- `design/web_gui_state_diagram_auth.puml` + `.png` — web GUI authentication flow state diagram (PlantUML)
- `design/web_gui_state_diagram_settings.puml` + `.png` — web GUI settings state diagram (PlantUML)
- `design/web_gui_state_diagram_tech.puml` + `.png` — web GUI technical/admin state diagram (PlantUML)
- `hardware/pcb/Output/20260501_Schema.pdf` — updated schematic PDF
- `hardware/pcb/Output/20260501_Bestukkingstekening.pdf` — updated component placement drawing
- `hardware/pcb/Output/20260501_PrintBedrukkingVoorzijde.pdf` — updated silk screen front PDF

### Changed
- `hardware/pcb/` — PCB design bumped to **v1.1.0**; schematic and layout updated following hardware assembly and test
- `documentation/Sensors/W-Sensecap-S200/` — connector photo replaced (`image.jpg` → `Connector.png`)
- `drivers/driverDevelopmentPlan.md` — updated to reflect all drivers completed

### Removed
- `hardware/pcb/Output/20260424_*.pdf` — superseded by 20260501 fabrication outputs

---

## [1.0.0] — 2026-04-24

### Added
- `realisation/installation.md` — new connector wiring guide covering all 12 PCB connectors (J1–J12): 24 V DC input, AC mains input, motor relay outputs M1/M2/M3, RS485 sensor connections (FG6485A and SenseCAP S200), I2C display, 4×4 keypad, alarm output, RS485 termination jumper, and SD card

### Changed
- `design/technicalDesignSpecification.md` — corrections and open issue resolution following PCB alpha release:
  - **§4.5.1** — relay implementation updated from external relay module board to **6 × SRD-05VDC-SL-C relays integrated on PCB**, each driven by a dedicated **2N7000 N-channel MOSFET**; contact rating updated to 10 A / 250 VAC
  - **§4.9** — LED colours corrected to match PCB: HB heartbeat changed from amber to **green**; relay indicator LEDs changed from red to **amber**; circuit diagram and architecture diagram updated accordingly
  - **Issue #1 closed** — RRK-3 motor feedback signal defined: external relay contact closes on alarm state, drives opto-isolated input J10 (OPTO_INPUT); signal definition referenced to RRK-3 interface documentation
  - **Issue #3 closed (out of scope)** — RS485 sensor cable routing to SenseCAP S200 is the installer's responsibility and outside the controller project scope
  - **Issue #4 closed** — enclosure confirmed as Multicomp Pro **MC001110** (222 × 146 × 55 mm, IP67) following PCB layout and 3D clearance check
  - **Issue #5 closed** — relay module selection resolved by discrete relay integration on PCB (see §4.5.1)
  - **Issue #7 closed** — time source confirmed as **DS1307 RTC** with CR2032 backup, fitted on PCB; TR-HW08 satisfied
  - **Issue #9 added (open)** — J5 pins 5–6 carry HEATING_POS / HEATING_NEG nets for the SenseCAP S200 heater supply; feature not yet documented in TDS; decision on voltage, current, and specification deferred
  - **Issue #8/#9 closed — dropped** — J5 heater supply connection (HEATING_POS / HEATING_NEG, pins 5–6) removed from PCB; no firmware support or documentation required; pins 5–6 of J5 left unconnected

### Added (earlier — 2026-04-02)
- `design/functionalRequirementsSpecification.md` — new constraints and requirements:
  - **C11** — all user-configurable setpoints and thresholds (temperature °C, humidity %, wind speed, wind direction degrees, time durations in minutes) are expressed and stored as integers; fractional values are not supported; fractional sensor readings are rounded before comparison
  - **C12** — temperature control is permanently active; humidity control and wind protection are each independently enable/disable configurable by the administrator; both default to enabled and are persisted across power cycles
  - **FR-C11** — temperature-based climate control shall always be active; it cannot be disabled
  - **FR-C12** — administrator can enable or disable humidity-based climate control; when disabled, RH is ignored for window decisions and conflict resolution is suppressed
  - **FR-WS09** — administrator can enable or disable wind protection (speed and direction); when disabled, no wind-safety close commands are issued
  - **FR-WS10** — persistent LCD warning shown whenever wind protection is inactive
  - **FR-WS11** — disabling wind protection is an admin-only action and shall be logged
  - **FR-CF12** — administrator setting to enable/disable humidity control
  - **FR-CF13** — administrator setting to enable/disable wind protection
  - FR-CR01 updated: conflict resolution is only active when humidity control is enabled
- `design/technicalDesignSpecification.md` §5.1 — added "Setpoint and threshold data types" and "Feature enable/disable flags" design constraints with NVS key names (`rh_ctrl_en`, `wind_prot_en`) and default values
- `design/technicalSoftwareDesignSpecification.md`:
  - §3 Design Constraints — added integer setpoint constraint (`int16_t` NVS storage, rounding rule) and feature enable/disable flag constraints
  - §4.3 T3 Safety Monitor — updated to check `wind_prot_en` flag before evaluating thresholds; suppresses CLOSE_ALL when wind protection is disabled
  - §5.2 Climate Control Logic — RH evaluation conditional on `rh_ctrl_en`; conflict resolution suppressed when humidity disabled; CLOSE_ALL from T3 conditional on `wind_prot_en`; log entry `value_a`/`value_b` fields updated to reflect integer values without scaling
  - §5.10 NVS Configuration Storage Layout — added Type column; `rh_ctrl_en` added to `climate` namespace; `wind_prot_en` added to `wind` namespace; types specified for all namespaces
- `firmware/` directory with PlatformIO project skeleton:
  - `firmware/platformio.ini` — board (`lolin_s3`), Arduino framework, 115200 baud monitor, commented library dependency stubs
  - `firmware/src/README.md` — describes expected source modules and their responsibilities
  - `firmware/test/README.md` — describes unit test structure, Unity framework, and `pio test` usage
- `hardware/` directory with KiCad PCB project structure:
  - `hardware/pcb/README.md` — KiCad tool version, expected project files, design references
  - `hardware/fabrication/README.md` — expected fabrication outputs per release, KiCad export instructions
- `README.md` — completely rewritten to describe the Greenhouse Ventilation Controller (replacing placeholder content from an unrelated project)
- `license.md` — dual-licence information for software and non-software artefacts
- `LICENSE` — canonical licence text for the repository

### Changed
- **Licences updated** throughout the repository:
  - Software (firmware and all code): source-available, non-commercial licence — free to use and modify; redistribution and commercial use not permitted
  - Hardware design files, documentation, and images: CC BY-NC-ND 4.0 (Attribution-NonCommercial-NoDerivatives 4.0 International)
- `design/technicalDesignSpecification.md` §2.1 — "Open Source" section replaced with "Project Licences" reflecting the dual-licence structure
- `design/technicalDesignSpecification.md` §2.5 — repository structure diagram expanded to include `documentation/`, `Archive/`, and all root-level files
- `design/technicalDesignSpecification.md` §3.2 — design principle "open and reproducible" updated to align with source-available rather than open-source framing

---

## [0.2.0] — 2026-03-26

*TDS hardware section complete; FRS v0.2 finalised.*

### Added
- `design/functionalRequirementsSpecification.md` v0.2 — complete functional and technical requirements:
  - Sensing (internal climate: FG6485A T/RH; external weather: SenseCAP S200 wind)
  - Window actuation (M1, M2, M3 via Hotraco RRK-3; timed relay pulses; dwell-time enforcement)
  - Automatic climate control (T and RH setpoints, hysteresis, graduated ventilation strategy)
  - Wind safety (speed threshold, direction exclusion angle, immediate close override)
  - Conflict resolution, window state tracking, operating modes
  - Local user interface (4×4 keypad, 16×2 LCD)
  - WiFi connectivity, MQTT integration, access control, event logging
- `design/technicalDesignSpecification.md` v0.2 — hardware design complete:
  - §4.1 Microcontroller: WEMOS LOLIN S3 (ESP32-S3, 16 MB flash, 8 MB PSRAM)
  - §4.2 Sensors: Seeed SenseCAP S200 (ultrasonic wind, Modbus RS485) and FG6485A (T/RH, Modbus RS485)
  - §4.3 Modbus RS485 bus topology and parameters
  - §4.4 User interface: 4×4 membrane keypad and Waveshare LCD1602 I2C (PCF8574)
  - §4.5 Motor controller interface: 6-ch relay board, potential-free contacts, opto-isolated feedback input
  - §4.6 Real-Time Clock: DS1307, I2C, CR2032 battery backup, external 32.768 kHz crystal
  - §4.7 Power supply: two-stage architecture (230 VAC → 24 VDC → 5 VDC), power budget analysis
  - §4.8 SD card (optional, SPI, FAT32)
  - §4.9 Status LEDs: PWR (green), HB heartbeat (amber), 6 × relay activity (red, shared GPIO)
  - §4.10 Enclosure: Multicomp Pro MC001110, 222 × 146 × 55 mm, IP67, transparent cover
  - §4.11 GPIO and peripheral assignment summary

### Changed
- Sensor selection: SenseCAP S200 confirmed as wind sensor (ultrasonic, no moving parts, single mast)

---

## [0.1.1] — 2026-03-07

*Simulation model refined; physical parameters recorded.*

### Changed
- Simulation model simplified to steady-state plant model; ACH parameters merged; humidity and temperature thresholds unified
- Measured physical greenhouse parameters recorded in simulation environment data

---

## [0.1.0] — 2026-03-06

*First complete design iteration committed.*

### Added
- `Archive/Iteration1/design.md` — first iteration design document including:
  - §3.7 Farmer-accessible configuration parameters
  - Partial window opening decision (not supported — recorded in §1.5 and §5)
  - Anti-oscillation guard (`t_min_dwell`) for motor protection
- `Archive/Iteration1/plantTranspirationRateConsiderations.md` — analysis of plant transpiration rate and its effect on humidity control
- `Archive/Iteration1/setpointConsiderations.md` — recommended T and RH setpoints for typical greenhouse crops
- `Archive/Iteration1/stateDiagram.puml` — PlantUML state diagram for the controller operating modes
- `Archive/Iteration1/Simulation/greenhouse_simulation.py` — Python simulation driven by historical weather data (5 scenarios: daytime solar gain, high humidity, 24 h day–night cycle, T below setpoint / RH critical, motor stall)
- `Archive/Iteration1/Environment/airTemperature_2025-05-01_to_2025-09-01.csv` — historical air temperature data used in simulation
- `Archive/Iteration1/Environment/outside_conditions.py` — outside conditions model for simulation
- `documentation/` — component reference material: sensor datasheets and notes (FG6485A, SenseCAP S200, RHS-10, RTS-2, keypad, anemometer)

### Changed
- Greenhouse physical layout documented: 40 × 16 m, east–west orientation; M1 south roof, M2 north roof, M3 north wall
- Window-to-motor mapping and RRK-3 circuit schematic references added to design

---

## [0.0.1] — 2026-03-05

*Project initialised.*

### Added
- Initial repository structure
- `design/technicalDesignSpecification.md` v0.1 — initial hardware architecture and component candidate evaluation
- `code_of_conduct.md`, `contributing.md` — community standards


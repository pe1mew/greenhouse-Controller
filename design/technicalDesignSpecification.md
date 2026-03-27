# Technical Design Specification
## Greenhouse Ventilation Controller

| Field        | Value                                    |
|--------------|------------------------------------------|
| Document     | Technical Design Specification           |
| Project      | Greenhouse Ventilation Controller        |
| Version      | 0.2 (draft)                             |
| Date         | 2026-03-26                              |
| Status       | Draft — Hardware section complete; Software section pending |
| Related docs | `functionalRequirementsSpecification.md` |
|              | `technicalSpecification.md`              |

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture and Development Principles](#2-architecture-and-development-principles)
3. [System Architecture Overview](#3-system-architecture-overview)
4. [Hardware Design](#4-hardware-design)
   - 4.1 Microcontroller — LOLIN S3 (ESP32-S3)
   - 4.2 Sensors
   - 4.3 Modbus RS485 Bus
   - 4.4 User Interface
   - 4.5 Motor Controller Interface
   - 4.6 Real-Time Clock
   - 4.7 Power Supply
   - 4.8 SD Card (Optional)
   - 4.9 Status LEDs
   - 4.10 Enclosure
   - 4.11 GPIO and Peripheral Assignment Summary
5. [Software Design](#5-software-design) *(to be completed)*
6. [Open Issues](#6-open-issues)

---

## 1. Introduction

### 1.1 Purpose
This document describes the technical design of the greenhouse ventilation controller: the selected hardware components, their interconnections, and — once completed — the software architecture. It translates the requirements in the Functional Requirements Specification (FRS) into concrete implementation decisions.

### 1.2 Scope
This version covers the architecture principles (§2) and the complete hardware design (§4). The software design (§5) is to be added in a subsequent revision.

### 1.3 Definitions

| Term | Definition |
|------|------------|
| ESP32-S3 | Espressif dual-core 32-bit microcontroller with integrated WiFi and BLE |
| LOLIN S3 | WEMOS LOLIN S3 development board based on the ESP32-S3 |
| Modbus RTU | Serial communication protocol used over RS485 in this system |
| RS485 | Differential serial physical layer; supports long cable runs and multi-drop |
| I2C | Two-wire serial bus (SDA + SCL) used for the display and RTC |
| RTC | Real-Time Clock; maintains calendar time independently of the MCU |
| MCU | Microcontroller Unit |
| PSU | Power Supply Unit |
| GPIO | General Purpose Input/Output pin |
| DE/RE | Driver Enable / Receiver Enable — direction control pin for RS485 transceiver |
| M1, M2, M3 | Window motor channels as defined in the FRS |
| RRK-3 | Hotraco RRK-3 three-channel window relay box |
| KiCad | Open-source PCB design suite |
| PlatformIO | Open-source embedded development platform and IDE extension |
| VSCode | Visual Studio Code — open-source code editor by Microsoft |
| OTA | Over-The-Air firmware update |

---

## 2. Architecture and Development Principles

This section defines the overarching principles that govern how the project is built, shared, and maintained. These principles apply to both the hardware and software parts of the project.

### 2.1 Project Licences

The project uses separate licences for software and for all other project artefacts (hardware design files, documentation, and images).

| Aspect | Licence |
|--------|---------|
| **Software licence** | Source-available, non-commercial licence. Free to use and modify for personal and non-commercial purposes. Redistribution and commercial use are **not** permitted. |
| **Hardware design licence** | Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International (CC BY-NC-ND 4.0). Permits sharing with attribution for non-commercial purposes; no modifications to the hardware design files are permitted. |
| **Documentation and images** | Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International (CC BY-NC-ND 4.0). Permits sharing with attribution for non-commercial purposes; no derivatives permitted. |
| **Rationale** | The CC BY-NC-ND 4.0 licence protects the integrity of the hardware design and documentation while permitting personal, educational, and non-commercial sharing. The software licence allows inspection and personal adaptation without enabling commercial exploitation or unauthorised redistribution. |

### 2.2 Version Control — GitHub / GitLab

All project artefacts (firmware source, KiCad files, documentation) are managed in a Git repository hosted on **GitHub** (or GitLab as an alternative).

| Practice | Description |
|----------|-------------|
| **Single repository** | Firmware, hardware (KiCad), and documentation are kept in one monorepo for traceability between hardware revisions and firmware versions. |
| **Branch strategy** | `main` holds stable releases; feature work is developed on feature branches and merged via pull requests (GitHub) or merge requests (GitLab). |
| **Releases and tags** | Hardware and firmware releases are tagged (e.g. `hw-v1.0`, `fw-v1.0`) so that the exact design state that was manufactured or deployed is always reproducible. |
| **Issues and discussions** | GitHub Issues (or GitLab Issues) are used to track bugs, open design questions, and the open issues listed in §6 of this document. |
| **Repository structure** | See the top-level README for the folder layout convention. |

### 2.3 Firmware Toolchain — PlatformIO + Visual Studio Code

The firmware is developed using **PlatformIO** as the build system and package manager, with **Visual Studio Code (VSCode)** as the editor.

| Tool | Role | Rationale |
|------|------|-----------|
| **Visual Studio Code** | Source code editor | Free, open-source, cross-platform, widely used; rich extension ecosystem |
| **PlatformIO** | Build system, library manager, upload, serial monitor, unit test runner | Abstracts the ESP-IDF / Arduino toolchain; handles library dependencies via `platformio.ini`; built-in support for ESP32-S3; integrates directly into VSCode as an extension |
| **ESP-IDF / Arduino framework** | Underlying MCU framework | PlatformIO supports both; the choice between them is a software design decision (§5) |
| **Unity / GoogleTest** | Unit test framework | Supported natively by PlatformIO's test runner for on-host unit testing of logic modules |

The `platformio.ini` at the root of the firmware project defines the target board (`lolin_s3`), framework, upload port, and all library dependencies. This ensures every developer uses identical build settings without manual toolchain setup.

### 2.4 PCB Design — KiCad

The PCB schematic and layout are designed in **KiCad**.

| Aspect | Details |
|--------|---------|
| **Tool** | KiCad EDA (version 8 or later) |
| **Licence** | KiCad is free and open-source (GPL); design files are human-readable and version-control friendly |
| **File storage** | All KiCad project files (`.kicad_sch`, `.kicad_pcb`, `.kicad_pro`, symbol and footprint libraries) are stored in the repository under `hardware/pcb/` |
| **Fabrication outputs** | Gerber files, drill files, and BOM are generated from KiCad and stored under `hardware/fabrication/` for each tagged release |
| **3D model** | KiCad's built-in 3D viewer is used to verify component placement and clearance before ordering |
| **Rationale** | KiCad is the de-facto standard for open-source PCB design; its file format is plain text, making diffs and merges in Git practical |

### 2.5 Repository Structure

```
greenhouse-controller/          ← Git repository root (GitHub / GitLab)
│
├── firmware/                   ← PlatformIO project (edit in VSCode)
│   ├── platformio.ini          ← Board, framework, library dependencies
│   ├── src/                    ← Application source code
│   └── test/                   ← Unit tests (PlatformIO test runner)
│
├── hardware/
│   ├── pcb/                    ← KiCad project files (.kicad_sch, .kicad_pcb, ...)
│   └── fabrication/            ← Gerbers, BOM, pick-and-place (generated per release)
│
├── design/                     ← Markdown design documents (FRS, TDS, TS)
│
├── documentation/              ← Component reference material
│   ├── Sensors/                ← Sensor datasheets and integration notes
│   ├── Motors/                 ← Motor and relay box documentation
│   └── VentilationSystem/      ← Ventilation system reference material
│
├── Archive/                    ← Historical design iterations (read-only reference)
│   └── Iteration1/             ← First design iteration: concept, simulation, environment data
│
├── README.md
├── LICENSE
├── license.md
├── changelog.md
├── contributing.md
└── code_of_conduct.md
```

---

## 3. System Architecture Overview

### 3.1 Hardware Block Diagram

```
                        230 VAC Mains
                             │
                         [Fuse]
                             │
                    [AC-DC PSU module]      ── [PWR LED] (green, always on)
                     230VAC → 24VDC
                             │ 24 V
              ┌──────────────┼──────────────────────────────┐
              │              │                              │
   [DC-DC buck converter]    │ 24 V (sensor supply)         │ 24 V (relay switching)
    24V → 5V  │              │                              │
              │ 5 V   [SenseCAP S200] ◄── RS485 + 24V       │
              │       [FG6485A T/RH ] ◄── RS485 + 24V       │
              │                                             │
   ┌──────────┴─────────────────────────────┐               │
   │            LOLIN S3 (ESP32-S3)         │──[HB LED]     │
   │                                        │   (amber)     │
   │RS485 (UART + MAX485 transceiver) ──────┤◄────────── sensors (data)
   │                                        │
   │8 GPIO ◄── [4×4 Keypad]                 │
   │I2C    ──► [LCD1602]   ──► Display      │
   │I2C    ──► [DS3231 RTC]                 │
   │                                        │
   │6 GPIO → [Relay board, 6 ch] ───────────┤── 24V switched ──► RRK-3 OPEN/CLOSE
   │      ↳ [6 relay LEDs] (red, shared)    │
   │                                        │
   │1 GPIO ← [Opto-input]                   │◄── RRK-3 alarm/feedback
   │                  ! see Open Issue #1   │
   │                                        │
   │SPI → [SD card slot]  (optional)        │
   │                                        │
   │ WiFi 2.4 GHz (integrated in ESP32-S3)  │◄──► WLAN / MQTT broker
   │ Native USB (diagnostic / OTA)          │◄──► PC / firmware update
   └────────────────────────────────────────┘
```

### 3.2 Design Principles

- **Minimal external components.** The ESP32-S3 integrates WiFi, BLE, USB OTG, multiple UART/I2C/SPI peripherals, and non-volatile flash. This reduces the PCB bill of materials significantly.
- **Digital interfaces throughout.** Both sensors use Modbus RTU over RS485, eliminating analogue calibration and drift.
- **Electrical isolation at the motor interface.** Relay outputs to the RRK-3 are potential-free (volt-free); the feedback input uses an opto-coupler to protect the MCU from the 24 V RRK-3 circuit.
- **Observable hardware state.** Status LEDs on the PCB make the operating state of the controller visible at a glance without requiring the LCD or a connected device.
- **Optional features additive.** The SD card and MQTT/WiFi features can be omitted without affecting core operation.
- **Open and reproducible.** All design files are open source and stored in version control, as described in §2.

---

## 4. Hardware Design

### 4.1 Microcontroller — LOLIN S3 (ESP32-S3)

The LOLIN S3 is a compact development/prototype board from WEMOS built around the Espressif ESP32-S3 system-on-chip.

#### 4.1.1 Key Specifications

| Parameter | Value |
|-----------|-------|
| Board | WEMOS LOLIN S3 |
| SoC | Espressif ESP32-S3 (revision 0.2 or later) |
| CPU | Dual-core Xtensa LX7, up to 240 MHz |
| Flash | 16 MB (on-board, QSPI) |
| PSRAM | 8 MB (on-board, QSPI Octal) |
| WiFi | 802.11 b/g/n, 2.4 GHz |
| Bluetooth | BLE 5.0 |
| USB | Native USB 2.0 OTG (via USB-C connector) |
| UART | 3 × hardware UART |
| I2C | 2 × hardware I2C (any GPIO) |
| SPI | 4 × SPI (any GPIO) |
| GPIO | Up to 45 usable GPIO |
| Operating voltage | 3.3 V (on-board LDO from 5 V supply) |
| Form factor | 25.4 × 65.3 mm |
| Programming | USB (native), OTA over WiFi |

#### 4.1.2 Non-Volatile Storage

The ESP32-S3 flash is partitioned using the ESP-IDF NVS (Non-Volatile Storage) library. Configuration settings (setpoints, thresholds, dwell times, credentials) are stored in the NVS partition. The event log ring buffer is stored in a dedicated log partition. The SD card, when present, provides supplemental log storage.

#### 4.1.3 Rationale for Selection

| Criterion | Justification |
|-----------|--------------|
| Processing power | Dual LX7 cores at 240 MHz easily handles all real-time tasks: Modbus polling, relay control, display updates, WiFi stack, and MQTT client simultaneously |
| Integrated WiFi | No separate WiFi module required (satisfies TR-HW09) |
| Native USB | Provides built-in serial console and firmware update path without additional USB-UART chip (satisfies TR-IF05) |
| Large flash | 16 MB accommodates firmware, NVS, web server files, and a substantial on-chip log partition |
| Ecosystem | Mature Arduino / ESP-IDF toolchain via PlatformIO; extensive library support for Modbus, I2C, MQTT, and web server |
| Availability | Widely available; approximate cost €10–15 |

---

### 4.2 Sensors

#### 4.2.1 Wind Speed and Direction — Seeed SenseCAP S200

| Parameter | Value |
|-----------|-------|
| Manufacturer / Model | Seeed Studio SenseCAP S200 |
| Measured quantities | Wind speed (m/s) and wind direction (°) |
| Technology | Ultrasonic — no mechanical moving parts |
| Interface | Modbus RTU over RS485 |
| Supply voltage | 5–30 VDC |
| Wind speed range | 0–60 m/s |
| Wind direction range | 0°–360° |
| Approximate price (NL) | €350 |
| Mounting | Single mast pole (integrated unit for both speed and direction) |

**Rationale:**
- Combines wind speed and direction in one unit on a single mast; reduces installation complexity.
- Ultrasonic measurement has no moving parts, making it more robust and maintenance-free than cup anemometers or vane sensors.
- Modbus RS485 digital interface avoids analogue signal conditioning, calibration drift, and noise susceptibility over long cable runs.
- Single digital cable pair serves both measured quantities.

#### 4.2.2 Temperature and Humidity — FG6485A

| Parameter | Value |
|-----------|-------|
| Model | FG6485A |
| Measured quantities | Air temperature (°C) and relative humidity (%) |
| Interface | Modbus RTU over RS485 |
| Supply voltage | 9–36 VDC (internal 24 V rail is within spec) |
| Current consumption | ≤ 15 mA (datasheet) |
| Approximate price (NL) | €61 |
| Installation | Inside the greenhouse |

**Rationale:**
- RS485/Modbus interface is more robust than analogue signals (0–3 V voltage output or PTC resistor) over the cable lengths involved in a greenhouse installation.
- Digital interface requires no analogue-to-digital calibration at the controller side.
- Priced competitively for the target application.

---

### 4.3 Modbus RS485 Bus

Both sensors communicate over a single shared Modbus RTU RS485 bus. Each sensor is assigned a unique Modbus device address, allowing the controller to address them independently on the same physical two-wire cable pair.

#### 4.3.1 Bus Topology

```
  LOLIN S3 UART ──► MAX485 transceiver ──┬── RS485 cable (A/B + GND shield)
                    (TTL ↔ RS485)        │
                                         ├── SenseCAP S200  (address 1)  [outside, on mast]
                                         └── FG6485A T/RH   (address 2)  [inside greenhouse]
```

#### 4.3.2 RS485 Transceiver

An external RS485 transceiver IC (e.g. MAX485, SP3485, or equivalent) converts the ESP32-S3 UART signals (3.3 V TTL) to the differential RS485 bus levels.

| Signal | Direction | Description |
|--------|-----------|-------------|
| UART TX | MCU → transceiver | Serial data to transmit |
| UART RX | transceiver → MCU | Received serial data |
| DE/RE | MCU → transceiver | Direction control (HIGH = transmit, LOW = receive) |

The DE and RE pins of the transceiver are tied together and driven by a single GPIO on the ESP32-S3.

#### 4.3.3 Bus Parameters

| Parameter | Value |
|-----------|-------|
| Protocol | Modbus RTU |
| Baud rate | 9600 baud (configurable; sensor default) |
| Data format | 8N1 (8 data bits, no parity, 1 stop bit) |
| Cable type | Twisted pair with shield (e.g. Belden 3105A or equivalent) |
| Maximum cable length | 1200 m (RS485 specification) |
| Termination | 120 Ω termination resistor at the far end of the bus |
| Number of devices | 2 (SenseCAP S200 + FG6485A) |

---

### 4.4 User Interface

#### 4.4.1 Keypad — 4×4 Membrane Matrix Keypad

| Parameter | Value |
|-----------|-------|
| Type | 4×4 membrane matrix keypad |
| Number of keys | 16 |
| Connector | 8-wire flat cable (4 row lines + 4 column lines) |
| Interface to MCU | 8 GPIO pins (direct matrix scan) |
| Approximate price | €2 |

**Operating principle:** The ESP32-S3 firmware drives the four row lines sequentially (one active at a time) and reads the four column lines to detect which key, if any, is pressed. This multiplexing technique requires no additional hardware. Internal pull-up resistors on the column GPIO pins prevent floating inputs.

#### 4.4.2 LCD Display — Waveshare LCD1602 I2C Module

| Parameter | Value |
|-----------|-------|
| Model | Waveshare LCD1602 I2C Module |
| Display type | 16 characters × 2 lines, character LCD |
| Backlight | LED backlight (controllable) |
| Interface | I2C (PCF8574 I/O expander on the module) |
| I2C address | 0x27 (default; A0–A2 solder jumpers configurable) |
| Supply voltage | 5 V (backlight) / 3.3–5 V (logic) |
| Interface to MCU | 2 GPIO pins (SDA + SCL) shared with RTC |

The LCD module integrates a PCF8574 I/O expander that converts I2C commands to the 4-bit parallel interface of the HD44780-compatible LCD controller. This reduces the wiring from the original 6–10 GPIO pins to just 2 (shared I2C bus).

**Mounting:** The LCD module is installed **inside the enclosure**, elevated above the main PCB on four supporting standoff screws. This keeps the display level, at a consistent height, and at a comfortable viewing angle through the transparent enclosure cover. No cutout in the cover is required for the display.

---

### 4.5 Motor Controller Interface

The controller interfaces to the Hotraco RRK-3 via six relay outputs (OPEN and CLOSE for each of M1, M2, M3) and one optically isolated digital input for feedback from the RRK-3.

#### 4.5.1 Relay Outputs (Motor Commands)

| Parameter | Value |
|-----------|-------|
| Number of relay channels | 6 (OPEN M1, CLOSE M1, OPEN M2, CLOSE M2, OPEN M3, CLOSE M3) |
| Contact type | Potential-free (volt-free) normally-open contacts |
| Contact rating | ≥ 0.5 A / 24 V (to match RRK-3 control input rating) |
| Coil drive | 5 VDC coil, driven by ESP32-S3 GPIO via transistor or relay driver IC |
| Connection | Screw terminals (OPEN, CLOSE, COMMON per channel) |

A relay module board with 6 independent relay channels (opto-isolated input stage, active-low trigger) is used. The relay contacts switch the internal **24 V DC** supply to the RRK-3 OPEN/CLOSE sturing terminals. The wiring is:

```
  Internal 24V+ ──► Relay COM
                    Relay NO ──► RRK-3 OPEN sturing  (or CLOSE sturing)
  Internal 24V− ──────────────► RRK-3 COMM sturing
```

The relay coil is driven by the 5 V logic supply via the MCU GPIO. The relay provides galvanic isolation between the MCU logic circuit (5 V) and the RRK-3 control circuit (24 V), satisfying the isolation requirement. The contact rating must be ≥ 0.5 A / 30 VDC.

> **Safety constraint:** The firmware must never energise the OPEN and CLOSE relay of the same motor simultaneously. This is enforced in software (see Software Design §5). A future hardware interlock using the relay common terminals could provide an additional layer of protection.

#### 4.5.2 Feedback Input (Motor Controller Status)

| Parameter | Value |
|-----------|-------|
| Number of inputs | 1 |
| Input type | Optically isolated digital input |
| Source signal | RRK-3 alarm relay output (24 V) |
| Interface to MCU | 1 GPIO pin (opto-coupler output) |
| Connection | Screw terminal |

> **⚠ OPEN ISSUE #1 — Motor feedback signal definition:** The exact nature of the RRK-3 feedback signal to be connected to this input (e.g. alarm relay contact, fault indication) is currently undefined. The hardware provision (screw terminal + opto-input) is included in the design. The software behaviour triggered by this input is to be defined once the RRK-3 alarm relay wiring has been confirmed. See also FRS Constraint C8.

#### 4.5.3 Screw Terminals

All connections to the RRK-3 relay box are brought out to screw terminal blocks on the controller PCB or DIN rail terminal strip. Terminal blocks shall be rated for at least 300 V / 6 A and clearly labelled with signal name and window channel (M1, M2, M3).

| Terminal group | Signals |
|----------------|---------|
| M1 — relay output | OPEN contact (NO + COM), CLOSE contact (NO + COM) |
| M2 — relay output | OPEN contact (NO + COM), CLOSE contact (NO + COM) |
| M3 — relay output | OPEN contact (NO + COM), CLOSE contact (NO + COM) |
| Feedback input | IN+, IN− (opto-coupler, 24 V) |

---

### 4.6 Real-Time Clock — DS3231

An external battery-backed RTC is required to provide accurate, persistent timestamps for the event log (FRS TR-HW08, Must).

| Parameter | Value |
|-----------|-------|
| IC | DS3231 (Maxim / Analog Devices) |
| Interface | I2C (address 0x68; shared I2C bus with LCD) |
| Accuracy | ±2 ppm (±1 min/year) — temperature-compensated crystal |
| Backup battery | CR2032 coin cell (maintains time during power loss) |
| Supply voltage | 2.3–5.5 V |
| Provides | Date, time (seconds resolution), temperature (bonus) |

The DS3231 is available as a small breakout module for approximately €1–3. It shares the I2C bus with the LCD display; the different I2C addresses (LCD: 0x27, RTC: 0x68) prevent conflicts.

**Rationale for external RTC:** The ESP32-S3 has an internal RTC counter but it is not battery-backed and loses time on every power cycle. An NTP time sync (WiFi) can compensate when connected, but the system must also keep accurate time when WiFi is unavailable or before a network connection is established.

---

### 4.7 Power Supply

The system uses a **two-stage power architecture**: a single AC–DC converter produces 24 VDC, which is distributed directly to the sensors and relay switching circuit. A DC–DC buck converter steps the 24 V down to 5 V for the MCU, relay coils, display, and logic circuits.

#### 4.7.1 Power Architecture Overview

```
 230 VAC ──[Fuse]──► [AC-DC PSU]──► 24 VDC ──┬──► Sensors (SenseCAP S200, FG6485A)
                                             ├──► Relay contacts → RRK-3 sturing (24 V)
                                             └──► [DC-DC Buck 24V→5V]
                                                       │
                                                  5 VDC ──► LOLIN S3 (→ 3.3 V LDO)
                                                       ├──► Relay coils (6-ch module)
                                                       ├──► LCD backlight
                                                       ├──► RS485 transceiver
                                                       ├──► DS3231 RTC
                                                       └──► Status LEDs
```

#### 4.7.2 Mains Input and Fuse

| Parameter | Value |
|-----------|-------|
| Mains input | 230 VAC / 50 Hz |
| Connection | Screw terminals (L, N, PE) inside enclosure |
| Fuse | 500 mA slow-blow (T0.5A), panel-mount fuse holder |
| Fuse purpose | Protects the AC–DC PSU and mains wiring against overload and short circuit |

Mains wiring must comply with local electrical installation regulations. A cable gland rated IP67 or better is used for the mains cable entry into the enclosure.

#### 4.7.3 AC–DC Power Supply Module (230 VAC → 24 VDC)

| Parameter | Value |
|-----------|-------|
| Module type | Enclosed PCB-mount AC–DC converter (e.g. Hi-Link HLK-10M24 or equivalent) |
| Input | 85–264 VAC / 50–60 Hz |
| Output | 24 VDC, 420 mA (10 W) |
| Efficiency | ≥ 80% |
| Isolation | Reinforced (input–output) |
| Approvals | CE, UL |
| Approximate price | €8–12 |

The 24 V rail feeds the sensors directly and the relay switching circuit. It also feeds the DC–DC buck converter.

#### 4.7.4 DC–DC Buck Converter (24 VDC → 5 VDC)

| Parameter | Value |
|-----------|-------|
| Module type | PCB-mount isolated or non-isolated DC–DC buck converter (e.g. Recom TSR-1-2450, Hi-Link HLK-1D2405, or equivalent) |
| Input | 24 VDC |
| Output | 5 VDC, 1 A |
| Efficiency | ≥ 85% |
| Approximate price | €3–6 |

The 5 V output feeds:
- The LOLIN S3 board (5 V in; on-board LDO produces 3.3 V for the MCU core)
- The 6-channel relay module (5 V coil supply)
- The LCD module backlight
- The RS485 transceiver (5 V supply; 3.3 V logic from the LOLIN S3 LDO)
- The DS3231 RTC module
- The status LEDs (via current-limiting resistors; see §4.9)

#### 4.7.5 Power Budget

**24 V domain:**

| Consumer | Current at 24 V |
|----------|----------------|
| SenseCAP S200 (via RS485 cable) | ~50 mA |
| FG6485A T/RH (via RS485 cable) | ≤ 15 mA (datasheet) |
| RRK-3 sturing inputs (via relay contacts, max 2 active) | ~10 mA |
| DC–DC buck converter input (for 5 V / 700 mA load) | ~175 mA (5 V × 700 mA ÷ 24 V ÷ 0.85) |
| **Total 24 V (worst case)** | **~250 mA** |

> The HLK-10M24 is rated at 420 mA. Load is approximately 250 mA, giving ~40% headroom.

**5 V domain:**

| Consumer | Current at 5 V |
|----------|---------------|
| LOLIN S3 (ESP32-S3, WiFi active) | ~250 mA |
| 6-channel relay module (all 6 relays energised) | ~360 mA (60 mA × 6) |
| LCD module (backlight on) | ~40 mA |
| RS485 transceiver | ~10 mA |
| DS3231 RTC | ~2 mA |
| Status LEDs (PWR + HB + 6 relay, all on) | ~16 mA (2 mA × 8) |
| **Total 5 V (worst case)** | **~678 mA** |

> The DC–DC buck converter is rated at 1000 mA. Load is approximately 678 mA, giving ~32% headroom.

> **Note:** All 6 relays being simultaneously energised is a worst case that does not occur in normal operation (at most 3 relays active: one per motor channel, either OPEN or CLOSE). Typical 5 V current is therefore significantly lower.

---

### 4.8 SD Card (Optional)

| Parameter | Value |
|-----------|-------|
| Interface | SPI (MOSI, MISO, CLK, CS) |
| Format | FAT32 |
| Purpose | Extended event log retention; offline retrieval |
| MoSCoW | Could |
| Notes | SD card slot to be included on PCB; populated only if the feature is required |

The ESP32-S3 SPI peripheral supports SD cards in SPI mode without additional hardware. The firmware uses SD card presence detection to determine whether to write logs to the card or fall back to internal NVS flash (FRS FR-LG08).

---

### 4.9 Status LEDs

Status LEDs on the PCB provide instant visual feedback on the operating state of the controller without needing a connected device. All LEDs are mounted directly on the PCB. Because the enclosure has a **transparent cover**, the LEDs are fully visible to the operator without any holes, light pipes, or panel-mount LED holders in the cover. This simplifies the mechanical design and preserves the IP67 rating of the sealed cover.

#### 4.9.1 LED Overview

| LED label | Colour | Quantity | Drive source | Extra GPIO |
|-----------|--------|----------|-------------|------------|
| PWR | Green | 1 | 5 V rail via resistor (hardware) | None |
| HB (Heartbeat) | Amber | 1 | Dedicated MCU GPIO | 1 |
| M1-OPEN | Red | 1 | Shared with relay M1-OPEN GPIO driver | None |
| M1-CLOSE | Red | 1 | Shared with relay M1-CLOSE GPIO driver | None |
| M2-OPEN | Red | 1 | Shared with relay M2-OPEN GPIO driver | None |
| M2-CLOSE | Red | 1 | Shared with relay M2-CLOSE GPIO driver | None |
| M3-OPEN | Red | 1 | Shared with relay M3-OPEN GPIO driver | None |
| M3-CLOSE | Red | 1 | Shared with relay M3-CLOSE GPIO driver | None |
| **Total** | | **8** | | **1 additional GPIO** |

#### 4.9.2 LED Descriptions

**PWR — Power indicator (green)**
- Driven directly from the 5 V PSU output via a series current-limiting resistor (~1 kΩ, ~5 mA).
- Lit whenever mains power is present and the PSU is operational.
- No MCU involvement; remains lit even if the MCU is in reset or has faulted.
- Provides immediate confirmation that the unit is powered before any other diagnostic step.

**HB — Heartbeat (amber)**
- Driven by one dedicated MCU GPIO output via a series resistor (~1.5 kΩ for ~2 mA at 3.3 V).
- The firmware toggles this LED to indicate software state:

| Blink pattern | Meaning |
|---------------|---------|
| 1 Hz steady blink (500 ms on / 500 ms off) | Normal operation (Automatic or Standby mode) |
| Fast blink (4 Hz) | Startup / initialisation (windows closing to home position) |
| Steady ON | Firmware has stopped — watchdog has not yet fired; indicates a software hang |
| Steady OFF | MCU not running (power fault or crash before LED initialisation) |

**Relay LEDs — M1-OPEN, M1-CLOSE, M2-OPEN, M2-CLOSE, M3-OPEN, M3-CLOSE (red)**
- Each LED is connected in parallel with the corresponding relay coil drive transistor output, via its own current-limiting resistor.
- The LED illuminates whenever the corresponding relay is energised, directly mirroring the relay state in real time.
- No additional GPIO is required; the relay-drive GPIO signals are shared.
- Allows an installer or technician to verify which relay commands are active without opening the enclosure or connecting a diagnostics tool.

#### 4.9.3 Relay LED Circuit (per channel)

```
  MCU GPIO ──► [relay driver transistor] ──► relay coil ──► GND
                        │
                   (collector)
                        ├──[R_LED ~470 Ω]──► [LED red] ──► GND
```

The LED and series resistor are placed from the transistor collector to ground, so the LED lights when the transistor conducts (relay energised). The resistor is sized for approximately 2 mA LED current when the relay coil is active.

#### 4.9.4 Layout (indicative)

The enclosure cover carries **only the membrane keypad**. Everything else — LEDs, LCD, PCB — is inside the housing and visible through the transparent cover.

**Cover (external — only item mounted here):**
```
┌─────────────────────────────────┐
│                                 │
│                                 │
│                                 │
│  [ 1 ][ 2 ][ 3 ][ A ]           │
│  [ 4 ][ 5 ][ 6 ][ B ]           │  ← 4×4 membrane keypad
│  [ 7 ][ 8 ][ 9 ][ C ]           │     (bonded to cover)
│  [ * ][ 0 ][ # ][ D ]           │
│                                 │
└─────────────────────────────────┘
```

**Visible through transparent cover (internal PCB components):**
```
┌─────────────────────────────────┐
│  [PWR]  [HB]                    │  ← Power / Heartbeat LEDs (on PCB)
│                                 │
│  M1: [OPEN] [CLOSE]             │  ← Relay status LEDs (on PCB)
│  M2: [OPEN] [CLOSE]             │
│  M3: [OPEN] [CLOSE]             │
│                                 │
│  [   LCD 16×2 display   ]       │  ← LCD on standoff screws above PCB
│  [   LCD 16×2 display   ]       │
│                                 │
│  (keypad flat cable enters here)│
└─────────────────────────────────┘
```

---

### 4.10 Enclosure

#### 4.10.1 Selected Housing

| Parameter | Value |
|-----------|-------|
| Manufacturer | Multicomp Pro |
| **Preferred model** | **MC001110** — 222 × 146 × 55 mm |
| Alternative model | MC001111 — 222 × 146 × 75 mm (20 mm deeper; use if component height requires it) |
| Series | Sealed Polycarbonate and ABS Enclosures with Mounting Flange |
| Housing body | Grey ABS |
| Cover | Transparent (clear) polycarbonate |
| Mounting | Wall-mount via integral flanges |
| IP rating | IP67 |
| Price (ex VAT) | MC001110: €22 / MC001111: €24 (Farnell) |
| Supplier | Farnell |

The MC001110 (55 mm depth) is the preferred choice. The MC001111 (75 mm depth, +€2) is the fallback if the internal component stack — PCB, relay module, PSU module, LCD on standoffs — exceeds the available depth. Final selection to be confirmed after PCB layout and 3D clearance check.

#### 4.10.2 Enclosure Properties

| Parameter | Value |
|-----------|-------|
| Internal dimensions (L × W × D) | To be verified from Multicomp Pro datasheet for MC001110 / MC001111 |
| Cutouts in cover | 1 × membrane keypad area (4×4 key matrix, bonded to cover) |
| Cable entries (housing body) | IP67-rated cable glands: mains cable, RS485 sensor cable, RRK-3 control cables |
| Fuse access | Panel-mount fuse holder in housing body, accessible without opening cover |
| Internal mounting | Main PCB (LOLIN S3, relay module, RS485 transceiver, screw terminals), PSU module, RTC module, LCD module on standoff screws |

**Transparent cover — key design consequence:**
The transparent cover eliminates the need for any openings, LED holders, or light pipes for the status indicators. The LCD display and all PCB-mounted LEDs are directly visible through the cover from outside without any cutouts other than the keypad opening. This:
- Preserves the IP67 rating of the cover (only one sealed opening for the keypad cable).
- Simplifies mechanical design — no individual LED holes to drill, seal, or align.
- Removes the need for a separate LCD window cutout and its sealing gasket.
- Allows full visibility of the PCB internals, which aids troubleshooting.

**LCD mounting:**
The LCD module is elevated on four M3 brass standoff screws above the PCB surface so that its display face is parallel to and close to the inner face of the transparent cover. The standoff height is chosen to position the display at the most readable viewing angle and distance through the cover thickness.

**Keypad connection:**
The membrane keypad is bonded to the outer face of the transparent cover. Its flat cable passes through the single cover opening, which is sealed with an IP67-rated sealed cable entry or a custom-moulded grommet to maintain the enclosure rating.

---

### 4.11 GPIO and Peripheral Assignment Summary

The table below lists all allocated functions on the ESP32-S3. Specific GPIO numbers are indicative; final assignment is confirmed during PCB layout to avoid conflicts with reserved pins (USB D+/D− on GPIO 19/20, boot strapping pins on GPIO 0/45/46).

| Function | Interface | ESP32-S3 peripheral | GPIO count |
|----------|-----------|---------------------|-----------|
| RS485 Modbus TX | UART | UART1 TX | 1 |
| RS485 Modbus RX | UART | UART1 RX | 1 |
| RS485 direction (DE/RE) | GPIO output | — | 1 |
| LCD display (SDA) | I2C | I2C0 SDA | 1 |
| LCD display (SCL) | I2C | I2C0 SCL | 1 |
| RTC DS3231 (SDA) | I2C | I2C0 SDA (shared) | — |
| RTC DS3231 (SCL) | I2C | I2C0 SCL (shared) | — |
| Keypad rows (4) | GPIO output | — | 4 |
| Keypad columns (4) | GPIO input (pull-up) | — | 4 |
| Relay OPEN M1 | GPIO output | — | 1 |
| Relay CLOSE M1 | GPIO output | — | 1 |
| Relay OPEN M2 | GPIO output | — | 1 |
| Relay CLOSE M2 | GPIO output | — | 1 |
| Relay OPEN M3 | GPIO output | — | 1 |
| Relay CLOSE M3 | GPIO output | — | 1 |
| RRK-3 feedback input | GPIO input | — | 1 |
| Heartbeat LED (HB) | GPIO output | — | 1 |
| SD card MOSI *(optional)* | SPI | SPI2 MOSI | 1 |
| SD card MISO *(optional)* | SPI | SPI2 MISO | 1 |
| SD card CLK *(optional)* | SPI | SPI2 CLK | 1 |
| SD card CS *(optional)* | SPI | SPI2 CS | 1 |
| WiFi | Internal | Radio (no GPIO) | — |
| USB (diagnostic / OTA) | Native USB | GPIO 19/20 (reserved) | — |
| **Total (mandatory)** | | | **21** |
| **Total (with optional SD)** | | | **25** |

> The PWR LED requires no GPIO (resistor from 5 V rail). The 6 relay LEDs share the existing relay-drive GPIO lines. Only the HB heartbeat LED adds 1 GPIO to the mandatory count.

The ESP32-S3 has up to 45 usable GPIO pins; the design uses at most 25, leaving substantial margin for future expansion.

---

## 5. Software Design

> **Status: To be completed.**

The software design section will cover:
- Firmware architecture and task structure (FreeRTOS tasks)
- Modbus RTU driver and sensor polling
- Control logic state machine (Automatic / Standby / Wind-override)
- Window state machine per channel (CLOSED / MOVING / OPEN + dwell timer)
- Conflict resolution algorithm
- Event log manager (NVS ring buffer + SD card)
- Local UI manager (keypad scan, LCD rendering, menu FSM)
- WiFi / web server / MQTT client
- OTA firmware update
- NVS configuration storage layout
- Watchdog and fault handling

---

## 6. Open Issues

| # | Issue | Owner | Status |
|---|-------|-------|--------|
| 1 | **Motor feedback signal** — It is unknown what signal from the RRK-3 is available for the feedback input and what its electrical characteristics are (voltage level, NO/NC, fault vs. position indication). Hardware provision (opto-isolated input + screw terminal) is in place; software response is undefined pending resolution. | Electrical engineer | Open |
| 2 | **Sensor supply voltage** — ~~Resolved~~. Internal 24 VDC rail confirmed compatible with both sensors: SenseCAP S200 rated 5–30 VDC; FG6485A rated 9–36 VDC, ≤ 15 mA (datasheet confirmed). | Hardware designer | **Closed** |
| 3 | **RS485 sensor cable routing** — Physical routing from the controller enclosure (inside the greenhouse) to the SenseCAP S200 (outside, on mast) needs to be designed, including weather-proof cable glands and UV-resistant cable selection. | Installer | Open |
| 4 | **Enclosure model selection** — Candidate housings identified: Multicomp Pro MC001110 (222 × 146 × 55 mm, €22 ex, Farnell) preferred; MC001111 (222 × 146 × 75 mm, €24 ex) as fallback if depth is insufficient. Final choice to be confirmed after PCB layout and 3D component-height clearance check. See §4.10. | Hardware designer | Pending PCB layout |
| 5 | **Relay module selection** — The specific 6-channel relay module (opto-isolated, 5 V coil, potential-free contacts ≥ 0.5 A / 24 V) must be selected and its PCB footprint confirmed. | Hardware designer | Open |
| 6 | **LED panel integration** — ~~Resolved~~. LEDs are on the PCB and visible through the transparent enclosure cover. No panel-mount LED holders or light pipes are required. | Hardware designer | **Closed** |

---

*End of document — version 0.2 draft*

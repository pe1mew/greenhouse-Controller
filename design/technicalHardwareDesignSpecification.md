# Technical Hardware Design Specification
## Greenhouse Ventilation Controller

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Document     | Technical Hardware Design Specification        |
| Project      | Greenhouse Ventilation Controller              |
| Version      | 0.3 (draft)                                   |
| Date         | 2026-03-29                                    |
| Status       | Draft                                         |
| Related docs | `functionalRequirementsSpecification.md`       |
|              | `technicalSoftwareDesignSpecification.md`      |

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
   - 4.6 Time Source
   - 4.7 Power Supply
   - 4.8 SD Card (Optional)
   - 4.9 Status LEDs
   - 4.10 Enclosure
   - 4.11 GPIO and Peripheral Assignment Summary
     - 4.11.1 Function Count Overview
     - 4.11.2 Pin Assignment — LOLIN S3
5. [Open Issues](#5-open-issues)

---

## 1. Introduction

### 1.1 Purpose
This document describes the hardware design of the greenhouse ventilation controller: the selected components, their interconnections, and the rationale for each design decision. It translates the hardware-related requirements in the Functional Requirements Specification (FRS) into concrete implementation choices.

### 1.2 Scope
This document covers the architecture principles (§2) and the complete hardware design (§4). The software design is documented separately in `technicalSoftwareDesignSpecification.md`.

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
| SIT65HVD08P | Selected RS485 half-duplex transceiver IC; 3.3 V supply, 3.3 V logic levels, 8-pin package |

---

## 2. Architecture and Development Principles

This section defines the overarching principles that govern how the project is built, shared, and maintained. These principles apply to both hardware and software parts of the project.

### 2.1 Project Licences

The hardware design files, documentation, and images are covered by a Creative Commons licence. The software licence is documented separately in `technicalSoftwareDesignSpecification.md` §2.1.

| Aspect | Licence |
|--------|---------|
| **Hardware design licence** | Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International (CC BY-NC-ND 4.0). Permits sharing with attribution for non-commercial purposes; no modifications to the hardware design files are permitted. |
| **Documentation and images** | Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International (CC BY-NC-ND 4.0). Permits sharing with attribution for non-commercial purposes; no derivatives permitted. |
| **Rationale** | The CC BY-NC-ND 4.0 licence protects the integrity of the hardware design and documentation while permitting personal, educational, and non-commercial sharing. |

### 2.2 Version Control — GitHub / GitLab

All project artefacts (firmware source, KiCad files, documentation) are managed in a Git repository hosted on **GitHub** (or GitLab as an alternative).

| Practice | Description |
|----------|-------------|
| **Single repository** | Firmware, hardware (KiCad), and documentation are kept in one monorepo for traceability between hardware revisions and firmware versions. |
| **Branch strategy** | `main` holds stable releases; feature work is developed on feature branches and merged via pull requests (GitHub) or merge requests (GitLab). |
| **Releases and tags** | Hardware and firmware releases are tagged (e.g. `hw-v1.0`, `fw-v1.0`) so that the exact design state that was manufactured or deployed is always reproducible. |
| **Issues and discussions** | GitHub Issues (or GitLab Issues) are used to track bugs, open design questions, and the open issues listed in §5 of this document. |
| **Repository structure** | See the top-level README for the folder layout convention. |

### 2.3 Firmware Toolchain — PlatformIO + Visual Studio Code

The firmware is developed using **PlatformIO** as the build system and package manager, with **Visual Studio Code (VSCode)** as the editor.

| Tool | Role | Rationale |
|------|------|-----------|
| **Visual Studio Code** | Source code editor | Free, open-source, cross-platform, widely used; rich extension ecosystem |
| **PlatformIO** | Build system, library manager, upload, serial monitor, unit test runner | Abstracts the ESP-IDF / Arduino toolchain; handles library dependencies via `platformio.ini`; built-in support for ESP32-S3; integrates directly into VSCode as an extension |
| **ESP-IDF / Arduino framework** | Underlying MCU framework | PlatformIO supports both; the choice is documented in `technicalSoftwareDesignSpecification.md` |
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

The full repository folder layout is documented in `technicalSoftwareDesignSpecification.md` §2.4.

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
   │RS485 (UART + SIT65HVD08P transceiver) ─┤◄────────── sensors (data)
   │                                        │
   │8 GPIO ◄── [4×4 Keypad]                 │
   │I2C    ──► [LCD1602]   ──► Display      │
   │I2C    ──► [DS1307 RTC]                 │
   │                                        │
   │6 GPIO → [6 × SRD-05VDC relay + 2N7000] ─┤── 24V switched ──► RRK-3 OPEN/CLOSE
   │      ↳ [6 relay LEDs] (amber, shared)  │
   │                                        │
   │1 GPIO ← [Opto-input]                   │◄── RRK-3 alarm relay contact
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

The 16 MB QSPI flash uses a custom partition table (`firmware/partitions.csv`) with six partitions: an NVS partition (84 KB) holding all configuration namespaces and the event-log ring buffer, an OTA metadata partition (8 KB), two firmware image banks of 2 MB each (`app0`/`app1`), and two LittleFS partitions of 1 MB each (`lfs0`/`lfs1`) storing web assets paired with their respective firmware bank. The SD card, when present, provides supplemental log storage.

#### 4.1.3 Rationale for Selection

| Criterion | Justification |
|-----------|--------------|
| Processing power | Dual LX7 cores at 240 MHz easily handles all real-time tasks: Modbus polling, relay control, display updates, WiFi stack, and MQTT client simultaneously |
| Integrated WiFi | No separate WiFi module required (satisfies TR-HW09) |
| Native USB | Provides built-in serial console and firmware update path without additional USB-UART chip (satisfies TR-IF05) |
| Large flash | 16 MB accommodates two 2 MB firmware banks, two 1 MB LittleFS web-asset partitions, and an 84 KB NVS partition; ~9.9 MB remains unused for future expansion |
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
  LOLIN S3 UART ──► SIT65HVD08P ─────────┬── RS485 cable (A/B + GND shield)
                    (TTL ↔ RS485)        │
                                         ├── SenseCAP S200  (address 1)  [outside, on mast]
                                         └── FG6485A T/RH   (address 2)  [inside greenhouse]
```

#### 4.3.2 RS485 Transceiver

The **SIT65HVD08P** is the selected RS485 half-duplex transceiver. It converts the ESP32-S3 UART signals (3.3 V TTL) to the differential RS485 bus levels and operates from the 3.3 V rail supplied by the LOLIN S3 on-board LDO, making it directly compatible with the ESP32-S3 logic levels without level shifting.

| Parameter | Value |
|-----------|-------|
| Part | SIT65HVD08P |
| Supply voltage | 3.3 V |
| Logic levels | 3.3 V (directly compatible with ESP32-S3 GPIO) |
| Mode | Half-duplex |
| Package | 8-pin |

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
| Termination | 120 Ω resistor R21, switchable via jumper J8 (see §4.3.4) |
| Number of devices | 2 (SenseCAP S200 + FG6485A) |

#### 4.3.4 RS485 Line Protection and Termination

**Line protection — SM712 TVS diode array (D13)**

A SM712 dual-rail TVS diode array is fitted on the RS485 A and B lines to protect the SIT65HVD08P transceiver against ESD and cable transients. The SM712 clamps differential and common-mode overvoltage events before they reach the transceiver.

**Bus bias resistors (R19, R20 — 20 kΩ)**

Two 20 kΩ resistors bias the RS485 A and B lines to defined idle-state voltage levels, ensuring the bus remains in a valid idle state when no device is actively driving it.

**Termination jumper (J8 — Load enable)**

A 2-pin jumper J8 connects in series with the 120 Ω termination resistor R21. This makes termination field-configurable without modifying the PCB.

| J8 state | Effect |
|----------|--------|
| Jumper fitted | 120 Ω termination enabled — use when this PCB is the **last device** on the RS485 bus |
| Jumper removed | Termination disabled — use when this PCB is **mid-bus** with further devices downstream |

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
| Interface | I2C (AiP31068L I2C-to-parallel bridge on the Waveshare module) |
| I2C address | 0x3E (fixed; AiP31068L — confirmed in LIB-4 driver) |
| Supply voltage | 5 V (backlight) / 3.3–5 V (logic) |
| Interface to MCU | 2 GPIO pins (SDA + SCL) shared with RTC |

The LCD module integrates an **AiP31068L** I2C-to-parallel bridge that converts I2C commands to the 4-bit parallel interface of the HD44780-compatible LCD controller. This reduces the wiring from the original 6–10 GPIO pins to just 2 (shared I2C bus at address 0x3E, not conflicting with DS1307 at 0x68).

**Mounting:** The LCD module is installed **inside the enclosure**, elevated above the main PCB on four supporting standoff screws. This keeps the display level, at a consistent height, and at a comfortable viewing angle through the transparent enclosure cover. No cutout in the cover is required for the display.

---

### 4.5 Motor Controller Interface

> **FRS requirements:** Motor control outputs shall be electrically isolated from the controller's internal circuitry (TR-HW03), and shall be compatible with the Hotraco RRK-3 control input specification: 24 VDC, potential-free (volt-free) contact (TR-HW05, TR-IF04). Both requirements are satisfied by the relay design described below.

The controller interfaces to the Hotraco RRK-3 via six relay outputs (OPEN and CLOSE for each of M1, M2, M3) and one optically isolated digital input for feedback from the RRK-3.

#### 4.5.1 Relay Outputs (Motor Commands)

| Parameter | Value |
|-----------|-------|
| Number of relay channels | 6 (OPEN M1, CLOSE M1, OPEN M2, CLOSE M2, OPEN M3, CLOSE M3) |
| Contact type | Potential-free (volt-free) normally-open contacts |
| Contact rating | 10 A / 250 VAC (SRD-05VDC-SL-C rating; well above the ≥ 0.5 A / 24 V required by RRK-3) |
| Coil drive | 5 VDC coil, driven by ESP32-S3 GPIO via dedicated 2N7000 N-channel MOSFET per channel |
| Connection | Screw terminals (OPEN, CLOSE, COMMON per channel) |

Six **SRD-05VDC-SL-C** relays are integrated directly on the PCB. Each relay is driven by a dedicated **2N7000 N-channel MOSFET**: the MCU GPIO pulls the gate high to energise the 5 V coil; the relay contact then switches the 24 V DC supply to the RRK-3 OPEN/CLOSE sturing terminal. A **1N4007 flyback diode** is fitted across each relay coil to suppress the back-EMF transient when the coil is de-energised. A **10 kΩ gate pull-down resistor** ensures each MOSFET remains off when the GPIO is tri-stated or undriven.

```
  Internal 24V+ ──► Relay COM
                    Relay NO ──► RRK-3 OPEN sturing  (or CLOSE sturing)
  Internal 24V− ──────────────► RRK-3 COMM sturing
```

The relay provides galvanic isolation between the MCU logic circuit (3.3 V GPIO, 5 V coil) and the RRK-3 control circuit (24 V), satisfying TR-HW03. The SRD-05VDC-SL-C contact rating (10 A / 250 VAC) is well above the RRK-3 requirement of ≥ 0.5 A / 24 V.

> **Safety constraint:** The firmware must never energise the OPEN and CLOSE relay of the same motor simultaneously. This is enforced in software (see `technicalSoftwareDesignSpecification.md` §5.4). A future hardware interlock using the relay common terminals could provide an additional layer of protection.

#### 4.5.2 Feedback Input (Motor Controller Status)

| Parameter | Value |
|-----------|-------|
| Number of inputs | 1 |
| Input type | Optically isolated digital input |
| Source signal | RRK-3 alarm relay output (24 V) |
| Interface to MCU | 1 GPIO pin (opto-coupler output) |
| Connection | Screw terminal |

> **✅ Issue #1 — Motor feedback signal — Closed.** The RRK-3 motor controller signals an alarm condition via an **external relay contact** that closes on alarm. This dry contact is wired to screw terminal J10 (OPTO_INPUT / GND) and drives the opto-isolated input on the PCB, producing a logic-level signal on MCU GPIO 42 (active when alarm is present). Signal definition is documented in the RRK-3 interface specification. See also FRS Constraint C8.

#### 4.5.3 Screw Terminals

All connections to the RRK-3 relay box are brought out to screw terminal blocks on the controller PCB or DIN rail terminal strip. Terminal blocks shall be rated for at least 300 V / 6 A and clearly labelled with signal name and window channel (M1, M2, M3).

| Terminal group | Signals |
|----------------|---------|
| M1 — relay output | OPEN contact (NO + COM), CLOSE contact (NO + COM) |
| M2 — relay output | OPEN contact (NO + COM), CLOSE contact (NO + COM) |
| M3 — relay output | OPEN contact (NO + COM), CLOSE contact (NO + COM) |
| Feedback input | IN+, IN− (opto-coupler, 24 V) |

---

### 4.6 Time Source

> **FRS requirement (TR-HW08):** The system shall maintain accurate time for event logging, including during and after mains power interruptions.

Accurate, persistent time is required for event log timestamps and, if implemented, for time-of-day based control strategies (e.g. day/night setpoints). Four options are analysed below. A design decision is pending — see Open Issue #7.

#### 4.6.1 Alternatives Considered

| Option | Running accuracy | Survives power-off | Additional hardware | Approx. cost | Complexity |
|--------|-----------------|-------------------|---------------------|-------------|------------|
| ESP32 internal RTC + NTP only | Good while online; ±100–200 ppm offline | No — time lost on every power cycle | None | None | Low |
| External RTC — DS1307 | ±20 ppm (crystal dependent) | Yes — CR2032 battery backup | I2C module + 32.768 kHz crystal, 2 shared GPIO | €1–3 | Low |
| GNSS receiver | < 1 µs (GPS atomic) | Yes — after fix re-acquired | GNSS module + antenna | €10–30 | Medium–High |
| DCF77 receiver | ±1 ms (atomic reference) | Yes — continuous reception | DCF77 module + ferrite antenna | €5–15 | Medium |

**ESP32 internal RTC + NTP only**
The ESP32-S3 contains an internal RTC counter that is not battery-backed. Time is lost on every power cycle and must be restored via NTP. While running, the internal oscillator drifts ±100–200 ppm (hardware- and temperature-dependent), producing up to ±17 s per 24 hours. Since WiFi is optional in this system, NTP alone cannot satisfy TR-HW08: log timestamps will be invalid after any power interruption until a network connection is re-established.

**External RTC — DS1307**
The DS1307 is a real-time clock IC with I2C interface and a CR2032 coin-cell backup. It requires an external 32.768 kHz crystal for oscillator operation. Accuracy is ±20 ppm (crystal dependent; approximately ±10 minutes per year). The backup cell maintains the clock through power interruptions of any duration. Connection uses the shared I2C bus with the LCD display (addresses 0x3E and 0x68 do not conflict); no additional GPIO is needed. The DS1307 operates from the 5 V rail. Module cost is €1–3. This is the lowest-cost, lowest-complexity option that satisfies TR-HW08. NTP synchronisation over WiFi, when available, can further correct long-term drift.

**GNSS receiver**
A GNSS (GPS/GLONASS) module provides highly accurate time (< 1 µs) and date independently of network connectivity. Geographic position is also available, from which sunrise and sunset times — including seasonal correction for summer/winter time — can be computed in firmware for any date, without a separate DST table. Disadvantages for this application: modules cost €10–30 and require an antenna (patch or external); a cold start takes 30 s to several minutes to acquire a fix; signal attenuation inside a greenhouse is moderate (polycarbonate and glass are largely transparent to L1 GPS frequencies, but a clear sky view is not guaranteed); switching-mode power supplies and motor drives in the greenhouse may cause RF interference; power consumption is significantly higher than an RTC. For an application where the sole benefit over a DS1307 is removing a €2 component, the cost and complexity are disproportionate.

**DCF77 receiver**
DCF77 is a German long-wave time signal broadcast on 77.5 kHz from Mainflingen, covering most of Western Europe (including the Netherlands) with a range of up to 2000 km. It provides UTC plus a daylight-saving-time (CET/CEST) flag, referenced to the German atomic clock — eliminating the need for a firmware DST table for Dutch installations. Receiver modules cost €5–15 and use a compact ferrite rod antenna. Disadvantages: greenhouse environments contain significant interference sources (switching power supplies, frequency inverters, motor drives, fluorescent or LED grow lights) that can degrade or completely block reception of the 77.5 kHz signal; reception reliability must be verified on-site before this option can be relied upon. The option is geographically restricted to Europe.

#### 4.6.2 Design Decision

**Selected: DS1307** (Open Issue #7 resolved).

The DS1307 is used as the primary time source. It satisfies TR-HW08 at minimal cost and complexity. NTP synchronisation over WiFi, when available, corrects accumulated crystal drift. The DS1307 module (dev board variant) is listed in the BOM; the production PCB uses the bare DS1307 IC with an external 32.768 kHz crystal and CR2032 holder.

---

### 4.7 Power Supply

> **FRS requirements:**
> - The system shall operate from standard mains power supply as available in a greenhouse installation (TR-PS01). This requires 230 VAC / 50 Hz input with reinforced isolation in the AC–DC converter.
> - The system shall maintain operation during brief mains power interruptions without resetting (TR-PS02). This is implemented via a buffer capacitor on the 5 V rail sized to sustain load for at least 1 s.

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
                                                       ├──► SIT65HVD08P (3.3 V via LOLIN S3 LDO)
                                                       ├──► DS1307 RTC
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
- The 6 × SRD-05VDC-SL-C relay coils (5 V, via 2N7000 MOSFET drivers)
- The LCD module backlight
- The SIT65HVD08P RS485 transceiver (3.3 V supply from the LOLIN S3 on-board LDO)
- The DS1307 RTC module
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
| 6 × SRD-05VDC-SL-C relays (all energised) | ~360 mA (~60 mA per relay coil × 6) |
| LCD module (backlight on) | ~40 mA |
| SIT65HVD08P RS485 transceiver (3.3 V via LOLIN S3 LDO — included in LOLIN S3 figure) | — |
| DS1307 RTC | ~2 mA |
| Status LEDs (PWR + HB + 6 relay, all on) | ~16 mA (2 mA × 8) |
| **Total 5 V (worst case)** | **~680 mA** |

> The DC–DC buck converter is rated at 1000 mA. Load is approximately 680 mA, giving ~32% headroom.

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

#### 4.8.1 SD Card Mount/Unmount Control

Safe removal of the SD card is managed in software. The operator or technician mounts and unmounts the SD card file system through the **LCD menu** or **web interface**. There is no dedicated hardware button and no status LED. The SD card state is reflected in the LCD status screen and web interface.

---

### 4.9 Status LEDs

Status LEDs on the PCB provide instant visual feedback on the operating state of the controller without needing a connected device. All LEDs are mounted directly on the PCB. Because the enclosure has a **transparent cover**, the LEDs are fully visible to the operator without any holes, light pipes, or panel-mount LED holders in the cover. This simplifies the mechanical design and preserves the IP67 rating of the sealed cover.

#### 4.9.1 LED Overview

| LED label | Colour | Quantity | Drive source | Extra GPIO |
|-----------|--------|----------|-------------|------------|
| PWR | Green | 1 | 5 V rail via resistor (hardware) | None |
| HB (Heartbeat) | Amber | 1 | Dedicated MCU GPIO | 1 |
| M1-OPEN | Amber | 1 | Shared with relay M1-OPEN GPIO driver | None |
| M1-CLOSE | Amber | 1 | Shared with relay M1-CLOSE GPIO driver | None |
| M2-OPEN | Amber | 1 | Shared with relay M2-OPEN GPIO driver | None |
| M2-CLOSE | Amber | 1 | Shared with relay M2-CLOSE GPIO driver | None |
| M3-OPEN | Amber | 1 | Shared with relay M3-OPEN GPIO driver | None |
| M3-CLOSE | Amber | 1 | Shared with relay M3-CLOSE GPIO driver | None |
| **Total** | | **8** | | **1 additional GPIO** |

#### 4.9.2 LED Descriptions

**PWR — Power indicator (green)**
- Driven directly from the 5 V PSU output via a series current-limiting resistor (~1 kΩ, ~5 mA).
- Lit whenever mains power is present and the PSU is operational.
- No MCU involvement; remains lit even if the MCU is in reset or has faulted.
- Provides immediate confirmation that the unit is powered before any other diagnostic step.

**HB — Heartbeat (amber)**
- Driven by one dedicated MCU GPIO output via a series resistor (560 Ω for ~2 mA at 3.3 V).
- The firmware toggles this LED to indicate software state:

| Blink pattern | Meaning |
|---------------|---------|
| 1 Hz steady blink (500 ms on / 500 ms off) | Normal operation (Automatic or Standby mode) |
| Fast blink (4 Hz) | Startup / initialisation (windows closing to home position) |
| Steady ON | Firmware has stopped — watchdog has not yet fired; indicates a software hang |
| Steady OFF | MCU not running (power fault or crash before LED initialisation) |


**Relay LEDs — M1-OPEN, M1-CLOSE, M2-OPEN, M2-CLOSE, M3-OPEN, M3-CLOSE (amber)**
- Each LED is connected in parallel with the corresponding relay coil drive transistor output, via its own current-limiting resistor.
- The LED illuminates whenever the corresponding relay is energised, directly mirroring the relay state in real time.
- No additional GPIO is required; the relay-drive GPIO signals are shared.
- Allows an installer or technician to verify which relay commands are active without opening the enclosure or connecting a diagnostics tool.

#### 4.9.3 Relay LED Circuit (per channel)

```
  MCU GPIO ──► [relay driver transistor] ──► relay coil ──► GND
                        │
                   (collector)
                        ├──[R_LED ~1 kΩ]──► [LED amber] ──► GND
```

The LED and series resistor are placed from the transistor collector to ground, so the LED lights when the transistor conducts (relay energised). The resistor is sized for approximately 3 mA LED current at 5 V when the relay coil is active.

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
│  [PWR]  [HB]                     │  ← Power / Heartbeat LEDs (on PCB)
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

> **FRS requirement (TR-HW01):** The system shall be suitable for continuous installation in a greenhouse environment. This requires an enclosure rated IP67 or higher to withstand the humidity, condensation, and water spray present in a greenhouse.

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

The **MC001110** (55 mm depth) is selected and confirmed following PCB layout and 3D component-height clearance check. The MC001111 (75 mm depth, +€2) remains the fallback if the internal stack — PCB with integrated relay circuits, PSU module, LCD on standoffs — does not fit.

#### 4.10.2 Enclosure Properties

| Parameter | Value |
|-----------|-------|
| Internal dimensions (L × W × D) | To be verified from Multicomp Pro datasheet for MC001110 / MC001111 |
| Cutouts in cover | 1 × membrane keypad area (4×4 key matrix, bonded to cover) |
| Cable entries (housing body) | IP67-rated cable glands: mains cable, RS485 sensor cable, RRK-3 control cables |
| Fuse access | Panel-mount fuse holder in housing body, accessible without opening cover |
| Internal mounting | Main PCB (LOLIN S3, discrete relay circuits SRD-05VDC-SL-C + 2N7000, SIT65HVD08P RS485 transceiver, screw terminals), PSU module (RS-15-24), LCD module on standoff screws |

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

#### 4.11.1 Function Count Overview

The table below lists all allocated functions on the ESP32-S3. Specific GPIO numbers are given in §4.11.2.

| Function | Interface | ESP32-S3 peripheral | GPIO count |
|----------|-----------|---------------------|-----------|
| RS485 Modbus TX | UART | UART1 TX | 1 |
| RS485 Modbus RX | UART | UART1 RX | 1 |
| RS485 direction (DE/RE) | GPIO output | — | 1 |
| LCD display (SDA) | I2C | I2C0 SDA | 1 |
| LCD display (SCL) | I2C | I2C0 SCL | 1 |
| RTC DS1307 (SDA) | I2C | I2C0 SDA (shared) | — |
| RTC DS1307 (SCL) | I2C | I2C0 SCL (shared) | — |
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

> The PWR LED requires no GPIO (resistor from 5 V rail). The 6 relay LEDs share the existing relay-drive GPIO lines. The HB heartbeat LED adds 1 GPIO to the mandatory count. SD card uses 4 SPI GPIOs when fitted; mount/unmount is managed via the LCD and web interface (no button, no status LED).

The ESP32-S3 has up to 45 usable GPIO pins; the design uses at most 25, leaving substantial margin for future expansion.

#### 4.11.2 Pin Assignment — LOLIN S3

This section maps every peripheral signal to a specific GPIO number on the LOLIN S3 board. This mapping is the primary reference for both the PCB schematic and the firmware pin definitions (`pins.h` or equivalent).

**Reserved / unavailable pins — must not be used:**

| GPIO | Reason |
|------|--------|
| GPIO 0 | Boot strapping pin (pull-down forces download mode); avoid general use |
| GPIO 19, 20 | Native USB D−, D+ — reserved for USB console and OTA |
| GPIO 22–25 | Not accessible on LOLIN S3 board header pins |
| GPIO 26–32 | Internally connected to QSPI flash — not accessible on header |
| GPIO 33–37 | Internally connected to QSPI PSRAM (8 MB) — not accessible on header |
| GPIO 43, 44 | UART0 TX/RX — reserved for debug console (serial monitor) |
| GPIO 45, 46 | Boot strapping pins — avoid general use |

**Proposed GPIO assignment:**

| Signal | GPIO | Direction | ESP32-S3 peripheral | Notes |
|--------|------|-----------|---------------------|-------|
| RS485 UART TX | **GPIO 17** | Output | UART1 TX | To SIT65HVD08P pin DI |
| RS485 UART RX | **GPIO 18** | Input | UART1 RX | From SIT65HVD08P pin RO |
| RS485 DE/RE | **GPIO 8** | Output | GPIO | High = transmit; Low = receive |
| I2C SDA | **GPIO 1** | Bidirectional | I2C0 SDA | Shared: LCD AiP31068L (0x3E) + DS1307 (0x68) |
| I2C SCL | **GPIO 2** | Output | I2C0 SCL | Shared: LCD + RTC |
| Keypad ROW 1 | **GPIO 3** | Output | GPIO | Drive low to scan |
| Keypad ROW 2 | **GPIO 4** | Output | GPIO | Drive low to scan |
| Keypad ROW 3 | **GPIO 5** | Output | GPIO | Drive low to scan |
| Keypad ROW 4 | **GPIO 6** | Output | GPIO | Drive low to scan |
| Keypad COL 1 | **GPIO 7** | Input | GPIO (pull-up) | Read key press |
| Keypad COL 2 | **GPIO 9** | Input | GPIO (pull-up) | Read key press |
| Keypad COL 3 | **GPIO 10** | Input | GPIO (pull-up) | Read key press |
| Keypad COL 4 | **GPIO 11** | Input | GPIO (pull-up) | Read key press |
| Relay OPEN M1 | **GPIO 12** | Output | GPIO | Active-low relay driver input |
| Relay CLOSE M1 | **GPIO 13** | Output | GPIO | Active-low relay driver input |
| Relay OPEN M2 | **GPIO 14** | Output | GPIO | Active-low relay driver input |
| Relay CLOSE M2 | **GPIO 15** | Output | GPIO | Active-low relay driver input |
| Relay OPEN M3 | **GPIO 16** | Output | GPIO | Active-low relay driver input |
| Relay CLOSE M3 | **GPIO 21** | Output | GPIO | Active-low relay driver input |
| RRK-3 feedback input | **GPIO 42** | Input | GPIO | Opto-coupler output; logic HIGH when RRK-3 alarm relay contact is closed |
| Heartbeat LED (HB) | **GPIO 41** | Output | GPIO | 560 Ω series resistor to amber LED |
| SD card MOSI *(optional)* | **GPIO 47** | Output | SPI2 MOSI | Fitted only when SD feature is enabled |
| SD card MISO *(optional)* | **GPIO 48** | Input | SPI2 MISO | Fitted only when SD feature is enabled |
| SD card CLK *(optional)* | **GPIO 39** | Output | SPI2 CLK | Fitted only when SD feature is enabled |
| SD card CS *(optional)* | **GPIO 40** | Output | SPI2 CS | Fitted only when SD feature is enabled |

> **Note:** GPIO 38 is the LOLIN S3 on-board WS2812 RGB LED data line and is not used by this design. GPIO 43/44 remain available as UART0 for the serial debug console.

**GPIO usage summary:**

| Pin range | Used | Purpose |
|-----------|------|---------|
| GPIO 1–2 | 2 | I2C bus |
| GPIO 3–11 | 9 | Keypad (4 rows + 4 cols) + RS485 DE/RE |
| GPIO 12–18 | 7 | Relays (6) + RS485 TX/RX |
| GPIO 21 | 1 | Relay CLOSE M3 |
| GPIO 39–42 | 4 | SD CLK, SD CS, SD MISO→48, feedback + HB |
| GPIO 47–48 | 2 | SD MOSI/MISO *(optional)* |
| **Total mandatory** | **21** | All except SD card signals |
| **Total with SD** | **25** | Full feature set (SD SPI only) |

> **PCB design constraint:** Confirm that the chosen GPIO numbers are routable to the correct connector positions on the LOLIN S3 header before finalising the schematic. Verify no conflicts remain with any board-specific boot-time strapping requirements by checking the LOLIN S3 schematic revision in use.

---

## 5. Open Issues

| # | Issue | Owner | Status |
|---|-------|-------|--------|
| 1 | **Motor feedback signal** — ~~Resolved~~. The RRK-3 signals an alarm condition via an external relay contact that closes on alarm. This dry contact drives the opto-isolated input J10 (OPTO_INPUT / GND), producing a logic-level signal on GPIO 42 (HIGH = alarm active). Signal definition is documented in the RRK-3 interface specification. See also FRS Constraint C8. | Electrical engineer | **Closed** |
| 2 | **Sensor supply voltage** — ~~Resolved~~. Internal 24 VDC rail confirmed compatible with both sensors: SenseCAP S200 rated 5–30 VDC; FG6485A rated 9–36 VDC, ≤ 15 mA (datasheet confirmed). | Hardware designer | **Closed** |
| 3 | **RS485 sensor cable routing** — ~~Out of scope~~. Physical routing from the controller enclosure to the SenseCAP S200 (outside, on mast), including weather-proof cable glands and UV-resistant cable selection, is to be resolved during installation. This is outside the scope of the controller project. | Installer | **Out of scope** |
| 4 | **Enclosure model selection** — ~~Resolved~~. **MC001110** (222 × 146 × 55 mm, IP67, transparent polycarbonate cover, €22 ex VAT, Farnell) confirmed after PCB layout and 3D component-height clearance check. See §4.10. | Hardware designer | **Closed** |
| 5 | **Relay module selection** — ~~Resolved~~. Six **SRD-05VDC-SL-C** relays are integrated directly on the PCB, each driven by a **2N7000 N-channel MOSFET** with 1N4007 flyback diode and 10 kΩ gate pull-down resistor. No separate relay module board is used. Contact rating 10 A / 250 VAC; coil 5 VDC. See §4.5.1. | Hardware designer | **Closed** |
| 6 | **LED panel integration** — ~~Resolved~~. LEDs are on the PCB and visible through the transparent enclosure cover. No panel-mount LED holders or light pipes are required. | Hardware designer | **Closed** |
| 7 | **Time source selection** — ~~Resolved~~. **DS1307 external RTC** with CR2032 battery backup fitted on the PCB. Fully satisfies TR-HW08; NTP synchronisation over WiFi corrects long-term drift when available. See §4.6 for full analysis. | Hardware designer | **Closed** |
| 8 | **J5 heater supply (HEATING_POS / HEATING_NEG)** — Pins 5–6 of connector J5 carry HEATING_POS and HEATING_NEG nets on the PCB, providing a heater supply connection for the SenseCAP S200. This feature is not yet specified in the THDS. To be decided: whether the heater supply is required, what voltage and current it provides, and whether the heater is permanently powered or firmware-controlled. | Hardware designer | Open |

---

*End of document — version 0.3 draft*

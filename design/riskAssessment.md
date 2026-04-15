# Risk Assessment — Greenhouse Ventilation Controller

| Field        | Value                                                          |
|--------------|----------------------------------------------------------------|
| Document     | Risk Assessment                                                |
| Project      | Greenhouse Ventilation Controller                              |
| Version      | 0.1                                                            |
| Date         | 2026-04-15                                                     |
| Status       | Draft                                                          |
| Author       | Remko Welling                                                  |
| Related docs | technicalHardwareDesignSpecification.md                        |
|              | technicalSoftwareDesignSpecification.md                        |
|              | functionalRequirementsSpecification.md                         |
|              | tasks.md                                                       |

---

## Table of contents

1. [Introduction](#1-introduction)
2. [Risk heat map](#2-risk-heat-map)
3. [Risk register by domain](#3-risk-register-by-domain)
   - [3.1 Power supply](#31-power-supply)
   - [3.2 RS485 / Modbus bus](#32-rs485--modbus-bus)
   - [3.3 Wind sensor — SenseCAP S200](#33-wind-sensor--sensecap-s200)
   - [3.4 Temperature / humidity sensor — FG6485A](#34-temperature--humidity-sensor--fg6485a)
   - [3.5 Relay outputs](#35-relay-outputs)
   - [3.6 Window / motor system](#36-window--motor-system)
   - [3.7 Real-time clock — DS1307](#37-real-time-clock--ds1307)
   - [3.8 LCD display](#38-lcd-display)
   - [3.9 SD card storage](#39-sd-card-storage)
   - [3.10 NVS / internal flash](#310-nvs--internal-flash)
   - [3.11 WiFi / network](#311-wifi--network)
   - [3.12 MQTT](#312-mqtt)
   - [3.13 OTA / web server](#313-ota--web-server)
   - [3.14 FreeRTOS tasks](#314-freertos-tasks)
   - [3.15 Keypad / UI](#315-keypad--ui)
   - [3.16 Software / control logic](#316-software--control-logic)
   - [3.17 Environmental](#317-environmental)
   - [3.18 Security](#318-security)
4. [Sorted risk summary](#4-sorted-risk-summary)
5. [Mitigation recommendations](#5-mitigation-recommendations)

---

## 1. Introduction

### 1.1 Purpose

This document identifies, classifies, and quantifies failure modes for every hardware and software domain of the greenhouse ventilation controller. Its goals are:

- Provide a structured risk register that guides mitigation work during firmware implementation.
- Inform fault-handling design for tasks T1–T13 (see `tasks.md`).
- Serve as input to the software test plan (`softwareTestPlan.md`) and acceptance testing.
- Define which events are classified as **Notice**, **Warning**, or **Error** and how they are logged.

### 1.2 Scope

**In scope:** all hardware and firmware subsystems from the 230 VAC mains input up to and including the MQTT broker connection — power supply chain, sensors, actuators, communication buses, data storage, user interface, networking, OTA, FreeRTOS task layer, control logic, and environment/security considerations.

**Out of scope:** the Hotraco RRK-3 motor relay box internals, the ventilation window motors and mechanical drive trains, and the physical greenhouse structure. These are treated as black boxes with defined interfaces.

### 1.3 System summary

The controller is an ESP32-S3 (LOLIN S3) based embedded system housed in an IP67 ABS enclosure (Multicomp Pro MC001110, 222 × 146 × 55 mm) wall-mounted inside the greenhouse. It monitors temperature, relative humidity, and wind speed/direction via two Modbus RTU RS485 sensors, and controls three motorised ventilation windows through six relay outputs driving a Hotraco RRK-3 three-channel motor relay box.

Power is derived from 230 VAC mains through a Hi-Link HLK-10M24 AC-DC module (24 V / 420 mA), a DC-DC buck converter (5 V / 1 A), and the on-board ESP32-S3 LDO (3.3 V). Optional features include SD card logging (SPI / FAT32), WiFi connectivity, MQTT telemetry, a web interface, and over-the-air firmware updates. Firmware is structured as 13 FreeRTOS tasks distributed across both cores of the ESP32-S3.

### 1.4 Risk methodology

**Risk score = Chance × Effect**

Both axes are scored 1–5. Risk scores range from 1 (negligible) to 25 (critical).

**Chance — likelihood of occurrence:**

| Score | Label         | Description                        |
|-------|---------------|------------------------------------|
| 1     | Very unlikely | Less than once per year            |
| 2     | Unlikely      | 1 – 4 times per year               |
| 3     | Possible      | Approximately once per month       |
| 4     | Likely        | Approximately once per week        |
| 5     | Very likely   | Daily or near-certain              |

**Effect — severity of impact on greenhouse operation:**

| Score | Label       | Description                                                        |
|-------|-------------|--------------------------------------------------------------------|
| 1     | Negligible  | No impact on greenhouse climate control                            |
| 2     | Minor       | Reduced visibility or logging; control unaffected                  |
| 3     | Moderate    | One window channel unavailable; partial climate control loss       |
| 4     | Major       | Climate control disabled; windows may remain in wrong position     |
| 5     | Critical    | All windows stuck open or closed in dangerous condition; crop risk |

**Risk priority bands:**

| Score range | Band     | Colour    |
|-------------|----------|-----------|
| 20 – 25     | Critical | 🔴 Red    |
| 12 – 19     | High     | 🟠 Orange |
| 6 – 11      | Medium   | 🟡 Yellow |
| 1 – 5       | Low      | 🟢 Green  |

### 1.5 Severity classification of events

Every detected fault or event is classified into one of three severity levels. Each event is logged to the SD card (when present) or to the NVS ring buffer as fallback, and optionally published over MQTT when the broker is configured and connected.

| Level | Name    | Meaning                                                                    | RGB LED     |
|-------|---------|----------------------------------------------------------------------------|-------------|
| 1     | Notice  | Informational; no impact on greenhouse climate control                     | Green       |
| 2     | Warning | Degraded operation; controller continues with limitations                  | Amber       |
| 3     | Error   | Critical failure; greenhouse operation significantly compromised           | Red         |

**Log record fields:** `timestamp, severity, event_type, initiator, channel, value_a, value_b`

---

## 2. Risk heat map

The table below provides an at-a-glance view. For full details see Section 3. Risks are grouped by domain and scored.

| RSK ID  | Domain                    | Short title                             | Sev     | C | E | Score | Band     |
|---------|---------------------------|-----------------------------------------|---------|---|---|-------|----------|
| RSK-001 | Power supply              | Complete mains failure                  | Error   | 3 | 5 | 15    | 🟠 High  |
| RSK-002 | Power supply              | AC-DC PSU failure                       | Error   | 2 | 5 | 10    | 🟡 Med   |
| RSK-003 | Power supply              | DC-DC buck converter failure            | Error   | 2 | 5 | 10    | 🟡 Med   |
| RSK-004 | Power supply              | LDO failure / overload                  | Error   | 1 | 5 | 5     | 🟢 Low   |
| RSK-005 | Power supply              | Brownout / undervoltage                 | Warning | 3 | 4 | 12    | 🟠 High  |
| RSK-006 | RS485 / Modbus bus        | Bus short or open circuit               | Error   | 2 | 5 | 10    | 🟡 Med   |
| RSK-007 | RS485 / Modbus bus        | Missing / wrong termination             | Warning | 3 | 3 | 9     | 🟡 Med   |
| RSK-008 | RS485 / Modbus bus        | Persistent CRC errors                   | Warning | 3 | 3 | 9     | 🟡 Med   |
| RSK-009 | RS485 / Modbus bus        | Address collision on bus                | Warning | 1 | 3 | 3     | 🟢 Low   |
| RSK-010 | Wind sensor (S200)        | Communication timeout                   | Error   | 3 | 5 | 15    | 🟠 High  |
| RSK-011 | Wind sensor (S200)        | Out-of-range / implausible value        | Warning | 2 | 4 | 8     | 🟡 Med   |
| RSK-012 | Wind sensor (S200)        | Total sensor failure                    | Error   | 2 | 5 | 10    | 🟡 Med   |
| RSK-013 | Wind sensor (S200)        | Physical mounting / exposure damage     | Error   | 2 | 5 | 10    | 🟡 Med   |
| RSK-014 | T/RH sensor (FG6485A)     | Communication timeout                   | Warning | 3 | 4 | 12    | 🟠 High  |
| RSK-015 | T/RH sensor (FG6485A)     | Condensation / fouling on sensor        | Warning | 4 | 3 | 12    | 🟠 High  |
| RSK-016 | T/RH sensor (FG6485A)     | Out-of-range / implausible value        | Warning | 2 | 3 | 6     | 🟡 Med   |
| RSK-017 | T/RH sensor (FG6485A)     | Total sensor failure                    | Warning | 2 | 4 | 8     | 🟡 Med   |
| RSK-018 | Relay outputs             | Relay contacts stuck closed             | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-019 | Relay outputs             | Relay contacts stuck open               | Warning | 2 | 3 | 6     | 🟡 Med   |
| RSK-020 | Relay outputs             | Driver MOSFET failure                   | Warning | 1 | 3 | 3     | 🟢 Low   |
| RSK-021 | Relay outputs             | Flyback diode failure                   | Warning | 1 | 3 | 3     | 🟢 Low   |
| RSK-022 | Window / motor system     | Window mechanically stuck               | Error   | 3 | 4 | 12    | 🟠 High  |
| RSK-023 | Window / motor system     | RRK-3 feedback signal loss              | Warning | 3 | 3 | 9     | 🟡 Med   |
| RSK-024 | Window / motor system     | Motor overcurrent / overload            | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-025 | Window / motor system     | Conflicting OPEN + CLOSE relay commands | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-026 | RTC (DS1307)              | Backup battery dead                     | Notice  | 4 | 2 | 8     | 🟡 Med   |
| RSK-027 | RTC (DS1307)              | I2C bus failure                         | Warning | 2 | 3 | 6     | 🟡 Med   |
| RSK-028 | RTC (DS1307)              | Excessive clock drift                   | Notice  | 3 | 2 | 6     | 🟡 Med   |
| RSK-029 | LCD display               | I2C failure — display blank             | Warning | 2 | 2 | 4     | 🟢 Low   |
| RSK-030 | LCD display               | Backlight failure                       | Notice  | 2 | 1 | 2     | 🟢 Low   |
| RSK-031 | SD card storage           | SD card absent                          | Notice  | 4 | 2 | 8     | 🟡 Med   |
| RSK-032 | SD card storage           | SD card full                            | Warning | 3 | 2 | 6     | 🟡 Med   |
| RSK-033 | SD card storage           | SD card mount failure                   | Warning | 2 | 2 | 4     | 🟢 Low   |
| RSK-034 | SD card storage           | File corruption on power loss           | Notice  | 3 | 2 | 6     | 🟡 Med   |
| RSK-035 | NVS / internal flash      | Configuration corruption                | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-036 | NVS / internal flash      | NVS schema migration failure            | Error   | 1 | 4 | 4     | 🟢 Low   |
| RSK-037 | WiFi / network            | No WiFi connection available            | Notice  | 4 | 1 | 4     | 🟢 Low   |
| RSK-038 | WiFi / network            | DHCP / DNS failure                      | Notice  | 3 | 1 | 3     | 🟢 Low   |
| RSK-039 | MQTT                      | MQTT broker unreachable                 | Notice  | 3 | 1 | 3     | 🟢 Low   |
| RSK-040 | MQTT                      | MQTT authentication failure             | Notice  | 2 | 1 | 2     | 🟢 Low   |
| RSK-041 | MQTT                      | MQTT publish failure / queue full       | Notice  | 3 | 1 | 3     | 🟢 Low   |
| RSK-042 | OTA / web server          | OTA flash interrupted mid-write         | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-043 | OTA / web server          | LittleFS web asset corruption           | Warning | 2 | 2 | 4     | 🟢 Low   |
| RSK-044 | OTA / web server          | Automatic OTA rollback triggered        | Warning | 2 | 3 | 6     | 🟡 Med   |
| RSK-045 | FreeRTOS tasks            | Task stack overflow                     | Error   | 2 | 5 | 10    | 🟡 Med   |
| RSK-046 | FreeRTOS tasks            | Inter-task queue overflow               | Warning | 3 | 4 | 12    | 🟠 High  |
| RSK-047 | FreeRTOS tasks            | Hardware watchdog timeout / MCU reset   | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-048 | FreeRTOS tasks            | Deadlock between tasks                  | Error   | 2 | 5 | 10    | 🟡 Med   |
| RSK-049 | Keypad / UI               | Key stuck / debounce failure            | Warning | 3 | 2 | 6     | 🟡 Med   |
| RSK-050 | Keypad / UI               | Keypad scan GPIO failure                | Warning | 1 | 2 | 2     | 🟢 Low   |
| RSK-051 | Software / control logic  | Wind protection manually disabled       | Warning | 3 | 4 | 12    | 🟠 High  |
| RSK-052 | Software / control logic  | Climate control runaway (oscillation)   | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-053 | Software / control logic  | Conflicting temperature / RH setpoints  | Warning | 3 | 3 | 9     | 🟡 Med   |
| RSK-054 | Environmental             | High humidity ingress into enclosure    | Error   | 3 | 4 | 12    | 🟠 High  |
| RSK-055 | Environmental             | Temperature extremes at controller      | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-056 | Environmental             | Vibration / shock damage                | Warning | 2 | 3 | 6     | 🟡 Med   |
| RSK-057 | Security                  | Unauthorized MQTT command injection     | Error   | 2 | 4 | 8     | 🟡 Med   |
| RSK-058 | Security                  | Unauthorized web interface access       | Warning | 2 | 3 | 6     | 🟡 Med   |

---

## 3. Risk register by domain

---

### 3.1 Power supply

The power chain runs: 230 VAC mains → fuse (T0.5A) → HLK-10M24 AC-DC module (24 V / 420 mA) → DC-DC buck converter (5 V / 1 A) → ESP32-S3 LDO (3.3 V). Both sensors and the RRK-3 steering inputs are powered from 24 V. The MCU, relays, LCD, RTC, and LEDs are powered from 5 V.

---

#### RSK-001 — Complete mains failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Power supply                   |
| Severity          | **Error**                      |
| Chance            | 3 — Possible (monthly)         |
| Effect            | 5 — Critical                   |
| Risk score        | **15** 🟠 High                 |
| LED indication    | N/A (controller unpowered)     |
| Log event type    | SYSTEM (on next boot)          |

**Failure mode:** Grid power outage or blown mains fuse causes total loss of 230 VAC supply. All downstream rails collapse simultaneously.

**Chain of affected components:**
230 VAC mains → Fuse / HLK-10M24 → 24 V rail → Sensors (S200, FG6485A), RRK-3 steering contacts → DC-DC buck → 5 V rail → Relays, LCD, RTC, MCU → All relay outputs de-energised → Windows move to mechanical stop position (defined by RRK-3 internal relay state at loss of power).

**Greenhouse operation impact:**
All controller functions cease. The RRK-3 relay box retains its last mechanical state when power is lost — windows remain at their last commanded position. If windows were open during a cold night or storm, crops are exposed to harmful conditions until power is restored.

**Mitigation in firmware:**
On power restoration T1 logs a SYSTEM restart event with `esp_reset_reason()`. T2 performs a CLOSE_ALL sequence on first startup if wind speed is above threshold (T3 re-evaluates). No firmware mitigation can prevent impact during power absence; hardware mitigation (UPS or generator) is outside controller scope.

---

#### RSK-002 — AC-DC PSU failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Power supply                   |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely (1–4×/year)       |
| Effect            | 5 — Critical                   |
| Risk score        | **10** 🟡 Medium               |
| LED indication    | N/A (controller unpowered)     |
| Log event type    | SYSTEM (on next boot)          |

**Failure mode:** HLK-10M24 module fails (component aging, overvoltage on mains, thermal stress). Output voltage collapses or goes out of regulation.

**Chain of affected components:**
HLK-10M24 → 24 V rail (sensors + RRK-3 steering) → DC-DC buck → 5 V rail → All downstream. Same consequence as RSK-001.

**Greenhouse operation impact:** Identical to RSK-001. Complete loss of control.

**Mitigation in firmware:** Same as RSK-001 — log restart reason on boot. Hardware recommendation: use industrial-grade PSU with CE mark; include electrolytic capacitor on 24 V rail to ride through brief transients.

---

#### RSK-003 — DC-DC buck converter failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Power supply                   |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 5 — Critical                   |
| Risk score        | **10** 🟡 Medium               |
| LED indication    | N/A (controller unpowered)     |
| Log event type    | SYSTEM (on next boot)          |

**Failure mode:** DC-DC buck converter fails open (no output) or fails short (24 V on 5 V rail). Failed-short scenario stresses all 5 V devices including the MCU and relays.

**Chain of affected components:**
DC-DC → 5 V rail → LOLIN S3, relays, LCD, RTC. 24 V sensors remain powered; their Modbus bus goes silent.

**Greenhouse operation impact:** MCU powers down; relay coils de-energise; windows stop at last mechanical position. In a fail-short scenario MCU and relays are damaged.

**Mitigation in firmware:** Not applicable. Hardware recommendation: select a module with built-in over-voltage and short-circuit protection; use a poly-fuse on the 5 V rail.

---

#### RSK-004 — LDO failure / overload

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Power supply                   |
| Severity          | **Error**                      |
| Chance            | 1 — Very unlikely (< 1×/year)  |
| Effect            | 5 — Critical                   |
| Risk score        | **5** 🟢 Low                   |
| LED indication    | N/A (controller unpowered)     |
| Log event type    | SYSTEM (on next boot)          |

**Failure mode:** On-board LOLIN S3 LDO overheats or fails under sustained high current (WiFi TX + 6 relays simultaneously). 3.3 V rail collapses; MCU resets or fails permanently.

**Chain of affected components:**
LDO → 3.3 V rail → ESP32-S3, RS485 transceiver, I2C devices → All firmware tasks stop; relay GPIOs tri-state; relay coils de-energise via pull-down resistors.

**Greenhouse operation impact:** MCU resets; windows stop at last position.

**Mitigation in firmware:** T1 monitors task activity; watchdog reset logs restart reason. Power budget analysis shows 3.3 V load is < 200 mA; LDO thermal headroom is adequate under normal conditions.

---

#### RSK-005 — Brownout / undervoltage

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Power supply                   |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 4 — Major                      |
| Risk score        | **12** 🟠 High                 |
| LED indication    | Amber (briefly before reset)   |
| Log event type    | SYSTEM                         |

**Failure mode:** Mains voltage sags (brownout), or DC-DC output drops under heavy transient load (all 6 relays switching simultaneously). ESP32-S3 brownout detector triggers at ~2.45 V (on 3.3 V rail, ~74%).

**Chain of affected components:**
Mains sag → HLK-10M24 output droops → DC-DC output droops → 3.3 V droops → ESP32-S3 brownout detector → controlled reset → T1 logs `ESP_RST_BROWNOUT` → T2 issues CLOSE_ALL on first reconnect.

**Greenhouse operation impact:** Momentary loss of control; windows stay at last position during reset (~2–3 s boot time). Repeated brownouts indicate a power quality problem requiring attention.

**Mitigation in firmware:** `esp_reset_reason()` check on startup logs `SYSTEM` event with code `ESP_RST_BROWNOUT`. T2 evaluates safety state immediately after init. Add bulk capacitance on 5 V rail to absorb relay switching transients.

---

### 3.2 RS485 / Modbus bus

Both sensors (SenseCAP S200 address 1, FG6485A address 2) share a single Modbus RTU RS485 half-duplex bus at 9600 bps. The transceiver is a SIT65HVD08P; DE/RE direction control is on GPIO 8. A 120 Ω termination resistor (R21) is jumper-selectable at J8.

---

#### RSK-006 — Bus short or open circuit

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | RS485 / Modbus bus             |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 5 — Critical                   |
| Risk score        | **10** 🟡 Medium               |
| LED indication    | Red                            |
| Log event type    | SENSOR                         |

**Failure mode:** Cable break, connector failure, or short between A and B lines (or to shield) causes total bus failure. Both sensors become unreachable simultaneously.

**Chain of affected components:**
RS485 cable → SIT65HVD08P → UART1 (no valid frames received) → T5 timeouts on both sensors → EG1.SENSOR_FAULT_W set → T3 issues CLOSE_ALL → EG1.SENSOR_FAULT_T set → T6 inhibits climate control.

**Greenhouse operation impact:** Windows are immediately closed (safe-fail). Climate control is suspended. Controller continues with the windows closed until bus fault clears.

**Mitigation in firmware:** T5 detects Modbus timeout (`MODBUS_ERR_TIMEOUT`); after configurable retry count sets `SENSOR_FAULT_W` and `SENSOR_FAULT_T`. T3 safe-fail: wind fault → CLOSE_ALL. RGB LED turns Red. Event logged as `SENSOR / Error`.

---

#### RSK-007 — Missing or wrong bus termination

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | RS485 / Modbus bus             |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **9** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** Jumper J8 not set or set twice; missing or double 120 Ω termination causes reflections and elevated CRC error rate at 9600 bps, especially at cable lengths > 10 m.

**Chain of affected components:**
RS485 reflections → intermittent CRC errors → MODBUS_ERR_CRC → T5 increments error counter → after threshold, sensor fault flag set → T3 / T6 may react as in RSK-006.

**Greenhouse operation impact:** Initially intermittent sensor reads; escalates to sensor fault handling if error rate exceeds threshold. Partially degraded control.

**Mitigation in firmware:** T5 tracks per-sensor consecutive error count; logs `SENSOR / Warning` on first CRC error, escalates to `SENSOR / Error` after N consecutive failures. Hardware: installation guide must document J8 jumper requirement.

---

#### RSK-008 — Persistent CRC errors from electrical interference

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | RS485 / Modbus bus             |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **9** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** EMI from relay switching, motor drives, or external sources (VFDs in neighbouring equipment) corrupts Modbus frames. SM712 TVS diodes on A/B lines provide ESD protection but not EMI suppression.

**Chain of affected components:**
Relay switching transients / external EMI → RS485 differential pair → CRC mismatch → same chain as RSK-007.

**Greenhouse operation impact:** Same as RSK-007.

**Mitigation in firmware:** Same as RSK-007. Hardware: use shielded twisted pair (e.g., Belden 3105A), tie shield to PE at one end only; route RS485 cable away from relay wiring and motor cables.

---

#### RSK-009 — Address collision on bus

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | RS485 / Modbus bus             |
| Severity          | **Warning**                    |
| Chance            | 1 — Very unlikely              |
| Effect            | 3 — Moderate                   |
| Risk score        | **3** 🟢 Low                   |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** A third Modbus device is inadvertently added to the bus with address 1 or 2, causing garbled responses.

**Chain of affected components:**
Bus collision → Modbus framing error (`MODBUS_ERR_FRAMING` or `MODBUS_ERR_CRC`) → same chain as RSK-007.

**Greenhouse operation impact:** Same as RSK-007 if persistent.

**Mitigation in firmware:** T5 logs unexpected Modbus exception codes as `SENSOR / Warning`. Commissioning checklist must verify that no other devices share the bus.

---

### 3.3 Wind sensor — SenseCAP S200

The SenseCAP S200 ultrasonic wind speed and direction sensor is mounted externally (single mast pole). It communicates via Modbus RTU at address 1, powered from 24 V. Wind safety (T3) uses it to trigger CLOSE_ALL when wind speed exceeds `v_max`.

---

#### RSK-010 — Communication timeout

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Wind sensor (S200)             |
| Severity          | **Error**                      |
| Chance            | 3 — Possible                   |
| Effect            | 5 — Critical                   |
| Risk score        | **15** 🟠 High                 |
| LED indication    | Red                            |
| Log event type    | SENSOR                         |

**Failure mode:** Sensor fails to respond within 200 ms (power loss to sensor, cable break at junction box, connector corrosion, sensor firmware hang).

**Chain of affected components:**
S200 timeout → `MODBUS_ERR_TIMEOUT` → T5 sets EG1.SENSOR_FAULT_W → T3 safe-fail: treats wind as exceeding all thresholds → posts CLOSE_ALL to Q1 → T2 closes all three windows → EG1.WIND_OVERRIDE set.

**Greenhouse operation impact:** All windows closed immediately. If the fault occurs during high-temperature conditions, the greenhouse may overheat. However, this is the conservative safe-fail position against storm damage.

**Mitigation in firmware:** T3 safe-fail behaviour is by design (`wind_prot_en` must be explicitly disabled to override). T5 retries once before setting fault flag. RGB LED turns Red. Fault clears automatically on next successful read. Event logged as `SENSOR / Error`.

---

#### RSK-011 — Out-of-range or implausible value

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Wind sensor (S200)             |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** Sensor returns a value outside physical range (e.g., wind speed > 60 m/s, direction > 359°) or a sudden step change from 0 to maximum in one poll cycle, suggesting sensor malfunction or a corrupted but valid-CRC frame.

**Chain of affected components:**
Implausible reading → T5 sanity check fails → reading discarded → if persistent, `SENSOR_FAULT_W` set → same chain as RSK-010.

**Greenhouse operation impact:** Single discarded readings have no impact. If implausible values persist over multiple cycles, wind protection triggers CLOSE_ALL.

**Mitigation in firmware:** T5 applies range validation (0 ≤ speed ≤ 60 m/s, 0 ≤ direction ≤ 359°) and rate-of-change plausibility check. Discarded readings log `SENSOR / Warning`.

---

#### RSK-012 — Total sensor failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Wind sensor (S200)             |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 5 — Critical                   |
| Risk score        | **10** 🟡 Medium               |
| LED indication    | Red                            |
| Log event type    | SENSOR                         |

**Failure mode:** Sensor electronics fail permanently (lightning strike, transceiver failure, internal component failure). Sensor does not respond at all.

**Chain of affected components:** Same as RSK-010; fault does not self-clear.

**Greenhouse operation impact:** Permanent CLOSE_ALL state until sensor is replaced and fault is cleared. Manual override via keypad allows temporary window operation; `wind_prot_en` can be disabled by admin to allow climate control to resume at operator's risk.

**Mitigation in firmware:** T5 logs `SENSOR / Error`. Admin menu allows `wind_prot_en` to be set to `false`, which logs a `SETPOINT / Warning` and turns LED Amber. Fault event includes sensor ID in `channel` field.

---

#### RSK-013 — Physical mounting or exposure damage

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Wind sensor (S200)             |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 5 — Critical                   |
| Risk score        | **10** 🟡 Medium               |
| LED indication    | Red                            |
| Log event type    | SENSOR                         |

**Failure mode:** Physical damage from storm, hail, falling debris, or vandalism displaces or destroys the sensor. Alternatively, water ingress at cable gland corrupts the RS485 cable.

**Chain of affected components:** Same as RSK-010/RSK-012 depending on outcome.

**Greenhouse operation impact:** Same as RSK-012.

**Mitigation:** Annual inspection of sensor mounting and cable glands. IP66 rated sensor enclosure provides weather protection. No firmware mitigation; treated same as RSK-012.

---

### 3.4 Temperature / humidity sensor — FG6485A

The FG6485A senses temperature (−40 to +120 °C) and relative humidity (0–99.9 % RH) inside the greenhouse. It communicates via Modbus RTU at address 2, powered from 24 V. T6 (Climate Control) uses its readings to control window position.

---

#### RSK-014 — Communication timeout

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | T/RH sensor (FG6485A)          |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 4 — Major                      |
| Risk score        | **12** 🟠 High                 |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** FG6485A fails to respond (power loss, connector fault, sensor reset). T5 receives no valid frame within 200 ms.

**Chain of affected components:**
FG6485A timeout → `FG6485A_ERR_COMM` → T5 sets EG1.SENSOR_FAULT_T → T6 inhibits climate control → windows remain at last commanded position.

**Greenhouse operation impact:** Climate control is suspended. Windows do not open or close automatically until fault clears. Wind protection (T3) continues operating independently. If temperature rises and windows are closed, crops may be stressed.

**Mitigation in firmware:** T5 retries once; logs `SENSOR / Warning` with sensor ID. T6 checks `SENSOR_FAULT_T` before issuing any window command. RGB LED turns Amber. Fault clears automatically on successful read.

---

#### RSK-015 — Condensation or biological fouling on sensor element

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | T/RH sensor (FG6485A)          |
| Severity          | **Warning**                    |
| Chance            | 4 — Likely (weekly in greenhouse) |
| Effect            | 3 — Moderate                   |
| Risk score        | **12** 🟠 High                 |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** High-humidity greenhouse environment (irrigated crops) causes condensation on the RH capacitive element or algae growth on the sensor housing. The sensor reports systematically elevated RH values (stuck near 99.9 %) without a communication fault.

**Chain of affected components:**
Fouled sensor → biased RH reading → T6 sees RH > `rh_max` setpoint → T6 opens windows unnecessarily → increased ventilation → potential temperature drop on cold nights.

**Greenhouse operation impact:** Unnecessary ventilation may lower greenhouse temperature below optimal range on cool days. Not immediately dangerous but degrades climate quality.

**Mitigation in firmware:** T5 applies plausibility: if RH == 99.9 % for > N consecutive polls (configurable, default 60 min), log `SENSOR / Warning` with value flagged as potentially saturated. Recommendation: sensor with replaceable filter cap; cleaning schedule every 3 months.

---

#### RSK-016 — Out-of-range or implausible value

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | T/RH sensor (FG6485A)          |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** Sensor returns temperature > 120 °C or < −40 °C, or RH > 100 % or < 0 %, indicating internal sensor fault or Modbus data packing error.

**Chain of affected components:**
Out-of-range value → T5 range check fails → reading discarded → if persistent, SENSOR_FAULT_T set → T6 inhibited.

**Greenhouse operation impact:** Same as RSK-014 if persistent.

**Mitigation in firmware:** T5 range validation and single-poll discard with log. After N consecutive failures escalate to `SENSOR_FAULT_T`.

---

#### RSK-017 — Total sensor failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | T/RH sensor (FG6485A)          |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SENSOR                         |

**Failure mode:** FG6485A electronics fail permanently. Sensor does not respond; fault does not self-clear.

**Chain of affected components:** Same as RSK-014; permanent `SENSOR_FAULT_T`.

**Greenhouse operation impact:** Climate control suspended indefinitely until sensor replaced. Wind protection continues. Manual window operation available via keypad.

**Mitigation in firmware:** T5 logs `SENSOR / Error`. Admin can disable `rh_ctrl_en` to acknowledge operating without T/RH control (LED remains Amber; `SETPOINT / Warning` logged).

---

### 3.5 Relay outputs

Six SRD-05VDC-SL-C relays driven by 2N7000 MOSFETs with 1N4007 flyback diodes. Relay GPIOs: M1-OPEN (GPIO 12), M1-CLOSE (GPIO 13), M2-OPEN (GPIO 14), M2-CLOSE (GPIO 15), M3-OPEN (GPIO 16), M3-CLOSE (GPIO 21). T2 owns all relay outputs exclusively.

---

#### RSK-018 — Relay contacts stuck closed (welded)

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Relay outputs                  |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red                            |
| Log event type    | RELAY                          |

**Failure mode:** Relay contacts weld together under inductive switching surge (worn contacts, RRK-3 inductive load). The relay cannot open even when the coil is de-energised, leaving the motor command permanently asserted.

**Chain of affected components:**
Stuck relay → motor command permanently active → RRK-3 motor channel runs to end-stop continuously → motor stalls or thermal protection trips → window may be driven hard against its stop; motor damage possible.

**Greenhouse operation impact:** One window channel uncontrollable. Depending on which relay (OPEN or CLOSE), the window is driven fully open or closed and cannot be commanded to the opposite position.

**Mitigation in firmware:** T2 monitors expected relay state vs. RRK-3 opto-feedback. If a CLOSE command is issued but window continues moving (feedback still active), T2 logs `RELAY / Error` and disables that channel's commands to prevent oscillation. Fault requires physical relay replacement.

---

#### RSK-019 — Relay contacts stuck open (failed open)

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Relay outputs                  |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | RELAY                          |

**Failure mode:** Relay contacts fail open (oxide buildup, mechanical spring fatigue). Coil energises but contacts do not close; motor channel receives no command.

**Chain of affected components:**
Failed-open relay → RRK-3 receives no input → window does not respond to OPEN or CLOSE command → T2 logs no movement on expected feedback → logs `RELAY / Warning`.

**Greenhouse operation impact:** One window channel non-functional. Climate control continues on remaining channels. Degraded but not critically impaired.

**Mitigation in firmware:** T2 cross-checks RRK-3 feedback against expected movement; if movement not detected after full run-time, logs `RELAY / Warning`. Operator informed via LCD.

---

#### RSK-020 — Driver MOSFET failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Relay outputs                  |
| Severity          | **Warning**                    |
| Chance            | 1 — Very unlikely              |
| Effect            | 3 — Moderate                   |
| Risk score        | **3** 🟢 Low                   |
| LED indication    | Amber                          |
| Log event type    | RELAY                          |

**Failure mode:** 2N7000 MOSFET fails open (gate, drain, or source open-circuit). Relay coil never energises regardless of GPIO state.

**Chain of affected components:**
MOSFET open → relay coil not energised → relay stays open → same consequence as RSK-019.

**Greenhouse operation impact:** Same as RSK-019.

**Mitigation in firmware:** Same detection as RSK-019. Hardware: 10 kΩ gate pull-down ensures relay is de-energised on MCU reset.

---

#### RSK-021 — Flyback diode failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Relay outputs                  |
| Severity          | **Warning**                    |
| Chance            | 1 — Very unlikely              |
| Effect            | 3 — Moderate                   |
| Risk score        | **3** 🟢 Low                   |
| LED indication    | Amber                          |
| Log event type    | RELAY                          |

**Failure mode:** 1N4007 flyback diode fails open. On relay coil de-energisation, inductive kickback voltage spike is not clamped. MOSFET gate/drain may be damaged; neighbouring circuit traces may be stressed.

**Chain of affected components:**
Flyback spike → MOSFET gate overstress → MOSFET fails → relay does not energise → same as RSK-020.

**Greenhouse operation impact:** Same as RSK-019/RSK-020.

**Mitigation:** Properly rated flyback diodes (1N4007, 1000 V, 1 A) well within rating. PCB layout keeps flyback loop short. No firmware detection — observable only after secondary MOSFET or relay failure.

---

### 3.6 Window / motor system

The Hotraco RRK-3 three-channel motor relay box controls M1 (South roof, 21 s), M2 (North roof, 21 s), and M3 (North side wall, 171 s). An opto-coupler input (GPIO 42) feeds back the RRK-3 alarm relay state.

---

#### RSK-022 — Window mechanically stuck

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Window / motor system          |
| Severity          | **Error**                      |
| Chance            | 3 — Possible                   |
| Effect            | 4 — Major                      |
| Risk score        | **12** 🟠 High                 |
| LED indication    | Red                            |
| Log event type    | RELAY                          |

**Failure mode:** Window jams due to debris, swollen timber frame, frozen mechanism (winter), or mechanical drive failure. Motor stalls; RRK-3 overcurrent protection trips.

**Chain of affected components:**
Stuck window → RRK-3 overcurrent trip → alarm relay closes → opto-coupler GPIO 42 activates → T2 detects alarm input → logs `RELAY / Error` → disables further commands to that channel.

**Greenhouse operation impact:** One or more windows cannot be opened or closed. If this occurs during high temperature, ventilation is insufficient; if during storm, window may be stuck open.

**Mitigation in firmware:** T2 monitors opto-coupler input. On alarm detection, channel is flagged as faulted; T6 marks channel unavailable. LCD displays channel fault. Operator must manually clear mechanical obstruction and reset fault.

---

#### RSK-023 — RRK-3 feedback signal loss

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Window / motor system          |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **9** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | RELAY                          |

**Failure mode:** Opto-coupler wiring between RRK-3 alarm relay and GPIO 42 breaks, or opto-coupler component fails. T2 can no longer detect manual override or RRK-3 alarm events.

**Chain of affected components:**
Opto-coupler → GPIO 42 → T2 cannot confirm alarm state → T2 loses awareness of RRK-3 alarm, manual override, and motor stall detection.

**Greenhouse operation impact:** T2 continues to issue commands but cannot verify outcomes. Risk of undetected motor stall (RSK-022) is elevated. Manual override by farm personnel is not detected in firmware.

**Mitigation in firmware:** T2 logs `RELAY / Warning` on persistent discrepancy between commanded state and expected completion time (run-time monitoring). NB: open issue #1 in design documents covers RRK-3 feedback implementation.

---

#### RSK-024 — Motor overcurrent / overload

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Window / motor system          |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red                            |
| Log event type    | RELAY                          |

**Failure mode:** Motor draws excessive current (aging windings, foreign object in mechanism). RRK-3 thermal overload trips. Feedback signal activates GPIO 42.

**Chain of affected components:** Same as RSK-022.

**Greenhouse operation impact:** Affected window channel unavailable until motor cools down and RRK-3 is reset (manual intervention).

**Mitigation in firmware:** Same detection as RSK-022. T2 logs `RELAY / Error` with channel ID in `channel` field.

---

#### RSK-025 — Conflicting OPEN + CLOSE relay commands

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Window / motor system          |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely (software defect) |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red                            |
| Log event type    | RELAY                          |

**Failure mode:** Software bug causes both OPEN and CLOSE relay GPIOs for the same channel to be simultaneously asserted. The RRK-3 receives conflicting commands; internal protection may trip.

**Chain of affected components:**
Both relay outputs active → RRK-3 receives OPEN + CLOSE simultaneously → RRK-3 inhibits both commands (built-in protection) or motor reversal occurs → window control lost.

**Greenhouse operation impact:** Window channel becomes uncontrollable until software conflict resolves.

**Mitigation in firmware:** T2 enforces a hardware mutex: OPEN and CLOSE GPIOs are never simultaneously asserted. A minimum dwell timer (dead-band) is enforced between opposite commands. Any attempt to assert both simultaneously is logged as `RELAY / Error` and the request is rejected.

---

### 3.7 Real-time clock — DS1307

The DS1307 RTC provides timestamps for all log events, day/night LED dimming, and NTP synchronisation reference. It shares the I2C bus with the LCD at GPIO 1 (SDA) and GPIO 2 (SCL). Backup power from a CR2032 coin cell.

---

#### RSK-026 — Backup battery dead

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | RTC (DS1307)                   |
| Severity          | **Notice**                     |
| Chance            | 4 — Likely (battery ~5 yr life)|
| Effect            | 2 — Minor                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Green (no control impact)      |
| Log event type    | SYSTEM                         |

**Failure mode:** CR2032 cell depletes after approximately 5 years. On power loss and restoration, the DS1307 loses the correct time; it returns 00:00:00 01/01/2000.

**Chain of affected components:**
Dead battery → power cycle → RTC reset to epoch → T8 detects invalid time → timestamps in log marked as invalid → NTP sync (if WiFi available) restores correct time → LED dimming uses default night hours until sync.

**Greenhouse operation impact:** Climate control is unaffected (temperature and RH setpoints are time-independent). Log timestamps are invalid until NTP sync. Day/night LED dimming uses fallback schedule.

**Mitigation in firmware:** T4 / T8 detect RTC time ≤ epoch+1 day as invalid and mark log entries. T10 performs NTP sync on WiFi connect and writes corrected time to RTC. Log entry `SYSTEM / Notice` indicates invalid time state. CR2032 holder is socketed for easy replacement.

---

#### RSK-027 — I2C bus failure (RTC + LCD share bus)

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | RTC (DS1307)                   |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** I2C bus becomes stuck (SDA held low by glitch or device lock-up), or a bus short occurs. Both RTC and LCD are inaccessible.

**Chain of affected components:**
I2C bus stuck → RTC unreachable (no timestamps) + LCD blank → T8 detects I2C failure → logs `SYSTEM / Warning` → HB LED continues; RGB LED Amber; core control tasks (T2, T3, T5, T6) unaffected.

**Greenhouse operation impact:** No display, no RTC timestamps. All window control continues. Operator has no local feedback.

**Mitigation in firmware:** I2C driver uses mutex and timeout. On repeated NACK, T8 logs error and attempts bus reset (GPIO bit-bang 9 clock pulses to unlock). Periodic retry every 30 s.

---

#### RSK-028 — Excessive clock drift

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | RTC (DS1307)                   |
| Severity          | **Notice**                     |
| Chance            | 3 — Possible                   |
| Effect            | 2 — Minor                      |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** DS1307 with ±20 ppm crystal drifts up to ~10 minutes per year. In environments with wide temperature swings the actual drift may be higher (crystal temperature coefficient).

**Chain of affected components:**
Clock drift → log timestamps progressively incorrect → day/night LED schedule drifts → NTP sync corrects when WiFi available.

**Greenhouse operation impact:** Negligible for control; log timestamps become unreliable over time without NTP.

**Mitigation in firmware:** T10 performs NTP sync on every WiFi connect event and writes result to RTC. Log entry `SYSTEM / Notice` on each NTP correction > 60 s delta.

---

### 3.8 LCD display

Waveshare LCD1602 I2C (16×2 characters) with AiP31068L bridge at address 0x3E. Driven by T8 (UI/Display task). Shares I2C bus with DS1307.

---

#### RSK-029 — I2C failure — display blank

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | LCD display                    |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 2 — Minor                      |
| Risk score        | **4** 🟢 Low                   |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** AiP31068L I2C interface fails or LCD module loses power. Display goes blank; T8 receives NACK.

**Chain of affected components:**
LCD NACK → T8 logs `SYSTEM / Warning` → display unavailable → operator has no local status feedback → core control tasks unaffected.

**Greenhouse operation impact:** Climate control continues normally. Operator cannot read status, sensor values, or alarms locally.

**Mitigation in firmware:** T8 retries I2C init every 30 s. RGB LED and HB LED provide minimal status indication. MQTT / web interface remain available when WiFi is connected.

---

#### RSK-030 — Backlight failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | LCD display                    |
| Severity          | **Notice**                     |
| Chance            | 2 — Unlikely                   |
| Effect            | 1 — Negligible                 |
| Risk score        | **2** 🟢 Low                   |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** LCD backlight LED burns out. Display characters are still readable in good ambient light but invisible in dim conditions.

**Chain of affected components:**
Backlight LED → reduced display readability → T8 not affected; character data still written correctly.

**Greenhouse operation impact:** No operational impact; cosmetic issue.

**Mitigation in firmware:** None required. Log `SYSTEM / Notice` if backlight can be detected (hardware-dependent). Backlight is independently replaceable on the module.

---

### 3.9 SD card storage

SD card (SPI, FAT32) is optional. T9 (Event Logger) prefers SD when present; falls back to NVS ring buffer. SD SPI pins: MOSI=GPIO47, MISO=GPIO48, CLK=GPIO39, CS=GPIO40.

---

#### RSK-031 — SD card absent

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | SD card storage                |
| Severity          | **Notice**                     |
| Chance            | 4 — Likely (by design optional)|
| Effect            | 2 — Minor                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** No SD card is installed (by choice or card removed during operation). `storage_init()` returns `STORAGE_ERR_NO_CARD`.

**Chain of affected components:**
No SD card → T9 uses NVS ring buffer (capacity ~1000 entries) → older events are overwritten when full → SD logs unavailable for external retrieval.

**Greenhouse operation impact:** No impact on control. Logging continues but with reduced retention. Events are still available via web interface and MQTT.

**Mitigation in firmware:** T9 logs `SYSTEM / Notice` on startup if no card detected. Web interface displays storage mode (SD / NVS). NVS ring buffer retains the most recent 1000 events.

---

#### RSK-032 — SD card full

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | SD card storage                |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 2 — Minor                      |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** Free space on SD card falls below 2 MB threshold (`storage_sd_free_bytes()` < 2 MB). T9 suspends SD writes and falls back to NVS.

**Chain of affected components:**
Low free space → T9 suspends SD logging → fallback to NVS ring buffer → same as RSK-031.

**Greenhouse operation impact:** No impact on control. Old SD log files should be retrieved and deleted; 10-file rotation normally prevents this, but a very small card or previously filled card can trigger this state.

**Mitigation in firmware:** T9 checks free space on each file rotation. Logs `SYSTEM / Warning` and transitions to NVS fallback. LCD and web interface alert operator. File rotation policy (10 files × 512 KB = 5 MB max) keeps consumption bounded on adequately sized cards (≥ 8 GB recommended).

---

#### RSK-033 — SD card mount failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | SD card storage                |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 2 — Minor                      |
| Risk score        | **4** 🟢 Low                   |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** SD card is present but cannot be mounted (`STORAGE_ERR_MOUNT`). Causes: corrupted FAT32 filesystem, incompatible card, SPI bus contention during init.

**Chain of affected components:**
Mount failure → `STORAGE_ERR_MOUNT` → T9 falls back to NVS → same as RSK-031.

**Greenhouse operation impact:** Same as RSK-031.

**Mitigation in firmware:** T9 retries mount once after 5 s. Logs `SYSTEM / Warning`. Operator can remove and reinsert card; T9 reattempts on next startup or hot-plug detection if supported.

---

#### RSK-034 — File corruption on unexpected power loss

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | SD card storage                |
| Severity          | **Notice**                     |
| Chance            | 3 — Possible                   |
| Effect            | 2 — Minor                      |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** Power loss occurs while T9 is writing to the SD card. The current CSV file may have a partial line or corrupt FAT entry. Previously closed files are intact.

**Chain of affected components:**
Power loss mid-write → partial CSV line in current file → on next mount, FAT32 driver may mark file as corrupted → T9 creates new file and continues; corrupted file may be unreadable.

**Greenhouse operation impact:** Loss of log entries written between last flush and power loss. No operational impact.

**Mitigation in firmware:** T9 writes each log entry as a complete, newline-terminated CSV line. Partial lines are self-contained and readable up to the last complete line. File is flushed (`fflush`) after every write. On mount, T9 checks if current file is valid; if not, creates a new file with current timestamp.

---

### 3.10 NVS / internal flash

NVS (Non-Volatile Storage) in ESP32-S3 internal 16 MB flash stores all configuration, setpoints, thresholds, and the fallback event log ring buffer.

---

#### RSK-035 — Configuration corruption

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | NVS / internal flash           |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red                            |
| Log event type    | SYSTEM                         |

**Failure mode:** NVS partition corrupted (flash wear, power loss during NVS write, firmware bug writing out-of-range value). All configuration is lost; defaults are loaded.

**Chain of affected components:**
NVS corruption → `nvs_cfg_init()` detects error → loads compiled-in default values → setpoints reset (e.g., `v_max` reset to factory default, `wind_prot_en` reset to `true`) → T6 and T3 operate on defaults → windows may behave unexpectedly.

**Greenhouse operation impact:** All user-configured setpoints and schedules are lost. Defaults provide a safe starting state (wind protection enabled, conservative thresholds) but the operator must reconfigure the system.

**Mitigation in firmware:** NVS driver uses ESP-IDF NVS wear-levelling which spreads writes across flash. On corruption detected, factory defaults are loaded and a `SYSTEM / Error` event is logged. LCD prompts operator to reconfigure. Double-write pattern (write to key, verify readback) should be used for critical setpoints.

---

#### RSK-036 — NVS schema migration failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | NVS / internal flash           |
| Severity          | **Error**                      |
| Chance            | 1 — Very unlikely              |
| Effect            | 4 — Major                      |
| Risk score        | **4** 🟢 Low                   |
| LED indication    | Red                            |
| Log event type    | SYSTEM                         |

**Failure mode:** Firmware update introduces a new NVS schema version. Migration from old to new schema fails (missing key, type mismatch). NVS driver cannot read existing configuration.

**Chain of affected components:**
Schema mismatch → migration error → fallback to defaults (same as RSK-035) → `SYSTEM / Error` logged.

**Greenhouse operation impact:** Same as RSK-035.

**Mitigation in firmware:** NVS driver stores schema version number. On version mismatch, migration function is called before any read. If migration fails, defaults are loaded and `SYSTEM / Error` is logged. OTA rollback (RSK-044) can restore previous firmware with compatible schema.

---

### 3.11 WiFi / network

WiFi is an optional feature. The controller is fully operational without a WiFi connection. T10 (Network Manager) manages WiFi lifecycle; T11 (Web Server) and T12 (MQTT) depend on it.

---

#### RSK-037 — No WiFi connection available

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | WiFi / network                 |
| Severity          | **Notice**                     |
| Chance            | 4 — Likely                     |
| Effect            | 1 — Negligible                 |
| Risk score        | **4** 🟢 Low                   |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** Configured WiFi AP is not reachable (AP offline, changed credentials, out of range). T10 cannot associate.

**Chain of affected components:**
WiFi not connected → T11 (web server) and T12 (MQTT) inactive → no remote monitoring or telemetry → core control (T2, T3, T5, T6) unaffected.

**Greenhouse operation impact:** No impact on climate control. Remote monitoring and MQTT telemetry unavailable.

**Mitigation in firmware:** T10 retries WiFi connect with exponential backoff. Logs `SYSTEM / Notice` on first failure. RGB LED stays Green (core control not affected).

---

#### RSK-038 — DHCP / DNS failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | WiFi / network                 |
| Severity          | **Notice**                     |
| Chance            | 3 — Possible                   |
| Effect            | 1 — Negligible                 |
| Risk score        | **3** 🟢 Low                   |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** WiFi associated but DHCP lease not obtained, or DNS resolution fails for NTP / MQTT broker hostname.

**Chain of affected components:**
No IP address → T10 marks network unavailable → T11, T12, NTP sync all fail → same consequence as RSK-037.

**Mitigation in firmware:** T10 supports static IP configuration as fallback. Logs `SYSTEM / Notice`. Recommend: configure static IP for controller in router or use reserved DHCP lease.

---

### 3.12 MQTT

MQTT is a Could-have optional feature (FR-MQ01–MQ05). T12 publishes sensor data and alarm flags; subscribes to remote command topics. Broker configuration stored in NVS namespace `mqtt`.

---

#### RSK-039 — MQTT broker unreachable

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | MQTT                           |
| Severity          | **Notice**                     |
| Chance            | 3 — Possible                   |
| Effect            | 1 — Negligible                 |
| Risk score        | **3** 🟢 Low                   |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** MQTT broker is offline or TCP connection times out. T12 cannot publish or receive.

**Chain of affected components:**
T12 TCP connect fails → MQTT session unavailable → no remote telemetry or commands → core control unaffected.

**Greenhouse operation impact:** No impact on control.

**Mitigation in firmware:** T12 retries with exponential backoff; logs `SYSTEM / Notice`. Buffered publish queue drains on reconnect (up to queue depth).

---

#### RSK-040 — MQTT authentication failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | MQTT                           |
| Severity          | **Notice**                     |
| Chance            | 2 — Unlikely                   |
| Effect            | 1 — Negligible                 |
| Risk score        | **2** 🟢 Low                   |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** Broker rejects connection (wrong username/password, expired certificate). T12 cannot publish or subscribe.

**Chain of affected components:** Same as RSK-039.

**Mitigation in firmware:** T12 logs `SYSTEM / Notice` with MQTT return code. Admin must update credentials via web interface or keypad menu.

---

#### RSK-041 — MQTT publish failure / queue full

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | MQTT                           |
| Severity          | **Notice**                     |
| Chance            | 3 — Possible                   |
| Effect            | 1 — Negligible                 |
| Risk score        | **3** 🟢 Low                   |
| LED indication    | Green                          |
| Log event type    | SYSTEM                         |

**Failure mode:** T12 publish queue fills (broker slow or offline during burst of alarm events). Oldest messages are dropped.

**Chain of affected components:** Messages lost → remote dashboard misses some events → SD/NVS log still complete.

**Mitigation in firmware:** T12 uses a bounded queue; on overflow, oldest entry discarded and `SYSTEM / Notice` logged. All events are independently stored in SD/NVS so MQTT loss is not a data integrity concern.

---

### 3.13 OTA / web server

T13 manages over-the-air firmware and LittleFS updates with dual-bank rollback. T11 serves the web interface from LittleFS.

---

#### RSK-042 — OTA flash interrupted mid-write

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | OTA / web server               |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red (if boot fails)            |
| Log event type    | SYSTEM                         |

**Failure mode:** Power loss or WiFi disconnect during OTA write. The inactive firmware partition receives a partial image.

**Chain of affected components:**
Partial OTA image → ESP-IDF OTA bootloader validates image hash → invalid image detected → bootloader boots from previous (valid) partition → T13 logs restart as OTA failure.

**Greenhouse operation impact:** Firmware reverts to previous version; all control resumes normally after boot (~3 s). No crop risk. If OTA was intended to fix a critical bug, the fix is lost and must be retried.

**Mitigation in firmware:** ESP-IDF dual-bank OTA with image validation. T13 marks new partition valid only after startup health check completes successfully. On three consecutive watchdog resets without successful health check, rollback to previous partition. Logs `SYSTEM / Error` with OTA partition info.

---

#### RSK-043 — LittleFS web asset corruption

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | OTA / web server               |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 2 — Minor                      |
| Risk score        | **4** 🟢 Low                   |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** LittleFS partition corrupted (power loss during asset OTA, flash wear). T11 cannot serve web pages.

**Chain of affected components:**
LittleFS corrupt → T11 serves 503 responses → web interface unavailable → MQTT and local keypad still functional.

**Mitigation in firmware:** T11 detects mount failure; logs `SYSTEM / Warning`. LittleFS OTA re-flashes the partition entirely, restoring assets. Core control tasks unaffected.

---

#### RSK-044 — Automatic OTA rollback triggered

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | OTA / web server               |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** New firmware fails startup health check (task init timeout, watchdog reset) three times consecutively. T13 rolls back to previous firmware partition.

**Chain of affected components:**
3× watchdog reset → bootloader selects previous partition → boot with old firmware → T13 logs rollback event.

**Greenhouse operation impact:** New firmware features or bug fixes are reverted. System operates on previous firmware. Control resumes normally after rollback.

**Mitigation in firmware:** T13 logs `SYSTEM / Warning` with rollback reason. LCD and MQTT alert operator that rollback occurred and new firmware should be reviewed before retrying.

---

### 3.14 FreeRTOS tasks

Thirteen tasks across two cores. Synchronisation: 5 mutexes, 6 queues (Q1–Q6), 4 task notifications, 1 event group (EG1). T1 kicks the hardware watchdog every 500 ms.

---

#### RSK-045 — Task stack overflow

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | FreeRTOS tasks                 |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 5 — Critical                   |
| Risk score        | **10** 🟡 Medium               |
| LED indication    | Red (watchdog reset)           |
| Log event type    | SYSTEM                         |

**Failure mode:** A task overflows its stack (recursive call, large local array, unexpected deep call chain). FreeRTOS stack overflow hook fires; system is typically unrecoverable without reset.

**Chain of affected components:**
Stack overflow → FreeRTOS `vApplicationStackOverflowHook` → assertion / controlled panic → watchdog not kicked → hardware watchdog reset → T1 logs `ESP_RST_INT_WDT` or `ESP_RST_TASK_WDT` on next boot.

**Greenhouse operation impact:** MCU resets. T2 issues CLOSE_ALL on boot. Relays de-energise during reset (~3 s gap). Repeated resets if stack overflow is reliably triggered.

**Mitigation in firmware:** Enable FreeRTOS stack overflow checking (method 2, watermark paint). In development, use `uxTaskGetStackHighWaterMark()` to size stacks correctly with 25% headroom. Stack overflow hook logs task name before reset. All stacks reviewed against worst-case call depth.

---

#### RSK-046 — Inter-task queue overflow

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | FreeRTOS tasks                 |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 4 — Major                      |
| Risk score        | **12** 🟠 High                 |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** A producer task generates events faster than the consumer can process them. Queue becomes full; new events are dropped (non-blocking send fails). Most critical: Q1 (relay commands) or Q3 (event log).

**Chain of affected components:**
Q1 overflow → relay commands dropped → T2 does not receive CLOSE_ALL or window commands → windows may not respond to safety or climate events.
Q3 overflow → log entries dropped → data integrity degraded.

**Greenhouse operation impact:** If Q1 overflows, a safety CLOSE_ALL command may be lost. This is the most severe queue failure scenario.

**Mitigation in firmware:** Q1 (relay commands) uses blocking send with timeout for critical safety commands (CLOSE_ALL). All queue overflow conditions log `SYSTEM / Warning`. Queue depths sized to handle burst: Q1 depth = 16 (one command per channel × 2 directions × 2 motors + margin). Q3 depth = 32. Monitor queue high-watermarks during testing.

---

#### RSK-047 — Hardware watchdog timeout / MCU reset

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | FreeRTOS tasks                 |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red (before reset)             |
| Log event type    | SYSTEM                         |

**Failure mode:** T1 (Watchdog/Heartbeat) is starved (preempted, deadlocked, or priority inverted) and fails to kick the hardware watchdog within its timeout window. MCU performs a hardware reset.

**Chain of affected components:**
Watchdog timeout → `ESP_RST_TASK_WDT` → all relay GPIOs tri-state → relay coils de-energise (windows stop) → boot → T1 logs reset reason → T2 issues CLOSE_ALL safety measure.

**Greenhouse operation impact:** Brief loss of control (~3 s boot). Windows de-energise during reset; they stop at last mechanical position. Repeated resets indicate a deeper firmware issue.

**Mitigation in firmware:** T1 runs at highest real-time priority on Core 1. Watchdog timeout set conservatively (e.g., 5 s; T1 kicks every 500 ms). Three consecutive resets trigger OTA rollback (T13). Reset reason is logged on every boot.

---

#### RSK-048 — Deadlock between tasks

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | FreeRTOS tasks                 |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely (design-time risk)|
| Effect            | 5 — Critical                   |
| Risk score        | **10** 🟡 Medium               |
| LED indication    | Red (watchdog reset)           |
| Log event type    | SYSTEM                         |

**Failure mode:** Two tasks hold mutexes in opposite order, creating a circular wait. Both tasks block indefinitely; T1 is starved; watchdog fires.

**Chain of affected components:**
Deadlock → T1 starved → watchdog reset → same as RSK-047.

**Greenhouse operation impact:** Same as RSK-047.

**Mitigation in firmware:** Design-time prevention: establish a strict mutex acquisition ordering (MX1 → MX2 → MX3 → MX4 → MX5). No task acquires mutexes in a different order. Use `xSemaphoreTakeWithTimeout()` with a finite timeout rather than blocking indefinitely; on timeout log `SYSTEM / Error` and release all held mutexes. Code review must verify ordering.

---

### 3.15 Keypad / UI

4×4 matrix keypad scanned by T7 at ~20 ms intervals. Row GPIOs 3–6 driven low; column GPIOs 7, 9, 10, 11 read with pull-up.

---

#### RSK-049 — Key stuck / debounce failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Keypad / UI                    |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 2 — Minor                      |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** A key membrane fails to release (moisture ingress, mechanical deformation, contamination). T7 sees a permanently asserted key, generates repeated key events, floods Q5 (keypad events), and may result in unintended menu navigation or PIN entry.

**Chain of affected components:**
Stuck key → T7 generates repeated events → T8 receives key flood → if in sensitive menu (setpoint change, PIN prompt), unintended values may be entered → if setpoints change, T6/T3 behaviour changes.

**Greenhouse operation impact:** Setpoints could be inadvertently modified. Session inactivity timer in T8 will eventually lock the UI menu, preventing further accidental changes.

**Mitigation in firmware:** T7 applies debounce (20 ms) and key-repeat rate limiting (first repeat after 500 ms, subsequent every 200 ms). T8 session timeout (5 min inactivity) locks menu. T8 logs `SYSTEM / Warning` on key held > 60 s. UI changes to setpoints are separately logged as `SETPOINT`.

---

#### RSK-050 — Keypad scan GPIO failure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Keypad / UI                    |
| Severity          | **Warning**                    |
| Chance            | 1 — Very unlikely              |
| Effect            | 2 — Minor                      |
| Risk score        | **2** 🟢 Low                   |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** One or more keypad GPIO fails (short, open, or MCU GPIO damage). T7 cannot scan part of the key matrix.

**Chain of affected components:**
Failed GPIO → partial or no key recognition → T8 cannot accept local input → operator cannot configure controller locally → web interface remains available.

**Mitigation in firmware:** T8 detects zero key events for > 10 min (if session is active) as potential scan failure; logs `SYSTEM / Warning`. Web interface provides full configuration alternative.

---

### 3.16 Software / control logic

---

#### RSK-051 — Wind protection manually disabled

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Software / control logic       |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 4 — Major                      |
| Risk score        | **12** 🟠 High                 |
| LED indication    | Amber (permanent while disabled)|
| Log event type    | SETPOINT                       |

**Failure mode:** Operator sets `wind_prot_en = false` (e.g., to allow ventilation when wind sensor is faulty). T3 no longer issues CLOSE_ALL on wind threshold. A real storm may now open or leave windows open, causing physical damage.

**Chain of affected components:**
`wind_prot_en = false` → T3 suppressed → EG1.WIND_OVERRIDE cleared → T6 may open windows → actual storm → window damage or crop loss.

**Greenhouse operation impact:** All wind safety is bypassed. This is an operator choice and accepted risk, but must be clearly signalled.

**Mitigation in firmware:** When `wind_prot_en = false`, RGB LED is permanently Amber regardless of other conditions. `SETPOINT / Warning` logged with operator identity (initiator field). LCD continuously displays "WIND PROT OFF" warning on status line. The setting is not retained across power cycles unless explicitly saved by admin.

---

#### RSK-052 — Climate control runaway / oscillation

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Software / control logic       |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red                            |
| Log event type    | ALARM                          |

**Failure mode:** T6 climate control logic oscillates — repeatedly opening and closing windows faster than the hysteresis dead-band prevents. This can overheat window motors and cause excessive wear.

**Chain of affected components:**
Oscillation → rapid relay toggling → T2 enforces dwell timer per channel → if dwell timer bypassed due to bug, motor is commanded rapidly → RRK-3 overcurrent may trip → RSK-022.

**Greenhouse operation impact:** Window motor wear; potential RRK-3 overcurrent trip disabling the affected channel.

**Mitigation in firmware:** T6 uses a hysteresis band (configurable, default ±2 °C / ±5% RH). T2 enforces a minimum dwell timer between opposite commands per channel (configurable, default 10 s). T6 logs `ALARM / Error` if it issues > N commands per hour to the same channel (oscillation detection). Operator alerted via LCD and MQTT.

---

#### RSK-053 — Conflicting temperature and RH setpoints

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Software / control logic       |
| Severity          | **Warning**                    |
| Chance            | 3 — Possible                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **9** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | ALARM                          |

**Failure mode:** Temperature setpoint requires windows closed (too cold) while RH setpoint requires windows open (too humid). Both conditions are active simultaneously; T6 cannot satisfy both.

**Chain of affected components:**
Conflict detected by T6 → T6 applies priority rule (temperature safety over RH by default when `rh_ctrl_en` enabled) → logs `ALARM / Warning` → one setpoint is temporarily ignored → operator informed.

**Greenhouse operation impact:** One climate parameter (RH or temperature) is not optimally controlled during the conflict period. Not critical but may degrade crop quality.

**Mitigation in firmware:** T6 detects conflict condition; logs `ALARM / Warning` with both active setpoints and the priority decision. LCD displays conflict indicator. Design documentation must define the priority rule explicitly (see `technicalSoftwareDesignSpecification.md` §5.2).

---

### 3.17 Environmental

---

#### RSK-054 — High humidity ingress into enclosure

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Environmental                  |
| Severity          | **Error**                      |
| Chance            | 3 — Possible (greenhouse env.) |
| Effect            | 4 — Major                      |
| Risk score        | **12** 🟠 High                 |
| LED indication    | Red (if electronics damaged)   |
| Log event type    | SYSTEM                         |

**Failure mode:** Despite IP67 rating, repeated cable gland stress, UV degradation of gaskets, or improper installation allows moisture ingress. Condensation forms on PCB; corrosion or short-circuits develop.

**Chain of affected components:**
Moisture on PCB → PCB leakage current / short → erratic GPIO behaviour / relay misfires / MCU crash → any combination of above risks (RSK-001 through RSK-048) may manifest.

**Greenhouse operation impact:** Potentially any control anomaly up to complete failure.

**Mitigation:** Annual inspection of cable glands and enclosure gasket. Use IP67-rated cable glands for all entries. Apply conformal coating to PCB assembly. Install silica gel desiccant inside enclosure; replace annually. No firmware mitigation possible after ingress.

---

#### RSK-055 — Temperature extremes at controller location

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Environmental                  |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red (if electronics fail)      |
| Log event type    | SYSTEM                         |

**Failure mode:** Controller is wall-mounted inside greenhouse. Summer peak temperatures inside a glass greenhouse can exceed +55 °C. ESP32-S3 operating range is −40 to +85 °C; ABS enclosure rated to +60 °C. HLK-10M24 PSU de-rates above +50 °C ambient.

**Chain of affected components:**
High ambient → PSU thermal derating → 24 V sags → sensors drop off → T5 reports sensor faults → safe-fail window closure.

**Greenhouse operation impact:** Paradoxically, high temperature inside the controller triggers window closure (safe-fail), which reduces ventilation and may worsen the temperature problem. Controller must be mounted in shade or in a ventilated position.

**Mitigation:** Mount controller on north-facing wall or in shaded position. Avoid direct solar radiation on enclosure. Enclosure temperature should not regularly exceed +50 °C. No firmware mitigation; `SYSTEM / Warning` can be added if an internal temperature sensor is available.

---

#### RSK-056 — Vibration or mechanical shock

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Environmental                  |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SYSTEM                         |

**Failure mode:** Vibration from nearby machinery or mechanical shock (door slamming, accidental impact) causes connector or solder joint failure on PCB. SD card may be ejected.

**Chain of affected components:**
Mechanical shock → connector loosening → SD card ejected (RSK-033) / I2C connector unreliable (RSK-027, RSK-029) / RS485 connector loosening (RSK-006).

**Greenhouse operation impact:** Depends on which connector fails; could range from logging loss (RSK-031) to sensor bus fault (RSK-006).

**Mitigation:** Use screw-terminal connectors for all field-wiring. SD card socket with positive retention latch. Mounting bolts for PCB standoffs at four corners. No firmware mitigation; each resulting fault is detected by existing sensor / bus / storage error handling.

---

### 3.18 Security

---

#### RSK-057 — Unauthorized MQTT command injection

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Security                       |
| Severity          | **Error**                      |
| Chance            | 2 — Unlikely                   |
| Effect            | 4 — Major                      |
| Risk score        | **8** 🟡 Medium                |
| LED indication    | Red                            |
| Log event type    | SESSION                        |

**Failure mode:** An attacker with access to the MQTT broker (misconfigured broker, no broker authentication, compromised broker credentials) publishes window OPEN/CLOSE commands or setpoint changes to the controller's subscribed topics.

**Chain of affected components:**
Malicious MQTT message → T12 receives → posts relay command to Q1 → T2 executes → window moves → potentially open during storm or closed during heat event.

**Greenhouse operation impact:** Unauthorized window commands could lead to storm damage (windows opened) or crop heat stress (windows closed), depending on attack timing.

**Mitigation in firmware:** T12 validates all inbound MQTT messages (length, topic structure, value range). Rate-limits command acceptance (max N commands per minute per topic). Logs all inbound commands as `SESSION / Notice` (normal) or `SESSION / Error` (rate exceeded). Recommends: MQTT broker with per-client authentication; restrict command topics to authenticated clients only. Web interface uses session tokens; MQTT over TLS if broker supports it.

---

#### RSK-058 — Unauthorized web interface access

| Field             | Value                          |
|-------------------|--------------------------------|
| Domain            | Security                       |
| Severity          | **Warning**                    |
| Chance            | 2 — Unlikely                   |
| Effect            | 3 — Moderate                   |
| Risk score        | **6** 🟡 Medium                |
| LED indication    | Amber                          |
| Log event type    | SESSION                        |

**Failure mode:** An attacker on the same WiFi network accesses the web interface. Since TLS is not used (open issue #4, accepted — infeasible on ESP32-S3 for full TLS), credentials are transmitted in plaintext. Brute force or credential sniffing allows access.

**Chain of affected components:**
Unauthorized login → web session created → attacker can change setpoints, disable wind protection, issue window commands → same consequence as RSK-051 or RSK-057.

**Greenhouse operation impact:** Setpoint tampering or unauthorized window commands.

**Mitigation in firmware:** T11 implements session tokens with short TTL (15 min), account lockout after 5 failed logins (30 min lockout), and rate-limiting. Failed login attempts logged as `SESSION / Warning`. Web interface only reachable on private LAN; WiFi AP disabled by default. Admin password hashed (SHA-256 minimum) in NVS. Design note: TLS accepted as not feasible; threat model limits exposure to local network attackers only.

---

## 4. Sorted risk summary

All 58 risks ranked by score (highest first). Ties broken alphabetically by RSK-ID.

| Rank | RSK ID  | Domain                    | Short title                             | Sev     | C | E | Score | Band       |
|------|---------|---------------------------|-----------------------------------------|---------|---|---|-------|------------|
| 1    | RSK-001 | Power supply              | Complete mains failure                  | Error   | 3 | 5 | 15    | 🟠 High    |
| 2    | RSK-010 | Wind sensor (S200)        | Communication timeout                   | Error   | 3 | 5 | 15    | 🟠 High    |
| 3    | RSK-005 | Power supply              | Brownout / undervoltage                 | Warning | 3 | 4 | 12    | 🟠 High    |
| 4    | RSK-014 | T/RH sensor (FG6485A)     | Communication timeout                   | Warning | 3 | 4 | 12    | 🟠 High    |
| 5    | RSK-015 | T/RH sensor (FG6485A)     | Condensation / fouling on sensor        | Warning | 4 | 3 | 12    | 🟠 High    |
| 6    | RSK-022 | Window / motor system     | Window mechanically stuck               | Error   | 3 | 4 | 12    | 🟠 High    |
| 7    | RSK-046 | FreeRTOS tasks            | Inter-task queue overflow               | Warning | 3 | 4 | 12    | 🟠 High    |
| 8    | RSK-051 | Software / control logic  | Wind protection manually disabled       | Warning | 3 | 4 | 12    | 🟠 High    |
| 9    | RSK-054 | Environmental             | High humidity ingress into enclosure    | Error   | 3 | 4 | 12    | 🟠 High    |
| 10   | RSK-002 | Power supply              | AC-DC PSU failure                       | Error   | 2 | 5 | 10    | 🟡 Medium  |
| 11   | RSK-003 | Power supply              | DC-DC buck converter failure            | Error   | 2 | 5 | 10    | 🟡 Medium  |
| 12   | RSK-006 | RS485 / Modbus bus        | Bus short or open circuit               | Error   | 2 | 5 | 10    | 🟡 Medium  |
| 13   | RSK-012 | Wind sensor (S200)        | Total sensor failure                    | Error   | 2 | 5 | 10    | 🟡 Medium  |
| 14   | RSK-013 | Wind sensor (S200)        | Physical mounting / exposure damage     | Error   | 2 | 5 | 10    | 🟡 Medium  |
| 15   | RSK-045 | FreeRTOS tasks            | Task stack overflow                     | Error   | 2 | 5 | 10    | 🟡 Medium  |
| 16   | RSK-048 | FreeRTOS tasks            | Deadlock between tasks                  | Error   | 2 | 5 | 10    | 🟡 Medium  |
| 17   | RSK-007 | RS485 / Modbus bus        | Missing / wrong termination             | Warning | 3 | 3 | 9     | 🟡 Medium  |
| 18   | RSK-008 | RS485 / Modbus bus        | Persistent CRC errors                   | Warning | 3 | 3 | 9     | 🟡 Medium  |
| 19   | RSK-023 | Window / motor system     | RRK-3 feedback signal loss              | Warning | 3 | 3 | 9     | 🟡 Medium  |
| 20   | RSK-053 | Software / control logic  | Conflicting T / RH setpoints            | Warning | 3 | 3 | 9     | 🟡 Medium  |
| 21   | RSK-011 | Wind sensor (S200)        | Out-of-range / implausible value        | Warning | 2 | 4 | 8     | 🟡 Medium  |
| 22   | RSK-017 | T/RH sensor (FG6485A)     | Total sensor failure                    | Warning | 2 | 4 | 8     | 🟡 Medium  |
| 23   | RSK-018 | Relay outputs             | Relay contacts stuck closed             | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 24   | RSK-024 | Window / motor system     | Motor overcurrent / overload            | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 25   | RSK-025 | Window / motor system     | Conflicting OPEN + CLOSE relay commands | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 26   | RSK-026 | RTC (DS1307)              | Backup battery dead                     | Notice  | 4 | 2 | 8     | 🟡 Medium  |
| 27   | RSK-031 | SD card storage           | SD card absent                          | Notice  | 4 | 2 | 8     | 🟡 Medium  |
| 28   | RSK-035 | NVS / internal flash      | Configuration corruption                | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 29   | RSK-042 | OTA / web server          | OTA flash interrupted mid-write         | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 30   | RSK-047 | FreeRTOS tasks            | Hardware watchdog timeout / MCU reset   | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 31   | RSK-052 | Software / control logic  | Climate control runaway / oscillation   | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 32   | RSK-055 | Environmental             | Temperature extremes at controller      | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 33   | RSK-057 | Security                  | Unauthorized MQTT command injection     | Error   | 2 | 4 | 8     | 🟡 Medium  |
| 34   | RSK-016 | T/RH sensor (FG6485A)     | Out-of-range / implausible value        | Warning | 2 | 3 | 6     | 🟡 Medium  |
| 35   | RSK-019 | Relay outputs             | Relay contacts stuck open               | Warning | 2 | 3 | 6     | 🟡 Medium  |
| 36   | RSK-027 | RTC (DS1307)              | I2C bus failure                         | Warning | 2 | 3 | 6     | 🟡 Medium  |
| 37   | RSK-028 | RTC (DS1307)              | Excessive clock drift                   | Notice  | 3 | 2 | 6     | 🟡 Medium  |
| 38   | RSK-032 | SD card storage           | SD card full                            | Warning | 3 | 2 | 6     | 🟡 Medium  |
| 39   | RSK-034 | SD card storage           | File corruption on power loss           | Notice  | 3 | 2 | 6     | 🟡 Medium  |
| 40   | RSK-044 | OTA / web server          | Automatic OTA rollback triggered        | Warning | 2 | 3 | 6     | 🟡 Medium  |
| 41   | RSK-049 | Keypad / UI               | Key stuck / debounce failure            | Warning | 3 | 2 | 6     | 🟡 Medium  |
| 42   | RSK-056 | Environmental             | Vibration / shock damage                | Warning | 2 | 3 | 6     | 🟡 Medium  |
| 43   | RSK-058 | Security                  | Unauthorized web interface access       | Warning | 2 | 3 | 6     | 🟡 Medium  |
| 44   | RSK-004 | Power supply              | LDO failure / overload                  | Error   | 1 | 5 | 5     | 🟢 Low     |
| 45   | RSK-029 | LCD display               | I2C failure — display blank             | Warning | 2 | 2 | 4     | 🟢 Low     |
| 46   | RSK-033 | SD card storage           | SD card mount failure                   | Warning | 2 | 2 | 4     | 🟢 Low     |
| 47   | RSK-036 | NVS / internal flash      | NVS schema migration failure            | Error   | 1 | 4 | 4     | 🟢 Low     |
| 48   | RSK-037 | WiFi / network            | No WiFi connection available            | Notice  | 4 | 1 | 4     | 🟢 Low     |
| 49   | RSK-043 | OTA / web server          | LittleFS web asset corruption           | Warning | 2 | 2 | 4     | 🟢 Low     |
| 50   | RSK-009 | RS485 / Modbus bus        | Address collision on bus                | Warning | 1 | 3 | 3     | 🟢 Low     |
| 51   | RSK-020 | Relay outputs             | Driver MOSFET failure                   | Warning | 1 | 3 | 3     | 🟢 Low     |
| 52   | RSK-021 | Relay outputs             | Flyback diode failure                   | Warning | 1 | 3 | 3     | 🟢 Low     |
| 53   | RSK-038 | WiFi / network            | DHCP / DNS failure                      | Notice  | 3 | 1 | 3     | 🟢 Low     |
| 54   | RSK-039 | MQTT                      | MQTT broker unreachable                 | Notice  | 3 | 1 | 3     | 🟢 Low     |
| 55   | RSK-041 | MQTT                      | MQTT publish failure / queue full       | Notice  | 3 | 1 | 3     | 🟢 Low     |
| 56   | RSK-030 | LCD display               | Backlight failure                       | Notice  | 2 | 1 | 2     | 🟢 Low     |
| 57   | RSK-040 | MQTT                      | MQTT authentication failure             | Notice  | 2 | 1 | 2     | 🟢 Low     |
| 58   | RSK-050 | Keypad / UI               | Keypad scan GPIO failure                | Warning | 1 | 2 | 2     | 🟢 Low     |

---

## 5. Mitigation recommendations

The following ten risks carry the highest scores or represent safety-critical failure modes that merit explicit design attention during firmware implementation.

---

### 5.1 RSK-001 / RSK-010 — Mains failure and wind sensor timeout (score 15)

These are the two highest-risk items and both funnel into the same safe-fail response.

**Recommended actions:**
1. Implement a startup safety sequence: immediately after boot, T5 must complete at least one successful sensor poll before T2 releases the CLOSE_ALL hold.
2. Persist the last known window state in NVS on each relay command so that after a power cycle T2 knows which windows were open and can log the gap.
3. Consider a hardware capacitor on the 24 V rail (1000 µF electrolytic) to keep sensors alive for 200 ms longer than the MCU during brownout, allowing a controlled shutdown log entry.

---

### 5.2 RSK-005 — Brownout / undervoltage (score 12)

**Recommended actions:**
1. Log `ESP_RST_BROWNOUT` reason on every boot and send a `SYSTEM / Warning` event over MQTT on the first reconnect after brownout.
2. Investigate and address root cause (relay switching transients on 5 V rail). Add 1000 µF bulk capacitance on 5 V rail and a soft-start relay sequencer if needed.

---

### 5.3 RSK-014 / RSK-015 — T/RH sensor faults (score 12)

**Recommended actions:**
1. Implement the RSK-015 saturation detection heuristic: if RH = 99.9 % for > 60 consecutive polls (60 min at 1-min poll rate), log a `SENSOR / Warning` event.
2. Document a maintenance schedule for sensor cleaning (every 3 months) in the installation and maintenance guide.
3. Provide a replacement sensor filter cap accessory in the BOM.

---

### 5.4 RSK-022 — Window mechanically stuck (score 12)

**Recommended actions:**
1. Complete open issue #1: implement RRK-3 opto-coupler feedback edge detection and debounce in T2.
2. Add run-time monitoring: if a channel command is issued but expected movement completion time elapses without a feedback edge, log `RELAY / Warning`.
3. Provide a per-channel fault flag in the data model that T8 displays on the LCD with the channel number.

---

### 5.5 RSK-046 — Inter-task queue overflow (score 12)

**Recommended actions:**
1. Q1 (relay commands) must use a blocking send with a short timeout for all safety-critical messages (CLOSE_ALL, sensor fault close). Non-blocking send is only acceptable for informational commands.
2. During integration testing, inject queue overflow conditions intentionally (fill Q1) and verify that CLOSE_ALL is still delivered within 200 ms.
3. Expose queue high-watermark statistics via web interface for commissioning diagnostics.

---

### 5.6 RSK-051 — Wind protection disabled (score 12)

**Recommended actions:**
1. When `wind_prot_en = false`, display a permanent warning on every LCD screen, not just the status page.
2. Log the disabling event with the operator's user role and timestamp.
3. Add an automatic re-enable timer: after 24 hours with `wind_prot_en = false` (and no active re-confirmation), automatically re-enable wind protection and log the re-enable event.

---

### 5.7 RSK-054 — Humidity ingress into enclosure (score 12)

**Recommended actions:**
1. Add conformal coating (acrylic or silicone) to the assembled PCB as a standard manufacturing step.
2. Include a silica gel desiccant pack inside the enclosure.
3. Document inspection interval in the maintenance guide (annual).
4. Torque specifications for cable glands should be included in the installation guide.

---

### 5.8 RSK-045 / RSK-048 — Stack overflow and deadlock (score 10)

**Recommended actions:**
1. Enable FreeRTOS stack overflow checking method 2 during all development and testing builds. Keep it enabled in production builds.
2. Establish and document the strict mutex acquisition order: MX1 → MX2 → MX3 → MX4 → MX5. Include this in the code review checklist.
3. Add a startup diagnostic that logs each task's stack high-watermark to the event log as `SYSTEM / Notice`. This provides early warning before stack overflow occurs.

---

### 5.9 RSK-035 — NVS configuration corruption (score 8)

**Recommended actions:**
1. Use a double-write pattern for critical setpoints: write the new value, read it back, verify before returning success.
2. Store a checksum or redundant copy of the most critical safety parameters (`v_max`, `wind_prot_en`, `t_max`) in a separate NVS key.
3. On corruption detection, before loading defaults, attempt to restore from the redundant copy.

---

### 5.10 RSK-057 — Unauthorized MQTT command injection (score 8)

**Recommended actions:**
1. Require MQTT broker authentication (username/password minimum) as part of commissioning; document this in the installation guide.
2. Implement command rate-limiting in T12: no more than 10 relay commands per minute from any MQTT source. Excess commands logged as `SESSION / Error`.
3. Log all inbound MQTT commands (topic + payload truncated to 64 chars) as `SESSION / Notice`.

---

*End of document — Risk Assessment v0.1 — 2026-04-15*

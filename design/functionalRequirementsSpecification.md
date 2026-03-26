# Functional Requirements Specification

## Greenhouse Ventilation Controller

| Field         | Value                          |
|---------------|-------------------------------|
| Document      | Functional Requirements Specification |
| Project       | Greenhouse Ventilation Controller |
| Version       | 0.2 (draft)                   |
| Date          | 2026-03-26                    |
| Status        | Draft                         |

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [System Overview](#2-system-overview)
3. [Stakeholders and User Roles](#3-stakeholders-and-user-roles)
4. [Physical Context](#4-physical-context)
5. [Functional Requirements](#5-functional-requirements)
   - 5.1 Sensing — Internal Climate
   - 5.2 Sensing — External Weather
   - 5.3 Window Actuation
   - 5.4 Automatic Climate Control
   - 5.5 Wind Safety
   - 5.6 Conflict Resolution
   - 5.7 Window State Tracking
   - 5.8 Operating Modes
   - 5.9 Local User Interface (Keyboard & Display)
   - 5.10 Configuration and Settings
   - 5.11 WiFi Connectivity
   - 5.12 MQTT Integration
   - 5.13 Access Control and Security
   - 5.14 Logging
6. [Technical Requirements](#6-technical-requirements)
7. [Constraints and Assumptions](#7-constraints-and-assumptions)
8. [MoSCoW Priority Reference](#8-moscow-priority-reference)

---

## 1. Introduction

### 1.1 Purpose
This document describes the functional and technical requirements for the greenhouse ventilation controller. It serves as the primary reference for design, implementation, and verification of the system.

### 1.2 Scope
The controller manages three motorised ventilation windows in a single greenhouse to regulate internal temperature and relative humidity. Control is achieved solely through ventilation; there is no heating, cooling, humidification, or dehumidification equipment.

### 1.3 Definitions

| Term | Definition |
|------|------------|
| M1 | Roof window, south slope (Dakbeluchting Zuid) |
| M2 | Roof window, north slope (Dakbeluchting Noord) |
| M3 | Side wall window, north wall (Zijwandbeluchting) |
| RH | Relative Humidity (%) |
| T | Temperature (°C) |
| RRK-3 | Hotraco RRK-3 three-channel window relay box |
| Farmer | The daily operator who sets climate setpoints |
| Administrator | The technical user who configures system parameters |
| MoSCoW | Prioritisation method: Must / Should / Could / Won't |
| MQTT | Message Queuing Telemetry Transport — lightweight IoT messaging protocol |
| AP | Access Point (WiFi) |

---

## 2. System Overview

The controller reads internal climate conditions (temperature and humidity) and external weather conditions (wind speed and wind direction). Based on configured setpoints and safety thresholds it opens or closes three motorised ventilation windows to bring the internal climate within the acceptable range defined by the farmer.

The controller is operated locally via a 4×4 keyboard and a 16×2 LCD display. Optionally, it can be accessed over WiFi, and can publish status data to an MQTT broker.

### 2.1 Context Diagram

```
┌───────────────────────────────────────────────────────────┐
│                        Greenhouse                         │
│                                                           │
│   [Temp/RH sensor] ──────────────────┐                    │
│                                      ▼                    │
│   [4×4 Keyboard] ──────► [Controller] ──► [RRK-3] ──► M1  │
│   [16×2 LCD]     ◄──────             │            ──► M2  │
│                                      │            ──► M3  │
└──────────────────────────────────────┼───────────────────-┘
                                       │
   [Wind speed meter] (outside) ───────┤
   [Wind direction meter] (outside) ───┘
                                       │
                              (optional WiFi)
                                       │
                              [WiFi client / browser]
                              [MQTT broker]
```

---

## 3. Stakeholders and User Roles

| Role | Description |
|------|-------------|
| **Farmer** | Daily operator. Sets acceptable temperature and humidity ranges for the crop. Views current status. |
| **Administrator** | Technical configurator. Sets wind safety thresholds, wind direction exclusion angle, network settings, and system parameters. Has elevated access rights. |
| **Maintenance technician** | Not a software user; works on the physical hardware. Requirements for this role are not in scope. |

---

## 4. Physical Context

### 4.1 Greenhouse Layout

The greenhouse is rectangular (40 m × 16 m), oriented with the long axis east–west. The north long wall is on the left when facing east. The roof is gabled; the ridge runs east–west.

### 4.2 Windows

| ID | Dutch name | Location | Opening area |
|----|-----------|----------|-------------|
| M1 | Dakbeluchting Zuid | South roof slope, full length | 8 m² |
| M2 | Dakbeluchting Noord | North roof slope, full length | 8 m² |
| M3 | Zijwandbeluchting | North wall (side), full length | 80 m² |

### 4.3 Motor Run-Times

| Window | Closed → Open | Open → Closed |
|--------|--------------|--------------|
| M1 | 21 s | 21 s |
| M2 | 21 s | 21 s |
| M3 | 171 s | 171 s |

### 4.4 Motor Interface

Windows are driven by a Hotraco RRK-3 relay box. The controller sends an OPEN or CLOSE pulse (24 V potential-free contact) per window. End-switches are wired to the RRK-3 directly and stop the motor automatically at the fully-open and fully-closed positions. **The controller receives no end-switch feedback and has no direct knowledge of the actual window position.**

> **⚠ OPEN ISSUE — Motor feedback:** It is currently unknown whether and how any status or feedback signal from the Hotraco RRK-3 (e.g. motor fault, alarm relay output) can be wired back to the controller. This must be resolved during detailed electrical design. Depending on the outcome, additional input requirements may be added. See also Constraint C8.

---

## 5. Functional Requirements

### 5.1 Sensing — Internal Climate

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-S01 | The system **shall** measure the internal greenhouse temperature. | Must |
| FR-S02 | The system **shall** measure the internal greenhouse relative humidity. | Must |
| FR-S03 | The system **shall** sample temperature and humidity at a configurable interval (default: 60 s). | Should |
| FR-S04 | The system **shall** detect and report a sensor fault (e.g. disconnected or out-of-range sensor). | Must |
| FR-S05 | On a sensor fault, the system **shall** maintain the last known window states and alert the user. | Must |

### 5.2 Sensing — External Weather

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-W01 | The system **shall** read wind speed from the external wind meter. | Must |
| FR-W02 | The system **shall** read wind direction from the external wind meter. | Must |
| FR-W03 | The system **shall** detect and report a fault on the wind sensor. | Must |
| FR-W04 | On a wind sensor fault, the system **shall** close all windows as a safe default. | Must |

### 5.3 Window Actuation

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-A01 | The system **shall** be able to command each window (M1, M2, M3) to fully open. | Must |
| FR-A02 | The system **shall** be able to command each window (M1, M2, M3) to fully close. | Must |
| FR-A03 | Window commands **shall** be issued as timed relay pulses for the duration of full opening or closing; the motor is stopped automatically by the RRK-3 end-switches. | Must |
| FR-A04 | The system **shall** not issue an OPEN and CLOSE command simultaneously to the same window. | Must |
| FR-A05 | The system **shall** maintain an estimated state for each window: `OPEN`, `CLOSED`, or `MOVING`. | Must |
| FR-A06 | After issuing a command, the system **shall** set the estimated state to `MOVING` and transition to the target state after the known motor run-time has elapsed. | Must |
| FR-A07 | The system **should** support partial window opening by timed motor stop (percentage of full travel). | Could |
| FR-A08 | If partial opening is implemented, the system **shall** clearly indicate to the user that the position is an estimate, not a measured value. | Could |
| FR-A09 | The administrator **shall** be able to configure a dwell time per window — the minimum time a window must remain fully open before it may be commanded to close. | Must |
| FR-A10 | The administrator **shall** be able to configure a dwell time per window — the minimum time a window must remain fully closed before it may be commanded to open. | Must |
| FR-A11 | The system **shall** not issue a new open or close command to a window until the applicable dwell time has elapsed since the window reached its last end position. | Must |
| FR-A12 | Dwell times **shall** be configurable independently for each window (M1, M2, M3) and independently for the open-dwell and close-dwell directions. | Should |

> **Note on FR-A07/FR-A08:** The technical specification explicitly cautions that partial opening via timed stop is unreliable without position feedback. Implementation is a "Could have" and requires an explicit design decision before the feature is included.

> **Note on FR-A09–FR-A12 (dwell time):** Dwell time prevents the motors from being cycled too rapidly, protecting mechanical components and reducing wear. The dwell timer starts when the estimated end-position state is entered (i.e. after the motor run-time has elapsed), not when the command is issued.

### 5.4 Automatic Climate Control

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-C01 | The farmer **shall** be able to set a minimum acceptable temperature (T_min). | Must |
| FR-C02 | The farmer **shall** be able to set a maximum acceptable temperature (T_max). | Must |
| FR-C03 | The farmer **shall** be able to set a minimum acceptable relative humidity (RH_min). | Must |
| FR-C04 | The farmer **shall** be able to set a maximum acceptable relative humidity (RH_max). | Must |
| FR-C05 | When the measured temperature exceeds T_max, the system **shall** open one or more windows to lower the temperature. | Must |
| FR-C06 | When the measured relative humidity exceeds RH_max, the system **shall** open one or more windows to lower the humidity. | Must |
| FR-C07 | When the measured temperature is below T_min, the system **shall** close windows to reduce heat loss. | Must |
| FR-C08 | When the measured relative humidity is below RH_min, the system **shall** close windows to reduce moisture loss. | Must |
| FR-C09 | The system **should** use a graduated ventilation strategy — opening additional windows as the deviation from setpoint increases — rather than opening all windows at once. | Should |
| FR-C10 | The system **should** apply hysteresis to window open/close decisions to prevent rapid toggling (short-cycling). | Should |

### 5.5 Wind Safety

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-WS01 | The administrator **shall** be able to set a maximum wind speed threshold (v_max, in m/s or Beaufort scale). | Must |
| FR-WS02 | When the measured wind speed exceeds v_max, the system **shall** immediately close all windows, overriding the climate control logic. | Must |
| FR-WS03 | The administrator **shall** be able to define a wind direction exclusion zone as a centre angle and a half-width (e.g. "close if wind is within ±30° of 315°N"). | Must |
| FR-WS04 | When the measured wind direction falls within the configured exclusion zone, the system **shall** close all windows, overriding the climate control logic. | Must |
| FR-WS05 | Wind safety closures **shall** take priority over all other window commands, including manual commands. | Must |
| FR-WS06 | When a wind safety override is active, the system **shall** show a dedicated wind-override alarm message on the LCD display, indicating which condition triggered the override (wind speed or wind direction). This indication **shall** remain visible on the display for as long as the override is active. | Must |
| FR-WS07 | When wind conditions return to safe values, the system **shall** resume automatic climate control. | Must |
| FR-WS08 | The administrator **should** be able to set a minimum duration that wind must be within safe limits before windows are re-opened (wind hysteresis timer). | Should |

### 5.6 Conflict Resolution

When temperature and humidity call for opposing window actions (e.g. temperature too high calls for opening, but humidity is already too low), a conflict exists.

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-CR01 | The system **shall** implement a defined conflict resolution strategy when temperature and humidity setpoints require opposing window actions. | Must |
| FR-CR02 | The default conflict resolution strategy **shall** prioritise the condition with the greater relative deviation from its setpoint. | Should |
| FR-CR03 | The administrator **shall** be able to configure the conflict resolution priority (T takes priority / RH takes priority / deviation-based). | Could |
| FR-CR04 | The system **shall** log or display a conflict event so the farmer is aware of the trade-off being made. | Should |

### 5.7 Window State Tracking

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-ST01 | The system **shall** maintain a software-tracked estimated state for each window: `OPEN`, `CLOSED`, or `MOVING`. | Must |
| FR-ST02 | On power-on or controller restart, the system **shall** command all windows to fully close before entering normal operation. This ensures the actual and estimated window states are synchronised at a known position. | Must |
| FR-ST03 | The system **shall** clearly label displayed window states as "estimated" since no physical position feedback is available. | Must |
| FR-ST04 | The system **should** provide a manual calibration command (accessible via the keyboard menu) that drives all windows to the fully-closed position (end-stop) to re-synchronise estimated and actual state at any time. | Should |

### 5.8 Operating Modes

> **Scope note:** Manual window control (physically opening or closing individual windows) is **outside the scope** of this controller. Manual window operation is performed directly on the Hotraco RRK-3 motor relay box. The controller provides only two operating modes for its own automatic control logic.

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-M01 | The controller **shall** support two operating modes: **Automatic** and **Standby**. | Must |
| FR-M02 | In **Automatic** mode, the controller **shall** continuously evaluate climate conditions and issue window commands according to the control logic. | Must |
| FR-M03 | In **Standby** mode, the controller **shall** suspend all automatic climate control commands; no open or close commands are issued by the control logic. | Must |
| FR-M04 | In **Standby** mode, wind safety logic **shall** remain fully active; the controller **shall** still issue close commands when wind conditions exceed safe thresholds. | Must |
| FR-M05 | The farmer **shall** be able to switch between Automatic and Standby mode via the local keyboard. | Must |
| FR-M06 | The display **shall** clearly indicate the current operating mode (AUTO / STANDBY) at all times. | Must |
| FR-M07 | A mode change **shall** be logged with a timestamp and the identity of the operator who initiated the change (see §5.14). | Must |

### 5.9 Local User Interface (Keyboard & Display)

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-UI01 | The controller **shall** have a 4×4 matrix keyboard for local input. | Must |
| FR-UI02 | The controller **shall** have a 16×2 character LCD display. | Must |
| FR-UI03 | The display **shall** show, in normal operation: current temperature, current humidity, and current operation mode (auto/manual). | Must |
| FR-UI04 | The display **shall** show the estimated state (OPEN/CLOSED/MOVING) of each window. | Must |
| FR-UI05 | The display **shall** show an alarm indication when a sensor fault or wind safety event is active. | Must |
| FR-UI06 | The keyboard **shall** allow navigation through a menu structure to access settings, mode switching, and status views. | Must |
| FR-UI07 | Menu navigation **shall** be achievable without requiring more than 4 key presses from the main screen to any first-level setting. | Should |
| FR-UI08 | The display **should** show the current wind speed and wind direction on a status screen. | Should |
| FR-UI09 | All prompts and labels **shall** be displayed in a language configurable by the administrator (default: Dutch). | Could |

### 5.10 Configuration and Settings

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-CF01 | The farmer **shall** be able to set T_min and T_max via the local keyboard. | Must |
| FR-CF02 | The farmer **shall** be able to set RH_min and RH_max via the local keyboard. | Must |
| FR-CF03 | The administrator **shall** be able to set the wind speed closure threshold (v_max). | Must |
| FR-CF04 | The administrator **shall** be able to set the wind direction exclusion zone (centre bearing and half-width angle). | Must |
| FR-CF05 | The administrator **shall** be able to set motor run-times per window (used for state estimation). | Must |
| FR-CF06 | All settings **shall** be stored in non-volatile memory so they are retained after a power cycle. | Must |
| FR-CF07 | The administrator **should** be able to set the sensor sampling interval. | Should |
| FR-CF08 | The administrator **should** be able to set hysteresis values for temperature and humidity control. | Should |
| FR-CF09 | The administrator **should** be able to set the wind safety hysteresis timer (FR-WS08). | Should |
| FR-CF10 | The administrator **shall** be able to set the open-dwell time for each window (M1, M2, M3) — the minimum time a window must remain open before it may be closed. | Must |
| FR-CF11 | The administrator **shall** be able to set the close-dwell time for each window (M1, M2, M3) — the minimum time a window must remain closed before it may be opened. | Must |

### 5.11 WiFi Connectivity (Optional)

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-NW01 | The controller **should** support WiFi connectivity. | Should |
| FR-NW02 | WiFi configuration **shall** be possible via a local WiFi Access Point hosted by the controller itself (captive portal or setup page). | Should |
| FR-NW03 | When connected to WiFi, the farmer **should** be able to view current status (T, RH, window states, alarms) from a web browser. | Should |
| FR-NW04 | When connected to WiFi, the farmer **should** be able to set climate setpoints (T_min, T_max, RH_min, RH_max) via the web interface. | Should |
| FR-NW05 | When connected to WiFi, the administrator **should** be able to configure all system settings via the web interface. | Should |
| FR-NW06 | The web interface **shall** require authentication (username and password) before any information is displayed or settings are changed. | Should |
| FR-NW07 | WiFi connectivity **shall** be optional; the controller **shall** operate fully without WiFi. | Must |

### 5.12 MQTT Integration (Optional)

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-MQ01 | The controller **could** connect to a user-configured MQTT broker when WiFi is available. | Could |
| FR-MQ02 | The controller **should** publish current temperature, humidity, window states, and alarm status to the MQTT broker at regular intervals. | Could |
| FR-MQ03 | The controller **could** accept window commands and setpoint changes via subscribed MQTT topics. | Could |
| FR-MQ04 | MQTT broker address, port, and credentials **shall** be configurable by the administrator. | Could |
| FR-MQ05 | MQTT connectivity **shall** be optional; the controller **shall** operate fully without an MQTT broker. | Must |

### 5.13 Access Control and Security

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-AC01 | The system **shall** support at least two user roles: Farmer and Administrator. | Must |
| FR-AC02 | Administrator functions (wind thresholds, network settings, motor parameters) **shall** be protected by an administrator password. | Must |
| FR-AC03 | The administrator **shall** be able to change the administrator password. | Must |
| FR-AC04 | The farmer role **should** be protected by a farmer PIN or password to prevent unintended setpoint changes. | Should |
| FR-AC05 | Web interface access **shall** require authentication; credentials **shall** be separate from local keyboard access. | Should |
| FR-AC06 | Passwords **shall** be stored in a hashed form; plain-text storage of passwords is not permitted. | Should |
| FR-AC07 | After a configurable number of failed login attempts, the system **should** impose a lockout delay. | Could |

### 5.14 Logging

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| FR-LG01 | The system **shall** maintain an event log. Each log entry **shall** include a timestamp (date and time) and the identity of the operator or the system component that triggered the event. | Must |
| FR-LG02 | The following events **shall** be logged: window state changes (open/close command issued), operating mode changes (AUTO ↔ STANDBY), setpoint changes, wind safety overrides (start and end), sensor faults (start and end), and controller restart. | Must |
| FR-LG03 | For events triggered by an operator action (mode change, setpoint change), the log entry **shall** record which user role (Farmer / Administrator) and, where applicable, which user account performed the action. | Must |
| FR-LG04 | For events triggered automatically by the control logic, the log entry **shall** record "SYSTEM" as the initiator and include the sensor values that triggered the event. | Must |
| FR-LG05 | The log **shall** be retrievable via the web interface (when WiFi is available) and via the serial/USB diagnostic port. | Should |
| FR-LG06 | The log **should** store a minimum of 1000 entries in non-volatile memory (ring buffer; oldest entries are overwritten when full). | Should |
| FR-LG07 | The system **could** write the event log to an SD card for extended retention and offline retrieval. | Could |
| FR-LG08 | If an SD card is present and functional, the system **should** prefer the SD card as the primary log storage; internal non-volatile memory acts as fallback. | Could |
| FR-LG09 | The log **should** include periodic sensor-value snapshots (temperature, humidity, wind speed, wind direction) at a configurable interval, to provide a climate history. | Should |

---

## 6. Technical Requirements

### 6.1 Hardware and Enclosure

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| TR-HW01 | The controller **shall** be housed in an enclosure rated IP67 or higher. | Must |
| TR-HW02 | The enclosure **shall** be suitable for wall mounting. | Must |
| TR-HW03 | All motor control signals **shall** be through potential-free relay contacts (volt-free). | Must |
| TR-HW04 | The controller **shall** provide 6 relay output channels — one OPEN and one CLOSE for each of M1, M2, and M3. | Must |
| TR-HW05 | The controller **shall** accept 24 VDC or 24 VAC control voltage for the relay interface to the RRK-3. | Must |
| TR-HW06 | The controller **shall** include a 4×4 matrix keyboard. | Must |
| TR-HW07 | The controller **shall** include a 16×2 LCD character display. | Must |
| TR-HW08 | The controller **shall** include a real-time clock (RTC) to provide accurate timestamps for the event log and sensor snapshots (see §5.14). | Must |
| TR-HW09 | The controller **should** include a WiFi-capable microcontroller or module. | Should |
| TR-HW10 | The controller **could** include an SD card slot for extended log storage. | Could |

### 6.2 Interfaces

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| TR-IF01 | The controller **shall** interface with the internal temperature and humidity sensor. | Must |
| TR-IF02 | The controller **shall** interface with the external wind speed sensor. | Must |
| TR-IF03 | The controller **shall** interface with the external wind direction sensor. | Must |
| TR-IF04 | The interface to the Hotraco RRK-3 **shall** comply with the RRK-3 control input specification (24 V, potential-free). | Must |
| TR-IF05 | The controller **should** expose a local serial/USB port for firmware updates and diagnostic access. | Should |

### 6.3 Power Supply

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| TR-PS01 | The controller **shall** operate from a standard 230 VAC / 50 Hz supply or a regulated 24 VDC supply. | Must |
| TR-PS02 | The controller **should** survive a brief power interruption (< 1 s) without resetting, using an appropriate buffer capacitor or UPS. | Could |
| TR-PS03 | On power restoration, the controller **shall** restore all settings from non-volatile memory and resume automatic operation. | Must |

### 6.4 Software and Firmware

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| TR-SW01 | All configuration settings **shall** be stored in non-volatile memory (EEPROM or flash). | Must |
| TR-SW02 | The firmware **shall** be updateable without opening the enclosure (e.g. via WiFi OTA or USB). | Should |
| TR-SW03 | The system **shall** implement a watchdog timer to recover from software lockups by performing a controlled restart. | Must |
| TR-SW04 | The system **shall** maintain a time-stamped, operator-attributed event log as defined in §5.14. | Must |
| TR-SW05 | The firmware **should** be structured to allow unit testing of the control logic independently of hardware. | Should |

### 6.5 Networking

| ID | Requirement | MoSCoW |
|----|-------------|--------|
| TR-NW01 | When WiFi is enabled, the controller **shall** support WPA2 (or higher) security. | Should |
| TR-NW02 | The controller **shall** be configurable as a WiFi client (STA) connecting to an existing network. | Should |
| TR-NW03 | The controller **shall** be able to host a local WiFi Access Point (AP mode) for initial setup. | Should |
| TR-NW04 | The web interface **shall** be served over HTTPS, or at minimum over HTTP with TLS where feasible on the target hardware. | Could |

---

## 7. Constraints and Assumptions

| # | Constraint / Assumption |
|---|------------------------|
| C1 | The only actuators are the three motorised ventilation windows. There is no heating, cooling, humidification, or dehumidification equipment. |
| C2 | The RRK-3 end-switches are not connected to the controller. The controller has no physical feedback of actual window position. All window states are estimated. |
| C3 | Partial opening via timed motor stop is unreliable without position feedback. The default design uses only fully-open and fully-closed end positions. See FR-A07/FR-A08. |
| C4 | Opening windows helps only when outside conditions (T and/or RH) are more favourable than inside. The controller has no outside temperature or humidity sensor. This is a recognised limitation. |
| C5 | The controller cannot actively raise temperature or humidity; it can only try to slow the rate of decrease by closing windows. |
| C6 | WiFi, MQTT, and SD card functionality are optional; the controller must be fully functional as a standalone unit without any of these. |
| C7 | The system is installed inside the greenhouse (IP67 enclosure required due to the greenhouse environment). |
| C8 | **OPEN ISSUE — Motor feedback:** It is currently unknown whether and how status signals from the Hotraco RRK-3 (e.g. alarm relay output) can be fed back to the controller. Additional input interface requirements may arise once this is resolved. |
| C9 | Manual window control (individual open/close of M1, M2, M3) is outside the scope of this controller. It is performed directly on the Hotraco RRK-3 motor relay box. The controller provides only automatic climate control (and wind safety override). |
| C10 | At startup, the controller commands all windows to close to establish a known baseline state. This means a power cycle will always result in a brief window-close sequence. |

---

## 8. MoSCoW Priority Reference

MoSCoW is a prioritisation technique widely used in requirements engineering and agile project management. The name is an acronym formed from the four priority categories below (the lower-case letters are added for readability). It was developed by Dai Clegg at Oracle in the 1990s and is a core technique in the Dynamic Systems Development Method (DSDM). Its purpose is to create a shared, unambiguous understanding between stakeholders and the development team about which requirements are critical for a release and which can be deferred or dropped, enabling informed trade-off decisions when time or budget is constrained.

> **Reference:** Clegg, D. & Barker, R. (1994). *CASE Method Fast-Track: A RAD Approach*. Addison-Wesley.
> A concise online overview is available at the DSDM Consortium: <https://www.dsdm.org/content/moscow-prioritisation>

| Priority | Meaning |
|----------|---------|
| **Must** | A mandatory requirement. The system is considered a failure if this is not delivered. |
| **Should** | A high-priority requirement that should be included if at all possible. |
| **Could** | A desirable requirement that will be included if time and resources allow ("nice to have"). |
| **Won't** | Explicitly out of scope for this release, but may be considered in the future. |

---

*End of document — version 0.2 draft*

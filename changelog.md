# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [Unreleased] — 2026-03-27

### Added
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
  - §4.6 Real-Time Clock: DS3231, I2C, CR2032 battery backup
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


# Greenhouse Ventilation Controller

An embedded controller for automated ventilation management of a single greenhouse. The controller reads internal climate (temperature and humidity) and external weather (wind speed and direction), and opens or closes three motorised ventilation windows to keep the climate within the setpoints configured by the farmer. A 4×4 keypad and 16×2 LCD provide local operation; optional WiFi and a built-in web GUI allow remote monitoring and configuration.

## Features

- Automatic climate control based on temperature and relative humidity setpoints (day / night)
- Three-step graduated ventilation strategy (M1 → M1+M2 → M1+M2+M3) with hysteresis and sliding-average smoothing
- Wind safety override: all windows close automatically when wind speed exceeds a configurable threshold or wind direction lies in an excluded zone
- Motor alarm handling: immediate stop on Hotraco RRK-3 alarm output, with 60 s guard + automatic CLOSE_ALL re-calibration on clearance
- Three independent motorised window channels (M1 south roof, M2 north roof, M3 north wall)
- Modbus RTU / RS485 sensors: Seeed SenseCAP S200 (wind speed + direction) and FG6485A (T/RH)
- Local user interface: 4×4 membrane keypad and 16×2 LCD with RGB status backlight (blue = OK, red = critical)
- PIN-based access control with two roles — Farmer (4-digit PIN) and Admin (8-digit PIN), salted-SHA-256 hashed
- Battery-backed DS1307 RTC for accurate timestamping; NTP sync when WiFi is available
- Event logging to internal NVS ring buffer; optional SD card for extended retention (CSV, parseable with the supplied `logparser`)
- Built-in web GUI (HTML/JS served from LittleFS) with WebSocket live status, REST API, and OTA firmware/asset updates
- Status LEDs: RGB (system status), heartbeat (firmware alive)

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | WEMOS LOLIN S3 (ESP32-S3, dual-core 240 MHz, 8 MB flash, 8 MB PSRAM) |
| Wind sensor | Seeed SenseCAP S200 — ultrasonic, Modbus RS485, 0–60 m/s |
| T/RH sensor | FG6485A — Modbus RS485, 9–36 VDC |
| RS485 transceiver | MAX485 (TTL ↔ RS485 conversion) |
| Motor relay box | Hotraco RRK-3 — three-channel, 24 V potential-free OPEN/CLOSE control + alarm output |
| Display | LCD1602 I2C (AiP31068L bridge, address 0x3E), 16×2 characters with PCA9633 RGB backlight |
| Keypad | 4×4 membrane matrix keypad |
| RTC | DS1307 (I2C, CR2032 backup, ±20 ppm) |
| SD card | SPI, FAT32 (max 32 GB) for log retention |
| Power supply | 230 VAC → 24 VDC + 24 V → 5 V buck converter |
| Enclosure | IP-rated polycarbonate, mounted in the greenhouse near the entrance |
| PCB design | KiCad EDA v8+ |

## Repository Structure

```
greenhouse-Controller/
│
├── README.md           ← this file
├── changelog.md        ← per-version firmware changes
├── license.md / LICENSE
├── contributing.md
├── code_of_conduct.md
│
├── firmware/           ← PlatformIO ESP32-S3 firmware
├── drivers/            ← Stand-alone peripheral driver projects
├── webUiMock/          ← Flask mock server for web GUI development
├── model/              ← Simulation, plant-model tuning, settings verification
├── log/                ← Logparser tool + example log files
├── test/               ← Integration tests against live hardware
├── bin/                ← Released firmware + web-asset binaries (per version)
├── hardware/           ← KiCad PCB design + fabrication outputs
├── design/             ← Functional & technical design documents
├── documentation/      ← Component datasheets, sensor & motor reference material
├── realisation/        ← Installation wiring guide
├── manual/             ← End-user manuals (boer + beheerder), Dutch, MD + PDF
├── finance/            ← Project budget and receipts
└── Archive/            ← Historical design iterations (read-only)
```

### Top-level directories — what is in each

| Directory | Contents | Start here |
|---|---|---|
| **`firmware/`** | PlatformIO project for the ESP32-S3 controller. Source in `src/`, web-asset files (HTML / JS / CSS served from LittleFS) in `data/`, configuration headers in `config/`, host-side unit tests in `test/`. Edit in VSCode with the PlatformIO extension. | [`firmware/platformio.ini`](firmware/platformio.ini), [`firmware/src/README.md`](firmware/src/README.md), [`firmware/test/README.md`](firmware/test/README.md) |
| **`drivers/`** | One PlatformIO project per peripheral driver (LCD1602_I2C, DS1307_RTC, FG6485A, modBus, sdCard, gpio, i2c, keyPad, littleFS, Lolin-S3, relay_sequence_test). Each is unit-tested on host before being integrated into the main firmware. | [`drivers/driverDevelopmentPlan.md`](drivers/driverDevelopmentPlan.md), per-driver READMEs in `drivers/<name>/` |
| **`webUiMock/`** | Lightweight Flask development server that serves the web GUI from `firmware/data/` and emulates every REST + WebSocket endpoint of the real firmware. Iterate on HTML/CSS/JS without flashing the board. | [`webUiMock/README.md`](webUiMock/README.md) |
| **`model/`** | Python software model of the controller (`simulation.py`) plus tools for plant-model calibration and settings verification. Used to tune parameters and validate firmware changes before deploying. Includes the May 2026 oscillation investigation. | [`model/README.md`](model/README.md) |
| **`log/`** | The `logparser.py` script that converts controller log CSVs (NVS ring buffer or SD-card files) into human-readable text, plus an example raw log and parsed output. | [`log/README.md`](log/README.md), [`log/logparser.md`](log/logparser.md) |
| **`test/`** | Integration tests against a live device + sensor emulator (PyTest + REST). Test-plan documents (`firmwareIntegrationTestPlan.md`, `softwareTestPlan.md`, `testPlan.md`, `manualREST.md`) and per-test scripts/results. | [`test/firmwareIntegrationTestPlan.md`](test/firmwareIntegrationTestPlan.md), [`test/softwareTestPlan.md`](test/softwareTestPlan.md) |
| **`bin/`** | Released firmware binaries and web-asset bundles, grouped per version (`bin/<version>/greenhouse-controller-<version>.bin` + `web-assets-<version>.zip`). Includes the `build_release.ps1` PowerShell script that produces a release. | [`bin/README.md`](bin/README.md) |
| **`hardware/`** | KiCad project for the PCB (`pcb/`), fabrication outputs (`fabrication/` — Gerbers, BOM, pick-and-place), Modbus RS485 cable junction design (`ModBusRS485CableJunction/`), and bench-test setup (`Testing/`). | [`hardware/pcb/README.md`](hardware/pcb/README.md), [`hardware/fabrication/README.md`](hardware/fabrication/README.md) |
| **`design/`** | Functional & technical design documents that drove the implementation: requirements, architecture, LCD GUI design, default-settings rationale, FreeRTOS task analysis, web-GUI mouseover specs, version-control discussions, etc. | [`design/functionalRequirementsSpecification.md`](design/functionalRequirementsSpecification.md), [`design/LCD_GUI_Design.md`](design/LCD_GUI_Design.md), [`design/defalutSettingsMotivation.md`](design/defalutSettingsMotivation.md) |
| **`documentation/`** | Reference material from third parties and component vendors: sensor datasheets (`Sensors/`), motor docs (`Motors/`), ventilation system reference (`VentilationSystem/`), and Dutch agronomy literature on greenhouse climate. | [`documentation/Sensors/sensors.md`](documentation/Sensors/sensors.md), [`documentation/regelingOverwegingen.md`](documentation/regelingOverwegingen.md) |
| **`realisation/`** | Field-installation guide: how to wire all external components (sensors, motor box, mains, network) to the PCB connectors. | [`realisation/installation.md`](realisation/installation.md) |
| **`manual/`** | End-user manuals in **Dutch**, both as Markdown source and as printable PDF: a Farmer manual for daily use, and a Beheerder (Admin) manual for installation, configuration, maintenance and diagnosis. | [`manual/boerHandleiding.md`](manual/boerHandleiding.md), [`manual/beheerderHandleiding.md`](manual/beheerderHandleiding.md) |
| **`finance/`** | Project budget and receipts. Internal-use material; not relevant for using or building the controller. | – |
| **`Archive/`** | Historical first-iteration design and simulation work, kept read-only as reference. | [`Archive/Iteration1/design.md`](Archive/Iteration1/design.md) |

## Windows

The greenhouse has three motorised ventilation windows, each driven by one channel of the Hotraco RRK-3 motor relay box.

| ID | Location | Motor run-time (open / close) | Opening area |
|----|----------|------------------------------|-------------|
| M1 | South roof slope (Dakbeluchting Zuid) | 21 s / 21 s | 8 m² |
| M2 | North roof slope (Dakbeluchting Noord) | 21 s / 21 s | 8 m² |
| M3 | North wall side window (Zijwandbeluchting) | 171 s / 171 s | 80 m² |

The controller does not have window-position feedback; it tracks position internally based on issued OPEN/CLOSE commands and the configured travel times. After every power-cycle a CLOSE_ALL calibration runs automatically (~3 minutes) to re-establish a known position.

## End-User Documentation (Dutch)

The two end-user manuals in `manual/` are the primary handover documents for greenhouse owners and their technical caretakers. They are kept in lock-step with the firmware version.

| Audience | Markdown source | Printable PDF |
|---|---|---|
| **Farmer / kasgebruiker** (daily use) | [`manual/boerHandleiding.md`](manual/boerHandleiding.md) | [`manual/boerHandleiding.pdf`](manual/boerHandleiding.pdf) |
| **Technical Beheerder / installer** (setup, configuration, maintenance, diagnosis) | [`manual/beheerderHandleiding.md`](manual/beheerderHandleiding.md) | [`manual/beheerderHandleiding.pdf`](manual/beheerderHandleiding.pdf) |
| Quick reference card for the farmer | [`manual/boerQuickRef.md`](manual/boerQuickRef.md) | [`manual/boerQuickRef.pdf`](manual/boerQuickRef.pdf) |

PDFs are generated from the Markdown sources by [`manual/build_pdf.py`](manual/build_pdf.py) — run `python manual/build_pdf.py` from the repo root. Requires Python 3.10+, the `markdown` package, and Microsoft Edge.

## Developer Documentation

| Document | Description |
|----------|-------------|
| [Functional Requirements Specification](design/functionalRequirementsSpecification.md) | What the system must do — all functional and technical requirements |
| [LCD GUI Design](design/LCD_GUI_Design.md) | Screen layouts, menu structure, FSM, user-interaction patterns |
| [Default settings motivation](design/defalutSettingsMotivation.md) | Why the firmware factory defaults are what they are |
| [Driver Development Plan](drivers/driverDevelopmentPlan.md) | Per-driver development roadmap and verification approach |
| [Firmware Implementation Plan & Results](firmware/firmwareImplementationPlan.md) | Phased implementation plan, with results in `firmware/firmwareImplementationResults.md` |
| [Software Test Plan](test/softwareTestPlan.md) + [Firmware Integration Test Plan](test/firmwareIntegrationTestPlan.md) | Test cases and verification matrix |
| [Model & Simulation README](model/README.md) | Plant-model calibration, settings verification workflow, oscillation example |
| [Logparser manual](log/logparser.md) | CSV log format reference and `logparser.py` usage |
| [Web UI Mock Server](webUiMock/README.md) | How to develop the web GUI without the device |
| [Installation Wiring Guide](realisation/installation.md) | Field wiring of sensors, motor box, mains and network |
| [Changelog](changelog.md) | Per-version firmware change log |

## Getting Started — Build & Flash the Firmware

### Prerequisites

- [Visual Studio Code](https://code.visualstudio.com/) with the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Git
- A WEMOS LOLIN S3 board, or the assembled controller PCB

### Build and Flash

```bash
git clone https://github.com/<your-org>/greenhouse-Controller.git
cd greenhouse-Controller
```

1. Open the `firmware/` folder in VS Code (PlatformIO will detect `platformio.ini` automatically).
2. Connect the LOLIN S3 board via USB-C.
3. Click **Upload** in the PlatformIO toolbar (or run `pio run -t upload` from `firmware/`).
4. Open the **Serial Monitor** (115200 baud) to view startup diagnostics.
5. To upload the web assets to LittleFS: PlatformIO → "Upload Filesystem Image" (or `pio run -t uploadfs`).

### First-time configuration

After a fresh flash or factory reset, the Beheerder configures the controller via:
- **LCD keypad** for first-time PIN entry, WiFi AP activation and date/time setting,
- **web GUI** (over the WiFi AP, then over the home WiFi) for everything else.

The full installation procedure is in [`manual/beheerderHandleiding.md`](manual/beheerderHandleiding.md) §11 and §13.

### Flashing a pre-built release

Released firmware and web-asset bundles for every version are in `bin/<version>/`. See [`bin/README.md`](bin/README.md) for the upload procedure (esptool / web-OTA / PlatformIO).

## License

See [`license.md`](license.md) for full details.

**Software** (firmware and all code): Source-available, non-commercial. Free to use and modify for personal/non-commercial purposes; redistribution and commercial use are not permitted.

**Hardware design, documentation, and images**: Licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.

<a rel="license" href="https://creativecommons.org/licenses/by-nc-nd/4.0/"><img alt="Creative Commons License" style="border-width:0" src="https://i.creativecommons.org/l/by-nc-nd/4.0/88x31.png" /></a><br />Hardware design, documentation, and images are licensed under a <a rel="license" href="https://creativecommons.org/licenses/by-nc-nd/4.0/">Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License</a>.

## Disclaimer

This project is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

# Greenhouse Ventilation Controller

An embedded controller for automated ventilation management of a single greenhouse. The controller reads internal climate (temperature and humidity) and external weather (wind speed and direction), and opens or closes three motorised ventilation windows to keep the climate within the setpoints configured by the farmer. A 4×4 keypad and 16×2 LCD provide local operation; optional WiFi and MQTT integration allow remote monitoring.

## Features

- Automatic climate control based on temperature and relative humidity setpoints
- Wind safety: all windows close automatically when wind speed exceeds a configurable threshold
- Three independent motorised window channels (M1 south roof, M2 north roof, M3 north wall)
- Interface to Hotraco RRK-3 three-channel relay box via potential-free relay contacts
- Modbus RTU / RS485 sensors: Seeed SenseCAP S200 (wind speed + direction) and FG6485A (T/RH)
- Local user interface: 4×4 membrane keypad and Waveshare LCD1602 I2C display
- Battery-backed DS3231 RTC for accurate timestamping
- Event logging to internal NVS flash; optional SD card for extended retention
- Optional WiFi connectivity and MQTT telemetry
- Status LEDs: power indicator, firmware heartbeat, per-relay activity

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | WEMOS LOLIN S3 (ESP32-S3, dual-core 240 MHz, 16 MB flash, 8 MB PSRAM) |
| Wind sensor | Seeed SenseCAP S200 — ultrasonic, Modbus RS485, 0–60 m/s |
| T/RH sensor | FG6485A — Modbus RS485, 9–36 VDC |
| RS485 transceiver | MAX485 or equivalent (TTL ↔ RS485 conversion) |
| Motor relay box | Hotraco RRK-3 — three-channel, 24 V potential-free OPEN/CLOSE control |
| Relay output board | 6-channel, opto-isolated, 5 V coil |
| Display | Waveshare LCD1602 I2C (PCF8574 expander), 16×2 characters |
| Keypad | 4×4 membrane matrix keypad |
| RTC | DS3231 (I2C, CR2032 backup, ±2 ppm) |
| Power supply | 230 VAC → 24 VDC (Hi-Link HLK-10M24 or equivalent) + 24 V → 5 V buck converter |
| Enclosure | Multicomp Pro MC001110, 222 × 146 × 55 mm, IP67, transparent polycarbonate cover |
| PCB design | KiCad EDA v8+ |

## Repository Structure

```
greenhouse-controller/
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
├── design/                     ← Markdown design documents
│   ├── functionalRequirementsSpecification.md
│   ├── technicalDesignSpecification.md
│   └── technicalSpecification.md
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

## Getting Started

### Prerequisites

- [Visual Studio Code](https://code.visualstudio.com/) with the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Git

### Build and Flash

1. Clone the repository:
   ```
   git clone https://github.com/<your-org>/greenhouse-controller.git
   cd greenhouse-controller/firmware
   ```
2. Open the `firmware/` folder in VS Code (PlatformIO will detect `platformio.ini` automatically).
3. Connect the LOLIN S3 board via USB-C.
4. Click **Upload** in the PlatformIO toolbar (or run `pio run -t upload` in the terminal).
5. Open the **Serial Monitor** (115200 baud) to view startup diagnostics.

### Configuration

On first boot, or after a factory reset, initial settings (setpoints, thresholds, network credentials) are entered via the 4×4 keypad and 16×2 LCD. Refer to the user interface section in the [Technical Design Specification](design/technicalDesignSpecification.md) for menu navigation.

## Documentation

| Document | Description |
|----------|-------------|
| [Functional Requirements Specification](design/functionalRequirementsSpecification.md) | What the system must do — all functional and technical requirements |
| [Technical Design Specification](design/technicalDesignSpecification.md) | How it is built — hardware component selection, interfaces, and design decisions |

## Windows

| ID | Location | Motor run-time (open / close) | Opening area |
|----|----------|------------------------------|-------------|
| M1 | South roof slope (Dakbeluchting Zuid) | 21 s / 21 s | 8 m² |
| M2 | North roof slope (Dakbeluchting Noord) | 21 s / 21 s | 8 m² |
| M3 | North wall side window (Zijwandbeluchting) | 171 s / 171 s | 80 m² |

## License

See the [license.md](license.md) file for full details.

**Software** (firmware and all code): Source-available, non-commercial. Free to use and modify for personal/non-commercial purposes; redistribution and commercial use are not permitted.

**Hardware design, documentation, and images**: Licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.

<a rel="license" href="https://creativecommons.org/licenses/by-nc-nd/4.0/"><img alt="Creative Commons License" style="border-width:0" src="https://i.creativecommons.org/l/by-nc-nd/4.0/88x31.png" /></a><br />Hardware design, documentation, and images are licensed under a <a rel="license" href="https://creativecommons.org/licenses/by-nc-nd/4.0/">Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License</a>.

## Disclaimer

This project is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
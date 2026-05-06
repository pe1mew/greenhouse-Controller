# Greenhouse Controller — Installation Wiring Guide

This document describes how to connect all external components to the connectors on the Greenhouse Controller PCB.

The PCB uses **Phoenix MKDS 1,5 screw terminals** for field wiring and **2.54 mm pin headers** for low-voltage signals.  
Connector locations are shown in [`hardware/pcb/20260414_PCB.png`](../hardware/pcb/20260414_PCB.png).

---

## Safety

> **WARNING — Mains voltage present.**  
> Connector J11 carries 230 V AC. Ensure mains power is switched off and locked out before connecting or disconnecting any wiring. Only qualified personnel should work on mains-voltage wiring.

---

## Power supply

### J2 — 24 V DC input (2-pin screw terminal)

Connects the external 24 V DC power supply (e.g. Mean Well RS-15-24) when the internal PSU is not installed.

> **CAUTION** — No polarity protection or over-current protection is provided. Verify supply polarity before applying power.

This connector may also be used to feed 24 V DC to an external motor controller. In that configuration, use the potential-free relay contacts on J7 (M1), J1 (M2) and J6 (M3) to issue the OPEN and CLOSE commands to the external controller.

| Pin | Label | Wire |
|-----|-------|------|
| 1 | GND | Negative (−) / common of PSU output |
| 2 | +24 V | Positive (+) of PSU output |

### J11 — AC mains input (3-pin screw terminal)

Supplies mains voltage to the relay switching circuits for the ventilation motors.

| Pin | Label | Wire |
|-----|-------|------|
| 1 | E | protective Earth (ground) |
| 2 | N | Neutral |
| 3 | L | Phase (Line) |

> Connect AC mains wiring in accordance with local electrical codes. Use cable rated for the motor load current.

---

## Ventilation motor outputs

Each motor connector carries two relay contact pairs: one for **OPEN** and one for **CLOSE**.  

With external +24VDC on the COMM connection of the control box, and then the negative on OPEN or CLOSE of the motor, the motor turns in the desired direction. When the signal is lost, the control stops.

Connect the motor's **OPEN winding** between Pin 1 and Pin 2, and the **CLOSE winding** between Pin 3 and Pin 4.

### J7 — Motor 1 (4-pin screw terminal)

| Pin | Signal | Motor connection |
|-----|--------|-----------------|
| 1 | RELAY_M1_OPEN | Motor 1 — Open terminal |
| 2 | Common (OPEN) | Motor 1 — Common / neutral |
| 3 | RELAY_M1_CLOSE | Motor 1 — Close terminal |
| 4 | Common (CLOSE) | Motor 1 — Common / neutral |

### J1 — Motor 2 (4-pin screw terminal)

| Pin | Signal | Motor connection |
|-----|--------|-----------------|
| 1 | RELAY_M2_OPEN | Motor 2 — Open terminal |
| 2 | Common (OPEN) | Motor 2 — Common / neutral |
| 3 | RELAY_M2_CLOSE | Motor 2 — Close terminal |
| 4 | Common (CLOSE) | Motor 2 — Common / neutral |

### J6 — Motor 3 (4-pin screw terminal)

| Pin | Signal | Motor connection |
|-----|--------|-----------------|
| 1 | RELAY_M3_OPEN | Motor 3 — Open terminal |
| 2 | Common (OPEN) | Motor 3 — Common / neutral |
| 3 | RELAY_M3_CLOSE | Motor 3 — Close terminal |
| 4 | Common (CLOSE) | Motor 3 — Common / neutral |

---

## Sensors

### J9 — RS485 / MODBUS — FG6485A temperature & humidity sensor (4-pin screw terminal)

The FG6485A is a MODBUS-RTU sensor (9600 bps, 8N1) measuring temperature (−40…120 °C) and relative humidity (0…99.9 %RH).  
See [`documentation/Sensors/T-RH_ FG6485A/`](../documentation/Sensors/T-RH_%20FG6485A/) for the data sheet.

| Pin | Label | Wire |
|-----|-------|------|
| 1 | +24 V | Sensor supply positive |
| 2 | GND | Sensor supply negative / common |
| 3 | RS485-A | RS485 A (Data+) |
| 4 | RS485-B | RS485 B (Data−) |

> Use shielded twisted-pair cable. Connect the shield to earth at one end only.

### J5 — RS485 / MODBUS — SenseCAP S200 wind speed & direction sensor (8-pin screw terminal)

The S200 supports both MODBUS-RTU and SDI-12.  
See [`documentation/Sensors/W-Sensecap-S200/`](../documentation/Sensors/W-Sensecap-S200/) for the user guide.

| Pin | Label | Wire |
|-----|-------|------|
| 1 | +24 V | Sensor supply positive |
| 2 | GND | Sensor supply negative / common |
| 3 | RS485-A | RS485 A (Data+) |
| 4 | RS485-B | RS485 B (Data−) |

> Use shielded twisted-pair cable. Connect the shield to earth at one end only.

---

## User interface

### J3 — I2C display (4-pin 2.54 mm pin header)

Connects an I2C LCD display module (e.g. 16×2 or 20×4 with I2C backpack).

| Pin | Label | Display module pin |
|-----|-------|--------------------|
| 1 | +5 V | VCC |
| 2 | GND | GND |
| 3 | SDA | SDA |
| 4 | SCL | SCL |

> Ensure the display module operates at 5 V logic. Match the pull-up resistors to the bus.

### J4 — 4×4 membrane keypad (8-pin 2.54 mm pin header)

Connects a standard 4×4 matrix membrane keypad.  
See [`documentation/Sensors/keypad/`](../documentation/Sensors/keypad/) for the keypad pinout image.

| Pin | Signal | Keypad wire |
|-----|--------|-------------|
| 1 | KP_ROW1 | Row 1 |
| 2 | KP_ROW2 | Row 2 |
| 3 | KP_ROW3 | Row 3 |
| 4 | KP_ROW4 | Row 4 |
| 5 | KP_COL1 | Column 1 |
| 6 | KP_COL2 | Column 2 |
| 7 | KP_COL3 | Column 3 |
| 8 | KP_COL4 | Column 4 |

> Plug the keypad flat-flex ribbon connector directly onto J4, pin 1 aligning with Row 1 of the keypad.

---

## Alarm output

### J10 — Alarm contact output (2-pin screw terminal)

Provides an alarm signal input from the motor controller as an external indicator or alarm relay.

| Pin | Label | Connection |
|-----|-------|------------|
| 1 | OPTO_INPUT | Alarm output signal (positive) |
| 2 | GND | Common / negative |

---

## Miscellaneous

### J8 — Load enable jumper (2-pin 2.54 mm pin header)

This 2-pin header controls the RS485 bus load / line termination.  
Install the jumper when this board is the **last device** on the RS485 bus (end-of-line termination).  
Remove the jumper if the board is in the **middle** of the RS485 bus.

| State | Jumper |
|-------|--------|
| End of bus (terminate) | Fitted |
| Mid-bus (no termination) | Removed |

### J12 — SD card (onboard socket)

The SD card socket is soldered directly to the PCB and uses the SPI interface internally.  
Insert a standard micro SD card (FAT32 formatted) into the socket for data logging.

*No external wiring is required.*

---

## Connector reference summary

| Ref | Description | Type | Pins |
|-----|-------------|------|------|
| J2 | 24 V DC power input | Phoenix screw terminal | 2 |
| J11 | AC mains input | Phoenix screw terminal | 3 |
| J7 | Motor 1 — OPEN / CLOSE relay | Phoenix screw terminal | 4 |
| J1 | Motor 2 — OPEN / CLOSE relay | Phoenix screw terminal | 4 |
| J6 | Motor 3 — OPEN / CLOSE relay | Phoenix screw terminal | 4 |
| J9 | RS485 MODBUS — FG6485A sensor | Phoenix screw terminal | 4 |
| J5 | RS485 MODBUS — S200 wind sensor | Phoenix screw terminal | 8 |
| J3 | I2C display | 2.54 mm pin header | 4 |
| J4 | 4×4 keypad | 2.54 mm pin header | 8 |
| J10 | Alarm output (optoisolated) | Phoenix screw terminal | 2 |
| J8 | RS485 load enable / termination | 2.54 mm pin header | 2 |
| J12 | SD card (onboard) | Onboard socket | — |

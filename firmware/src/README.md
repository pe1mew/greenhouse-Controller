# firmware/src

This directory contains the application source code for the Greenhouse Ventilation Controller firmware.

The firmware is built with **PlatformIO** targeting the **WEMOS LOLIN S3** (ESP32-S3) board.
Refer to [`../platformio.ini`](../platformio.ini) for board configuration, framework selection, and library dependencies.

## Expected contents (to be added)

| File / module | Responsibility |
|---------------|----------------|
| `main.cpp` | Application entry point; task initialisation |
| `modbus.*` | Modbus RTU master — reads SenseCAP S200 and FG6485A sensors |
| `climate.*` | Climate control logic (temperature / RH setpoints, window decisions) |
| `windSafety.*` | Wind speed / direction safety override |
| `windowController.*` | Relay output management, timed motor run, dwell-time enforcement |
| `ui.*` | Keypad scan and LCD1602 display driver |
| `rtc.*` | DS3231 RTC interface (I2C) |
| `logger.*` | Event log — NVS ring buffer and optional SD card write |
| `mqtt.*` | Optional MQTT client for remote telemetry |
| `config.*` | NVS-backed configuration store (setpoints, thresholds, credentials) |

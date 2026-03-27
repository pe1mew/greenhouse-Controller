# firmware/test

This directory contains the unit tests for the Greenhouse Ventilation Controller firmware.

Tests are executed using the **PlatformIO** test runner (`pio test`), which supports on-host (native) execution of logic modules without requiring a connected board.

## Test framework

PlatformIO uses **Unity** (C) for the Arduino / ESP-IDF framework environment. Tests that exercise pure logic (climate control, dwell-time enforcement, Modbus frame parsing, etc.) are compiled for the native host environment so they run on the development PC without hardware.

## Running tests

```bash
# Run all native tests
pio test -e native

# Run all tests on the connected LOLIN S3
pio test -e lolin_s3
```

## Expected contents (to be added)

| File | Tests |
|------|-------|
| `test_climate.cpp` | Temperature / RH setpoint logic, hysteresis, conflict resolution |
| `test_windSafety.cpp` | Wind speed threshold and direction exclusion angle |
| `test_windowController.cpp` | Motor run-time, dwell-time, simultaneous OPEN/CLOSE prevention |
| `test_modbus.cpp` | Modbus RTU frame encoding / decoding |
| `test_logger.cpp` | Ring buffer behaviour, overflow handling |

# Board Test

**Date:** 2026-05-01  
**Board:** Greenhouse Controller v1.0.0  
**Tester:** Remko Welling

## Test Overview

| Section | Component | Test IDs | Pass | Fail | Skip | Pending | Result |
|---|---|---|---|---|---|---|---|
| Voltages | Power rails | HW-VLT-001..007 | 7 | 0 | 0 | 0 | **PASS** |
| GPIO | Relays / LEDs / Input | HW-GPIO-001..014 | 14 | 0 | 0 | 0 | **PASS** |
| Keyboard | LIB-5 keyPad/ | HW-KP-003..HW-KP-0019 | 18 | 0 | 0 | 0 | **PASS** |
| SD Card | LIB-8 sdCard/ | HW-SD-001..010 | 9 | 0 | 0 | 1 | **PASS\*** |
| RTC | LIB-3 DS1307_RTC/ | HW-RTC-001..006 | 5 | 0 | 0 | 1 | **PASS\*** |
| LCD | LIB-4 LCD1602_I2C/ | HW-LCD-001..008 | 7 | 0 | 1 | 0 | **PASS** |
| Modbus | modBus/ | HW-MB-xxx | — | — | — | ALL | **PENDING** |

\* SD-010 (absent-card) not yet executed. RTC-006 battery backup tested in breadboard setup only; full PCB power-cycle test pending.

### Coverage assessment

Overall coverage is **satisfactory** for a first board bring-up session:
- All power rails verified at nominal values.
- All relay outputs and indicator LEDs confirmed with hardware.
- Full 4×4 keypad matrix exercised key-by-key including idle and multi-press rejection.
- SD card covers mount, read, write, append, delete, and 512 KB stress write.
- RTC covers init, set, advance, and battery backup by DS1307 VCC disconnect (breadboard setup); full PCB power-cycle test pending.
- LCD driver verified: 7/7 PASS, 1 SKIP (backlight hardwired to VCC).
- **Gap:** Modbus RS485 connector and bus communication not yet tested on hardware.

---

## Voltages

*Apply 220V, measure with multimeter.*

- [HW-VLT-001] Test LED D15: shall be ON — is ON: **PASS** *(Note: too bright, R1K should be increased to dim light)*
- [HW-VLT-002] Test J2: shall be 24V — is 24V: **PASS**
- [HW-VLT-003] Test J3: shall be 5V — is 4.9V: **PASS**
- [HW-VLT-004] Test U2: shall be 5V — is 4.9V: **PASS**

*Insert LOLIN board.*

- [HW-VLT-005] Test 3.3V out on LOLIN board: shall be 3.3V — is 3.29V: **PASS**
- [HW-VLT-006] Test VCC J12 pin 4: shall be 3.3V — is 3.29V: **PASS**
- [HW-VLT-007] Test VCC U4 pin 8: shall be 3.3V — is 3.29V: **PASS**

```
PASS: 7 / 7
```

---

## GPIO Relay, LED and Input

*While running the test code, relay contacts observed with an ohm meter; LEDs observed visually.*

- [HW-GPIO-001] Activation of M1 OPEN  — screw terminal presents closed contact: **PASS**
- [HW-GPIO-002] Activation of M1 CLOSE — screw terminal presents closed contact: **PASS**
- [HW-GPIO-003] Activation of M2 OPEN  — screw terminal presents closed contact: **PASS**
- [HW-GPIO-004] Activation of M2 CLOSE — screw terminal presents closed contact: **PASS**
- [HW-GPIO-005] Activation of M3 OPEN  — screw terminal presents closed contact: **PASS**
- [HW-GPIO-006] Activation of M3 CLOSE — screw terminal presents closed contact: **PASS**
- [HW-GPIO-007] Activation of M1 OPEN  — LED is ON: **PASS**
- [HW-GPIO-008] Activation of M1 CLOSE — LED is ON: **PASS**
- [HW-GPIO-009] Activation of M2 OPEN  — LED is ON: **PASS**
- [HW-GPIO-010] Activation of M2 CLOSE — LED is ON: **PASS**
- [HW-GPIO-011] Activation of M3 OPEN  — LED is ON: **PASS**
- [HW-GPIO-012] Activation of M3 CLOSE — LED is ON: **PASS**
- [HW-GPIO-013] Heartbeat LED works independently from other GPIO: **PASS**
- [HW-GPIO-014] INPUT is read correctly and controls M1 OPEN during test: **PASS**

```
================================================
  Relay + HB LED sequence test
  Each relay: 1 s ON  ->  release  ->  next
================================================
  M1 OPEN  (GPIO 12) ... released
  M1 CLOSE (GPIO 13) ... released
  M2 OPEN  (GPIO 14) ... released
  M2 CLOSE (GPIO 15) ... released
  M3 OPEN  (GPIO 16) ... released
  M3 CLOSE (GPIO 21) ... released

M1 OPEN relay will follow OPTO_INPUT (LOW = closed, relay ON; HIGH = open, relay OFF)
M1 OPEN relay input test complete.
  HB LED (GPIO 41) blink test ... done
================================================
  Sequence complete. Entering heartbeat loop.
================================================
```

```
PASS: 14 / 14
```

---

## Keyboard (LIB-5  keyPad/)

Running test developed for keyboard driver verification.

**Note:** HW-KP-005 appears twice in the firmware output — once for the multi-press test and once for key [2]. This is a known firmware numbering defect in the test suite. The numbering format also changes from three digits (HW-KP-003..009) to four digits with a leading zero (HW-KP-0010..0019); both are firmware artefacts.

```
================================================
  LIB-5 Keypad Matrix — hardware verification
================================================
Press each key when requested (30 s timeout).
Pressing multiple keys simultaneously is discarded;
release all keys and press only the requested one.
------------------------------------------------
[HW-KP-003] Idle test — do NOT press any key for 5 s ...
[PASS] HW-KP-003: no spurious output during idle period
------------------------------------------------
[HW-KP-005] Multi-press test — hold ANY TWO keys simultaneously for 5 s ...
  Waiting for two keys to be held down...
  Multi-press detected — verifying discard for 5 s...
[PASS] HW-KP-005: multi-press correctly discarded (KP_NO_KEY)
  Release all keys...
------------------------------------------------
Press key [ 1 ]  (Row1/Col1)  — timeout 30 s
[PASS]    HW-KP-004: key [ 1 ]
Press key [ 2 ]  (Row1/Col2)  — timeout 30 s
[PASS]    HW-KP-005: key [ 2 ]
Press key [ 3 ]  (Row1/Col3)  — timeout 30 s
[PASS]    HW-KP-006: key [ 3 ]
Press key [ A ]  (Row1/Col4)  — timeout 30 s
[PASS]    HW-KP-007: key [ A ]
Press key [ 4 ]  (Row2/Col1)  — timeout 30 s
[PASS]    HW-KP-008: key [ 4 ]
Press key [ 5 ]  (Row2/Col2)  — timeout 30 s
[PASS]    HW-KP-009: key [ 5 ]
Press key [ 6 ]  (Row2/Col3)  — timeout 30 s
[PASS]    HW-KP-0010: key [ 6 ]
Press key [ B ]  (Row2/Col4)  — timeout 30 s
[PASS]    HW-KP-0011: key [ B ]
Press key [ 7 ]  (Row3/Col1)  — timeout 30 s
[PASS]    HW-KP-0012: key [ 7 ]
Press key [ 8 ]  (Row3/Col2)  — timeout 30 s
[PASS]    HW-KP-0013: key [ 8 ]
Press key [ 9 ]  (Row3/Col3)  — timeout 30 s
[PASS]    HW-KP-0014: key [ 9 ]
Press key [ C ]  (Row3/Col4)  — timeout 30 s
[PASS]    HW-KP-0015: key [ C ]
Press key [ * ]  (Row4/Col1)  — timeout 30 s
[PASS]    HW-KP-0016: key [ * ]
Press key [ 0 ]  (Row4/Col2)  — timeout 30 s
[PASS]    HW-KP-0017: key [ 0 ]
Press key [ # ]  (Row4/Col3)  — timeout 30 s
[PASS]    HW-KP-0018: key [ # ]
Press key [ D ]  (Row4/Col4)  — timeout 30 s
[PASS]    HW-KP-0019: key [ D ]
================================================
  PASSED:  18
  FAILED:  0
  TIMEOUT: 0
  RESULT: PASS
================================================
Verification complete. Board is idle.
```

---

## SD Card (LIB-8  sdCard/)

```
================================================
  LIB-8 SD Card — hardware verification
================================================
[  3125][W][sd_diskio.cpp:186] sdCommand(): token error [8] 0x5
[INFO] storage_init returned: 0
[INFO] SD card mounted (FAT32)
[PASS] HW-SD-001: SPI bus initialises and card mounts
[INFO] Free bytes: 124201984
[PASS] HW-SD-002: Free bytes > 0
[INFO] Write append line 1: 0
[PASS] HW-SD-003: Write-append creates file
[INFO] File size after 2 appends: 34 bytes
[PASS] HW-SD-004: Write-append grows existing file
[INFO] Read offset 0: "line1,data,value
line2,data,value
"
[PASS] HW-SD-005: Read from offset 0 returns correct content
[INFO] list_csv: 20260410120000.csv,
[PASS] HW-SD-006: CSV file appears in directory listing
[INFO] bigfile.csv size: 524288 bytes
[PASS] HW-SD-007: 512 KB stress write succeeds
[INFO] Delete bigfile.csv: 0
[PASS] HW-SD-008: Delete removes file
[INFO] Delete bigfile.csv (2nd time): 4
[PASS] HW-SD-009: Delete non-existent file → STORAGE_ERR_NOT_FOUND

------------------------------------------------
PASS: 9
FAIL: 0
------------------------------------------------
Note: HW-SD-010 (absent card) — remove card, reset board. PENDING
```

---

## RTC (LIB-3  DS1307_RTC/)

**Note:** HW-RTC-006 (battery backup) was executed by disconnecting only the DS1307 VCC wire during a breadboard/development setup — this does not represent a full system power cycle. A complete PCB power-down test shall be performed later to verify that the CR2032 retains time when the entire board is de-energised. HW-RTC-005 is not defined in this test suite (sequence goes 004 → 006).

```
=== LIB-3 DS1307 RTC — hardware verification ===

[   306][I][esp32-hal-i2c.c:75] i2cInit(): Initialising I2C Master: sda=1 scl=2 freq=400000
DS1307 init: OK
[PASS] HW-RTC-001: Driver initialises and detects device
[PASS] HW-RTC-003: Time can be set
Set time: 2026-04-10 12:00:00 (dow 5)
Oscillator stop flag: CLEAR
[PASS] HW-RTC-002: Oscillator stop flag is clear
Read 1:   2026-04-10 12:00:00 (dow 5)
Read 2:   2026-04-10 12:00:03 (dow 5)
Delta: 3 s
[PASS] HW-RTC-004: Time is advancing (3 s ±1 s)

HW-RTC-006: Battery backup test.
Disconnect ONLY the DS1307 VCC wire when prompted.
Keep GND, SDA, SCL and the CR2032 battery connected.
  Disconnect VCC in 10 s ...
  Disconnect VCC in 9 s ...
  Disconnect VCC in 8 s ...
  Disconnect VCC in 7 s ...
  Disconnect VCC in 6 s ...
  Disconnect VCC in 5 s ...
  Disconnect VCC in 4 s ...
  Disconnect VCC in 3 s ...
  Disconnect VCC in 2 s ...
  Disconnect VCC in 1 s ...
>>> DISCONNECT DS1307 VCC NOW <<<
Waiting 10 s (RTC running on CR2032 only) ...
>>> RECONNECT DS1307 VCC NOW <<<
  Reconnect VCC in 10 s ...
  Reconnect VCC in 9 s ...
  Reconnect VCC in 8 s ...
  Reconnect VCC in 7 s ...
  Reconnect VCC in 6 s ...
  Reconnect VCC in 5 s ...
  Reconnect VCC in 4 s ...
  Reconnect VCC in 3 s ...
  Reconnect VCC in 2 s ...
  Reconnect VCC in 1 s ...
Settling 2 s ...
Elapsed since reference: 22 s
OSF after reconnect: CLEAR
[PASS] HW-RTC-006: Battery backup: time retained across RTC power cycle

Result: 5 passed, 0 failed
=== PASS ===
```

---

## LCD Display (LIB-4  LCD1602_I2C/)

**Note:** Serial log captures only the last two entries; tests HW-LCD-001 through HW-LCD-006 output is not shown. Firmware summary confirms 7 / 7 PASS. HW-LCD-007 (backlight control) is SKIP — backlight LED is hardwired to VCC on this module. Conclusion: **PASS**.

```
----------------------------------------
HW-LCD-007: Backlight control
  SKIP: AiP31068L has no I2C backlight register.
        Backlight LED is hardwired to VCC on this module.
        lcd_backlight_on/off are accepted stubs (return LCD_OK).
----------------------------------------
HW-LCD-008: Backlight on
----------------------------------------

=== SUMMARY ===
PASS: 7 / 7
FAIL: 0 / 7
All hardware tests PASSED.
```

```
PASS: 7 / 7
```

---

## Modbus (modBus/)

**Status: PENDING**

Hardware testing of the Modbus RS485 interface has not yet been performed. The on-board connector, transceiver (U4), and DE/RE control line require verification with a real RS485 slave device on the bus.

Planned test IDs: HW-MB-001..HW-MB-011 (aligned with the HIL loopback suite in `drivers/modBus/test/test_hw_loopback/`).

**Prerequisites before testing:**
- RS485 slave device (e.g. S200 wind sensor or FG6485A T/RH sensor) connected to J12.
- Run `pio test -e lolin_s3_loopback` for automated loopback verification (requires three jumper wires: GPIO17→GPIO38, GPIO21→GPIO18, GPIO8→GPIO16).
- Alternatively, connect a live slave and run the manual verification sketch (`lolin_s3` environment).

**Existing automated coverage (not a substitute for hardware test):**
- Unit tests with mocks: `pio test -e native` (HW-MB-001..012 in `drivers/modBus/test/test_modbus_rtu/`).
- HIL loopback tests verify driver timing and CRC on real silicon but do not test the PCB RS485 transceiver circuit.

```
PASS: 0 / 0   PENDING: ALL
```

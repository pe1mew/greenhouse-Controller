# GPIO on board Test

Hardware acceptance test for the six motor relay outputs, the opto-coupler feedback input, and the heartbeat LED on the Greenhouse Controller board.

## Target

| Item | Value |
|---|---|
| Board | LOLIN S3 (ESP32-S3) |
| Framework | Arduino (PlatformIO) |
| Serial port | UART0 — GPIO 43 TX / GPIO 44 RX, 3.3 V logic, 115 200 baud |

> **Note:** Use a USB-serial adapter on UART0. Do not use USB-CDC; it is not reliable here.

## Pin assignments

| Signal | GPIO | Direction |
|---|---|---|
| M1 OPEN relay | 12 | Output |
| M1 CLOSE relay | 13 | Output |
| M2 OPEN relay | 14 | Output |
| M2 CLOSE relay | 15 | Output |
| M3 OPEN relay | 16 | Output |
| M3 CLOSE relay | 21 | Output |
| Opto-coupler input (OPTO_INPUT) | 42 | Input, pull-up |
| Heartbeat LED (amber) | 41 | Output |

The relay module uses an **active-low driver**: a HIGH signal on the relay pin energises the coil; releasing the pin (LOW) de-energises it.

## Prerequisites

- The relay module **must** be connected before powering up. Running the test while relay output pins are jumpered for the GPIO loopback sketch will produce incorrect results.
- Flash and open a serial monitor at 115 200 baud on UART0.

## Test sequence

The test runs once on power-up inside `setup()`, then enters a heartbeat loop.

### Phase 1 — Relay sequence

Each of the six relay outputs is exercised in order:

```
M1 OPEN → M1 CLOSE → M2 OPEN → M2 CLOSE → M3 OPEN → M3 CLOSE
```

For each relay:
1. The heartbeat LED turns on.
2. The relay is energised for **1 000 ms**.
3. The relay is released.
4. The heartbeat LED turns off.
5. A **500 ms** gap follows before the next relay.

The LED mirrors each relay so you have a visual indication that the firmware reached each step, independent of serial output.

### Phase 2 — Opto-coupler input test

After the relay sequence, M1 OPEN is placed under control of the opto-coupler input (GPIO 42):

- OPTO_INPUT **LOW** (contact closed) → M1 OPEN relay energised.
- OPTO_INPUT **HIGH** (contact open) → M1 OPEN relay released.

The phase waits until the input has been toggled **low once and high once** (both transitions observed), then releases the relay and continues. A 15-second timeout exits the phase if the input is not exercised.

### Phase 3 — LED standalone blink test

Five rapid blinks (100 ms on / 100 ms off) verify the heartbeat LED independently of the relay sequence.

### Phase 4 — Heartbeat loop

`loop()` toggles the heartbeat LED every 1 000 ms (0.5 Hz) indefinitely as a running indicator.

## Expected serial output

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

## Pass criteria

| Check | Pass |
|---|---|
| All six "... released" lines appear in order | Relay outputs and driver wiring correct |
| Each relay audibly clicks on and off | Relay coils energised |
| HB LED lights for each relay, then stays lit between each | LED wiring and firmware correct |
| M1 OPEN follows OPTO_INPUT during phase 2 | Opto-coupler input and pull-up correct |
| Five fast LED blinks after input test | LED standalone verify |
| LED blinks at ~0.5 Hz continuously after sequence | Heartbeat loop running |

# 5C88: M3 characterisation failed twice, 2026-10-31

**Status:** diagnosed, mechanism not proven, no fix applied. Firmware 2.17.0 on 5C88. 2.17.1 work is on hold.

**Evidence:** `5C88_20261031003413.csv` (00:35 to 09:35), the screenshots in this folder, `/api/status` from rfsee.net at 13:34, and the session record of 2026-10-30. The log ends before the 10:14 hourly bus row, so the failures cannot be split per run.

## Conclusion

M3's motor knocks out the Modbus bus. The characterisation and the teach are not at fault, and the firmware stopped both runs correctly.

M3 had never run on the current wiring. The teach of 2026-10-30 ran before the bus fix, with the T/RH unplugged and the window sensor's 120 Ω still on. After the fix (boot at 11:48) M3 did not move until this morning, so "100 % after the fix" described a bus at rest.

## What happened

| Event | Run 1 | Run 2 |
|---|---|---|
| Run started, rest 30 s | 09:19:39 | 09:30:31 |
| M3 relay on, opening | 09:19:39 | 09:30:32 |
| Last good reads, both 0 mm | 09:19:39, 09:19:40 | 09:30:32, 09:30:33 |
| Address 40 lost (gate reason 2) | 09:19:43 | 09:30:36 |
| Run ended, `char_err` 3 (sensor), 1 motor start | 09:19:44 | 09:30:37 |
| Wind sensor fault set, false wind override | 09:20:50 and 09:21:49 | 09:30:37 |
| M3 closing, because of that override | 09:21:37, after 118 s | 09:31:13, after 41 s |
| Wind fault cleared for good | 09:22:48 | 09:31:36 |
| Address 40 answering again | 09:23:16 | 09:32:08 |
| M3 closed, relay off | 09:24:33 | 09:34:09 |

Run 1 also had one good probe at 09:20:45, lost again at 09:20:47.

## Findings

- **The whole bus is affected, not only the encoder.** Failed reads since boot on `/api/status`:

  | Address | Sensor | Errors | Longest run |
  |---|---|---|---|
  | 40 | Window sensor | 59 | 8 |
  | 44 | Wind | 10 | 2 |
  | 1 | T/RH | 9 | 1 |

  The log's hourly rows, up to 09:15, account for only 9, 0 and 1 of these. The rest came either after 09:15 or before the log starts at 00:35, so the log cannot show how many came from the two runs.
- **The encoder did not restart:** no param 247 row, and no start-up bit in its status.
- **The wind override was false.** Real wind was about 1 m/s. The sensor fault triggered the safe-fail, which closed M3.
- **The margin is thin even without the motor.** Address 40 failed 2, 3 and 4 times in the hours ending 04:23, 05:21 and 06:20, at about 95 % RH, and 0 times in the other hours in the log.
- **The data cannot separate direction, opening current and position.** While the gate is shut T17 re-probes only every 30 s, and T5 polls every 30 s.

## Likely mechanism (not proven)

- The idle bias is small, about 10 mV at 60 Ω: 20 kΩ pairs at the controller (R19, R20) and, per its schematic, on the wire encoder board (R3, R2), so 10 kΩ each way.
- The far end of the M3 leg is unterminated. The second terminator sits mid-bus, at the T/RH (measured, see below).
- Motor noise on that leg can then cross the receivers' thresholds.

## Measured: about 120 Ω at the T/RH, but not in every FG6485A

Operator, 2026-10-31, with the window sensor's 120 Ω and J8 off:

| Measured | A–B |
|---|---|
| All sensors connected | 60 Ω |
| FG6485A disconnected | 120 Ω |
| The spare FG6485A on its own | 150 kΩ |

So about 120 Ω sits on the sensor side of where the T/RH was disconnected, **but it is not a property of the model**: the spare has none. It is either a resistor fitted in or at the installed sensor, or a difference between the units. None of the four items in `documentation/Sensors/T-RH_ FG6485A` mentions one.

**The spare was never installed on site** (operator, 2026-10-31), so the two FG6485As used on 2026-10-30 are other units. Only the installed one has been characterised, by the bus reading above. If the other one reads high like the spare, the recorded 60 Ω was right: box 1 plus the window sensor, with the bus failing about 40 % on a terminator at each physical end. If it also reads about 120 Ω, the bus should have read 40 Ω and the 60 Ω stays unexplained. The removed unit decides this.

**The bisect of 2026-10-30** (reads at rest unless noted):

| On the bus | Read OK |
|---|---|
| Wind only | 100 % |
| Window sensor only | about 95 % |
| Wind + window sensor | 100 %, also with M3 moving |
| T/RH + window sensor | 0 % |
| Everything | about 40 % |
| Everything, second FG6485A | 29-69 % |
| Everything, window sensor's 120 Ω off | 100 % at rest, **fails with M3 running** |

Every failure at rest had an FG6485A and the window sensor's 120 Ω on the bus together.

## Ruled out

- **Calibration:** run 2 was accepted, and a start refuses anything but a valid calibration. The card's "not taught" and "Calibration UNKNOWN" only mean the sensor could not be read at that moment. **Do not re-teach.**
- **WiFi:** the failure is on RS485.
- **Firmware alone:** the same run passed on 2344 (2.17.1-bench, 123 starts), but that rig has no greenhouse motor, so this is not a full clearance.

## Production state

At 13:34: M3 closed, gate ok, Lineair in force, AUTOMATIC. Any M3 drive can repeat this. It fails safe, because the false wind override closes the windows. Switching M3 to timed would not help, because the motor would still disturb the bus.

## Current wiring

Trunk: mast box 1, 10 m, box 2 (controller), 6 m, box 3 (T/RH stub about 1 m), 6 m, window sensor. S200 stub 2 m. Active terminators: box 1 and about 120 Ω at the T/RH, both measured. See `design/modbusBusInstallation5C88.puml`.

## Planned next steps (on site)

All hardware changes with the power off. Run this from the site LAN during every M3 test:

```bash
python bin/bus_watch.py --host 192.168.1.104 --every 10
```

1. **Done: A–B measured.** 60 Ω, and 120 Ω without the FG6485A. The T/RH's terminator is real.

2. **Reproduce it on the current wiring.** Drive M3 fully open and closed from the LCD. Note when failures start and stop against the motor, the direction and the position.

3. **If the FG6485A removed on 2026-10-30 is at hand, measure it unpowered, yellow to white.** It settles the 60 Ω, but nothing below waits for it.

4. **Fit the spare FG6485A at box 3 and enable the window sensor's 120 Ω,** power off. Set the spare's address switches to 1 first. That puts a terminator at each physical end without opening a sensor, and A–B should read about 60 Ω. Keep the installed sensor safe and unchanged.

   Then watch the bus at rest for a few minutes before moving M3, and repeat step 2:

   | Result | Means | Next |
   |---|---|---|
   | Clean at rest and with M3 running | The resistor's position was the problem | Go to step 6 |
   | Fails at rest | An FG6485A plus the window sensor's 120 Ω is the problem, whatever the sensor's own resistor. Disable the 120 Ω again | Go to step 5 |
   | Clean at rest, fails with M3 running | Motor noise, not termination | Go to step 5 |

5. **If it still fails:**
   - Compare with the T/RH stub unplugged, to see whether the T/RH is the discriminator.
   - Check whether the M3 leg runs alongside the motor cable.
   - Fit stronger bias at the controller: about 470 Ω from A to +3.3 V and from B to GND, in place of the 20 kΩ.

6. **Pass criterion, before retrying the characterisation:** a full M3 open and close with no gate drop, no wind or T/RH sensor fault, and no run of two failed reads on any address.

7. **Then retry the characterisation** with `bus_watch.py` running beside it. Plan for 1.5 to 3 hours.

## Parked while 2.17.1 is on hold

- A failed run sends no stop, so M3 carries on to its target. Here only the wind safe-fail ended the drive (`characterise.cpp` has no `CMD_STOP`).
- The card says "not taught" when the calibration is only unreadable, which invites a pointless re-teach.
- 2.17.1 open items: gh#98, gh#100, gh#103 item 7, gh#99.

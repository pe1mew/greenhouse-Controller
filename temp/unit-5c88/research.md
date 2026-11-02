# 5C88: M3 characterisation failed twice, 2026-10-31

**Status:** cause open, see the review below. No fix applied. Firmware 2.17.0 on 5C88.

**Evidence:** `5C88_20261031003413.csv` (00:35 to 09:35), the screenshots in this folder, `/api/status` from rfsee.net at 13:34, and the session record of 2026-10-30. The log ends before the 10:14 hourly bus row, so the failures cannot be split per run.

## Review, 2026-11-01: the first conclusion was too strong

The operator disputed "M3's motor knocks out the Modbus bus". The log does not support it as written. **What caused the failures is open.** Below, what the data shows.

**Corrections to what follows:**

- **The wind fault's log times were wrong.** Alarm rows use a clock cached for up to 60 s. Placed by their order in the file, the failed wind polls are 09:21:35, 09:22:36 and 09:31:11. The table below is corrected.
- **"The whole bus is affected" was wrong.** T/RH and wind read normally at most polls while address 40 was lost.
- **"The motor" was never separated from anything else.** "M3 running" in the bisect and in both runs also means T17's reads, and a stroke-start write to the encoder.

**What the data shows:**

| Finding | Evidence |
|---|---|
| Wind failures happen with M3 moving | 3 failed polls of 10 with M3 moving, 0 of the other 1063 in this log |
| It is not the packet count | From 13:34 on 2026-10-31 to 11:15 the next day, about 2 600 more polls: wind errors still 10, address 40 still 59. By the read counts M3 did not move in that time |
| It is not a high packet rate on 5C88 | T17 polls at travel / 150: 1.14 s here, about 2 transactions a read. The rig polls at 100 ms, about 10x faster. Address 40 was lost after 2 to 4 reads |
| The motor alone does not stop address 40 | 09:23:16 to 09:23:34: 15 reads with the leaf closing, none failed |
| All three losses were on opening or on re-opening during opening | 09:19:43, 09:20:47, 09:30:36. Each came 2 to 4 s after T17 started reading a moving leaf. Not explained |
| Real sensors fall silent on their own | Before any encoder, on 2.3.1 with one bus caller: six T/RH faults of about 59 s and two wind faults, none within 120 s of a relay (gotcha 2026-08-26) |
| The sensors can look silent or garbled, and the log cannot say which | `s200_status_t` collapses timeout, CRC and exception into `S200_ERR_COMM`. **"Silent" is not proven** |
| The rig cannot show any of this | Address 1 and 44 are emulated. Neither manual gives a reply time or a polling limit |
| One bad poll closes the greenhouse | T5 retries once after 100 ms, then faults, and T3 safe-fails at once. Every wind fault since commissioning closed the windows, in wind of 0.6 to 1.6 m/s |

**Rig, 2026-11-01, 2.18.0-bench, with the failure type in the log and a real FG6485A on the bus for the first time:**

| Run at T17's fastest polling | Spare FG6485A | Result | Encoder | Wind |
|---|---|---|---|---|
| 12:32 | On | **Ended `sensor` at 3 starts, a target overran to the OPEN end: 5C88's failure** | 3 silent of 176 | 2 silent of 4 |
| 12:39 | Off | 55 starts, stopped by hand | 0 of 684 | 0 of 7 |
| 12:45 | On | 28 starts, stopped by hand | 0 of 419 | 1 silent of 5 |

- **Every failure on the rig is a silent slave, zero bytes.** So the failures this rig can make are not damaged replies. On 5C88 this is still not known: it needs 2.18.0.
- **The failure needs no greenhouse motor.** It ran on the rig's small actuator.
- **The FG6485A is a suspect, not a finding.** One failure in two runs with it, none in one without.
- **Stroke test, 2026-11-02, standard rig, spare off, 10 % to 90 % and back by the bench hook, encoder only:** 2.18.0-bench 4 failed in 12 drives, 2.17.4-bench 2 during its boot recalibration and 4 in 42 drives. All on a closing movement. Twice on 2.18.0 M3 overran a 12 % target to CLOSED, as a lost position does. **So the encoder failure is not new in 2.18.0.** The bus code is unchanged from 2.17.3, whose soak had 0 of 8 538. Something on the rig changed between that soak and 2026-11-01 12:05, when its bus was first rewired. The emulated wind's failures began at the same moment. Whether they share a cause is not known, and the emulator cannot explain 5C88, which has none.
- **Where the rig's encoder fails, 2026-11-02:** at the ENDS, with the relay on. Control soak: 31 of 38 failures came in the overrun after the closed end sensor, 7 at rest at the closed end, none with the leaf travelling. End-to-end test on 2.18.0-bench, 11 drives: 27 failures, every drive had some, 26 at 0 or 1500.0 mm and 1 at 1418.4 mm, both ends alike. Targets short of the ends: 0 failures in the 10 such drives on 2.18.0-bench and in the first 16 on 2.17.4-bench. So "closing" was the wrong reading: closing is just the direction that reached an end. **This is not yet 5C88's failure:** on 2026-10-31 5C88's encoder read cleanly through 60 s of overrun at the closed end, and lost address 40 leaving it. It polls 11 times slower.
- **What it takes, 2026-11-02, relay on for the full drive, T17 at full rate:** motor unplugged at the closed end, 58 failures in 10 drives. Motor unplugged at 45 %, away from both end sensors, **0 failures in 1 020 reads in 10 drives**. So it needs neither the motor nor the movement, and the relay and the fast polling alone do not do it. Only the end does. The sensor's firmware has nothing blocking there: `regs_note_end_stop()` only stores a value, at the transition, and flash is written only when a setting changes. So the code gives no reason for it; the end sensor's circuit is the next suspect. Why the 2.17.3 soak had none is still unknown.
- **With the closed-end sensor unplugged, 2026-11-02 19:05, motor off, leaf at the closed end:** encoder 0 failures in 1 021 reads, and the **emulated wind's failures stopped too**: 0 in 12 polls and more, after failing on most polls for over a day. Wind kept failing at 45 % with the switch connected but not active, so for wind it is the connected sensor or its cable, not the active state. One unplugging so far: it must be repeated. If it holds, a part that has degraded would explain a rig that changed without being touched. The same sensor type is on 5C88, whose whole bus got worse after 2026-11-01 18:29: unproven, but a cheap test on site.
- **The soak with the spare on, 12.25 h on 2.18.0-bench:** the real encoder failed 157 times where the 2.17.1 and 2.17.3 soaks had 0, every one silent: 124 closing, 5 opening, 28 at rest. Emulated wind failed 304 times, 280 at rest, silent. The real FG6485A failed 0 times. Wind faults 0, because T5's retry answered. The firmware and the emulator's T/RH address changed too, so this does not name a cause. Note that here it is CLOSING, and on 5C88 all three losses were opening.
- **The emulator's own log, 13:21 to 13:26, across three failed polls:** one RX and one TX per poll, also on the polls where the controller recorded a silent first attempt. So that attempt's request was not accepted at all: the retry was the one answered. Its log drops nothing at this rate, but a frame with a bad CRC goes to its serial console only.
- **Likely why, not proven: the emulator's frame gap is 20 ms (`RS485_FRAME_TIMEOUT_MS`) and the controller's inter-frame gap is 20 ms (`MODBUS_IFG_US`).** With no margin, a reply from another slave and the request after it can run together into one frame with a bad CRC, and the emulator then answers nothing. That needs a reply it did not send itself, so it fits the encoder's traffic and a real FG6485A. It does not explain why the second read failed with the spare off. **This concerns the emulator's wind, not the real encoder's silences,** and not a real S200, whose frame gap is unknown.
- At rest with the spare on, nothing failed on the encoder in about 10 minutes.
- The heating read was dropped, so 5C88's wind traffic halves with 2.18.0, and one failed request no longer loses a good reading.

**Next, in the order that separates most:**

1. **Done: the failure type is in the log** (2.18.0).
2. **Under way: the spare FG6485A on the rig.** No S200 spare exists.
3. **5C88 on site, motor without T17:** set `wpos_fitted_m3` to 0 so T17 leaves the bus, drive M3 from the LCD, and run `bus_watch.py --every 10`. This is the split never made.
4. **5C88 on site, traffic without motor:** a laptop RS485 adapter in place of the controller, polling the real sensors. It also gives the first real reply times.
5. **Done: a wind fault needs the whole averaging window** (2.18.0, the operator's decision).

## First conclusion, 2026-10-31 (superseded, see above)

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
| Wind sensor fault set, false wind override (corrected) | 09:21:35 and 09:22:36 | 09:31:11 |
| M3 closing, because of that override | 09:21:37, after 118 s | 09:31:13, after 41 s |
| Wind fault cleared for good (corrected) | 09:23:07 | 09:31:42 |
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

At 13:34: M3 closed, gate ok, Lineair in force, AUTOMATIC. Any M3 drive can repeat this. **On 2026-10-31 the windows closed only because the wind sensor faulted too.** A target drive that loses the position finishes on the travel timer, at the end, by design, so without that fault M3 runs to full open, and then T6 decides on timed control (gh#104). Switching M3 to timed would not help, because the motor would still disturb the bus.

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

## Filed from this analysis

- **gh#104:** a run that ends early leaves M3 driving, and with the position lost it runs to the end. Here only the wind safe-fail ended the drives. The 20 % target of run 1 ran for 118 s.
- **gh#105:** the card says "not taught" when the calibration is only unreadable, which invites a pointless re-teach.
- 2.17.1 open items: gh#98 (admin half), gh#99 (needs a go).

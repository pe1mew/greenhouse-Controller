# 5C88: the reboot of 2026-09-28, and the log check to 2026-10-02

| | |
|---|---|
| Unit | **5C88** (unit ID 0x5C88 = 23688), production, Herenboeren Wenumseveld |
| Firmware | 2.3.1 (`mainstream`) |
| Logs | `model/campaign-fall-2026/2026-09-28_143049.log` (the reboot), `2026-09-30_093233.log`, `2026-10-02_042327.log` (the latest). For comparison: `model/campaign-summer-2026/2026-07-04_210848.log` |
| Analysed | 2026-10-03. Rows were read against the decode tables in `log/logparser.py`; the parser itself was not run |
| Times | Controller local time (CEST) as logged, unless marked *corrected* |

## Summary

- **A power-on reset, not a crash.** On 2026-09-28 at about 15:09, 5C88 was without power for about
  2¼ minutes (between 2 min 05 s and 2 min 30 s). It booted with `esp_reset_reason` 1 (POWERON). This
  is the only reboot in the logs since 2026-09-17.
- **Probably a power interruption on the farm.** This is an inference: nobody was logged in, and the
  unit's network needed about 50 s to come back.
- **What it cost:** the boot calibration closed all three windows. M1 had been open, and it stayed
  closed for the rest of the afternoon, while RH rose from 74 % to 84 %. A short T/RH sensor fault
  followed 29 minutes later.
- **The latest log** (to 2026-10-02 06:22) has no reboot and no alarm.

## Timeline

| Log time | Row(s) | What happened |
|---|---|---|
| 13:43:00 | `MODE` 1 | Step 1: M1 open, M2 closes |
| 15:07:13 | `SENSOR_HR` ch 2 = 2 | Window states: M1 OPEN, M2 and M3 CLOSED. T 25.8 °C, RH 73 % |
| 15:07:24 | `SYSTEM` WEB 1/0 | Last row before the gap: a status POST that succeeded |
| (15:07:43) | none | The next 30 s sample never arrives |
| 1970-01-01 01:00:00 | `SYSTEM` 7/8/12 | Heap rows written at boot, before the clock was set (248 KB free) |
| 15:10:13 | `SYSTEM` 5/**1** | **BOOT, `esp_reset_reason` 1 = POWERON** |
| 15:10:13 | `SYSTEM` 11/23688 | Unit ID 0x5C88 |
| 15:10:13 | `RELAY` ch 1–3 = 4 | Boot calibration: all three windows MOVING_CLOSE |
| 15:10:23 | `SYSTEM` 1/0, 2/0 | The first WiFi attempt fails, and NTP times out |
| 15:10:25 | `SYSTEM` WEB 0/0 | A status POST fails |
| 15:10:39 | `RELAY` ch 1, 2 = 1 | M1 and M2 CLOSED (26 s) |
| 15:10:53 | `SYSTEM` 22/3 | ROTA check skipped: clock not ready |
| 15:11:03 | `SYSTEM` 1/1 | WiFi connected, ~50 s after the boot row |
| 15:13:09 | `RELAY` ch 3 = 1 | M3 CLOSED (176 s) |
| 15:15:05 | `SYSTEM` 2/1, 4/1 | NTP synced on the 300 s retry, and geolocation ran. The clock steps back 21 s |
| 15:39:25 | `ALARM` ch 4 = 1 | T/RH sensor fault: two consecutive read failures |
| 15:40:24 | `ALARM` ch 4 = 0 | The fault clears |
| 2026-09-29 11:59:41 | `MODE` 2 | The first step change after the boot |

**The stamps from the boot row until 15:15:05 are 21 s fast.** They come from the DS1307, which read
+21/+22 s against NTP all day before the cut (`SYSTEM` 21 rows). NTP stepped the clock back 21 s at
15:15:05: the heap rows read 15:15:12 and then 15:15:51, 39 s apart instead of 60.

So power returned at about **15:09:50 (corrected)**. The cut began between 15:07:24 and 15:07:43, so
the outage lasted **2 min 05 s to 2 min 30 s**.

## Cause

**Power, not firmware.** The reset reason is POWERON (1). It is not PANIC (4), a watchdog (5–7),
BROWNOUT (9) or a software restart (3), and no coredump row (`SYSTEM` 18) follows. Nothing unusual
came before the cut either: no motor ran in the 84 minutes before it (the last relay event was 13:43:41).

**Probably the farm's supply, not someone at the controller.** This is an inference; the log cannot
prove it.
- **Nobody logged in.** There is no `SESSION` or `PIN_AUTH` row anywhere in the fall logs
  (2026-09-17 → 2026-10-02).
- **The network came back slowly.** After the boot, the first WiFi attempt failed and the unit needed
  ~50 s to connect. Before the cut, status POSTs had gone through every 2 minutes. That fits network
  equipment that was restarting too.
- **The only other power-on reset since June looks different.** On 2026-07-04 an admin logged in on
  the keypad at 09:47:58 (`SESSION` ADMIN 2), and the unit booted 10 s later, at 09:48:08. WiFi and NTP
  were back within 5 s of that boot. That one looks like someone at the controller.
- **Every other boot since June is a software restart** (reason 3). Four of them are at about 01:00,
  ROTA's night window.

**To confirm:** check the uptime of the farm's router, or whether anyone on site noticed a power cut or
a tripped breaker on 2026-09-28 at about 15:08.

## Consequences

- **M1 stayed closed for the rest of the afternoon.** The calibration closed it, and T6 logged no new
  step until 2026-09-29 11:59.
  - The temperature stayed around 26 °C, and RH rose from 74 % (15:00, M1 open) to 81 % at 15:30 and
    84 % at 16:00.
  - This fits the stepped law restarting from "all closed" with its hysteresis memory gone. Before the
    cut, step 1 was being held at the same ~26 °C, and from closed that temperature apparently does not
    reach the threshold to open again.
  - T6's internal state is not in the log.
- **A T/RH sensor fault, 15:39:25 – 15:40:24,** 29 minutes after the boot. It is the only alarm in the
  fall logs. 2.3.1 does not log the driver status (`value_b` 0), so it cannot be told whether the fault
  is related.
- **No setting changed.** The only `SETPT` rows are the four geolocation rows (param 21: 52, 368, 4, 904)
  that every boot writes, identical to every boot since June.
- **Everything else recovered:** within a minute of the boot the heap was back to 85 KB free with a
  31 KB largest block, and the ROTA checks read up to date from 16:10 on.

## The latest log: `2026-10-02_042327.log`

It covers 2026-09-30 11:32:06 → 2026-10-02 06:22 (local). Nothing newer was in the repository on
2026-10-03.

- **No reboot, no alarm, no login.** Every ROTA check read up to date (2.3.1 on `mainstream`).
- **Heap:** 83 KB free with a 31 KB largest block, at both the start and the end. It dips to 57–63 KB
  (largest block 28 KB) for a few minutes at a time and recovers each time, e.g. on 09-30 11:44–11:52
  and 10-01 21:26–21:34.
- **Status POSTs failed** 10 times on 09-30 between 13:03 and 15:53, and once on 10-02 at 01:57. No
  WiFi disconnect was logged, so the problem was upstream (the internet or the server), not the unit.
- **Daily log upload:** at 03:30 on 10-01 and 10-02 the slot found no closed file to upload (`SYSTEM`
  WEB 0/2). This is benign.
- **The DS1307 still jumps.** Against NTP it read +46 s on 09-30 12:19–14:18, then +12/+13 s from 21:11
  to 10-01 14:59, and nothing after that. This is harmless while NTP holds, since the chip is ignored
  then, but it is why the 2026-09-28 boot rows are 21 s fast.
- **Side note, on control:** all three windows stayed open overnight.
  - From 21:52 on 09-30 until 07:42 on 10-01, M1, M2 and M3 were open. The last decision, at 21:42, was
    step 3: temperature asked for step 1, humidity for step 3.
  - The greenhouse was at 18.7–19.6 °C and 88–93 % RH. All three closed at 07:42, when temperature's
    step fell to 0.
  - Before that, M3 had opened three times between 20:26 and 21:52, closing in between.
  - M2 and M3 were open on humidity alone. That is the v1 law in 2.3.1. Under v2 (2.15.0, gh#84),
    humidity alone opens M1 at most.

## Evidence

| Claim | Where |
|---|---|
| Boot, reason 1 | `2026-09-28_143049.log:23833` |
| Last row before the gap | `2026-09-28_143049.log:23828` |
| Pre-clock heap rows | `2026-09-28_143049.log:23829-23831` |
| Calibration | `2026-09-28_143049.log:23835-23837`, `:23841-23842`, `:23867` |
| WiFi, NTP and ROTA after the boot | `2026-09-28_143049.log:23838-23839`, `:23846-23847`, `:23893-23894` |
| The 21 s clock step | `2026-09-28_143049.log:23887-23904` |
| DS1307 +21/+22 s before the cut | `SYSTEM` 21 rows from 06:17 to 14:12, e.g. `2026-09-28_143049.log:23304` |
| M1 open before the cut | `MODE` at `2026-09-28_143049.log:23028`; window bits at `:23827` |
| T/RH fault | `2026-09-28_143049.log:24133`, `:24138` |
| First step change after the boot | `2026-09-30_093233.log:11130` |
| The 2026-07-04 power-on reset | `model/campaign-summer-2026/2026-07-04_210848.log:18002-18013` |
| Status POST failures in the latest log | `2026-10-02_042327.log:886` … `:2518`, `:22049` |
| Overnight venting | `2026-10-02_042327.log:4511`, `:5873`, `:5957`, `:11573-11605` |
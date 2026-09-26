# Before You Start - the long form

`CLAUDE.md`'s **Before You Start** table is an index: each row names the trap that bites you and
points here. This file holds **the full text those rows used to carry, verbatim** - moved on
2026-09-24 because the table had grown to 27 050 of CLAUDE.md's 37 762 characters, which is most of
an auto-loaded file that is supposed to be terse.

**Nothing was deleted.** Every sentence below stood in CLAUDE.md before the move, and the rows that
point here keep the part you must know *before* you start; the history, the enumerations and the
incident narratives are here. Paths are relative to the repository root, so a link written for
CLAUDE.md (`design/foo.md`) resolves from here as `../design/foo.md` - the links below have been
rewritten accordingly.

Read the row in CLAUDE.md first. Come here when you are actually going to touch the thing.


## 1. Changing the T9/T14 audit log format

`log/logparser.py` must learn the new value_a/value_b/param encoding alongside the firmware change
(and check `model/campaign-summer-2026/plot_daily.py` — it decodes ALARM/SENSOR_HR rows too.
Verified 2026-09-10: it reads `param` **only** for the 240-243 ALARM band, so a new config-band
param needs no change there — `logparser.py` is the only param table in the repo). **A second
emitter on an existing event type is a trap:** `LOG_MODE_CHANGE` is written by T6 (vent step) *and*
by `dm_set_standby_ex()` (STANDBY), and all three consumers decode only the first — every STANDBY
row parses as a ventilation decision (gh#54). A new emitter needs its own `param_id` and a parser
branch in the same change. **`log_type_t` has 10 members as of 2.5.0** (`types/app_types.h`, NOT
`event_logger.h`): SENSOR, RELAY, MODE_CHANGE, SETPOINT, SESSION, ALARM, SYSTEM, SENSOR_HR, SUN,
**PIN_AUTH** (gh#58). **Append, never insert** — the ordinal is stored in every archived CSV row and
NVS blob. **`LOG_SYSTEM value_a` is occupied -2, -1 and 0..30 as of 2.6.0** — and the table in
`event_logger.h` was STALE at 0..21 until then, which is how gh#59 came to be filed claiming 22 was
free when 22/23/24 had been ROTA since 2.2.0. **The emitters are authoritative, not the comment:
`grep -rn 'value_a' --include=*.cpp firmware/src/` and read the helper call sites (ROTA's go through
`audit_row()`) before claiming a subtype is free.** `LOG_MODE_CHANGE` now discriminates its two
emitters by `param_id`: 0 = T6 vent step, **47 = `LOG_PARAM_MODE_STANDBY`** (gh#54); `plot_daily.py`
and `vent_step_replay.py` must skip 47. `LOG_SENSOR` is **reserved**: no emitter since rc.1.4.0,
kept only so ordinal 0 still decodes old archives

## 2. Adding or changing a config key, or its bounds

**A key is declared ONCE, in [firmware/config/cfg_desc.inc](../firmware/config/cfg_desc.inc) — one
row (gh#64, 2026-09-14).** The clamp, the shadow write, the boot default, the audit id, the kind and
`GET /api/config/limits` all read it; **enrolling a key is that one row.** The row still *names*
symbols that must exist — a `K_*` constant, a `cfg_shadow_t` field, `CFG_MIN/MAX_*` in
`cfg_limits.h`, a `DEF_*` in `cfg_defaults.h`, a `LOG_PARAM_*` — but each of those has exactly one
home and the compiler fails loudly if any is missing. What is gone is the *duplicate enrolment* in
six or seven separate lists, where an omission was silent. (`logparser.py`'s param table is still
separate and still needs the new id.) Verify with `python bin/check_cfg_desc.py` (`-v` prints the
table), wired into `.githooks/pre-commit`. **The collapse bought one place to declare a key and
introduced exactly one new way to be wrong: the shadow write is driven by `offsetof()`, so naming
the WRONG FIELD compiles, runs, and quietly writes a real but different setting** — nothing
downstream notices, so if the checker complains about a shadow field, believe it. **gh#64 said six
tables; there was a SEVENTH it did not count — the boot loader
(`nvs_load_climate/wind/motor/system/web()`), which carried the key list AND the factory defaults.
COLLAPSED 2026-09-14:** the five helpers are one `cfg_load_group()` call each, the descriptor
carries a `def` field (still referencing the `cfg_defaults.h` macros, which remain the only place a
default is written), and a key missing from the boot path is now impossible rather than merely
detectable. Before that, a key present everywhere else but absent there was accepted, clamped,
audited and published, then **silently reset to 0 on every reboot**, with nothing logged. **`kind`
and `shadow_off` are INDEPENDENT** — `kind` governs the Q4 write path, `shadow_off` says where the
value lives; conflating them is what first hid the four `ota_*` fields, which are `CFG_KEY_NOT_Q4`
and yet have real `cfg_shadow_t` fields the boot loader writes. `cfg_shadow_store()` is the single
place a value enters the shadow, shared by the Q4 write and the loader. **Verified on hardware by
wiping NVS on 12F0: 42 keys boot to exactly their descriptor default** (`python
bin/at_cfg_roundtrip.py --host <ip> --expect-defaults`). **`lat_*`/`lon_*` will NOT read as their
defaults on a wiped unit and that is not a bug** — T10's `do_geo_sync()` posts all four coordinate
keys to Q4 within seconds of boot. It also catches a `webUiMock/mock_server.py` drift (the one copy
of the limits still outside the table) and any consumer that starts growing its own key list again.
**The table was DERIVED from the six it replaced, not retyped, and that derivation stays
re-runnable: `python bin/gen_cfg_desc.py --from-rev ':/tools.gh#64' --check`  (git's
search-by-message syntax, not a hash — the commit is cherry-picked between branches, which rewrites
its hash).** `bin/check_cfg_tables.py` is the retired six-table checker — it now delegates, and is
kept because `gen_cfg_desc.py` imports its parser; **don't gut it.** `--list-gaps` still prints the
17 historical omissions, every one of which is now a *field value* (`CFG_KEY_NVS_ONLY`,
`CFG_KEY_NOT_Q4`, `LOG_PARAM_NONE`, no `CFG_F_PUB`) rather than an absence. **Before gh#64 six
tables carried the key list and drifted every single time** (gh#53, gh#57 ×2, gh#51 group D):
`cfg_clamp()`, `ns_key_to_log_id()`, `cfg_key_kind()`, the shadow ladder, `LIMITS_JSON`, and the
mock. 2.5.0 closed an 11-key gap where a key was in the shadow ladder but in **neither** the clamp
nor the published limits, so `POST /api/config` stored any `int32`. Bounds still live in
`firmware/config/cfg_limits.h` and nowhere else (the descriptor references the macros), but **that
file is not automatically the authority** (gh#57 part 2): it was created whole in v1.16.25 and gave
`poll_interval` 30-300 when FR-S03/FR-CF07, the TSDS in five places, and T5's own
`SP_POLL_MIN_S`/`SP_POLL_MAX_S` all said 15-120. **Before trusting a bound, check the consuming task
for its own clamp and the FRS/TSDS for the requirement** - a config-layer bound that is wider than
the consumer's produces a silent lie (stored 300, polled 120). Corrected in 2.5.1. `avg_win` still
diverges: FR-CF17/FR-S07 say 1-60 min, `cfg_limits.h` says 1-30; `webUiMock/mock_server.py`'s
`CONFIG_LIMITS` must mirror them. **`POST /api/config` is asynchronous** — it enqueues on Q4 and T4
applies it a loop later, so a read-back immediately after the POST returns the PREVIOUS value;
settle or poll before asserting. [Since 2.14.0 the four `led_*` keys this sentence describes no longer exist — gh#67 retired them to constants in `watchdog.cpp`; kept as it read before the move.] Four of the keys (`led_*`) are write-only: no API exposes them, so
a clamp on them cannot be verified over the network

## 3. Changing motor travel/dwell config, or anything T2 caches

**The two rigs have DIFFERENT physical M3 windows, so `travel_m3` belongs to the RIG, never to the
module:** production's real 40 m rope flap traverses in **176 s** (`travel_m3 = 171`), the dev rig's
test window in **13 s** (`travel_m3 = 13`). T2 drives `travel_m3 + MOTOR_TRAVEL_MARGIN_S_DEFAULT`
and the margin is a fixed 5 s (`cfg_defaults.h:75`, applied at `relay_controller.cpp:1054`).
**Over-driving is mechanically HARMLESS** — both mechanics have an **end switch** (the switch that
stops the motor at the physical end, invisible to firmware — distinct from the **end sensor**, which
reports 0 %/100 % via bit 3) that cuts drive at the limit, by design and outside the controller's
view, so the motor cannot be overloaded and **the firmware defaults are NOT to be changed for the
rig** (operator, 2026-09-12). What the value still governs is the controller's own state model: how
long it believes M3 is `MOVING`, when the dwell timers arm, the `SENSOR_HR ch2` bitmask, and T17's
`travel/150` poll. **The end switch protects over-travel completely and does nothing for
UNDER-travel, which is the real risk: a rig value in production drives 18 s of a 176 s traverse, so
M3 opens about a tenth while the log records fully OPEN — no stall, no alarm.** Since the dev rig
takes a swappable module (see Identity), **read `travel_s` from `/api/config` and set `travel_m3` to
match the wired window after every swap, reset or reflash, before anything reboots.** **Likewise
`wpos_fitted_m3` = 1 (gh#73, 2.9.0): it defaults to 0, and a module left at 0 stops reading the
rig's encoder without raising anything.** **In mode 2 (`ctrl_mode_m3` = 1, 2.12.0) M3's open AND
close dwell are REPLACED by `min_intv_m3`, which defaults to **600 s** (decided 2026-09-20,
`model/closedloop/linearDwell.md`: free under today's law, the floor for a faster one; never above
900; 0 only for a deliberate test)** (contract §7; `ch_dwell_ms()` in T2 and T6 both read the one
key). The mode that acts is the EFFECTIVE one (`dm_m3_ctrl_mode()`), which also needs a trusted
position and falls back on its own.
[design/fixMotorTimingRefresh.md](../design/fixMotorTimingRefresh.md) — gh#51, shipped across
2.4.2/2.4.3/2.4.4. T2 keeps a **private** copy of travel/dwell; T4 pushes `T2_NOTIFY_CFG_CHANGED` on
any `motor` write and T2 re-reads NVS at the top of its loop. `load_motor_timings()` touches
**only** the three timing fields — folding channel state or the deadlines back in would cancel a
running stroke. **gh#52 (operating mode) and gh#53 (unknown-key writes) are FIXED in 2.4.6,
corrected in 2.4.7** — `dm_reload_all_cfg(initiator, channel)` restores mode via
`dm_set_standby_ex()` outside MX4 *after* notifying T2, and `POST /api/config` returns 400 for a key
it cannot apply. **The key classifier is three-way** (`cfg_key_kind()`: UNKNOWN / SHADOW /
**NVS-ONLY**) — 2.4.6 shipped it as a boolean and broke `wifi/ap_enable`, which has no shadow field
because T10 polls NVS for it. **Before touching Q4, `/api/config`, `cfg_shadow_t` or any key table,
enumerate every producer with `grep -rn 'xQueueSend(Q4\|post_q4('` — there are five, two of them not
the web GUI** (see
[design/releaseComparison_2.3.1_vs_2.4.6.md](../design/releaseComparison_2.3.1_vs_2.4.6.md) §2.2).
**Dwell timers defer `SRC_T6` only** — `SRC_OPERATOR_MANUAL` and `SRC_T3` bypass them, but a
completed manual move still *sets* the deadline, so T6 inherits a debt it never incurred (up to 25
min on M3). `session_close()` now clears it via `T2_NOTIFY_CLEAR_DWELL` and recalibrates. **If T6
"is out of sync", it is being refused, not confused** — see the 2026-09-10 recurrence entry in the
gotcha log

## 4. Touching the Modbus/RS485 bus

[design/addModbusMutex.md](../design/addModbusMutex.md) — the driver **does** hold a bus mutex since
gh#49 (hardware-verified), but **single-caller is still policy**: ~215 ms of blocking must stay out
of T2/T3. Two traps: the receive loop **never yields**, so a back-to-back poller starves the idle
task and trips the 5 s TWDT; and `modbus_init()` deletes/reinstalls the UART, so a re-init during
another task's transaction is a use-after-delete — **it happened 2026-09-16** (gh#69: two boot
panics); a re-init now takes the bus mutex. **A third, found the hard way 2026-09-12: the mutex
serialises ACCESS but not the SILENCE the RTU protocol needs between frames.** `MODBUS_IFG_US` sat
at the spec floor plus 9.7 % (3.84 character times) and, with a single caller, **had never once
executed** — frames were 30 s apart. T17 made frames adjacent and the S200 began discarding requests
it read as a continuation of the previous frame, which presents as **zero bytes and a timeout, never
a CRC error**, and reached the operator as a wind alarm closing the greenhouse on a calm day (T3
safe-fails on `EG1_BIT_SENSOR_FAULT_W`). Now 20 000 us, **paid before `bus_unlock()`** so the lock
means "the bus is yours AND idle"; the RX FIFO is drained on every exit for the same reason; and
`*_ERR_BUSY` is carried through the sensor drivers instead of collapsing into `*_ERR_COMM`. **When
adding any caller to a shared resource, audit the constants that were latent under single use.** **A
fourth (gh#70, fixed 2026-09-16): a task-released DE/RE lost the encoder's ~4.5 ms reply under flash
writes (up to 665 ms stall); the UART drives it now (RS485 mode, RTS, `CONFIG_UART_ISR_IN_IRAM`).
Fail-first: `bin/at_modbus_ota.py`.** Diagnosis needs `modbus_get_counters()` —
`to_received`/`to_expected` is what distinguishes "the slave said nothing" from "the frame was cut
short". Three hardware tests, all **fail-first — run each against a lock-disabled build and confirm
it FAILS before trusting a pass**: `pio test -e lolin_s3_loopback -d drivers/modBus` (12 tests,
needs 3 jumpers on the **12F0 bench board**, not a unit in service), `pio run -e lolin_s3_mbprobe`
(real-slave probe, FDA4), and **`python bin/at_modbus_reinit.py`** (re-init under traffic;
lock-disabled build: `MODBUS_FAILFIRST_UNLOCKED_REINIT`; STANDBY, windows CLOSED). **Identify any
board by MAC before flashing** — the CH340 gives identical COM ports to different boards. **Probing
the bus with a scope: a single-ended threshold probe cannot decode a reply that arrives within a few
ms of the master releasing the line** (2026-09-14). The encoder at addr 40 answers in ~5.2 ms
(measured, n=26) against ~22 ms for the FG6485A and ~73 ms for the S200, and the RS485 turnaround
glitch decodes as a spurious byte that shifts every following byte by one bit — so a 9 h capture
decoded 1 598 transactions from addr 1 and addr 44 and **not one** from addr 40, while the
controller read it thousands of times an hour with `crc 0` and `framing 0`. **The firmware was never
at fault; check `GET /api/diag/windowpos` counters before blaming the bus.** Measuring addr 40 needs
differential probing across A/B with bias, or a decoder that re-locks per byte. **The emulated
slaves are out of spec on LEVELS, measured 2026-09-15 (gh#68): their replies never assert the
positive differential rail (+0.076 V against the master's +1.44 V and the real encoder's +1.52 V),
so 0 % of the reply reaches the receiver's +200 mV threshold and ~20 % of the waveform is
undecidable — a per-bit error rate of ~4e-05 where a healthy link is below 1e-12.** That is why addr
1 fails 1 in 272 and addr 44 1 in 4 069 while the real encoder at addr 40 is **0 in 12 828** on the
same bus, same master, same firmware, and why every earlier look found `crc 0` / `framing 0`: a
sub-threshold reply times out rather than arriving corrupt. **Consequence: any rig fault rate,
latency or retry figure for addr 1 or addr 44 characterises a slave that is out of spec, and gh#66's
arm A/B comparison is confounded by it — do NOT raise `MODBUS_IFG_US` past 20 ms on rig evidence.**
gh#68 was **parked** until the `ropeSensor` work merged, which happened in 2.8.0, so it is due for a
decision; production shows similar degradation, so it is not only a harness artefact

## 5. Fitting a window position sensor (M3, T17)

**By default nothing acts on position**: in mode 1 (`ctrl_mode_m3` = 0, the default) T17 measures,
logs and detects and T2 stops on its timer. **Since 2.12.0, mode 2 drives M3 to a measured target**
when an operator asks for it and the position is trusted (plan §5b); it falls back to mode 1 on its
own. **Since 2.10.0 (plan §5d, "confirm only") T17 gives every M3 drive a verdict when T2 ends it**
— confirmed, not reached, not judged (`ALARM ch6` param 251) — and checks `travel_m3` against the
measured traverse (param 252); both only report (status flags, web badges). It reaches 5C88 only
through a `promote`, which needs the release comparison and a fresh instruction. **Built:** driver
`drivers/windowPos/`, IDF component, task **T17** `firmware/src/window_pos/` (priority 4, polls only
while M3 travels, cadence `travel_m3`/150, 30 s at rest), log encodings **SENSOR_HR ch3** (position
0.1 mm, -1 = fault; signed rate) and **ALARM ch6** (params 244-252), admin-only `GET
/api/diag/windowpos`. Sensor at Modbus **address 40**; the encoder also reports M3's two end sensors
as status bit 3. **`wpos_fitted_m3` must be 1 on a rig module** — see the motor travel/dwell row.
**The presence gate publishes the control law** via `windowpos_task_ctrl_mode()`: POSITION with a
trusted sensor, TIMED without one. Demotion is immediate, promotion only at a stroke boundary; a
shut gate re-probes every 30 s; a BENCH build latches. **Since 2.9.1 (gh#72) the two §12.4 rules
judge each DRIVE** (T2's `t2_get_drive()`), so a reversal is two and `strokes` counts drives. **Five
traps.** (1) **Soaks count JUDGED strokes** (`strokes - at_end_exempt`), or a soak made of
recalibrations passes on strokes rule 1 never looked at. (2) **Before 2.10.0 rule 2 was wrong twice
([gh#78](https://github.com/pe1mew/greenhouse-Controller/issues/78)): it never judged a full close**
(bit 3 at the OPEN end counted as corroboration) **and false-tripped on a close that started
part-way open** (position reads 0 ~1.2 s before the closed end sensor makes) — read older builds'
param 250 rows and `early_stops` with that in mind. (3) **Check the diag counters before blaming the
bus** — a scope probe cannot decode address 40's fast reply, and the firmware was never at fault.
(4) **A teach needs BOTH end sensors made after arming, and holds STANDBY until its admin session
ends** (gh#65); T17 aborts a teach the commissioning path does not own (`orphan_aborts`). (5)
**Measure positions LIVE** (2026-09-21): `GET /api/diag/windowpos`'s top-level fields are a fresh
device read, while its `t17` block and `/api/status` carry T17's cache — which after a targeted stop
holds the cut reading until the settle read ~1.1 s later; AT-WP02's first run judged mid-coast
values. Its `t2` block is T2's learned overrun lead. **Harnesses:** `bin/at_wp_gh72.py` (stages
`flap`, `stale`, `latejoin`, `reversal`; bench hook `POST /api/diag/windowpos {"inject": …}` with
`absent`, `fault` or `stuck`, fail-first `-DWPOS_FAILFIRST_GH72`), `bin/at_wp_confirm.py` (2.10.0's
nine stages; injections `ends` and `race`, fail-first `-DWPOS_FAILFIRST_292`), `at_wp_soak.py` (with
`at_wp_strokes.py` making its strokes through T6), `at_wp09.py`, `at_wp07.py`, `at_wp_teach.py`,
`at_wpos_fitted.py`; **2.12.0:** `at_wp_target.py` (nine target stages, hook `{"target_x10": N}`,
plus `"source":"t6"` to send it as T6 does: dwell and gh#48 apply), `at_wp_fallback.py` (the fall
back through T6; `aborted` and `sincemove` check what the law is told) and `at_wp_rule1.py` (rule
1's exemption; injections `noend`, `short`) — fail-first via the bits in
`firmware/src/types/failfirst_212.h`, **one bit per run, and pass a value** (a bare flag is 1).
**Read before touching any of it:**
[design/integrateWindowPositionSensor.md](../design/integrateWindowPositionSensor.md) **§2a** — the
switch/limit/overlap geometry is not what the earlier drafts assumed, and the calibration and rig
measurements live there; **§5b/§5c** for Phase 5's scope (mode 1 timed, mode 2 linear M3; 2.10.0
confirms only; position reaches T2/T6 through a T4 pass-through that must never buffer it); the
control law itself sits behind [design/ventModelContract.md](../design/ventModelContract.md) with
`drivers/ventModel/` as its tested reference. Sensor choice and the procurement blocker:
[design/windowPositionSensorRequirements.MD](../design/windowPositionSensorRequirements.MD). T17's
task-level detail: [memory/architecture.md](architecture.md).

## 6. Touching SD mount state, SD logging, or T9's automount

[memory/gotcha-log.md](gotcha-log.md) — **`s_sd_ok == false` has two meanings**: card *unavailable*
(absent/failed — T9 retries every 60 s, by design, for hot-insertion) and card *released on request*
(`/api/sd/unmount`). The `s_sd_released` latch added in 2.4.10 (gh#61) keeps them apart at **both**
automount call sites (`event_logger.cpp:1162` and `:1186` — the second one is the one that actually
fires, because the sensor poll keeps Q3 busy); before that a deliberate unmount was silently
remounted within a minute, against a manual that makes unmounting mandatory before pulling the card.
Do not add a retry path that ignores the latch. **Never decide anything from a list-based directory
scan (gh#82, 2.12.2): `storage_sd_list_csv()` filled the caller's buffer, DROPPED the names that did
not fit and returned `STORAGE_OK`, and FAT lists oldest-first, so retention saturated at the cap and
deleted nothing (113 files against 30), the listing showed only old files, and the boot resume and
both upload enumerators took their answer from the same partial view. It is DELETED.
`storage_sd_foreach_csv()` (one callback per file, constant memory) is the only enumerator; every
caller aggregates during that pass. Retention is PER UNIT** (`<unit>_YYYYMMDDHHMMSS.csv`, 30 of this
unit's own, never the other module's, trimming `SD_TRIM_PER_ROTATION` back towards the cap because
one delete per rotation never catches up), **and any bounded listing must report what it left out**
(`on_card`) — see
[design/technicalSoftwareDesignSpecification.md](../design/technicalSoftwareDesignSpecification.md)
§5.3 (Event Log Manager: rotation, per-unit retention, the non-truncating scan, the listing
contract)

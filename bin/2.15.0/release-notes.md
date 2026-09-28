# Release 2.15.0

**Date:** 2026-09-27
**Built on:** 2.14.1
**Fixes:** [gh#84](https://github.com/pe1mew/greenhouse-Controller/issues/84) — under `cr_priority` 1 a dry house closed every window against heat, and humidity opened the house at night with nothing to stop it cooling the crop
**Laws:** `stepped` **v2** and `graded` **v2** (both v1 until now), behind interface **3** of `design/ventModelContract.md`

**Minor**, because the control law changes behaviour, the SD log gains a row (`MODE` param 56), the
status payload gains a field (`windows.law`), and two operator surfaces change (T min is settable
again; the conflict priority is a choice of two).

> **At `cr_priority` 0 — the default, and the setting of 5C88 and of the development rig — every
> decision is the same as in 2.14.1.** The law's behaviour changes only under priority 1 or 2. The
> only difference a priority-0 unit shows is in its log: the humidity vote (`step_rh`) now holds at 1
> for a while after humidity falls back under `rh_max` (the new close guard, rule 4), where 2.14.1
> logged "no vote".

> **Verified on hardware and soaked** on 2344, the development rig's module: the release build first
> (2026-09-27), then **a 12.25 h soak of 2.15.0-bench with `cr_priority` 1**, so that the new rules
> acted, on 5C88's live climate. Every fault counter stayed at 0. All 42 decisions the SD log recorded
> follow v2's rules: humidity opened **M1 only**, 9 times, and the night floor both held the house shut
> and let it open where it should. The published image is the release build of the same commit
> (`13bb604`). See *Verification*.

## What changed

### The law: `stepped` v2 (gh#84)

Prototyped in the simulator first (`model/closedloop/humidityControl.md`, *The package, prototyped*;
`model/closedloop/humidity_prototype.py`). Five rules:

1. **Dryness never closes against heat.** If the temperature wants to open and humidity votes 0 (below
   `rh_min`), the temperature's step wins **under every priority**. v1's priority 1 ("humidity first")
   closed a hot, dry house.
2. **Humidity may open the house on its own only under priority 1 or 2, and only from `t_min` + 2 °C.**
   An opening that is already live holds down to `t_min` + 1 °C, so the whole-degree average does not
   flicker M1 across the floor. `t_min` is the day or night value, as for `t_max`.
3. **Capped at M1.** Humidity alone never opens M2 or M3.
4. **A close guard at `rh_max`.** A live humidity vote holds at step 1 until the average is
   max(`hyst_rh` / 3, 1) % under `rh_max` (71 % at the defaults: 75 % and 12 %). The humidity branch
   is evaluated only above `rh_max`, so the ladder's own close hysteresis never engaged for it: v1
   dropped the vote the moment humidity touched the ceiling.
5. **Priority 2 behaves exactly as priority 1.** Rule 1 removed the only conflict in which they
   differed.

The margins (2 °C, 1 °C) and the cap (step 1) are the law's constants
(`STEPPED_FLOOR_MARGIN_C`, `STEPPED_FLOOR_HYST_C`, `STEPPED_RH_ALONE_CAP`), not settings.

**One consequence, documented and not fixed:** with rule 1, the "too dry" vote can no longer change
any decision, so **`rh_min` is inert under every priority** (it already was under priority 0). The
setting stays, and its vote is still computed and logged. FR-C08 is marked *not met, by decision*.

### `graded` v2

No code change in `vent_model_graded.cpp`. It embeds `stepped` for the step, M1, M2 and the humidity
branch, so it inherits v2 and bumps with it (contract §2): M3 opens for humidity only while the
temperature also vents.

### The interface: `VENT_MODEL_API` 3

`vent_in_t` gains `int16_t t_min_c10` after `t_max_c10`; T6 fills it from `t_min_day` / `t_min_ngt` by
`is_daytime`. A new field moves every field after it, so the simulator's mirror moved with it
(`model/closedloop/ventmodel.py`, `ventmodel_ffi.cpp`, `firmware.py`, `settings.py`, where `t_min_*`
is now a law setting).

### The law in force is published

Until now the law's name reached only the serial console and its version nothing at all, so no log
could say which version decided (contract §2, gh#84).

- **SD log: `LOG_MODE_CHANGE` param 56**, a fourth emitter on that row type, written with the param 54
  row at boot and on every change of the effective mode. `value_a` = which law (1 `stepped`, 2 `graded`,
  0 one the firmware's list does not know), `value_b` = its version, `ch` = 0. The id is keyed by the
  law's **name** (`law_log_id()` in `climate_control.cpp`), never by the table index. 56 was found free
  by grepping every emitter, as the gh#54 rule requires. `log/logparser.py` decodes it in the same change
  (*Control law in force: stepped v2*).
- **`/api/status` `windows.law`**, e.g. `"graded v2"`, next to `M3_ctrl_mode` and always present with
  the block. It comes from `cc_law_str()`, a lookup in the table T6 selects its law from, keyed by the
  same effective mode, so it cannot name a law T6 is not running. It reaches the WebSocket push and the
  status POST too (one builder). The live payload on 2344 was 1 054 bytes with no flags. All 16 flags
  add about 340 bytes, so the new ~19-byte field leaves more than 600 bytes spare in the POST's
  2 048-byte buffer. That is an estimate from the flag strings, not a measured maximum.
- **The model tools no longer keep a skip list.** `model/closedloop/logdata.py`,
  `model/campaign-summer-2026/plot_daily.py` and `model/vent_step_replay.py` skipped MODE params 47 and
  54. They now keep param 0, the vent step, and skip every other MODE row, so a fifth emitter cannot be
  read as a decision. That would have been gh#54's trap a third time.

### What the operator sees

- **Conflict priority: two choices.** The web GUI offers *Temperature first* and *Humidity may also
  open M1, above T min + 2 °C*. The LCD edits `T/RH prio (0/1)`. The config clamp stays 0–2, so a
  stored 2 is still valid and behaves as 1. The GUI and the LCD show it as the second choice, and
  confirming it unedited on the LCD stores 1. The tooltip no longer mentions *Largest deviation* or a
  *T-floor*.
- **T min is settable again**, on the LCD (Day and Night browse, now 4 setpoints each, `1/4`) and in the
  web GUI. It had been hidden as "heating control not implemented", and now it is the humidity floor.
  In the web GUI it greys with the other humidity-dependent rows when humidity control is off. This
  also makes FR-CF01 true again.
- **Tooltips that promised a close that no longer happens**:
  - *RH min* now says it changes no window decision.
  - *RH max* says humidity opens further windows while the temperature vents, and may open M1 on its
    own only under the second choice.
  - *RH hysteresis* gives the close guard and a worked example.
- **Manuals:** boer 1.26 and beheerder 1.30. Updated:
  - boer §4 *Conflict-prioriteit* and its table of situations;
  - beheerder *Hoe de twee regel-assen worden gecombineerd* and the *Conflict-prioriteit* bullet;
  - both crop tables' *CR-prio* legend (RH is now safe in heat) and *T_min* legend (the humidity floor);
  - the LCD walkthroughs.
- **Design documents:**
  - the contract (interface 3, the version table, the logging table; the mode 2 row it described was
    never built, and it now says so);
  - FRS FR-C07, FR-C08, FR-CR03 (two choices) and FR-CR04 (still open);
  - TSDS §5 (conflict resolution) and §5.16;
  - `logparser.md` 1.24;
  - the status-site specification 2.1;
  - the test plan §6.4.

## Verification

| Check | Result |
|---|---|
| Host tests, `drivers/ventModel`, `pio test -e native` | **PASS** — 49 of 49: `stepped` 30, `graded` 19 |
| Fail-first: the new and changed tests against the v1 law | **FAIL as they must** — the v1 sources, built against the interface-3 header with MinGW g++ 14.2 and Unity: `stepped` 9 of 30 fail and `graded` 4 of 19, exactly the new and the deliberately changed tests; every other test passes on v1 |
| Tests changed on purpose (they pinned v1) | `stepped`: *conflict follows cr_priority* (priority 1 and 2 now give 1, not 3); *too dry demands close* → *too dry never closes against heat*. `graded`: *humidity alone opens M3 fully* → *never opens M3*; *conflict follows cr_priority* → *dryness never closes against heat, M3 aimed*. Each carries a comment saying so |
| New tests | rule 1; the floor and its 1 °C hysteresis (18 → 17 → 16 → 17 → 18 °C gives 1, 1, 0, 0, 1); the M1 cap; the close guard (80 → 73 → 71 % gives 1, 1, 0); priority 2 = priority 1 over a sweep of more than 1 000 inputs; priority 0 unchanged; and **a digest per law**: an FNV-1a hash of 20 000 pseudo-random decisions pinned with the version, so a behaviour change without a version bump fails the build (gh#83 changed `graded` without a bump and nothing noticed) |
| The prototype's rule checks, run on the REAL law | **PASS** — 11 of 11 (the 10 checks of `humidity_prototype.py` plus the versions), with `t_min_c10` 160 by day and 140 by night |
| `model/vent_step_replay.py`, 5C88's summer logs | **PASS** — law `stepped v2`; reproduces **97.9 % of 2 268** logged T-demands, the figure the model session had on v1. With the September logs added: 98.0 % of 2 293 |
| `model/closedloop/closed_loop.py gate-control` | **PASS**, unchanged gate. Against the same run on v1: T-demand identical (97.1 % first, 97.6 % window), 386 MODE rows written in both. RH-demand 90.8 → 89.5 % and 94.2 → 92.9 %: **5 rows**, each logged `(step, step_t, step_rh)` = `(n, n, -1)` against a simulated `(n, n, 1)`. Step and `step_t` match every time, and only the humidity vote differs: the close guard holding where the v1 firmware logged no vote. All 31 of v1's misses are unchanged |
| Firmware builds | **yes** — release and bench, no errors. The only warnings are in files this release does not touch. RAM 19.9 %, flash 67.3 %; image 1 411 168 bytes (+336 against 2.14.1) |
| `python bin/check_cfg_desc.py` | **PASS** — 51 keys, 40 published; no key or bound changed |
| `log/logparser.py` | decodes param 56 as *Control law in force: stepped v2* / *graded v2*; an unknown id reads *law #n*; param 54 and 0 rows unchanged |
| Web GUI on `webUiMock` | **PASS**. A stored `cr_priority` 2 shows as *Humidity may also open M1, above T min + 2 °C* (2 options). T min day/night load 16/14 with their sliders bounded 5–40 / 0–30 by `/api/config/limits`. The T min rows grey to 0.35 with humidity control off, and T max does not. The mock's `/api/status` carries `"law":"stepped v2"`. No script errors |
| **Release build on 2344** | **PASS** — 2026-09-27 13:57, OTA push. After the reboot `fw_ver` **and** `asset_version` read 2.15.0. `windows.law` read `stepped v2` while M3 was still timed, then `graded v2` once Lineair engaged by itself (62 s). The SD log carries the new row, `MODE` param 56 = `graded v2`, beside param 54, and `logparser.py` decodes it. All 9 rig settings were unchanged, and the served `index.html` was byte-identical to the repo's. No `stepped v2` row was written at that boot: T6's first cycle came after the switch, so nothing was decided under stepped. Like every T6 `MODE` row, these are stamped with T4's cached clock, up to a minute early |
| **The new priority control, on the unit** | The operator set `cr_priority` 0 → 1 from the web GUI's two-choice select (2026-09-27 16:46:28, `SETPT` param 12, initiator `WEB`) |
| **Soak on 2344, `cr_priority` 1** | **PASS** — 2.15.0-bench (built from `13bb604`), 2026-09-27 16:55:59 to 2026-09-28 05:10:59: **12.25 h, 25 judged strokes** (12 confirmed closes, 13 openings not judged by design); `stall_faults`, `early_stops`, `rejected_rate`, `err_comm` and `not_reached` all 0; `mode_changes` 0; gate `position`; **no reboot** (uptime 26.1 h spans the window). All 12 scripted stroke sessions ran, none skipped or failed. Each steers `cr_priority` to 2 to open and 0 to close for a few minutes, then restores 1 |
| **The law in the soak's SD log** | **CONFORMS** — every vent-step decision from the soak's start to 2026-09-28 19:00 was checked against v2's resolver, under the priority in force at that moment (taken from the `SETPT` rows): **42 decisions, none against the rules**. Humidity alone opened **M1 only, 9 times**, never M2 or M3. It was refused 11 times: 10 during the sessions' priority-0 phases and 1 by the floor. Seven openings overlap a per-minute watch of T5's averages, and all 7 were at or above the floor. **The night floor** (`t_min_ngt` 14 + 2 = 16 °C) held M1 shut from 22:07, at 15.2 °C, down through 14.3 °C. It opened on 15.5 °C averages (T5 rounds to 16), and an opening already made held down to a 15.1 °C average (rounded 15) and never below: the 1 °C hysteresis. Outside the sessions M1 stood open on humidity alone in 372 of the one-minute samples. By day (floor 18 °C) humidity opened M1 at 24.5 and 21.2 °C. **Not exercised:** *dryness never closes against heat*, since humidity stayed at 59–94 % and never fell below `rh_min`; and the `rh_max` close guard, since no opening saw humidity fall back through the ceiling |
| **Heap through the soak** | 734 one-minute samples: free 34–68 KB, largest block 19–31 KB; in the last hour 62–67 KB free with a 26–31 KB largest block. The since-boot floor stepped 28 → 19 KB in the first 3 h and then stayed flat (2.14.0's soak: 8 KB) |
| **ROTA pull on 2344** | **PASS** — published as seq 59 at 19:03:53 on 2026-09-28. Plain 2.14.1 was pushed to 2344 at 19:07 (a bench build is never offered its own release). A forced check at 19:14:33 was offered 2.15.0, but its download failed 9 s later, sub-code 1 (TLS, transport, non-200 or buffer allocation; the unit cannot tell which), as the first downloads after 2.14.0's publish did. The unit's own hourly check at 20:11:42 found it again, **downloaded and verified it in 11 s**, and **committed the apply at 20:12:06**. After the reboot `fw_ver` **and** `asset_version` read 2.15.0, the next check read up to date, Lineair engaged with `windows.law` `graded v2` and the matching `MODE` param 56 row, and every rig setting survived (`cr_priority` still 1, the soak's setting) |
| LCD screens (`T/RH prio (0/1)`, the 1/4 browse) | **`T/RH prio (0/1)` confirmed** on 2344's LCD by the operator (2026-09-28). The Day/Night browse with T-min (`1/4`) has not been checked on hardware yet |

## Upgrading

- **Units on `cr_priority` 0** (the default): no window decision changes. The SD log gains the param
  56 row at boot, and `step_rh` reads 1 instead of −1 for a while after a humidity excursion ends.
- **Units on `cr_priority` 1:** a dry house no longer closes against heat. Humidity alone now opens M1
  only, and only from `t_min` + 2 °C, which at night is `t_min_ngt` + 2.
- **Units on `cr_priority` 2:** behave as 1, so humidity alone no longer opens M2 or M3. The GUI shows
  the setting as the second choice.
- **Check T min before choosing the second priority.** Until now `t_min_day` / `t_min_ngt` did nothing
  and were not even visible in the web GUI, so a stored value may never have been looked at. The
  defaults, 16 and 14 °C, give floors of 18 and 16 °C.
- **Status site:** `windows.law` is an added key and optional to render (status-site specification
  2.1). A site on 2.0 ignores it.
- **Log tools:** a tool that decodes MODE rows by shape, instead of branching on `param`, will misread
  param 56. That is the gh#54 rule again. The repo's own tools are updated.

## Known limitations

- **`rh_min` is inert** under every priority, a consequence of rule 1. Documented, not fixed.
- **FR-CR04 stays open:** no conflict event is logged. The vent-step row carries both demands, so a
  conflict can be read afterwards.
- **`model/closedloop/humidity_prototype.py` stops with "patch anchor found 0 times".** It patches the v1
  source, and v2 is now the source. Expected; the model session retires or re-anchors it.
- **`test/3_4_Conflict_Resolution.py` is stale** and marked so. It pins v1 in two cases. It was already
  broken before this release: it reads `windows` as a list, which the canonical status does not return.
- The floor's margins and the M1 cap are constants in the law. Making them keys is the contract's
  *Adding a tunable* path, not taken here.

# Rebasing `ropeSensor` onto `main` — preparation

**Prepared 2026-09-12, against `main` at `d83e328` (2.7.0) and `ropeSensor` at `0078e86`.**
Analysis only. The rebase itself is a git write and is left to the operator; §6 has the
commands.

## 1. Verdict

**Mechanically easy and semantically safe.** Four files conflict and all four are
documentation or a version string. **Every source file applies clean**, including the
three that shared an encoding space and were the real risk: `types/app_types.h`,
`log/logparser.py` and `plot_daily.py`.

The two hazards named in `CLAUDE.md` are both cleared by inspection, not by assumption:

- **No commit on `main` deletes a sensor file** (§3), so nothing makes git read a deletion
  as intentional and strip T17 out.
- **No log-encoding collision** (§4). The two branches extended disjoint parts of the
  encoding space.

## 2. The divergence

Fork point `0caff3f` ("fix(lcd): manual session end clears dwell debt and recalibrates").

**Four commits replay** (`ropeSensor` only):

```
9260613  feat(driver): wire-encoder window position sensor (Phase 1)
cd63521  feat(T17): window position task with derived config (Phase 2)
02b326c  feat(log): position samples + device events (Phase 3)
0078e86  docs: curate — T17 in the architecture, window-sensor state in memory
```

**Fourteen commits they land on** (`main` only): 2.4.6, 2.4.7, 2.4.8, 2.4.10, 2.5.0,
2.5.1, 2.6.0, the 2.6.0 ROTA publish, four docs commits, the config-table checker, and
2.7.0.

So `ropeSensor` is the whole 2.4.6-to-2.7.0 run behind. Rebasing it forward is the
smaller job of the two directions, which is why it is the prescribed one.

## 3. Deletion hazard — cleared

`CLAUDE.md` warns that a commit on `main` deleting the sensor files makes git treat the
deletion as intentional and strip T17 from `ropeSensor`. Checked:

- At the fork point only the two **design documents** existed
  (`integrateWindowPositionSensor.md`, `windowPositionSensorRequirements.MD`), and `main`
  still carries both.
- `git log --diff-filter=D $B..main` names **no** sensor file.
- The driver, the IDF component and the T17 task are pure **additions** made after the
  fork, so there is nothing on `main` for a deletion to have removed.

## 4. Encoding-collision audit — no overlap

This is the part that would have been expensive to discover during the rebase, because a
collision is silent: two producers writing the same encoding, and a parser that decodes
only one. That is gh#54 exactly.

| space | `ropeSensor` uses | `main` added since the fork | collide? |
|---|---|---|---|
| `log_param_id_t` | **244–247** (`WPOS_FAULT`, `_TEACH`, `_STATUS`, `_RESTART`) | **47** `LOG_PARAM_MODE_STANDBY` (gh#54) | no |
| `log_type_t` | nothing | **ordinal 9** `LOG_PIN_AUTH` (gh#58) | no |
| `LOG_SYSTEM value_a` | **nothing at all** | **25–30** (gh#59) | no |
| `LOG_SENSOR_HR` channel | **ch 3** (position, rate) | none | no |
| `LOG_ALARM` channel | **ch 6** | none | no |
| config keys (Q4/NVS) | **none** — T17 *derives* its poll cadence from `travel_m3` | 11 keys clamped + published (gh#57) | no |

The last row matters for a reason beyond collisions: because `ropeSensor` introduces no
config key, the new `bin/check_cfg_tables.py` gate will pass on the rebased branch
without a new `EXPECTED_GAPS` entry. `ropeSensor` does not touch `data_manager.cpp`,
`cfg_limits.h` or `mock_server.py` at all, and its `web_server.cpp` change does not go
near `LIMITS_JSON`.

## 5. Conflict surface — four files, all mechanical

Established with a read-only trial (`git diff $B ropeSensor -- <file> | git apply --check -`)
against `main` as it stands. Everything not listed here applies clean.

| file | why | resolution |
|---|---|---|
| `firmware/platformio.ini` | `ropeSensor` bumped 2.4.4 → 2.4.5; `main` is now 2.7.0 | **take `main`'s 2.7.0.** `ropeSensor` changed nothing else in this file — the IDF component is wired in `firmware/components/windowPos/CMakeLists.txt` and `firmware/src/CMakeLists.txt`, both clean |
| `bin/2.4.5/release-notes.md` | add/add: both sides created it, 80 lines each | **byte-identical** (md5 `ee1861f8…` on both). Take either side |
| `memory/gotcha-log.md` | both appended entries at the end | **keep both sets.** Append `ropeSensor`'s window-sensor entries after `main`'s; nothing is contradictory |
| `CLAUDE.md` | `main`'s copy was rewritten heavily on 2026-09-12 | **take `main`'s**, then re-add `ropeSensor`'s window-sensor pointer row. Do **not** take `ropeSensor`'s side wholesale: it predates the swappable-module correction, the two-windows facts, the six-table checker row and the 2.4.6–2.7.0 history |

### Two resolutions that are not just "keep both"

**`memory/gotcha-log.md` — `ropeSensor`'s only new entry is already fixed on `main`.**
Its single added heading is *"2026-09-10 — a refused manual LCD command leaves NO trace,
so 'it got rejected' is unreconstructable"*. That is gh#59 item 3, and `main` closed it in
2.6.0: `LOG_SYSTEM value_a = 27` now records a Q1 command discarded while the motor alarm
is active, packing the action in the high byte and the **source** in the low byte, so a
refused `SRC_OPERATOR_MANUAL` command is distinguishable from a refused T6 or T3 one.
So carry the entry forward **marked `[RESOLVED — 2.6.0, LOG_SYSTEM value_a=27]`** rather
than as a live gotcha. Keeping it open would leave the log asserting a gap that no longer
exists.

**`CLAUDE.md` — the window-sensor row needs editing, not just re-adding.** Its text on
`ropeSensor` is stale in two ways beyond the rebase:

- it says *"Phase 0 is blocked until the wire sensor is physically on FDA4's bus"*, but
  phases 0–3 are complete and hardware-verified;
- it names **FDA4** as the dev unit, which is no longer how the rig works — FDA4 and 2344
  are swappable modules and **2344 is the one currently fitted**, at `192.168.20.160`.

The row as it stands on `ropeSensor` is saved verbatim in the session scratchpad; re-add it
onto `main`'s copy with those two corrections, and keep `main`'s Identity section, which
already describes the swappable rig.

### Method limitation, stated

The trial tests the **squashed end state**. A real rebase replays the four commits one at
a time, so an *intermediate* commit can conflict even when the end state does not — most
likely on `changelog.md`, where `ropeSensor`'s 2.4.5 section and `main`'s 2.4.6–2.7.0
sections both live at the top of the file. It reported clean for the end state; treat a
stop there as expected rather than alarming.

## 6. Commands (operator)

Take the safety net first — it costs nothing and makes the whole thing reversible:

```bash
git branch ropeSensor-prerebase ropeSensor     # recoverable snapshot
git checkout ropeSensor
git rebase main
```

`main` is never checked out and never moves: `git rebase main` while on `ropeSensor`
replays `ropeSensor`'s commits on top of `main`'s tip and rewrites only `ropeSensor`.
That is what "preserving main" means here.

On each stop, resolve per §5, then:

```bash
git add <resolved files>
git rebase --continue
```

Afterwards the branch has been rewritten, so the remote needs:

```bash
git push --force-with-lease origin ropeSensor
```

`--force-with-lease` rather than `--force`: it refuses if the remote moved under you.
Branch protection rejects merge commits on `main` but `ropeSensor` is not `main`, and
nothing here touches `main`'s history.

If it goes wrong at any point: `git rebase --abort`, and the snapshot branch is still
there.

## 7. Post-rebase gates, in order

1. **`python bin/check_cfg_tables.py`** — expected to pass, per §4. This gate is new to
   `ropeSensor`; it has never run against this branch.
2. **`git config core.hooksPath`** — the hooks are opt-in per clone and were enabled on
   2026-09-12. The pre-commit hook now enforces the gh#9 manifest placeholder and the
   config-table check on any commit touching a key table.
3. **Build:** `pio run -e lolin_s3`. T17 pulls in the `windowPos` IDF component, so a
   clean build here is the first real evidence the rebase produced a coherent tree.
4. **Version:** the rebased `platformio.ini` says 2.7.0. The sensor work is a new task and
   new log encodings, so it lands as **2.8.0**, not a patch.
5. **Parser round-trip:** feed a CSV containing both branches' encodings through
   `log/logparser.py` — `SENSOR_HR ch3` and `ALARM ch6` params 244–247 from `ropeSensor`,
   and `MODE param 47`, `PIN_AUTH` and `SYSTEM 25–30` from `main`. All must decode with no
   `raw:` fallback. This is the cheap proof that §4's audit holds in the merged code and
   not just in the enum.
6. **Do not flash to a unit in the rig without reading §8 first.**

## 8. What the rebase surfaces that are NOT rebase problems

Pre-existing items that become live again the moment `ropeSensor` is buildable:

- **The sensor-presence gate is still missing.** Both guard branches in
  `window_pos_task.cpp` only *log*: "T17 idle" does not idle and "REFUSING" does not
  refuse, and T17 is spawned unconditionally. A unit with no sensor at Modbus address 40
  would hold the bus ~215 ms per failed poll for every stroke. **This is the first thing
  to build in Phase 4, and `ropeSensor` must not reach 5C88 before it lands.**
- **`log/logparser.md` never learned the window-sensor encodings.** `logparser.py` did, but
  the reference document did not — it is absent from `ropeSensor`'s changed files
  entirely. After the rebase it will carry `main`'s content, documented up to `PIN_AUTH`
  and subtype 30, with `SENSOR_HR ch3` / `ALARM ch6` / params 244–247 missing. Per the
  standing rule that a new encoding must be learned in the same changeset, this is an
  outstanding debt from Phase 3, not something the rebase broke.
- **The rig's M3 is a 13 s window, production's is 176 s**, and each module carries its own
  NVS. Check `travel_s` from `/api/config` after any flash: T17's poll cadence is derived
  from `travel_m3`, so a wrong value gives a wrong cadence as well as a wrong stroke model.
- **`ropeSensor` predates the module-swap reality.** Its notes assume FDA4 is the dev unit.
  2344 is the module currently fitted, at `192.168.20.160`, and `ota_push.py` defaults to
  FDA4's `.169`.

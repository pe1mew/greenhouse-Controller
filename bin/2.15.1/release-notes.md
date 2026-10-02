# Release 2.15.1

**Date:** 2026-10-02
**Built on:** 2.15.0
**Changes:** M3's targeted stop judges where the leaf is now (the AT-WP02 scatter work,
`design/integrateWindowPositionSensor.md` §0 item 4)

**Patch. One behaviour change, in mode 2 (Lineair) only.** T2 used to cut a targeted M3 stop on the
first reading past its aim. It now cuts when the leaf is past the aim. On the rig this narrowed where M3
comes to rest by about 40 %. A unit in mode 1, or without a fitted and taught position sensor, never
makes a targeted stop and behaves exactly as 2.15.0. No setting, key or payload changed.

> **Soaked** on the bench build of the same code (12.25 h, PASS). **Published to the ROTA soak channel
> as seq 60** on 2026-10-02, and 2344 pulled it by itself (see *Verification*).

## What changed

- **A targeted stop judges where the leaf is now** (`m3_ahead_x10()`, `relay_controller.cpp`).
  - **Why:** T17 reads the encoder about every 180 ms on the rig (its poll is a sleep after a ~75 ms
    read), and the encoder publishes a new value once per 100 ms window. So the first reading past the
    aim lay anywhere up to ~2 % of the stroke past it, and that was ~90 % of AT-WP02's per-stop
    scatter.
  - **How:** on each 20 ms tick T2 carries the latest reading forward by the encoder's own rate
    (`30012`) times the reading's age, and cuts when that position reaches the aim.
  - **Guards:**
    - only toward the target;
    - only once two readings of the drive have been seen, because the first may straddle the start;
    - over at most two of the drive's read intervals, so a T17 that has stopped reading cannot carry a
      stopped leaf into its target. `TARGET_MAX_AGE_MS` still accepts a reading 3 s old.
  - **The taught window** that converts mm/s to % comes from a new T4 accessor, `dm_m3_window_mm()`,
    read once per drive.
- **The default lead is 250 ms of travel** (it was 420 ms). Carrying readings forward took ~1 % off the
  overrun, so the rig's stops now settle on a lead of ~1.7–2.1 % rather than ~3.2 %. The old default
  made the first run after a boot land 1.3 % apart by direction. The lead is still learned per
  direction, from every settled stop.
- **Bench builds only (`MODBUS_BENCH`), none of it in this image:**
  - a log of targeted stops: `GET /api/diag/windowpos?cuts`, read by `bin/at_wp_cuts.py`;
  - a relay pulse hook for the minimum-move measurement: `CMD_PULSE` on Q1,
    `POST /api/diag/windowpos {"pulse_ms":N,"dir":"open"|"close"}` (STANDBY only), `?pulses`, and
    `bin/at_wp_minmove.py`;
  - fail-first bit 8192 (`FF212_NOEXTRAP`), which restores the cut on the reading. The mask guard is now
    16383.
- **Build:** `firmware/dependencies.lock` has been tracked since 2.15.0 was released, so a fresh clone
  builds the same managed components as this image (littlefs 1.21.1, led_strip 2.5.5).
- **Version 2.15.1.**

## Verification

| Check | Result |
|---|---|
| Release image | 1 411 200 B, sha256 `0ab68fd718bfe123…`, version string `2.15.1`, no bench suffix; assets stamped `2.15.1` |
| What the image carries | step 2's `m3_ahead_x10()` and `dm_m3_window_mm()` are in the ELF; none of the bench tools are (no `t2_get_cut`, `t2_get_pulse`, `pulse_tick`, `diag_windowpos_*`) |
| `python bin/check_cfg_desc.py` | **PASS**: 51 keys, 40 published; no key or bound changed |
| Scatter, the cause (step 1, `06cd415`) | 20 stops on 2344 split each stop into the reading's place past the aim (σ 0.53 %) and the run-on (σ 0.23 %) |
| **Scatter, the change** (2344, bench builds, 10 stops to 50 % per run) | the old rule: AT-WP02 2.2 % FAIL and 2.0 % PASS (σ 0.56 %), and 1.7 % PASS on fail-first bit 8192 the same evening (σ 0.49 %). Carried forward with the 420 ms default: 2.4 % FAIL while the lead was still learning, then 0.8 % PASS (σ 0.33 %). **With the 250 ms default, from a boot: 1.0 % PASS** (σ 0.24 %, hysteresis 0.0 %). Pooled, 30 stops each: **σ 0.53 % → 0.30 %**. AT-WP03 and `settle` passed on every build |
| Less than predicted | step 1 predicted σ ~0.23 %. The encoder's value is up to one window older than T17's read, which T2 cannot see. Under the old rule that staleness hid in the sample term; carried forward, it shows as run-on |
| **Soak** (bench build of `9bcc1c4`, 2026-10-01 22:16:52 to 2026-10-02 10:31:52) | **PASS**: 12.25 h, 18 judged strokes, every fault counter 0, `mode_changes` 0, gate `position`, no reboot. **In ordinary running:** 9 targeted stops to `graded`'s ~25 %, each judged 0.0–0.3 % past its aim and never before it, came to rest within σ 0.26 % of their targets. 19 vent decisions conform to v2. Heap: 40–67 KB free, largest block 20–31 KB, floor 19 KB from start to end |
| Soaked code against this image | since `9bcc1c4` the release code changed only by `MODBUS_BENCH` blocks (the timing macros compile to nothing in a release build) and comments. Not the same bytes as anything soaked: the version string and line numbers differ |
| **ROTA publish** | **verified**: GitHub Release `v2.15.1`, published 2026-10-02 14:14:50. Not a draft or prerelease, the tag is on `f2b5ed3`, both files downloaded back hash-identical, manifest seq 60. **The channel, verified from the device:** the first forced check, 14:25:01, was offered 2.15.1 |
| **ROTA pull on 2344** | **PASS**, from 2.15.0-bench. The forced check's download failed a second later with sub-code 1 (TLS/pin), as the first downloads after 2.14.0 and 2.15.0 did. The unit's own hourly check found it again at 15:25:35, downloaded and verified it in 12 s, and committed the apply at 15:26:00. After the reboot, `fw_ver` **and** `asset_version` read **2.15.1**. The next check, 15:26:42, read up to date. M3 came up in Lineair (reason `setting`, gate `ok`, law `graded v2`), all 9 rig settings survived, and the heap read 71 KB free with a 31 KB largest block. The boot rows are stamped 15:25:18, before the apply, because the rig's DS1307 runs 47 s behind NTP (its own `SYSTEM 21` row, 15:16:14) |

## Upgrading

- Nothing to do: no setting, key or payload changed.
- **Units in mode 2 with a fitted, taught sensor:** targeted stops land closer to their targets. The
  learned lead settles lower, and the first stops after a boot start from the 250 ms default.
- **5C88 (production)** runs 2.3.1 on `mainstream` with no sensor, and nothing changes there until a
  promotion and a sensor. The rig's figures do not transfer as they stand: plan §3.6, *What transfers to
  5C88*.

## Known limitations

- **Measured on the rig only.** On 5C88 the scatter is probably set by the mechanics (reversal slack,
  rope stretch, the 40 m shaft) more than by timing, and a heavier flap and different relays change the
  rest (plan §3.6).
- **The encoder's staleness within its window stays.** It is up to 100 ms on the rig, and T2 cannot see
  it: the register map has no counter or timestamp. It is now the largest residual on the rig.
- **The deadband is still a fixed 20 mm**, not derived from `travel_m3` (decisions 8 and 10). The
  minimum move was measured on the rig, but on 5C88 the reversal loss probably sets it.

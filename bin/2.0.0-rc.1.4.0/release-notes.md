# 2.0.0-rc.1.4.0 — SD-log format upgrade (`LOG_SENSOR_HR` triplet + `LOG_SUN`)

Minor-version release on top of rc.1.3.3. The minor bump signals an **on-disk format break**: the legacy single-row `LOG_SENSOR` snapshot is sunset and replaced by a three-row `LOG_SENSOR_HR` triplet per sensor cycle, and a new `LOG_SUN` event type persists sunrise/sunset whenever the cached values change. Rotation defaults are bumped to keep the new ~3× row volume comfortably absorbed.

Supersedes rc.1.3.3 as the Phase 7 soak candidate. The 14-day soak clock restarts at day 0.

## Why a minor bump

Three reasons that together justify going from `rc.1.3.x` to `rc.1.4.0` rather than the next patch number:

1. **On-disk row format changes**: post-rc.1.4.0 SD log files contain `SENSOR_HR` and `SUN` rows that pre-rc.1.4.0 firmware never emitted. Operators reading the version string see immediately that a downloaded log file from this unit is not interchangeable shape-with what the prior firmware produced.
2. **Permanent rotation-defaults change**: the per-file cap is now 1 MB (was 512 KB) and the file count is 30 (was 10). Once the new firmware writes any file, those parameters are baked into the SD-card layout going forward; reverting to rc.1.3.x firmware works but reads the new larger files unchanged.
3. **Two coordinated changes ship together**: bundling A (`LOG_SENSOR_HR`) and B (`LOG_SUN`) in one release means the analyst-side parser and plotter only need a single update pass.

The full design discussion lives in `model/logUpdatePlan.md` (status: Approved, all decisions locked 2026-05-23).

## What changed

### Change A — `LOG_SENSOR` sunset → `LOG_SENSOR_HR` triplet

The legacy `SENSOR` row (single row per sensor cycle, T in integer °C, RH in integer %, no wind or window state) is **sunset and no longer emitted**. In its place T4 emits **three companion rows** per 30 s sample, sharing the same Unix timestamp, discriminated by `channel`:

| `channel` | Subject | `value_a` | `value_b` |
|--:|---|---|---|
| 0 | Temperature + humidity | `t_c10` (raw, °C × 10) | `rh` (raw, 0..100 %) |
| 1 | Wind | `wind_dms` (raw, m/s × 10) | `wind_dir_deg` (raw, 0..359 °) |
| 2 | Window-state bitmask | 16-bit packed state — see encoding below | 0 (reserved) |

The bitmask in `channel = 2, value_a`:

```
bits  1..0  = M1 state    (0=CLOSED, 1=MOVING_OPEN, 2=OPEN, 3=MOVING_CLOSE)
bits  3..2  = M2 state    (same encoding)
bits  5..4  = M3 state    (same encoding)
bits 11..6  = reserved (0)
bit  12     = EG1_BIT_WIND_OVERRIDE
bit  13     = EG1_BIT_MOTOR_ALARM
bit  14     = EG1_BIT_CALIBRATING
bit  15     = reserved (0)
```

The pre-existing `LOG_SENSOR` enum value and `"SENSOR"` CSV type-column string are retained in the firmware so historical SD files served via `/api/log/download` continue to display unchanged. Only the emit site at `data_manager.cpp::handle_sensor_reading()` is changed.

### Change B — new `LOG_SUN` event type

A new row records sunrise + sunset (local-time minutes from midnight) whenever T4's cached values change. In steady-state operation:

- Once at boot (sentinel cache vs first computed value)
- Once per local-midnight rollover (sunrise/sunset shifts ~1–2 min/day)
- Once per operator coordinate edit via Q4

Row shape: `SUN,SYS,0,0,sunrise_min,sunset_min`. Example: `2026-05-23T00:01:12,SUN,SYS,0,0,330,1296` (sunrise 05:30, sunset 21:36 local).

Closes the gap surfaced during the thermal-profile plotter work: prior to this change the plotter had to fetch `/api/status` for sunrise/sunset because the SD log carried no record, which broke per-day night-shading for historical data.

### Rotation defaults bumped (permanent — new operational defaults)

| Parameter | Pre-rc.1.4.0 | rc.1.4.0+ |
|---|---:|---:|
| `SD_ROTATE_BYTES` | 512 KB | 1 024 KB |
| `SD_MAX_FILES` | 10 | 30 |
| `SD_MIN_FILES` | 3 | 5 |
| `SD_FREE_MIN_BYTES` | 2 MB | 4 MB |

At ~483 KB/day from Change A (vs ~166 KB/day pre-sunset), the new defaults give ~63 days of on-SD history at full cap. SD-card footprint at full retention is ~30 MB — negligible against any reasonable card size, and the daily T14 upload removes uploaded files anyway.

## What did NOT change

- `firmware.bin` API surface — no new endpoints, no removed endpoints, no JSON-schema changes.
- NVS schema — no new keys, no removed keys.
- T14 / status-website wire protocol — unchanged; the canonical status JSON still carries the same fields.
- Web assets — `web-assets-2.0.0-rc.1.4.0.zip` is byte-identical to rc.1.3.3 apart from the embedded version stamp.
- Bootloader and partition table — byte-identical to rc.1.3.3.
- All prior fixes (rc.1.1, rc.1.2, rc.1.2.1, rc.1.3, rc.1.3.1, rc.1.3.2, rc.1.3.3) preserved verbatim. In particular the rc.1.3.3 fix for the lwIP-asserted panic in the 24-hour NTP-resync path (missing `esp_sntp_stop()`) is retained.
- The legacy `LOG_SENSOR` enum value and `"SENSOR"` CSV type-column string are retained for historical-file readability — only the emit site is removed.

## Build delta vs rc.1.3.3

| Metric | rc.1.3.3 | rc.1.4.0 | Δ |
|---|---:|---:|---:|
| Firmware bin | 1 352 464 B | 1 353 088 B | **+624 B** (new SENSOR_HR triplet emit + LOG_SUN helper + bitmask packer + change-detect cache) |
| ELF (symbols) | 13 048 260 B | 13 051 944 B | +3 684 B |
| Web assets ZIP | 101 133 B | 101 133 B | 0 (byte-identical) |
| Bootloader | 22 528 B | 22 528 B | 0 |
| Partitions table | 3 072 B | 3 072 B | 0 |
| RAM static | unchanged | unchanged | 0 |

## SHA-256

```
64b55449090dadf37994ffdc6da80cfe3217431f165093f099069039b2b2248c  greenhouse-controller-2.0.0-rc.1.4.0.bin
5691221e54df582269143620b418d975fbb873397b8d0d1684c59e7da63d91cb  web-assets-2.0.0-rc.1.4.0.zip
```

## Deployment record

**Not flashed yet** — bench unit 0x2344 (192.168.20.160) was deliberately kept on rc.1.3.3 during the build pass so this firmware can land at a chosen kick-off moment rather than as an immediate-OTA event. The build itself succeeded cleanly (PIO exit 0, no warnings worth flagging) on the developer's workstation.

When ready, deployment follows the same path as rc.1.3.3:

1. Re-authenticate as admin via `/api/login`.
2. `POST /api/ota/firmware` with `greenhouse-controller-2.0.0-rc.1.4.0.bin`.
3. `POST /api/ota/assets` with `web-assets-2.0.0-rc.1.4.0.zip` (or rely on the 120 s firmware-only fallback timer since the assets are unchanged).
4. T13's `reboot_worker_task` reboots the unit.
5. Verify `/api/status` reports `fw_ver = 2.0.0-rc.1.4.0` and `asset_version = 2.0.0-rc.1.4.0`.
6. Optional: erase any stale coredump via `POST /api/coredump/erase`.

## Verification on bench (post-flash)

Per `model/logUpdatePlan.md` §4.3 — six checks the operator should walk through on the bench unit during the first hour after flashing:

1. **Boot smoke**: within the first 60 s of new-firmware uptime the SD log shows at least one each of `SENSOR_HR,SYS,0,0,*,*`, `SENSOR_HR,SYS,1,0,*,*`, `SENSOR_HR,SYS,2,0,*,*`, plus exactly one `SUN,SYS,0,0,*,*` with plausible sunrise (200..500) and sunset (1000..1400) minutes. No new `SENSOR,SYS,*,*` rows.
2. **Bitmask integrity**: trigger an M1 OPEN via the dashboard. The next `SENSOR_HR,SYS,2,*,*` row reflects the bitmask change (bits 1..0 transition through `MOVING_OPEN` (=1) → `OPEN` (=2)).
3. **Midnight `SUN`**: leave the unit running across local midnight. Confirm exactly one new `SUN,SYS,0,0,*,*` row appears within ~10 minutes after 00:00 local. Sunrise should shift ±1–2 minutes vs the prior day.
4. **Coordinate-edit `SUN`**: from the web GUI change lat/lon by 0.5°. The resulting `SETPT,WEB,0,21,*,*` rows are followed by exactly one fresh `SUN,SYS,0,0,*,*` row. Restore the coordinates afterwards; verify another `SUN` row reflects the restoration.
5. **24-hour quiet test**: with no operator interaction and no boot, the SD log contains exactly one `SUN` row per local day. `grep -c "SUN,SYS" <today's-file>` returns 1 after the unit has run through one midnight.
6. **Daily T14 upload**: trigger manual upload; the new file containing `SENSOR_HR` + `SUN` rows reaches the status server intact (file size ≈ a third bigger per day than under rc.1.3.x).

## Phase 7 soak — day-counter resets

Day-counter starts at day 0 against rc.1.4.0 at the moment of OTA-flash. Acceptance criteria (14 days continuous):

- Zero `ESP_RST_PANIC` events.
- Zero `ESP_RST_TASK_WDT` events.
- Zero coredump captures.
- Three or more successful 24-hour NTP-resync windows traversed (the rc.1.3.3 fix continues to hold).
- SD log shows the expected row pattern: continuous `SENSOR_HR` triplets at the configured poll cadence, one `SUN` row per local day, no anomalous gaps.
- T14 daily upload success rate ≥ 95 %.

If any criterion fails, fix on dev branch, bump to `2.0.0-rc.1.4.1` (or the appropriate sub-version), restart the 14-day clock.

## Analyst follow-on (already complete in this release pass)

- ✅ `log/logparser.py` — `SENSOR_HR` and `SUN` decoders added; legacy `SENSOR` retained for pre-rc.1.4.0 files.
- ✅ `log/logparser.md` — new event-type sections documenting both row families with the bitmask encoding spec; top-of-file note explaining the rc.1.4.0 format upgrade and backward compatibility.
- ✅ `temp/plot_daily.py` — extended to consume `SENSOR_HR_0` (T+RH at 0.1 °C precision), `SENSOR_HR_1` (continuous wind trace, replaces the old "spot values from ALARM rows" fallback when present), `SENSOR_HR_2` (bitmask), and `SUN` (per-day night-shading; falls back to live `/api/status` only when no `SUN` row exists in the day's window). Backward-compatible with legacy `SENSOR` rows; mixed-format days render with both source types properly stitched.
- ✅ `design/technicalSoftwareDesignSpecification.md` §5.3 — event-log spec rewritten around the triplet + sun row + new rotation defaults; §5.5 + §4.3 T8 fixed (`STATUS_PAGES = 6 → 7`, page 2 corrected from stale "window-demand" to "Mode + Session", missing page 6 "firmware" added); §4.3 T4 extended to document the `LOG_SUN` change-detect emit.
- ✅ `design/functionalRequirementsSpecification.md` — `FR-LG06` updated (rotation default 512 KB → 1 024 KB, file count 10 → 30); `FR-LG09` rewritten (three-row triplet, 0.1 °C T precision, wind + window-state in the snapshot); new `FR-LG12` added (sun-time persistence).
- ✅ `model/thermalProfileCampaign.md` §5.1 — bitmask encoding already locked (GAP-fold note) from the prior decision pass; no further update needed.

## Files

| File | Description |
|---|---|
| `greenhouse-controller-2.0.0-rc.1.4.0.bin` | Firmware image — flash via OTA `/api/ota/firmware` |
| `firmware-2.0.0-rc.1.4.0.elf` | Symbol-bearing ELF — keep for future coredump decode |
| `bootloader-2.0.0-rc.1.4.0.bin` | Bootloader (byte-identical to rc.1.3.3) |
| `partitions-2.0.0-rc.1.4.0.bin` | Partition table (byte-identical to rc.1.3.3) |
| `web-assets-2.0.0-rc.1.4.0.zip` | STORE-only ZIP for `/api/ota/assets` (byte-identical to rc.1.3.3 apart from manifest version stamp) |

Per `.gitignore`, the binaries above are not tracked in git — they're rebuildable from the rc.1.4.0 source commit via `bin/build_release.ps1`. Only this release-notes.md is tracked.

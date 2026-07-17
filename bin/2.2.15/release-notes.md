# Release notes — 2.2.15

- **Date:** 2026-07-17
- **Built on:** 2.2.14
- **Type:** bug-fix (patch)
- **Closes:** gh#42 (incomplete gh#36 fix — T14 upload enumerators)

## Summary

T14 log upload no longer stops silently once a unit's SD card holds more than
~21 CSV files (gh#42). The two enumerators that choose the next file to upload
now scan into a correctly-sized buffer, so the newest closed files stay visible
to the uploader.

## Problem (field-confirmed on 5C88)

5C88 (production, fw 2.1.3) stopped delivering logs to the status site after
2026-07-13 19:29 with **no error** — while status POSTs, TLS, heap, SD rotation
and free space all stayed healthy. Three consecutively-rotated local files
(`5C88_20260713192815` -> `..0715142154` -> `..0717090944`, ~1.8-day cadence)
proved T9 logging was fine; the SD audit rows showed the daily upload slot
logging `value_a=0, value_b=2` ("no closed file on SD") at 03:30 every day even
when closed files demonstrably existed.

## Root cause

`event_logger_next_pending()` (`event_logger.cpp:1020`) and
`event_logger_newest_closed()` (`:953`) — the enumerators T14's daily slot and
on-rotation trigger both drain through (`upload_pending()` in `status_post.cpp`)
— scanned the SD file list into a local `char list[512]`. The correct size is
the shared `SD_LIST_BUF_LEN` = `SD_MAX_FILES(30) * (SD_NAME_ONLY_LEN(28)+1) + 1`
= 871 B. At ~24 B per entry, 512 B holds only ~21 names; past that the scan
silently truncates (gh#36 `storage_sd_list_csv()` behaviour) and drops the
**newest** closed files — exactly the ones lex-greater than the `log_last_up`
dedup latch — so `next_pending()` returns false and nothing is uploaded.

## What changed

- `firmware/src/event_logger/event_logger.cpp`:
  - `:953` `event_logger_newest_closed()` — `char list[512]` -> `list[SD_LIST_BUF_LEN]`.
  - `:1020` `event_logger_next_pending()` — `char list[512]` -> `list[SD_LIST_BUF_LEN]`.
  - `:437` `check_free_space()` and `:668` `write_to_sd()` keep `char list[512]`
    (count-only vs `SD_MIN_FILES`=5; truncation can't change that verdict, and
    they run on T9's 6 KB stack) — clarifying comment added.

Only these two buffer sizes change behaviourally; no API, log-format, or control
change.

## Why patch (not minor)

Pure bug-fix: no new API field, NVS key, config-shadow field, or feature. Same
class of fix as gh#36 (2.1.2).

## What did NOT change

- No `logparser.py` change (audit codes unchanged).
- ROTA / OTA-apply / quiet gate / anti-downgrade — untouched.
- No farmer-visible behaviour; no boer-manual change (log upload is backend/admin).
- T9 rotation, SD write path, free-space handling — unchanged.

## Interim recovery for a stuck unit (no firmware update)

Delete ~10 of the oldest already-uploaded CSVs via the web GUI Log tab (keep
anything newer than the current `log_last_up` latch). The file count drops under
~21, the scan fits, and `upload_pending()` drains the backlog on the next
daily/rotation trigger. Verify a new file lands in the status site's
`.../hbwv/log/logs/`.

## Build artefacts

| Artefact | Size (bytes) | SHA-256 |
|---|---|---|
| `greenhouse-controller-2.2.15.bin` | _(filled after build)_ | _(filled after build)_ |
| `web-assets-2.2.15.zip` | _(filled after build)_ | _(filled after build)_ |

## Verify post-OTA

- **Paired commit:** `fw_ver == asset_version == 2.2.15` from `/api/status`.
- **gh#42:** on a unit whose SD holds >21 CSVs, the next rotation / daily slot
  uploads the newest closed file (audit row `value_a=1, value_b=1`), and the
  `value_a=0, value_b=2` "no closed file" rows stop appearing while closed files
  exist. To exercise the truncation in soak, the card must carry >21 files (or
  temporarily lower `SD_MAX_FILES`).

## Soak target

FDA4 / 2344 (ROTA pull). Note: a freshly-booted card with few files uploads fine
on *any* build — the regression only manifests past ~21 files, so soak must run
long enough (or be seeded with >21 files) to actually prove the fix.

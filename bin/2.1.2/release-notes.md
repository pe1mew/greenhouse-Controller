# 2.1.2 — release notes

**Date built:** 2026-07-04
**Built on top of:** 2.1.1 (gh#34 — HTTP status in T14 audit row)
**Closes:** **gh#36** — SD log file listing truncated beyond ~20 files
**Scope:** 2 source files changed + 1 header refactored; no API or NVS changes.

---

## The problem this addresses

`storage_sd_list_csv()` silently truncates its output when the caller's buffer is full — it always returns `STORAGE_OK` with no indication that entries were dropped.

Three callers used 512-byte buffers. With `SD_MAX_FILES = 30` files at ~25 bytes/entry the required size is ~750 bytes, so the buffer overflowed once ~20 files accumulated on the SD card. Two separate symptoms resulted:

**1. Web GUI `/api/log/files` stopped showing new files**
The handler additionally had `LOG_FILES_MAX = 12` (hard cap) and `LOG_FNAME_MAX = 24` (off by 4 vs `SD_NAME_ONLY_LEN = 28`), none of which matched `SD_MAX_FILES`.

**2. T14 upload stopped posting new files to the remote server**
On startup, T14 calls `sd_open_active_file()` which uses `sd_scan()` to find the newest closed file. The truncated scan returned June 24 as newest; all subsequent rotations were silently skipped. Both production unit 5C88 and soak unit 2344 were affected.

---

## What changed

### `firmware/src/event_logger/event_logger.h` — new section added

All SD capacity constants moved here as the **single source of truth**, with a derived buffer-size macro:

```c
#define SD_MAX_FILES      30u
#define SD_NAME_ONLY_LEN  28
#define SD_LIST_BUF_LEN   ((SD_MAX_FILES) * ((SD_NAME_ONLY_LEN) + 1) + 1)  // 871 bytes
```

Callers that `#include "event_logger.h"` (which includes `web_server.cpp`) get these automatically. Changing `SD_MAX_FILES` now propagates to every scan buffer and the web handler cap in one edit.

### `firmware/src/event_logger/event_logger.cpp` — 4 buffer changes

Private `#define` block (SD_ROTATE_BYTES, SD_MAX_FILES, SD_MIN_FILES, SD_FREE_MIN_BYTES, SD_FILENAME_LEN, SD_NAME_ONLY_LEN) removed; all four scan buffers updated:

| Function | Before | After |
|---|---|---|
| `sd_scan()` internal `raw` | `char raw[512]` | `char raw[SD_LIST_BUF_LEN]` |
| `delete_oldest()` | `char list[512]` | `char list[SD_LIST_BUF_LEN]` |
| `rotate_sd_file()` | `char list[512]` | `char list[SD_LIST_BUF_LEN]` |
| `sd_open_active_file()` (T14 startup) | `char list[512]` | `char list[SD_LIST_BUF_LEN]` |

### `firmware/src/web_server/web_server.cpp` — 3 constant changes

| Constant | Before | After |
|---|---|---|
| `LIST_LEN` | `512u` | `SD_LIST_BUF_LEN` |
| `LOG_FILES_MAX` | `12` | `(int)SD_MAX_FILES` |
| `LOG_FNAME_MAX` | `24` | `SD_NAME_ONLY_LEN` |

---

## What this does NOT change

| Subsystem | Reason |
|---|---|
| SD log format | No row changes; only internal buffer sizing |
| NVS / API fields | No new keys, no API schema changes |
| Farmer / boer LCD menus | Unaffected |
| Web UI | No HTML/JS changes; `/api/log/files` now returns the full list |
| OTA paired-commit invariant | No change |

---

## Build artefacts

| File | Size | SHA-256 |
|---|---:|---|
| `greenhouse-controller-2.1.2.bin` | 1 360 384 B | `09399276e20e92665bd306d48416c02cd99eeb845a4d0ee53cdfda002a981ccb` |
| `web-assets-2.1.2.zip`            |   108 073 B | `90bbc4bba02c9c35098ea44fe007dac86475fa32e0d78975f569cb977ffdd9df` |
| `bootloader-2.1.2.bin`            |    22 528 B | `c0d8cf81c55ea10236b94317e2c6bc837cdfd00bc251c6d86153656d821c7959` |
| `partitions-2.1.2.bin`            |     3 072 B | `18fbe59ac37567be8897bc7f5266aec2ba2df85934a3b8fb9b229d8e59e7e74d` (unchanged) |
| `firmware-2.1.2.elf`              |  12 795 KB  | (for coredump decoding) |
| `firmware-2.1.2.map`              |  10 765 KB  | (for coredump decoding) |

---

## Verifiable post-OTA

1. `GET /api/status` → both `fw_ver` AND `asset_version` must read `2.1.2`. ✓ (confirmed on 2344)
2. `GET /api/log/files` (admin session) → response now includes all files up to `SD_MAX_FILES = 30`, not capped at 12.
3. After next log rotation, T14 upload resumes from the actual newest closed file.

---

## Open issues after this release

- **gh#7** — bug: serial-port WDT freeze
- **gh#27** — T15 heap-drop sampling timing
- **gh#32** — SD handling on LCD/keypad GUI
- **gh#34** — HTTP return code in SD audit row (partially addressed in 2.1.1; server-side limit now resolved)
- **gh#35** — independent wind averaging window (`avg_win_wind`)

# 2.0.0-alpha.6.24 — bug-fix bundle (LED + GUI serving)

Two operator-visible issues reported during alpha.6.23 verification, both root-caused and fixed in a single tag. Several adjacent latent bugs were uncovered and fixed in the same pass.

## Operator-visible fixes

| # | Issue | Symptom | Root cause | Fix |
|---|---|---|---|---|
| 1 | Heartbeat LED wrong cadence | LED blinking at ~0.1 Hz instead of 1.20.3's 1 Hz | `gpio_toggle(PIN_HB_LED)` lived inside `heartbeat_task` (5 s period). T1 didn't toggle it at all. | Moved `gpio_toggle(PIN_HB_LED)` into `task_watchdog` at the 500 ms tick. Removed the duplicate in `heartbeat_task`. Matches 1.20.3 design exactly. |
| 2 | GUI not visible — only "test data" | Browser shows a small placeholder page saying "Web assets not yet uploaded" | 4 sub-issues compounded. See below. | See below. |

## Sub-issues underneath the "no GUI" symptom

| Sub | Component | Defect | Fix |
|---|---|---|---|
| 2a | `serve_lfs_file` (web_server.cpp) | Single 4 KB heap buffer + `strlen()`-based response sizing. Truncated any file > 4 KB and treated 0x00 bytes as EOF. `index.html` (38 900 B) capped to 4 095 B. | Rewrote as a stdio loop (`fopen` + `fread` + `httpd_resp_send_chunk`) against the VFS mountpoint. Streams arbitrarily large files; no NUL hazard. |
| 2b | LIB-9 driver header | `select_mountpoint` was `static`, callers couldn't compose VFS paths. | Added public `littlefs_mountpoint(partition)` accessor returning `"/lfsa"` or `"/lfsb"`. |
| 2c | `s_asset_ver[16]` (data_manager.cpp) | Same off-by-one trap as the `fw[16]→[24]` fix in alpha.6.17.1. `"2.0.0-alpha.6.24"` (16 chars) NUL-truncated to `"2.0.0-alpha.6.2"`. | Bumped to `[24]`. |
| 2d | T13 OTA-assets | `littlefs_mount` of the **inactive** partition fails on a fresh chip (the lfs1 partition was never formatted — only lfs0 was, by the alpha.2.10 tickle). `set_error_locked("inactive LittleFS mount failed")` aborted T13 before any extraction. | Added a `littlefs_format(inactive)` fallback in T13. The format costs ~10 s of erase but only runs on a genuine first-use of the inactive bank. |
| 2e | `serve_lfs_file` 404 placeholder | The placeholder told operators to upload assets via `POST /api/web`. That endpoint is for status-website **settings**, not asset upload. Confusing dead-end. | Corrected to `POST /api/ota/assets` with a hint about the 1.20.x web-assets ZIP. |
| 2f | Route table | `/index.html` direct URL returned 404 — only `/` (root) was registered. The 1.20.3 ESPAsyncWebServer had a wildcard fallback that fell through to LFS. | Added `s_uri_index` pointing the same `root_handler`. Now `/` and `/index.html` are byte-identical. |

## What changed

- `firmware/src/watchdog/watchdog.cpp` — `gpio_toggle(PIN_HB_LED)` added at the top of T1's tick loop, plus `#include "gpio_util.h"`.
- `firmware/src/main.cpp` — removed `gpio_toggle(PIN_HB_LED)` from `heartbeat_task` (kept the read-back for the log line).
- `firmware/src/web_server/web_server.cpp` — `serve_lfs_file` rewritten (stdio + chunked send); `s_uri_index` added; placeholder text corrected.
- `firmware/src/ota_manager/ota_manager.cpp` — T13 format-on-mount-failure for inactive partition.
- `firmware/src/data_manager/data_manager.cpp` — `s_asset_ver[16] → [24]`.
- `drivers/littleFS/src/littlefs_storage.h` + `.cpp` — public `littlefs_mountpoint()` accessor.
- `firmware/platformio.ini` — `FIRMWARE_VERSION` → `2.0.0-alpha.6.24`.

## Acceptance — hardware verified on 192.168.20.160

### LED — serial confirms 1 Hz cadence

```
I (51119) T1: [T1] tick=100  uptime=50s        ← tick 100 at 50 s = 500 ms/tick
```

### Assets matrix after uploading 1.20.3 web-assets ZIP

```
GET /              → HTTP 200, 38 900 B, text/html
GET /index.html    → HTTP 200, 38 900 B, text/html
GET /style.css     → HTTP 200, 10 623 B, text/css
GET /app.js        → HTTP 200, 40 810 B, application/javascript
GET /manifest.json → HTTP 200,     50 B, application/manifest+json
```

Before alpha.6.24:
- `/app.js` capped at **4 095 B** (= 4096-1 NUL)
- `/index.html` direct URL returned 404
- `asset_version` returned `"2.0.0-alpha.6.2"` (truncated)

After alpha.6.24:
- All five paths serve full content
- `/api/status` reports `asset_version: "2.0.0-alpha.6.24"` (full, untruncated)
- `/api/ota/status` reports `state:"idle", accepted:true`

### Live OTA performed during this tag

Uploaded `bin/1.20.3/web-assets-1.20.3.zip` (90 775 B, STORE-only) via `POST /api/ota/assets`. T13 logged:

```
[T13] inactive LFS mount failed (5) — formatting first-time
[T13] inactive LFS formatted + mounted
[T13] Asset extraction starting (90775 B)
[T13] Asset-only OTA — mirroring to active LFS A
[T13] Active LFS mirrored OK (4 file(s))
[T13] Asset OTA complete — reboot in 1 s
```

Device rebooted cleanly. Active LittleFS now holds the 1.20.x GUI. Both partitions (lfsa + lfsb) now have valid filesystems.

## Build delta vs alpha.6.23

| Metric | alpha.6.23 | alpha.6.24 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 305 213 B | **1 307 136 B** | +1 923 B |
| RAM static | 60 256 B | (similar) | unchanged |

bin sha256: `8620AC00EB9234FE…`

+1.9 KB for the chunked-streaming loop, new `/index.html` URI struct, format-fallback code in T13, and the small adjustments in data_manager / watchdog.

## Notable

This is the first alpha where the **actual GUI is live on the device**. Every prior alpha (since 6.16) served the 404 placeholder because nothing had ever populated LittleFS with the real assets. Operator workflow going forward:

```
# After first flash of any 2.0.0-alpha.6.X firmware:
admin_login=$(curl -c jar -X POST .../api/login -d '{"role":"admin","pin":"12345678"}')
curl -b jar -X POST --data-binary @bin/1.20.3/web-assets-1.20.3.zip http://device/api/ota/assets
# Device reboots in ~1 s; GUI live ~12 s after that.
```

The 1.20.3 ZIP is forward-compatible with the alpha.6.x firmware (the GUI just doesn't display the new alpha.6 features yet; it sees them via `/api/status` though). A 2.0.0 web-asset bundle will be built when the alphas converge.

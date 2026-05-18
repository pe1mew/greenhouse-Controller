# 2.0.0-alpha.6.20 — Phase 6.16-ζ (T11 OTA + web-tab routes)

## What landed

Five new routes on the minimal T11 web server:

| Route                          | Method | Role gate | Purpose                                                      |
|--------------------------------|--------|-----------|--------------------------------------------------------------|
| `/api/ota/status`              | GET    | farmer+   | Current OTA state machine (state, progress, error, bank, accepted) |
| `/api/ota/firmware`            | POST   | admin     | Streams `.bin` chunks → `ota_firmware_begin/write/end` (T13) |
| `/api/ota/assets`              | POST   | admin     | Streams `.zip` chunks → `ota_assets_begin/accumulate/end` (spawns T13) |
| `/api/web`                     | GET    | admin     | Returns web-tab settings (URL, interval, expose mask, log-upload schedule) |
| `/api/web`                     | POST   | admin     | Validates + writes web-tab settings; synchronous `dm_reload_web_cfg()` |

Brings T11 to **24 / 25** of the v1.20.3 route set. Only `/ws` (WebSocket dashboard push) remains for Phase 6.16-η.

## Bug fix shipped under this tag

`ota_get_state()` previously called `xSemaphoreTake(s_mx, portMAX_DELAY)` unconditionally. `s_mx` is created lazily inside `ota_mx_init()` which only runs from `ota_firmware_begin()` / `ota_assets_begin()` — so a fresh boot followed by `GET /api/ota/status` panicked the httpd worker (assertion failure on `xSemaphoreTake(NULL, …)`).

Pre-2.0 nothing read OTA state before write began, so the bug had been dormant. The new `/api/ota/status` handler is the first read-before-write caller. Fix is minimal: read accessor now mirrors `set_state_locked`'s NULL-safe pattern (skip the lock when `s_mx == NULL` — the read is a single byte and benign).

## Acceptance — hardware verified on 192.168.20.160

Login as admin (PIN `12345678`), then:

```
GET  /api/ota/status   → {"ok":true,"state":"idle","progress":0,"error":"","bank":"A","accepted":true}
GET  /api/web          → {"url":"","interval_s":240,"enable":0,"expose":63,"log_h":3,"log_m":15,"log_rot":1,…}

POST /api/web — validation matrix:
  url=example.com/api.php           → 400 "URL must start with http:// or https://"
  url=https://x.com/api.php?x=1     → 400 "URL must not contain ? or #"
  url=https://x.com/foo.bar         → 400 "URL must end with \"api.php\""
  secret="short"                    → 400 "secret too short"
  interval_s=30                     → 400 "interval out of range"
  log_h=30                          → 400 "bounds"
  
  Full valid payload                → 200 {"ok":true}
  Follow-up GET                     → all fields persisted, including 16-byte secret (not echoed)

Role enforcement (farmer cookie):
  GET  /api/ota/status              → 200 (farmer min role passes)
  GET  /api/web                     → 403 "admin only"
  POST /api/web                     → 403 "admin only"
  POST /api/ota/firmware            → 403 "admin only"
```

OTA POST upload paths (firmware + assets) NOT exercised on a live unit because it would replace the running firmware. Code path inspected against the 1.20.3 archived original (lines 1133-1258) — identical chunk-receive + `ota_*_begin/write/accumulate/end` sequence, just with `httpd_req_recv` instead of `AsyncWebServerRequest::onBody`.

## Artifacts

| File                                  | Bytes   | Notes                          |
|---------------------------------------|---------|--------------------------------|
| firmware-2.0.0-alpha.6.20.bin         | 1296496 | Flash @ 0x20000                |
| firmware-2.0.0-alpha.6.20.elf         | 12605024| For `addr2line` if needed      |
| partitions.bin                        | 3072    | Same partition table as alpha.0|
| bootloader.bin                        | 22528   | Unchanged from alpha.1         |

Flash usage: **1296481 bytes (61.8 %)** of the 2 MB OTA bank.
RAM usage: **60248 bytes (18.4 %)** of 320 KB DRAM.

## Deferred to later phases

- `/ws` WebSocket + status push task → Phase 6.16-η
- T1 watchdog + `ota_check_rollback` boot wiring + `main.cpp` replaces `app_main_stub.cpp` → Phase 6.N
- 14-day soak → Phase 7 (2.0.0-rc.1)

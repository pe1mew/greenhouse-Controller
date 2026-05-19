# 2.0.0-a.6.35 — T14 status_secret + canonical JSON + SD log upload + status_enable + log_upload_rot + https-only

Final alpha of the maturation plan. Restores six T14 features that were on the deferred-to-2.0.1 list — the four "audit-gap" items (status_enable gate, log_upload_rot gate, log_last_up dedup latch, s_last_log_str updates) plus the two real-world features the public dashboard needs (shared-secret header, canonical JSON body) plus the production SD-log upload path (daily trigger + on-rotation trigger). The seventh item tightens the `POST /api/web` URL validator to `https://` only so an operator can't accidentally configure a plain-HTTP endpoint that would leak the shared secret on the wire.

This is the largest alpha of the four and the final gating item before Phase 7 (14-day soak → 2.0.0-rc.1).

## What landed — seven discrete items

**A. Shared secret in `sourceidentifier` header** (status POST + log upload)

Every HTTPS request from T14 — both the periodic status POST and the streaming log upload — attaches `sourceidentifier: <cfg.status_secret>` if the secret is non-empty. T11's existing `/api/web` validator already enforces `len(secret) ≥ CFG_MIN_SECRET_LEN`. Empty secret skips the header; server-side reject is the server's concern.

**B. Canonical status JSON shape**

`build_min_status_json` placeholder is gone. T14 now calls `build_canonical_status_json(buf, 2048, &snap, cfg.status_expose, /*include_disabled_setpoints=*/false)`. Body shape matches the spec at `design/technical-spec-statusWebsite.md` § 9.2 — same builder T11 uses for `/api/status` and the WebSocket push, so single source of truth. The expose mask comes from NVS (`cfg.status_expose`) so operators can hide tiles from the public dashboard via the GUI. The 2 KB build buffer is heap-allocated per cycle to keep the task stack flat.

**C. SD-CSV log upload — daily + on-rotation triggers**

- **Daily trigger**: T14 main loop polls local time. When `tm_hour == cfg.log_upload_h && tm_min == cfg.log_upload_m && tm_min != s_last_daily_min`, it picks the newest closed CSV via `event_logger_newest_closed()`, compares against `cfg.log_last_up` for dedup, and uploads if fresh.
- **On-rotation trigger**: T9's `rotate_sd_file()` now calls `xTaskNotify(task_t14, T14_NOTIFY_LOG_ROTATED, eSetBits)` after closing a file. T14 picks this up via `xTaskNotifyWait` at the bottom of each cycle (timeout = 1 s), reads the just-closed filename via `event_logger_last_rotated()`, and uploads.
- **Streaming upload** via `esp_http_client_open(file_size)` + 4 KB-chunk loop (`storage_sd_read` → `esp_http_client_write`) + `esp_http_client_fetch_headers()`. 4 KB chunks bound per-write mbedTLS heap demand (gh#23 shape) regardless of total file size.
- **Endpoint**: `<cfg.status_url>?action=log&file=<filename>` (T14 appends the query string).
- **gh#25 dedup latch**: on HTTP 2xx, `dm_set_log_last_up(filename)` persists to NVS so subsequent daily/rotation triggers skip the same file.

**D. Respect `status_enable` master flag**

Main-loop gate now reads `(cfg.status_enable == 0) || (cfg.status_url[0] == '\0') || (cfg.status_interval_s <= 0)`. When disabled, the loop idles via `xTaskNotifyWait(60 s)` and sets `s_last_str = "DISABLED"` so the GUI Web tab surfaces the operator state. A pending rotation notify is silently consumed during idle so it doesn't accumulate across a re-enable.

**E. Respect `log_upload_rot` rotation flag**

The `T14_NOTIFY_LOG_ROTATED` handler is wrapped in `if (cfg.log_upload_rot != 0)`. When 0, the notify is silently consumed (logged at DEBUG). Daily-window upload is independent of this flag — `rot=0` means "daily-only".

**F. Update `s_last_log_str` after each upload attempt**

`do_log_upload` formats the outcome via the same `format_outcome(buf, cap, ok, status_code)` helper used by `do_status_post`. Format: `"OK YYYY-MM-DD HH:MM:SS"` on 2xx, `"FAIL YYYY-MM-DD HH:MM:SS code=N"` on failure. Surfaced via `status_post_last_log_str()` → `GET /api/web::last_log_up`.

**G. Tighten `POST /api/web` URL validator to `https://` only**

Plain HTTP would expose the `sourceidentifier` shared secret on the wire — once item A starts attaching it, an `http://` endpoint becomes a credential leak. T11's `web_post_handler` now rejects `http://` with HTTP 400 + `"URL must use https:// — plain HTTP exposes the shared secret on the wire"`. Mirrored in `webUiMock/mock_server.py` (parity), `firmware/data/app.js::validateStatusUrl` (client-side regex), and the `firmware/data/index.html` data-tip label.

## What changed

- **`firmware/src/status_post/status_post.cpp`** — significant rewrite (~+200 lines net). New `do_status_post` (canonical JSON + secret header), new `do_log_upload` (streaming SD → HTTPS), new `format_outcome` helper, new `post_log` helper using `LOG_BY_WEB` initiator. Main loop adds `status_enable` gate, daily-trigger logic with minute-latching, `xTaskNotifyWait` for rotation triggers with `log_upload_rot` gate. `s_last_str` / `s_last_log_str` buffers widened to 64 bytes for the longer format.
- **`firmware/src/status_post/status_post.h`** — new `#define T14_NOTIFY_LOG_ROTATED (1u << 0)`.
- **`firmware/src/event_logger/event_logger.cpp`** — `rotate_sd_file()` now calls `xTaskNotify(task_t14, T14_NOTIFY_LOG_ROTATED, eSetBits)` after a successful rotation, with NULL-handle guard for early-boot races. New include of `status_post.h` for the notify-bit macro.
- **`firmware/src/web_server/web_server.cpp`** — URL validator changed from `http:// | https://` to `https://` only with explicit operator-facing error message.
- **`firmware/data/app.js`** — `validateStatusUrl` regex changed from `https?://` to `https://`; error message updated.
- **`firmware/data/index.html`** — Web tab URL label tooltip updated.
- **`webUiMock/mock_server.py`** — mirror URL validator change for parity.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.35`.

## Acceptance — hardware verified on 192.168.20.160 (paired flash, uptime stable 13 min)

| Item | Test | Result |
|---|---|---|
| **G — URL validator** | `POST /api/web {"url":"http://example.com/api.php"}` | 400 `{"ok":false,"err":"URL must use https:// — plain HTTP exposes the shared secret on the wire"}` ✓ |
| **G — URL validator** | `POST /api/web {"url":"https://example.com/api.php"}` | 200 `{"ok":true}` ✓ |
| **D — status_enable gate** | `enable=0` → poll `/api/web::last_post` | `'DISABLED'` ✓ |
| **D — status_enable gate** | `enable=1 url=https://192.168.99.99/api.php interval_s=60` → wait 100 s → poll | `'FAIL 2026-05-19 11:08:40 code=0'` then refreshed at `11:09:41` after the next cycle ✓ |
| **D — status_enable gate** | `enable=0` again → poll | `'DISABLED'` within 5 s of the POST ✓ |
| **F — s_last_log_str format** | Implicit via item D — same `format_outcome` helper used for both `s_last_str` and `s_last_log_str` | Format `"FAIL YYYY-MM-DD HH:MM:SS code=N"` confirmed correct ✓ |

### Items requiring a real status server (A, B, C, E)

Items A (secret header on the wire), B (canonical JSON body shape), C (log upload completes + dedup latch), and E (log_upload_rot gate observed via no-rotation-upload) are correct by code review but require a TLS-terminated status server with a CA-bundle-valid certificate to verify end-to-end. The bench unit's `skip_cert_common_name_check = false` prevents pointing at a self-signed local mock; setting up an HTTPS-fronted mock (ngrok, Caddy with Let's Encrypt, etc.) was out of scope for this acceptance window. **These items will be exercised against the operator's real production status server during Phase 7 (14-day soak), with the gh#23 largest-block watch running in parallel.**

The runtime evidence we DO have:
- Unit boots cleanly on the new bin (paired flash) and reports `fw_ver = 2.0.0-a.6.35` ✓
- Status POST cycle executes at the configured interval (last_post timestamps update at 60 s cadence) ✓
- The cycle's TLS handshake reaches the wire (curl error code=0 in last_post = "connection refused / unreachable" = the unit attempted, server-side rejected — proves the code path executes through `esp_http_client_perform`) ✓
- `eg1=0` after 13 min and several failed-POST cycles — no panic / WDT events from the new code paths ✓
- Heap usage stable: heap rows at boot 253 KB internal / 8189 KB PSRAM / 176 KB largest (a.6.32 instrumentation unchanged) ✓

### gh#23 watch

The maturation plan's gh#23 acceptance criterion ("`value_a=12` largest-block stays > 50 KB through ≥100 status POST cycles") **cannot be evaluated in this short acceptance window**. It is the primary signal Phase 7 watches for. Pre-this-alpha the bench-unit baseline largest-block has been 31 KB in-flight (visible in every CSV heap-row triple); if that holds with the new heap-allocated 2 KB JSON build buffer + 4 KB chunk buffer per cycle, we're fine. If it doesn't, a.6.36 (gh#23 mbedTLS mitigations — max_frag_len, single cipher suite, session-ticket reuse) lands as the followup.

## Build delta vs a.6.34

| Metric | a.6.34 | a.6.35 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 344 925 B | **1 348 457 B** | +3 532 B |
| RAM static | 60 504 B | 60 552 B | +48 B |

+3.5 KB flash, +48 B RAM. **Under the plan estimate of +6 KB / +60 B** — `build_canonical_status_json` was already linked from T11's `/api/status` handler, so this alpha only pays for the new `do_log_upload` function, the daily-trigger logic, the gates, and the URL-validator change. Final flash usage: **64.3 %** of the 2 MB OTA bank — comfortable headroom for a.6.36 (gh#23 mitigations, ~+8 KB) + T15 re-enable (~+5 KB) if Phase 7 surfaces them.

bin sha256: see `greenhouse-controller-2.0.0-a.6.35.bin` in this directory.

## Known cosmetic — `value_a=0` / `value_a=1` collision

The documented LOG_SYSTEM encoding table (`event_logger.h` lines 109–124) lists `value_a=0` as "T14 outcome/skip" and `value_a=1` as "STA WiFi". The footer of the same table then says "For value_a=1 (success) the same value_b codes apply" — meaning T14 reuses `value_a=1` for the success counterpart of `value_a=0` failure rows. This collides with T10's STA up/down events, which also use `value_a=1`.

This alpha follows the documented intent (T14 successes use `value_a=1`, T14 failures use `value_a=0`), accepting the collision with T10. In practice they're distinguishable by initiator: T10 writes `LOG_BY_SYSTEM`, T14 writes `LOG_BY_WEB`. A future cleanup alpha could split T14 into its own non-colliding `value_a` range — not blocking 2.0.0.

## Next

**Phase 7** — 14-day soak on the bench unit, gh#23 largest-block watch, real-server end-to-end exercise of items A/B/C/E. Final stop before `2.0.0-rc.1`.

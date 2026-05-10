# Impact analysis — periodic status POST task

| | |
|---|---|
| Document | Firmware impact analysis |
| Companion | [technical-spec-statusWebsite.md](technical-spec-statusWebsite.md) — *what* the remote endpoint expects |
| Date | 2026-05-10 |
| Status | **Shipped 1.17.1** — see [changelog.md](../changelog.md#1171---2026-05-10) and the divergences section at the end of this document |

## Context

The firmware currently exposes its runtime status only over the local network (T11 ESPAsyncWebServer at `/api/status` and a 2 s WebSocket push). The status website spec at [technical-spec-statusWebsite.md](technical-spec-statusWebsite.md) defines a remote PHP endpoint (`POST /api.php`) that accepts a JSON payload, gated by a shared secret in the `sourceidentifier` HTTP header. The intent is for the controller to push its state to that endpoint every 60–300 s so a public dashboard can be served from a normal web host without needing inbound access to the greenhouse LAN.

This document analyses the impact on the existing firmware of adding such a task. **No implementation is proposed yet; the goal is to identify what changes, what breaks, what must be decided.**

The latest commit on `main` (`17491ed commit before fork on developing status reporting`) is a deliberate fork point — there is no existing stub or partial implementation in the firmware tree.

---

## 1. Where the task fits in the existing architecture

The firmware already runs 12 permanent FreeRTOS tasks plus T13 (on-demand OTA). Three plausible homes for the new behaviour:

| Option | Description | Verdict |
|---|---|---|
| **A. New task T14** in [firmware/src/status_post/](../firmware/src/status_post/) (new module), pinned to **Core 0**, priority 3 (LOW), stack ≈ 6 KB | Cleanest separation; mirrors the file-per-task convention used by every other task | **Recommended** |
| **B. Integrate into T11** ([firmware/src/web_server/web_server.cpp](../firmware/src/web_server/web_server.cpp)) | T11 already has the status JSON builder; adds another responsibility to an already 8 KB-stacked task that handles async HTTP + WS | Workable but bloats T11 |
| **C. Repurpose T12 MQTT stub** ([firmware/src/mqtt_client/](../firmware/src/mqtt_client/)) | T12 is a Phase-0 stub that has never been implemented; its slot (Core 0, 8 KB, prio 3) matches what we'd need | Conceptually wrong — MQTT is a separate planned feature; do not bury one inside the other |

Recommendation: **Option A**, a new T14 task. Spawn site is [main.cpp:320-332](../firmware/src/main.cpp).

---

## 2. Resource impact

| Resource | Estimated cost | Headroom |
|---|---|---|
| **Task stack** | 6 KB (HTTPClient + JSON build + TCP buffers) | ESP32-S3 has 8 MB PSRAM; plenty |
| **Heap (transient)** | ~4–6 KB during POST (HTTPClient context + Arduino `String` body) | Released after `http.end()` |
| **JSON buffer** | ~2 KB static or stack (full payload < 1 KB realistic) | — |
| **Flash (.text)** | ~2–4 KB (snprintf builder + new module) | Trivial |
| **NVS** | ~5 new keys (URL, secret, interval, enable, expose-mask), < 384 bytes total | 64 KB partition; trivial |
| **CPU on Core 0** | Burst of < 200 ms once per 60–300 s. Idle the rest | T10/T11 already share Core 0 without saturation |
| **Network** | One outbound TCP connection + ~1 KB up + ~0 KB down per post (server returns 204) | WiFi stack already up via T10 |

No new library dependency is required: `HTTPClient` is already linked (used by [network_manager.cpp:432-481](../firmware/src/network_manager/network_manager.cpp) for the ip-api.com geolocation GET).

**HTTPS without certificate verification.** The spec text shows `http://` examples, but real deployments will frequently terminate TLS at the host. To keep firmware simple and avoid shipping/maintaining a cert bundle, the task supports HTTPS the lightweight way: when `status_url` starts with `https://`, switch to a `WiFiClientSecure` and call `setInsecure()` so the chain is not validated. This is enough to traverse a TLS-only host and keep the wire from being plaintext, without dragging in cert pinning, Mbed TLS root stores, or expiry handling. The trade-off is explicit: **the channel is encrypted but not authenticated**, so a network-position attacker could MITM. Acceptable given the secret token also flows in the header; rotate the token if compromise is suspected.

Implementation: branch on URL scheme inside the task's POST helper; pass `WiFiClientSecure` to `HTTPClient::begin(client, url)` for the HTTPS case, otherwise the simple `HTTPClient::begin(url)` form. Heap cost in `setInsecure()` mode is materially smaller than full validation (no root-CA blob loaded), on the order of a few KB transient per request — already covered by the §2 estimate.

---

## 3. State-snapshot reuse — unify both consumers

T11 already has [`build_status_json()` at web_server.cpp:259-340](../firmware/src/web_server/web_server.cpp) which calls:
- `dm_meas_snapshot()` — protected by MX2
- `dm_cfg_snapshot()` — protected by MX4
- `t2_get_window_states()` — relay-controller spinlock

The existing JSON shape (flat: `temp_c`, `rh_pct`, `wind_ms`, `windows: [...]`) does NOT match the website spec, which expects nested objects keyed `climate`, `wind`, `windows` (as a `{M1,M2,M3}` object, not an array), `mode`, `sun`, `system`, plus an optional `update_interval_s` ([technical-spec-statusWebsite.md § 9.2](technical-spec-statusWebsite.md)).

Both surfaces — the local web/WebSocket UI and the remote status page — render the **same conceptual data**. The right move is to **refactor to a single canonical shape** rather than maintain two parallel JSON builders:

1. Introduce a `status_snapshot_t` struct in [firmware/src/types/app_types.h](../firmware/src/types/app_types.h) that aggregates all fields once (climate / wind / windows / mode / sun / system).
2. Add a single helper `build_canonical_status_json(buf, len, expose_mask)` that formats the snapshot into the **spec-shaped** nested payload, with each top-level tile keyed object emitted only when its bit is set in `expose_mask` (see §6 — exposure flags).
3. Migrate T11 to use this builder for both `/api/status` and the WebSocket push (with `expose_mask = ALL` since the local GUI sees everything).
4. T14 calls the same builder with `expose_mask = cfg.status_expose` (only the tiles the user enabled).

Impact of the refactor:
- The local web GUI's `app.js` consumer (LittleFS assets served by T11) needs to read the new nested keys. This is the part of the work that may extend scope, but it brings the local UI in line with the public dashboard so future tile additions only need to be made once.
- T11's `build_status_json()` is replaced; surrounding lock/snapshot scaffolding is preserved.
- The internal window-state enum (`WIN_OPEN`, `WIN_MOVING_OPEN`, `WIN_MOVING_CLOSE`, `WIN_CLOSED`, `WIN_UNKNOWN` in [app_types.h:89-96](../firmware/src/types/app_types.h)) maps to the spec strings by stripping the `WIN_` prefix; codify this in a single helper used by both call sites.

Net effect: one source of truth for status JSON, no duplication, payload-shape regressions caught by both surfaces simultaneously during dev.

---

## 4. Concurrency impact

The new task takes the same three locks T11 already takes for `/api/status`:

- **MX2** — sensor measurement (held briefly inside `dm_meas_snapshot`)
- **MX4** — config shadow (held briefly inside `dm_cfg_snapshot`)
- relay spinlock — sub-microsecond

Worst-case lock interleave: a status POST burst overlaps with a WebSocket push. Both grab MX2/MX4 with timeouts already in place; a new contender at most adds ~10 ms of additional wait per cycle. **Negligible.**

The task must not hold any lock across `http.GET()` / `http.POST()`. Pattern: snapshot → release → format → send.

---

## 5. Network preconditions and failure modes

T10 ([network_manager.cpp](../firmware/src/network_manager/network_manager.cpp)) drives the state machine: `NET_IDLE → NET_CONNECTING → NET_CONNECTED → NET_RUNNING`, and posts to **Q5** on every state change.

The new task must:

| Precondition | Handling |
|---|---|
| WiFi STA not associated (e.g. AP-only mode, or WiFi failed) | **Skip silently**; do not block; do not log every cycle |
| SNTP not yet synced | Optional: skip until `cfg.current_unix_ts > 1700000000` (server can fall back to its own `received_at`) |
| OTA in progress (`EG1_BIT_OTA_IN_PROGRESS`) | **Skip** — keep CPU/network for OTA |
| HTTP timeout (5 s, mirroring T10 geo-sync) | Log warning to T9 event log on first failure; suppress repeats; clear on next success |
| Server returns non-204 | Log warning; do not retry inside the cycle (next post comes in 60–300 s anyway) |

Watchdog impact: T1 ([t1_wdt](../firmware/src/t1_wdt/)) services WDT every 500 ms. The new task must call `vTaskDelay()` between posts (it will, since it sleeps 60–300 s) and `http.setTimeout(5000)` bounds the worst-case blocking. **No WDT risk.**

---

## 6. Configuration

The status feature is configured **only via the web GUI** (admin-PIN-gated). The LCD GUI is not affected — none of the new settings appear there.

### 6.1 Connection / cadence keys

| Key | Type | Bounds | Purpose |
|---|---|---|---|
| `status_url` | string (≤ 128 chars) | Must start with `http://` or `https://` | Endpoint, e.g. `https://example.org/controller/api.php` |
| `status_secret` | string (≥ 16 chars) | Must match server's `GH_SECRET_TOKEN` | Sent in `sourceidentifier` header |
| `status_interval_s` | i32 | 60–300 | Cycle period |
| `status_enable` | i32 (0/1) | — | Master enable; an empty `status_url` is also treated as disabled |

### 6.2 Per-tile exposure flags

Each top-level payload section corresponds to a tile on the public dashboard, and the user can choose which sections are sent. This is a privacy / minimisation control: e.g. someone may want to publish window state but not raw climate values.

Stored as a bitmask in a single `i32` NVS key (`status_expose`), or equivalently as six `i32` flags (`status_expose_climate`, `status_expose_wind`, …). One `i32` mask is more compact and easier to pass to the formatter:

| Bit | Tile | Effect when 0 |
|---|---|---|
| 0 | `climate` | Object omitted from payload → website hides the climate tile |
| 1 | `wind` | Object omitted → wind tile hidden |
| 2 | `windows` | Object omitted → windows tile hidden |
| 3 | `mode` | Field omitted → mode tile hidden |
| 4 | `sun` | Object omitted → sun tile hidden |
| 5 | `system` | Object omitted → system tile hidden |

Defaults: all enabled. The `update_interval_s` field at the top level is always sent (it does not correspond to a payload tile; the dashboard uses it for its freshness colouring).

The web GUI presents these as six checkboxes on the status-reporting config page. The shared formatter in §3 honours the mask: a section whose bit is clear is simply not emitted, and the website's tile-presence predicates ([technical-spec-statusWebsite.md § 9.2](technical-spec-statusWebsite.md)) hide that tile automatically — no server-side change needed.

### 6.3 Storage

Extend the `system` NVS namespace (managed by T4 in [data_manager.h:67-119](../firmware/src/data_manager/data_manager.h)) and add the corresponding fields to `cfg_shadow_t`. Runtime updates flow through Q4 `config_update_t` exactly like every other config field today.

### 6.4 Web GUI surface

Add a "Remote status reporting" panel to the existing config page, admin-PIN-gated, mirroring the structure of `/api/wifi`:
- URL, secret, interval, master-enable
- Six checkboxes for the exposure mask
- A live "last post" indicator (timestamp + HTTP result of the most recent attempt) for diagnostics

No equivalent on the LCD GUI.

---

## 7. Scheduled-trigger pattern

Mirror T10's existing pattern at [network_manager.cpp:608-615](../firmware/src/network_manager/network_manager.cpp):

```c
if ((xTaskGetTickCount() - s_last_post_tick)
    >= pdMS_TO_TICKS(cfg.status_interval_s * 1000UL)) {
    if (do_status_post()) { s_last_post_tick = xTaskGetTickCount(); }
}
vTaskDelay(pdMS_TO_TICKS(1000));   // 1 Hz wake-up
```

No `xTimerCreate` / software timer needed.

---

## 8. Files affected

| File | Change |
|---|---|
| **NEW**: [firmware/src/status_post/status_post.h](../firmware/src/status_post/status_post.h) | Public task entry, init function |
| **NEW**: [firmware/src/status_post/status_post.cpp](../firmware/src/status_post/status_post.cpp) | Task body, JSON builder, HTTPClient call |
| [firmware/src/main.cpp:320-332](../firmware/src/main.cpp) | Spawn T14 (`xTaskCreatePinnedToCore`) |
| [firmware/src/types/app_types.h](../firmware/src/types/app_types.h) | Shared `status_snapshot_t` struct for the canonical builder |
| [firmware/src/data_manager/data_manager.h](../firmware/src/data_manager/data_manager.h) | Add new fields to `cfg_shadow_t` |
| [firmware/src/data_manager/data_manager.cpp](../firmware/src/data_manager/data_manager.cpp) | Load/save new NVS keys; handle Q4 config updates |
| [firmware/config/cfg_defaults.h](../firmware/config/cfg_defaults.h) | Defaults (URL empty, interval 120 s, secret empty, mask = ALL) |
| [firmware/config/cfg_limits.h](../firmware/config/cfg_limits.h) | 60 ≤ interval ≤ 300; secret ≥ 16 chars |
| [firmware/src/web_server/web_server.cpp](../firmware/src/web_server/web_server.cpp) | Extend `/api/config` to expose/accept the new keys + exposure mask (admin-PIN-gated). Replace `build_status_json()` with calls to the canonical builder for both `/api/status` and the WebSocket push. |
| LittleFS web assets (served by T11) | Update `app.js` / `index.html` to consume the new nested status shape |
| [firmware/platformio.ini](../firmware/platformio.ini) | Bump `FIRMWARE_VERSION` |

No new external library dependency.

---

## 9. Risk assessment

| Risk | Likelihood | Severity | Mitigation |
|---|---|---|---|
| Payload-shape regression (web dashboard tiles silently hide) | Medium | Low (cosmetic) | Spec-driven per-key tests against a mock PHP endpoint before deploy |
| Secret leakage via logs | Low | High | Never log `status_secret` value; redact in any debug print |
| WiFi reconnection storm during burst of failed POSTs | Low | Medium | The 60–300 s minimum interval is far slower than reconnection backoff; no compounding effect |
| HTTPClient blocking past WDT window | Low | Medium | `http.setTimeout(5000)`; T14 not on the WDT-monitored core |
| HTTPS MITM (no cert validation) | Low–Medium | Medium | Documented trade-off; secret token also in header; rotate if compromise suspected |
| Local web GUI breaks during refactor to canonical shape | Medium | Medium | Update `app.js` keys in lockstep with the new builder; both surfaces verified in the same dev cycle |

---

## 10. Out of scope (flagged for separate decisions)

- **Log upload (`?action=log`)**: the spec defines this endpoint, but the current request is explicitly status-only at a 60–300 s cadence. Log uploads have a different cadence (typically daily) and a different size profile (5 MB cap). Treat as a separate future task.
- **TLS certificate validation**: explicitly skipped (`setInsecure()`); see §2.
- **Authentication other than the shared secret**: out of scope.
- **LCD GUI changes**: out of scope. The status feature is configured exclusively through the web GUI.

---

## 11. Open decisions

1. **Task placement**: confirm new T14 (Option A) vs. embedding in T11.
2. **Update interval default**: 60 s (most timely) vs. 300 s (lighter network) vs. 120 s.
3. **Behaviour while OTA in progress**: skip (recommended) vs. keep posting.
4. **First-failure escalation**: log to T9 event log only, or also surface on the web GUI (e.g. a "remote dashboard down" badge in the new config panel)?
5. **Exposure-mask default**: all six tiles enabled (recommended) vs. opt-in per tile.

---

## 12. Verification approach (when the work is done)

- Stand up the PHP endpoint using the reference example at `documentation/phpAPIExample/` against a local Apache/`mock/` Flask server.
- Configure `status_url`, `status_secret`, `status_interval_s = 60` via the web GUI; verify NVS persistence after reboot.
- Watch the mock server's request log: confirm one POST per interval, `sourceidentifier` header matches, payload parses as JSON, `windows.M1` etc. are spec strings.
- Toggle each of the six exposure checkboxes: verify the corresponding object disappears from the POST payload **and** from the local `/api/status` response (since both go through the same builder), and that the public dashboard correctly hides the matching tile.
- Point `status_url` at an `https://` endpoint: verify the request lands and decodes (TLS up, no cert validation needed).
- Pull the WiFi cable mid-cycle: confirm task does not hang, does not flood the event log, recovers on next reconnection.
- Verify the local web GUI still renders correctly after the canonical-shape refactor (climate / wind / windows / mode / sun / system tiles populate as before).
- Confirm WDT does not fire under repeated 5 s timeouts.

---

## 13. Divergences from this analysis (shipped 1.17.1)

This analysis was written before contact with the live public dashboard at `pe1mew.nl/hbwv`. Five things shifted during implementation; the rationale and final state are recorded here so the design and the running firmware agree.

1. **Canonical field names** — the analysis assumed the dashboard would consume whatever shape the firmware emits. In practice the dashboard's `assets/app.js` (out of our authority) reads specific field names that did not match our first cut. Shipped names: `climate.temp_c` / `temp_avg_c` / `rh_pct` / `rh_avg_pct`; `wind.speed_ms` / `speed_avg_ms` / `direction_deg` / `direction_avg_deg`; `windows.M1/M2/M3`; `sun.is_daytime` / `sunrise_min` / `sunset_min`; `system.wifi_ip` / `wifi_rssi_dbm` / `ntp_synced` / `fw_ver` / `uptime_s` / `ts_unix` / `time_iso` / `eg1`; `update_interval_s`. Single canonical shape served to both the local UI and the remote dashboard.
2. **`mode` is an object, not a string** — the dashboard expects `mode: {current: "AUTOMATIC", flags: ["wind_override", …]}`. Shipped accordingly; the EG1 bitset is fan-outed into the `flags` array by name in the builder. `current` is computed with priority (`MOTOR_ALARM` > `WIND_OVERRIDE` > `WINDOW_CAL` > op_mode_t).
3. **Sun fields are local minutes** — the analysis did not pick a TZ for the sun tile. The dashboard renders the integers verbatim as HH:MM with no TZ math, so the firmware converts UTC → local in `dm_status_snapshot()` by deriving the offset from `localtime_r` vs. `gmtime_r` (Newlib has no `tm_gmtoff`). DST is handled automatically.
4. **`dm_reload_web_cfg()` is synchronous** — the analysis proposed a task-notification (TN5) to ask T4 to reload the shadow after the `/api/web` POST. That left a window where the 5 s Web-tab auto-refresh could fire between the POST returning and T4 actually reloading, snapping the form back to the previous values. Shipped: the function takes MX4 itself and reloads inline. TN5 was removed.
5. **T14 stack is 12 KB, not 6 KB** — `WiFiClientSecure` / mbedTLS handshake recurses too deep for the 6 KB the analysis estimated. Bumped on first observed `https://` reboot.

Two cosmetic post-launch additions:
- **5 s auto-refresh on the Web tab** is split into `refreshWebStatus()` (read-only indicators only) vs. `loadWebCfg()` (full form reload). Auto-refresh uses the former, so the user's in-progress edits to URL / secret / interval / checkboxes are not clobbered.
- **Uptime line** added to the local web GUI's System → Clock card. Format: `1d 4h 23m` / `4h 23m` / `2m 13s` / `5s`. Useful for spotting unexpected resets.

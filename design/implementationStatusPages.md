# Implementation plan — Status website reporting

| | |
|---|---|
| Document | Implementation plan |
| Audience | Firmware implementer |
| Companions | [technical-spec-statusWebsite.md](technical-spec-statusWebsite.md) — server-side spec • [impact-analysis-statusReporting.md](impact-analysis-statusReporting.md) — firmware impact |
| Date | 2026-05-10 |
| Status | **Shipped 1.17.1** — see [changelog.md](../changelog.md#1171---2026-05-10) and the implementation notes at the end of this document |

## 0. Scope

Add a new "web" feature to the greenhouse controller that:

1. POSTs the controller's runtime status to a configurable REST endpoint at a configurable interval (60–300 s), gated by a shared secret.
2. Uploads the most recently closed log file from the SD card to the same endpoint, **on log rotation** and as a **daily fallback** at a configurable time-of-day (deduplicated by filename).
3. Adds a new admin-only "Web" tab in the local web GUI to configure all of the above and the per-tile exposure mask. An **Apply** button commits all changes in one POST.
4. Persists all settings in NVS with sane defaults in [firmware/config/cfg_defaults.h](../firmware/config/cfg_defaults.h).
5. Logs every POST attempt — success and failure — through the existing T9 event logger.

The implementation reuses existing patterns (`HTTPClient`, T10 NTP cadence loop, T9 event logger, T11 admin POST handler).

---

## 1. Design decisions (called out for review)

These choices have alternatives. Recommendation in **bold**. Override before implementation if you disagree.

### D1. Log-upload trigger: rotation, daily, or both?

The brief says "alternatively". The two approaches have different failure modes:

| Approach | Pro | Con |
|---|---|---|
| Daily at fixed time-of-day | Predictable cadence; user sees fresh data every morning | Re-uploads the same file if no rotation occurred in 24 h |
| On rotation (when T9 closes a CSV) | One upload per closed file; no redundancy; file is well-formed | Quiet greenhouses may not rotate for weeks |

**Accepted: do both, deduplicated.** Trigger an upload on rotation, AND once per day at the configurable hour. Persist the last-uploaded filename in NVS; the daily check skips if the latest closed file is already that filename. The user gets best-of-both with zero duplicates.

### D2. Default update interval

**Accepted: 120 s.** Light enough to be invisible on the LAN, fast enough that the public dashboard's "freshness bar" stays green. The spec's `GH_DEFAULT_INTERVAL_S = 30` is irrelevant — that's the dashboard's *assumption when the field is missing*; we always send `update_interval_s` so the website knows the real cadence.

### D3. Default daily log-upload time-of-day

**Accepted: 03:15 local.** Off-peak; after most natural rotation events; well before morning checks.

### D4. POST-event logging granularity

The brief asks for "a posting to the REST API shall be logged" and "if a posting fails due to timeout it shall be logged." Literal compliance means 288–1440 events/day (at 300–60 s).

The NVS ring buffer is only 250 entries (`CONFIG_NVS_LOG_CAPACITY` in [platformio.ini:55](../firmware/platformio.ini)) — it would saturate in hours. The SD CSV (512 KB rotation, 10 files) handles the volume fine.

**Accepted: split.**
- **SD CSV**: log every POST attempt (success and failure) — fine-grained audit trail.
- **NVS ring**: log only **transitions** (success→fail, fail→success) and the **first failure** of a streak. Avoids drowning the durable buffer.

`log_post()` already writes to both; the split needs a small addition: pass a flag to `log_post()` (or create a sibling `log_post_sd_only()`) so high-frequency events skip the NVS ring.

### D5. Behaviour during OTA, AP-only, no-NTP

skip silently in all three.

### D6. JSON-shape refactor (already approved in impact analysis)

T11's existing `build_status_json()` is replaced with a single canonical builder that both `/api/status` (local) and the new POST task (remote) call. Local web GUI's `app.js` is updated to read the new nested keys. This unifies the two surfaces. **Locked in §3 of the impact analysis; reaffirmed here.**

### D7. Should the LittleFS web UI also visualise the public-dashboard payload?

No — keep the local UI's tile rendering as it is today. The shape changes; the tile catalogue does not.

### D8. Apply-button transactionality

The brief specifies "to activate the settings, the apply button shall be clicked." Two options:

| Approach | Behaviour |
|---|---|
| Single POST per Apply | Frontend bundles all fields into one `POST /api/web`; backend writes them as one transaction |
| Per-field POST | Each input change posts immediately |

**Accepted: single POST per Apply.** Matches the user's mental model and avoids partial-state windows.

---

## 2. Phasing

Implement in this order; each phase compiles and passes flash before the next starts.

| Phase | Deliverable | Verification |
|---|---|---|
| **A** | NVS schema + `cfg_shadow_t` fields + `cfg_defaults.h` + `cfg_limits.h` + Q4 wiring | Boot; check NVS contains new keys at defaults |
| **B** | `status_snapshot_t` + `build_canonical_status_json()` + T11 migration | `curl /api/status` returns the new nested shape; local web GUI still renders (after `app.js` update in same phase) |
| **C** | T14 `status_post` task — status POST only | Mock PHP endpoint receives one POST per interval with correct payload and header |
| **D** | New "Web" tab in admin GUI; `POST /api/web` handler | All fields editable; Apply persists; reload reflects |
| **E** | Log-upload trigger (rotation + daily) with dedup | Force a rotation; verify just-closed file uploads. Skip clock to 03:15; verify daily upload fires once and only once |
| **F** | End-to-end verification (§ 11) | All checks pass against `mock/` PHP server |

---

## 3. Phase A — Configuration schema

### 3.1 NVS keys (namespace `system`, extending T4's existing namespace)

Add to [firmware/src/data_manager/data_manager.cpp](../firmware/src/data_manager/data_manager.cpp) load/save helpers:

| Key | NVS type | Default | Bounds |
|---|---|---|---|
| `status_url` | str | `""` | ≤ 128 chars; must start `http://` or `https://` if non-empty |
| `status_secret` | str | `""` | ≤ 64 chars; ≥ 16 chars when non-empty |
| `status_interval_s` | i32 | 120 | 60 ≤ x ≤ 300 |
| `status_enable` | i32 | 0 | 0 / 1 |
| `status_expose` | i32 | `0x3F` | bits 0–5 (climate/wind/windows/mode/sun/system) |
| `log_upload_h` | i32 | 3 | 0–23 (local hour) |
| `log_upload_m` | i32 | 15 | 0–59 (local minute) |
| `log_upload_rot` | i32 | 1 | 0 / 1 (also upload on rotation) |
| `log_last_up` | str | `""` | last uploaded filename (T14 owns this; the user does not edit it) |

### 3.2 `cfg_shadow_t` extension

Add the matching fields to the struct in [firmware/src/data_manager/data_manager.h](../firmware/src/data_manager/data_manager.h). Snapshot reads (`dm_cfg_snapshot()`) automatically pick them up.

### 3.3 `cfg_defaults.h` and `cfg_limits.h`

Add the defaults above to [firmware/config/cfg_defaults.h](../firmware/config/cfg_defaults.h). Add the bounds to [firmware/config/cfg_limits.h](../firmware/config/cfg_limits.h). T4's NVS-load path uses these for first-boot population.

### 3.4 Q4 `config_update_t`

Extend `config_update_t` with a `STATUS_*` group of param IDs so `/api/web` POSTs flow through the same validation path used by every other config write.

---

## 4. Phase B — Canonical status JSON

### 4.1 Snapshot struct

Add to [firmware/src/types/app_types.h](../firmware/src/types/app_types.h):

```c
typedef struct {
    // climate
    int16_t   t_now_c10;     // current temp × 10
    int16_t   t_avg_c10;
    uint8_t   rh_now_pct;
    uint8_t   rh_avg_pct;

    // wind
    uint16_t  w_now_ms10;
    uint16_t  w_avg_ms10;
    uint16_t  w_dir_now;
    uint16_t  w_dir_avg;

    // windows
    window_state_t win[3];   // M1, M2, M3

    // mode
    op_mode_t mode;
    uint32_t  eg1_bits;      // for badges (wind override / motor alarm)

    // sun
    bool      is_daytime;
    int32_t   sunrise_unix;
    int32_t   sunset_unix;

    // system
    uint32_t  ts_unix;
    bool      ntp_synced;
    char      ip[16];
    int8_t    rssi;
    char      fw[16];
    uint32_t  uptime_s;

    // top-level
    uint16_t  update_interval_s;
} status_snapshot_t;
```

### 4.2 Builder

New file `firmware/src/status_post/status_json.cpp` (header alongside). Public API:

```c
size_t build_canonical_status_json(char *buf, size_t cap,
                                   const status_snapshot_t *s,
                                   uint32_t expose_mask);
```

- Returns the number of bytes written (excluding NUL).
- `expose_mask` bits 0–5 gate the six top-level objects.
- Window states emitted by stripping `WIN_` prefix (`WIN_OPEN` → `"OPEN"` etc.); shared helper `window_state_str()`.
- Output shape exactly matches [technical-spec-statusWebsite.md § 9.2](technical-spec-statusWebsite.md):

```json
{
  "climate":  { "t_c": 23.4, "t_avg_c": 23.1, "rh_pct": 65, "rh_avg_pct": 64 },
  "wind":     { "ms": 3.5, "avg_ms": 3.2, "dir_deg": 180, "avg_dir_deg": 178 },
  "windows":  { "M1": "OPEN", "M2": "CLOSED", "M3": "MOVING_OPEN" },
  "mode":     "AUTOMATIC",
  "sun":      { "is_daytime": true, "sunrise_unix": 1715308800, "sunset_unix": 1715361600 },
  "system":   { "ts_unix": 1715340000, "ntp": true, "ip": "192.168.1.50", "rssi": -62, "fw": "1.16.39", "uptime_s": 84321 },
  "update_interval_s": 120
}
```

### 4.3 Snapshot helper

New `dm_status_snapshot(status_snapshot_t *out)` in [data_manager.cpp](../firmware/src/data_manager/data_manager.cpp). Internally:
- Acquires MX2 → fills climate + wind → releases.
- Acquires MX4 → fills mode/sun/system + interval → releases.
- Calls `t2_get_window_states(out->win)`.
- No allocation; no I/O; deterministic time bound (~1 ms).

### 4.4 T11 migration

Replace [`build_status_json()` at web_server.cpp:259-340](../firmware/src/web_server/web_server.cpp) with:

```cpp
status_snapshot_t snap;
dm_status_snapshot(&snap);
build_canonical_status_json(buf, cap, &snap, 0xFFFFFFFFu);  // local UI sees everything
```

Apply at both call sites (`/api/status`, line ~580; WebSocket push, line ~1184).

### 4.5 Local web GUI update

Update [firmware/data/app.js](../firmware/data/app.js) and [firmware/data/index.html](../firmware/data/index.html) to read the new nested keys (`s.climate.t_c` instead of `s.temp_c`, etc.). Keep tile structure identical — only the data path changes.

---

## 5. Phase C — T14 status POST task

### 5.1 Module layout

```
firmware/src/status_post/
├── status_post.h      // task entry, init
├── status_post.cpp    // task body
├── status_json.h      // builder declaration (Phase B)
└── status_json.cpp    // builder body (Phase B)
```

### 5.2 Task spawn (`main.cpp` near existing block at lines 320–332)

```cpp
xTaskCreatePinnedToCore(t14_status_post_task, "T14_WEB",
                        6144, NULL, 3, NULL, 0);   // Core 0, prio LOW
```

### 5.3 Task body — status loop

```c
static TickType_t s_last_post_tick = 0;
static bool       s_last_post_ok   = true;   // for streak detection (D4)

void t14_status_post_task(void *) {
    cfg_shadow_t cfg;
    for (;;) {
        dm_cfg_snapshot(&cfg);

        if (cfg.status_enable && cfg.status_url[0] && wifi_connected() &&
            !(eg1_get() & EG1_BIT_OTA_IN_PROGRESS)) {

            TickType_t now = xTaskGetTickCount();
            if ((now - s_last_post_tick) >= pdMS_TO_TICKS(cfg.status_interval_s * 1000UL)) {
                bool ok = do_status_post(&cfg);
                log_post_result(ok, /*streak_changed=*/ ok != s_last_post_ok);
                s_last_post_ok   = ok;
                s_last_post_tick = now;
            }
        }

        // Phase E hooks here
        maybe_upload_log(&cfg);

        vTaskDelay(pdMS_TO_TICKS(1000));   // 1 Hz wake-up
    }
}
```

### 5.4 `do_status_post()` (HTTPS-aware)

```c
static bool do_status_post(const cfg_shadow_t *cfg) {
    status_snapshot_t snap;
    dm_status_snapshot(&snap);

    static char body[1536];
    size_t n = build_canonical_status_json(body, sizeof(body), &snap, cfg->status_expose);
    if (!n) return false;

    HTTPClient http;
    bool ok;
    if (strncmp(cfg->status_url, "https://", 8) == 0) {
        WiFiClientSecure c;
        c.setInsecure();
        ok = http.begin(c, cfg->status_url);
    } else {
        ok = http.begin(cfg->status_url);
    }
    if (!ok) return false;

    http.setTimeout(5000);
    http.addHeader("Content-Type",     "application/json");
    http.addHeader("sourceidentifier", cfg->status_secret);  // never logged

    int code = http.POST((uint8_t*)body, n);
    http.end();

    return (code == 204 || (code >= 200 && code < 300));
}
```

### 5.5 Event logging (D4)

```c
static void log_post_result(bool ok, bool streak_changed) {
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = LOG_SYSTEM;
    ev.initiator  = LOG_BY_WEB;
    ev.value_a    = ok ? 1 : 0;
    ev.value_b    = 0;          // 0 = status post (1 = log upload, see § 6.4)

    // SD always; NVS only on transition (D4)
    log_post_filtered(&ev, /*to_nvs=*/ streak_changed);
}
```

`log_post_filtered()` is a small new helper in [event_logger.cpp](../firmware/src/event_logger/event_logger.cpp): writes to SD CSV unconditionally, gates the NVS ring on the second arg.

---

## 6. Phase D — Web GUI "Web" tab

### 6.1 HTML — new tab button and pane

In [firmware/data/index.html](../firmware/data/index.html), tab bar around line 95–102:

```html
<button class="tab-btn admin-only" onclick="showTab('tab-web')">Web</button>
```

New pane (after `tab-log`):

```html
<div id="tab-web" class="tab-pane">
  <h2>Status website</h2>

  <fieldset>
    <legend>Endpoint</legend>
    <label>URL <input id="web-url" type="url" maxlength="128"
                     placeholder="https://example.org/api.php"></label>
    <label>Shared secret <input id="web-secret" type="password" maxlength="64"></label>
    <label>Interval (s) <input id="web-interval" type="number" min="60" max="300" value="120"></label>
    <label><input id="web-enable" type="checkbox"> Enabled</label>
  </fieldset>

  <fieldset>
    <legend>Expose to public dashboard</legend>
    <label><input id="web-exp-climate" type="checkbox"> Climate</label>
    <label><input id="web-exp-wind"    type="checkbox"> Wind</label>
    <label><input id="web-exp-windows" type="checkbox"> Windows</label>
    <label><input id="web-exp-mode"    type="checkbox"> Mode</label>
    <label><input id="web-exp-sun"     type="checkbox"> Sun</label>
    <label><input id="web-exp-system"  type="checkbox"> System</label>
  </fieldset>

  <fieldset>
    <legend>Log upload</legend>
    <label>Daily upload time
      <input id="web-log-h" type="number" min="0"  max="23" value="3" style="width:3em">:
      <input id="web-log-m" type="number" min="0"  max="59" value="15" style="width:3em">
    </label>
    <label><input id="web-log-rot" type="checkbox"> Also upload on rotation</label>
  </fieldset>

  <fieldset>
    <legend>Status</legend>
    <p>Last post: <span id="web-last">—</span></p>
    <p>Last log upload: <span id="web-last-log">—</span></p>
  </fieldset>

  <button id="web-apply" onclick="applyWeb()">Apply</button>
  <span id="web-fb"></span>
</div>
```

### 6.2 JS — load on tab show, post on Apply

In [firmware/data/app.js](../firmware/data/app.js), add:

```js
function showTab(id) {
  /* … existing logic … */
  if (id === 'tab-web' && g_role === 'admin') loadWebCfg();
}

async function loadWebCfg() {
  const r = await fetch('/api/web');
  if (!r.ok) return;
  const c = await r.json();
  document.getElementById('web-url').value      = c.url      || '';
  document.getElementById('web-secret').value   = '';                  // never echoed
  document.getElementById('web-interval').value = c.interval_s || 120;
  document.getElementById('web-enable').checked = !!c.enable;
  ['climate','wind','windows','mode','sun','system'].forEach((k, i) => {
    document.getElementById('web-exp-' + k).checked = !!(c.expose & (1 << i));
  });
  document.getElementById('web-log-h').value      = c.log_h ?? 3;
  document.getElementById('web-log-m').value      = c.log_m ?? 15;
  document.getElementById('web-log-rot').checked  = !!c.log_rot;
  document.getElementById('web-last').textContent      = c.last_post     || '—';
  document.getElementById('web-last-log').textContent  = c.last_log_up   || '—';
}

async function applyWeb() {
  let mask = 0;
  ['climate','wind','windows','mode','sun','system'].forEach((k, i) => {
    if (document.getElementById('web-exp-' + k).checked) mask |= (1 << i);
  });
  const body = {
    url:       document.getElementById('web-url').value.trim(),
    secret:    document.getElementById('web-secret').value,   // empty = unchanged
    interval_s: parseInt(document.getElementById('web-interval').value, 10),
    enable:    document.getElementById('web-enable').checked ? 1 : 0,
    expose:    mask,
    log_h:     parseInt(document.getElementById('web-log-h').value, 10),
    log_m:     parseInt(document.getElementById('web-log-m').value, 10),
    log_rot:   document.getElementById('web-log-rot').checked ? 1 : 0
  };
  const fb = document.getElementById('web-fb');
  fb.textContent = '…';
  const r = await fetch('/api/web', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  });
  fb.textContent = r.ok ? 'Saved' : 'Failed';
  setTimeout(() => fb.textContent = '', 2500);
}
```

### 6.3 Backend — `/api/web` (mirroring `/api/wifi` at web_server.cpp:697-717)

```cpp
// GET — return current values (secret never echoed)
srv.on("/api/web", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req_role(req) != SESSION_ADMIN) {
        req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
        return;
    }
    cfg_shadow_t cfg; dm_cfg_snapshot(&cfg);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"url\":\"%s\",\"interval_s\":%d,\"enable\":%d,\"expose\":%u,"
         "\"log_h\":%d,\"log_m\":%d,\"log_rot\":%d,"
         "\"last_post\":\"%s\",\"last_log_up\":\"%s\"}",
        cfg.status_url, cfg.status_interval_s, cfg.status_enable, cfg.status_expose,
        cfg.log_upload_h, cfg.log_upload_m, cfg.log_upload_rot,
        t14_last_post_str(), cfg.log_last_up);
    req->send(200, "application/json", buf);
});

// POST — admin only; single transaction
srv.on("/api/web", HTTP_POST, [](AsyncWebServerRequest *req){}, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
    if (req_role(req) != SESSION_ADMIN) {
        req->send(403, "application/json", "{\"ok\":false,\"err\":\"admin only\"}");
        return;
    }
    char body[640] = {};
    memcpy(body, data, len < sizeof(body) - 1 ? len : sizeof(body) - 1);

    char url[129] = {}, secret[65] = {};
    int  interval = 0, enable = 0, expose = 0, log_h = 0, log_m = 0, log_rot = 0;
    bool h_url    = json_get_str(body, "url",    url,    sizeof(url));
    bool h_sec    = json_get_str(body, "secret", secret, sizeof(secret));
    bool h_iv     = json_get_int(body, "interval_s", &interval);
    bool h_en     = json_get_int(body, "enable",     &enable);
    bool h_ex     = json_get_int(body, "expose",     &expose);
    bool h_lh     = json_get_int(body, "log_h",      &log_h);
    bool h_lm     = json_get_int(body, "log_m",      &log_m);
    bool h_lr     = json_get_int(body, "log_rot",    &log_rot);

    // Bounds (cfg_limits.h)
    if (h_iv  && (interval < 60 || interval > 300))           goto bad;
    if (h_lh  && (log_h    <  0 || log_h    >  23))           goto bad;
    if (h_lm  && (log_m    <  0 || log_m    >  59))           goto bad;
    if (h_url && url[0] && strncmp(url, "http://",  7) != 0
              && strncmp(url, "https://", 8) != 0)            goto bad;
    if (h_sec && secret[0] && strlen(secret) < 16)            goto bad;

    if (h_url)              nvs_cfg_set_str("system", "status_url",      url);
    if (h_sec && secret[0]) nvs_cfg_set_str("system", "status_secret",   secret);
    if (h_iv)               nvs_cfg_set_int("system", "status_interval_s", interval);
    if (h_en)               nvs_cfg_set_int("system", "status_enable",   enable);
    if (h_ex)               nvs_cfg_set_int("system", "status_expose",   expose);
    if (h_lh)               nvs_cfg_set_int("system", "log_upload_h",    log_h);
    if (h_lm)               nvs_cfg_set_int("system", "log_upload_m",    log_m);
    if (h_lr)               nvs_cfg_set_int("system", "log_upload_rot",  log_rot);

    dm_reload_cfg_shadow();          // republish to MX4 so T14 sees it next cycle
    req->send(200, "application/json", "{\"ok\":true}");
    return;

bad:
    req->send(400, "application/json", "{\"ok\":false,\"err\":\"bounds\"}");
});
```

Note: `secret` empty in the body = unchanged (it is never echoed back on GET). This is the standard "send empty to keep" idiom and matches the existing `/api/wifi` behaviour for `psk` / `ap_psk`.

---

## 7. Phase E — Log file upload

### 7.1 Rotation hook

T9 currently rotates internally without notifying anyone (verified — no Q3 event, no callback). Add a single new function in [event_logger.h](../firmware/src/event_logger/event_logger.h):

```c
// Returns the filename (no path) of the most recently *closed* CSV log on SD.
// Empty string if there is none yet (e.g. fresh card).
const char *event_logger_last_closed(void);
```

Implementation: T9 sets a `static char s_last_closed[32]` immediately after the rotation step at [event_logger.cpp:434](../firmware/src/event_logger/event_logger.cpp). Atomic publish via a mutex held briefly in the getter. T14 polls this once per cycle (1 Hz) and compares against `cfg.log_last_up`.

This is simpler than wiring a queue/notification and avoids any coupling change in T9's hot path.

### 7.2 Daily fallback

In `maybe_upload_log()` (inside T14), also evaluate the wall-clock once per minute:

```c
static int s_last_check_min = -1;
struct tm lt; time_t now = time(NULL); localtime_r(&now, &lt);
if (lt.tm_min != s_last_check_min) {
    s_last_check_min = lt.tm_min;
    if (lt.tm_hour == cfg->log_upload_h && lt.tm_min == cfg->log_upload_m) {
        try_log_upload(cfg);   // dedup check inside
    }
}
```

`try_log_upload()` reads `event_logger_last_closed()`, compares against `cfg->log_last_up`, and if different attempts the upload. On success, persists the new filename to NVS via `nvs_cfg_set_str("system", "log_last_up", ...)` and republishes the cfg shadow.

### 7.3 Upload helper

```c
static bool do_log_upload(const cfg_shadow_t *cfg, const char *filename) {
    // Build absolute path and open
    char path[48];
    snprintf(path, sizeof(path), "/%s", filename);
    File f = SD.open(path, "r");
    if (!f) return false;
    size_t bytes = f.size();
    if (bytes == 0 || bytes > 5UL * 1024UL * 1024UL) { f.close(); return false; }

    // Build URL with ?action=log
    char url[160];
    snprintf(url, sizeof(url), "%s?action=log", cfg->status_url);

    HTTPClient http;
    bool ok;
    if (strncmp(cfg->status_url, "https://", 8) == 0) {
        WiFiClientSecure c; c.setInsecure();
        ok = http.begin(c, url);
    } else {
        ok = http.begin(url);
    }
    if (!ok) { f.close(); return false; }

    http.setTimeout(30000);                                     // 30 s for log uploads
    http.addHeader("Content-Type",     "text/plain");
    http.addHeader("sourceidentifier", cfg->status_secret);
    http.addHeader("Content-Length",   String((unsigned)bytes));

    int code = http.sendRequest("POST", &f, bytes);             // streamed; no buffer
    http.end();
    f.close();

    return (code == 204 || (code >= 200 && code < 300));
}
```

Streaming the file (`sendRequest` with a `Stream*`) keeps RAM use bounded; we never load 512 KB into a buffer.

### 7.4 Logging the upload

```c
log_event_t ev = {};
ev.timestamp  = (uint32_t)time(NULL);
ev.event_type = LOG_SYSTEM;
ev.initiator  = LOG_BY_WEB;
ev.value_a    = ok ? 1 : 0;
ev.value_b    = 1;                  // 1 = log upload event
log_post(&ev);                      // both NVS and SD — log uploads are rare enough
```

---

## 8. Phase F — Verification

Stand up the mock PHP server (`mock/` per the website spec) and run through:

### 8.1 Configuration round-trip
- [ ] First boot: NVS is populated with defaults from §3.1; `/api/web` GET returns them.
- [ ] Set URL, secret, interval=60, enable; click Apply; reload; values persist except secret (input cleared).
- [ ] Send `interval_s=400`; backend returns 400 with `bounds`.
- [ ] Send `secret` of length 8; backend returns 400.

### 8.2 Status POST
- [ ] Mock server log shows one POST per 60 s, header `sourceidentifier` matches, body parses as JSON.
- [ ] Each top-level key in the body maps to a tile and matches the spec shape.
- [ ] `windows.M1`/`M2`/`M3` are spec strings (not enum names with `WIN_` prefix).
- [ ] Toggle each of the six exposure checkboxes; corresponding key disappears from the POST AND from local `/api/status`.
- [ ] Set URL to an `https://` endpoint; POSTs land (TLS up, cert not validated).
- [ ] T9 SD CSV contains a row per POST (success and failure); NVS ring contains only transitions.

### 8.3 Failure modes
- [ ] Pull WiFi cable: T14 stops posting; no error storm in event log; resumes on reconnection.
- [ ] Stop mock server: each cycle logs a failure to SD; first failure also logs to NVS ring; recovery on restart logs the fail→ok transition.
- [ ] Trigger OTA: T14 skips posts for the duration; resumes after.

### 8.4 Log upload
- [ ] Force a T9 rotation (write 512 KB via flood-test): just-closed file appears at `/log/logs/` on the mock server.
- [ ] Repeat: deduplicated — no second upload of the same file.
- [ ] Skip system clock to 03:15 with no rotation since last upload: nothing happens (already uploaded).
- [ ] Skip system clock to 03:15 with a fresh closed file: upload fires once.
- [ ] Disable "upload on rotation": only the daily upload fires.

### 8.5 Local UI regression
- [ ] All six tiles in the local web GUI populate correctly after the canonical-shape refactor.
- [ ] WebSocket push still updates live (every 2 s).

### 8.6 Watchdog
- [ ] Run for 24 h with deliberate POST timeouts; T1's WDT does not fire; uptime monotonic.

---

## 9. Files affected (consolidated)

| File | Status | Phase |
|---|---|---|
| **NEW** `firmware/src/status_post/status_post.h` | New | C |
| **NEW** `firmware/src/status_post/status_post.cpp` | New | C, E |
| **NEW** `firmware/src/status_post/status_json.h` | New | B |
| **NEW** `firmware/src/status_post/status_json.cpp` | New | B |
| [firmware/src/types/app_types.h](../firmware/src/types/app_types.h) | Add `status_snapshot_t` | B |
| [firmware/src/main.cpp](../firmware/src/main.cpp) | Spawn T14 | C |
| [firmware/src/data_manager/data_manager.h](../firmware/src/data_manager/data_manager.h) | Extend `cfg_shadow_t`; declare `dm_status_snapshot()`, `dm_reload_cfg_shadow()` | A, B |
| [firmware/src/data_manager/data_manager.cpp](../firmware/src/data_manager/data_manager.cpp) | NVS load/save for new keys; snapshot helper | A, B |
| [firmware/config/cfg_defaults.h](../firmware/config/cfg_defaults.h) | New defaults | A |
| [firmware/config/cfg_limits.h](../firmware/config/cfg_limits.h) | New bounds | A |
| [firmware/src/event_logger/event_logger.h](../firmware/src/event_logger/event_logger.h) | `event_logger_last_closed()`; `log_post_filtered()` | D4, E |
| [firmware/src/event_logger/event_logger.cpp](../firmware/src/event_logger/event_logger.cpp) | Publish last-closed filename on rotation; SD-only log path | D4, E |
| [firmware/src/web_server/web_server.cpp](../firmware/src/web_server/web_server.cpp) | Replace `build_status_json()` with canonical builder; add `/api/web` GET+POST | B, D |
| [firmware/data/index.html](../firmware/data/index.html) | New "Web" tab button + pane | D |
| [firmware/data/app.js](../firmware/data/app.js) | New tab JS (`loadWebCfg`/`applyWeb`); update tile renderers for canonical shape | B, D |
| [firmware/platformio.ini](../firmware/platformio.ini) | Bump `FIRMWARE_VERSION` | F |

No new external library dependency.

---

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Refactor of `build_status_json()` breaks the local UI in subtle ways (hidden subkeys) | Phase B includes the `app.js` update; verify both surfaces in same dev cycle (§8.5) |
| Secret-token leakage in logs | `do_status_post()` never logs the secret; the GET `/api/web` endpoint does not echo it; web UI password-typed input |
| Log upload hits server's 5 MB cap | T9 rotates at 512 KB; we never approach the cap |
| Daily window misfires across DST transition | We compare wall-clock H/M every minute and only fire once when both match; DST jumps cause at most a missed or doubled upload, dedup catches the double |
| HTTPS MITM (no cert validation) | Documented trade-off; rotate secret if compromise suspected |
| NVS key-name collisions in `system` namespace | Audit during Phase A; new prefixes (`status_*`, `log_upload_*`, `log_last_up`) are unused |

---

## 11. Out of scope

- TLS certificate validation (deliberately disabled).
- Authentication other than the shared secret.
- LCD GUI changes (the Web feature is configured exclusively via the web UI).
- HTTP retry logic beyond "next cycle will try again".
- Compression of the log file body before upload.

---

## 12. Implementation notes — what shipped vs. plan (1.17.1)

All six phases (A–F) completed. The verification battery in §8 passed against the live `pe1mew.nl/hbwv` dashboard. Differences worth flagging for future maintainers:

### Canonical JSON shape
The plan's §4.2 example used the field names from `design/technical-spec-statusWebsite.md`. The actual public dashboard at `pe1mew.nl/hbwv` uses different names. The shipped builder matches the dashboard:

| Tile | Spec / plan | Shipped |
|---|---|---|
| climate | `t_c`, `t_avg_c` | `temp_c`, `temp_avg_c` |
| wind | `ms`, `dir_deg` (+ avgs) | `speed_ms`, `direction_deg` (+ avgs) |
| mode | bare string `"AUTOMATIC"` | object `{current, flags[]}` |
| sun | `sunrise_mins_utc`, `sunset_mins_utc` | `sunrise_min`, `sunset_min` (LOCAL minutes) |
| system | `ip`, `rssi`, `ntp`, `fw` | `wifi_ip`, `wifi_rssi_dbm`, `ntp_synced`, `fw_ver` |

`mode.flags` is an EG1-bitset fan-out into named strings (`wind_override`, `sensor_fault_temp`, `sensor_fault_wind`, `ota_in_progress`, `motor_alarm`, `calibrating`), plus two task-state extras: `net_backoff_active` (T14 status-website circuit breaker open) and — since 2.0.0-a.6.35.4 — two operator-disabled-feature flags surfaced from the cfg shadow:

| Flag string | Source | Condition | Local-GUI badge class |
|---|---|---|---|
| `wind_override` | EG1 bit set by T3 safety_monitor | wind safety active, all windows closed | `alarm` (red) |
| `motor_alarm` | EG1 bit set by T2 relay_controller | emergency stop triggered | `alarm` (red) |
| `sensor_fault_temp` | EG1 bit set by T5 sensor_poll | two consecutive FG6485A read failures | `warn` (yellow) |
| `sensor_fault_wind` | EG1 bit set by T5 sensor_poll | two consecutive S200 read failures | `warn` (yellow) |
| `ota_in_progress` | EG1 bit set during firmware/asset OTA | OTA cycle running | `warn` (yellow) |
| `calibrating` | EG1 bit set during boot-time window calibration | T2 closing all windows to establish position | `warn` (yellow) |
| `net_backoff_active` | `status_post_backoff_active()` (task-private) | T14 circuit breaker open | `warn` (yellow) |
| `wind_protect_off` | `cfg.v_max <= 0` (a.6.35.4+) | operator disabled wind-speed-driven auto-close | `warn` (yellow) |
| `humidity_ctrl_off` | `cfg.rh_ctrl_en == 0` (a.6.35.4+) | operator disabled RH-driven window control | `info` (blue) |

The first six are EG1 bits emitted by the producer indicated. The last three are not EG1 bits — they're queried directly from `status_post_backoff_active()` and the cfg shadow respectively, and emitted by `status_json.cpp` as additional `mode.flags` entries. Same flag-name → CSS-class mapping in the local GUI's `app.js::flagBadges` table; dashboard implementers can mirror the table to render the same badges on the public status page.

`mode.current` is computed with priority (`MOTOR_ALARM` > `WIND_OVERRIDE` > `WINDOW_CAL` > op_mode_t). Operator-disabled-feature flags do NOT influence `mode.current` — they're purely informational badges; the controller is still in `AUTOMATIC` when wind protect / humidity ctrl are off, just with the corresponding sub-feature disabled.

### TZ handling
The plan put UTC minutes in the sun fields. The dashboard renders the value verbatim, so the firmware now converts UTC → local in `dm_status_snapshot()` using `localtime_r` − `gmtime_r` (Newlib has no `tm_gmtoff`). The snapshot also re-applies `TZ` defensively against `configTime()`'s `UTC0` reset, gated by a `strcmp` against `getenv("TZ")` so the 2 s WS push doesn't churn `setenv` allocations.

### `/api/web` reload
The plan proposed a task-notification (TN5) to ask T4 to reload the cfg shadow asynchronously. Shipped synchronously inside `dm_reload_web_cfg()` to close the race where the 5 s Web-tab auto-refresh fired between the POST returning and T4 processing the notification, snapping the form back to the previous URL.

### Web tab auto-refresh
Originally full `loadWebCfg()`. Now split: `refreshWebStatus()` updates only the read-only Last-post / Last-log-upload / Last-uploaded-file lines; `loadWebCfg()` (full form reload) runs only on tab open and on Apply success.

### Buffer / stack sizing
- T11 `/api/status` and WS push buffers: 1024 → **2048 bytes** (canonical worst-case ~720 B). The matching `ps_malloc` size and `build_status_json` size argument are pinned per call site.
- T14 stack: 6 KB → **12 KB**. `WiFiClientSecure` / mbedTLS handshake needs substantially more stack than plain `HTTPClient` over HTTP.

### Extras not in the plan
- **Uptime line** added to the local web GUI's System → Clock card (`Uptime: 1d 4h 23m`). Reads `system.uptime_s` from the canonical payload.
- **URL syntax validation** on both client and server: must start `http(s)://`, must not contain `?` or `#`, must end with `api.php` — Apache's `DirectoryIndex` cannot be assumed, and `HTTPClient` does not follow 301 redirects.


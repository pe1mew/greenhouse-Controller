# 2.0.0-alpha.6.27 — `/api/history` JSON shape fix + mock alignment

## Bug fix

The dashboard's "Sensor history" table never populated. Root cause was a two-layer mismatch between firmware output and GUI expectation:

| Layer | What firmware emitted | What GUI / mock expected |
|---|---|---|
| Envelope | Bare array `[…]` | `{"rows":[…]}` |
| Field names | `t`, `rh`, `ws`, `wd` (raw, avg-only) | `temp_c`, `temp_avg_c`, `rh_pct`, `rh_avg_pct`, `speed_ms`, `speed_avg_ms`, `direction_deg`, `direction_variation_deg` (both raw + avg) |

### How the GUI was silently failing

`firmware/data/app.js::loadHistory()`:

```js
.then(function(data) {
  if (!data || !data.rows) return;        // ← short-circuits silently on bare array
  ...
  data.rows.slice().reverse().forEach(function(row) {
    const f1 = v => (v !== undefined ? v.toFixed(1) : '—');
    const i0 = v => (v !== undefined ? v             : '—');
    tr.innerHTML =
      '<td>' + f1(row.temp_c)     + '</td>'  // ← undefined → '—'
      ...
```

The firmware response was `[{"ts":…,"t":21,…}, …]` — `data.rows` is `undefined`, so the inner `if` returned and no row was ever written. Even if the envelope had been right, every cell would have rendered as `—` because `row.temp_c` is `undefined` (the firmware named the field `t`).

### Where the shape contract lives

`webUiMock/mock_server.py::_build_history` is the authoritative design reference. Its comment spells it out:

> "Field names match the keys inside the canonical status JSON's `climate` and `wind` blocks so the same name carries the same number on `/api/status` and `/api/history`."

The mock emits `{"rows":[{"ts":…,"temp_c":…,"temp_avg_c":…,"rh_pct":…,"rh_avg_pct":…,"speed_ms":…,"speed_avg_ms":…,"direction_deg":…,"direction_variation_deg":…}]}` — the firmware now does the same.

### Per-row size grew → buffer cap bumped

Each row is now ~160 B (was ~80 B). For `n=60` worst case that's ~9.6 KB. Bumped the body buffer from 8 KB → 12 KB.

### Mock also lost an incorrect 401 gate

`webUiMock/mock_server.py::history()` had `if not _get_role(): return {"ok": False}, 401` — required a session cookie. That contradicted the design contract that the mock itself documents and that `firmware/data/app.js` comments on. Removed; mock is now public-by-default for `/api/history`, matching the firmware and `/api/status` / `/ws` policy.

## What changed

- **`firmware/src/web_server/web_server.cpp::history_handler`** — JSON output rewritten:
  - Envelope `[…]` → `{"rows":[…]}`
  - Field names switched to the canonical `temp_c/temp_avg_c/rh_pct/rh_avg_pct/speed_ms/speed_avg_ms/direction_deg/direction_variation_deg`
  - Both raw (`temperature_c`/`humidity_pct`/`wind_speed_ms10`/`wind_dir_deg`) and avg (`t_avg_c`/`rh_avg_pct`/`wind_speed_avg_ms10`) values emitted
  - Temperature emitted as `%d.0` (sensor delivers whole-°C integers; .toFixed(1) friendly)
  - Wind speed emitted as `%u.%u` (×10 fixed-point decoded, mirrors the status_json.cpp tenths trick)
  - Body buffer 8 KB → 12 KB
- **`webUiMock/mock_server.py::history`** — dropped the `_get_role()` auth check; updated docstring.
- **`webUiMock/README.md`** — endpoint table updated: `/api/history` is now `none` auth (was `farmer+`); response shape documented.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.27`.

## Acceptance — hardware verified on 192.168.20.160

```
GET /api/history?n=3 (no cookie)
→ {"rows":[
    {"ts":1779126289,"temp_c":21.0,"temp_avg_c":21.0,"rh_pct":86,
     "rh_avg_pct":86,"speed_ms":1.0,"speed_avg_ms":1.0,
     "direction_deg":305,"direction_variation_deg":0},
    {"ts":1779126319,…},
    {"ts":1779126259,…}]}
```

PowerShell-side key-presence check (every dashboard reader has a match):

```
envelope has 'rows' field      : True
rows count                     : 3
row[0].ts                      : 1779126259
row[0].temp_c                  : 21.0
row[0].temp_avg_c              : 21.0
row[0].rh_pct                  : 86
row[0].rh_avg_pct              : 86
row[0].speed_ms                : 1.0
row[0].speed_avg_ms            : 1.0
row[0].direction_deg           : 305
row[0].direction_variation_deg : 0
```

Mock smoke-tested: `GET /api/history?n=3` without a cookie now returns `HTTP 200` (was 401); first row carries all nine expected keys.

## Build delta vs alpha.6.26

| Metric | alpha.6.26 | alpha.6.27 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 307 152 B | 1 307 408 B | +256 B |
| RAM static | ~60 256 B | ~60 256 B | unchanged |

bin sha256: `5C79875DB459E715…`

+256 B for the longer JSON format strings and the body buffer cap bump.

## Carried forward

- Manifest mismatch flag (`fw_ver` 6.27 vs `asset_version` 6.25). Cosmetic — assets are forward-compatible. Clear by uploading a fresh 6.27 web-assets ZIP if you care about the badge.
- Phase 7 14-day soak still pending.

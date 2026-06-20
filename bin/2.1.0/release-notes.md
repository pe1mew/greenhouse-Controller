# 2.1.0 — release notes

**Date built:** 2026-06-20
**Built on top of:** 2.0.3 (gh#33 — T14/T10 L3 self-recovery ladder)
**Closes:** **gh#35** — independent wind averaging window (`avg_win_wind`)
**Scope:** ~30 lines across 7 firmware source files + 2 web-UI files. Decouples wind averaging (speed + direction) from temperature averaging; adds an admin-only configurable parameter `avg_win_wind` with slider in the web GUI Wind tab.

---

## Why a minor bump (2.0.3 → 2.1.0)

New public API field (`avg_win_wind` in `GET /api/config`), new NVS key in the `wind` namespace, new struct field in `cfg_shadow_t`, and new web-UI element visible to administrators. These constitute additive surface changes — a minor bump under SemVer.

---

## The problem this addresses

Wind speed and direction averaging was hardcoded to share the temperature window:

```c
// sensor_poll.cpp:423 (before this release)
const uint16_t win_w  = win_t;   /* wind window tracks temperature window */
```

This coupling forced a trade-off: a longer temperature window (to reduce HVAC chatter) also lengthened the wind window (slowing gust detection), and vice versa. Humidity already had its own window (`avg_win_rh`) since 2.0.0; wind did not.

**Operational consequence:** On 5C88, the administrator had no way to tune wind-safety response time independently of climate-control averaging. The wind averaging window was effectively locked to whatever `avg_win_t` was set to.

---

## What changed

### `firmware/src/sensor_poll/sensor_poll.cpp` — core decoupling (1 line)

```c
// Before:
const uint16_t win_w  = win_t;

// After:
const uint16_t win_w  = calc_win(cfg.avg_win_wind, poll_s_cfg);
```

The existing reset logic (lines 436–441) and `avg_push()` calls (lines 504–505) already operate on `win_w` and required no changes.

### `firmware/src/data_manager/data_manager.cpp` — 5 mechanical additions

All in existing pattern blocks, following `avg_win_rh` as the model:

| Location | Addition |
|---|---|
| Wind K_ constants | `static const char K_AVG_WIN_WIND[] = "avg_win_wind";` |
| `nvs_load_wind()` | `nvs_cfg_get_i32_or_default(NVS_NS_WIND, K_AVG_WIN_WIND, DEF_AVG_WIN_WIND, &v); s_cfg.avg_win_wind = (int16_t)v;` |
| `cfg_clamp()` wind block | `else if (strcmp(key, K_AVG_WIN_WIND) == 0) _CLAMP(CFG_MIN_AVG_WIN, CFG_MAX_AVG_WIN);` |
| `ns_key_to_log_id()` wind block | `if (strcmp(key, K_AVG_WIN_WIND) == 0) return LOG_PARAM_AVG_WIN_WIND;` |
| `apply_config_update()` wind block | `else if (strcmp(key_str, K_AVG_WIN_WIND) == 0) { old_val = s_cfg.avg_win_wind; s_cfg.avg_win_wind = v16; }` |

### `firmware/src/data_manager/data_manager.h`

`avg_win_wind int16_t` added to `cfg_shadow_t` in the Wind block.

### `firmware/src/types/app_types.h`

`LOG_PARAM_AVG_WIN_WIND = 38` added (next free ID after `LOG_PARAM_WIND_PROT_EN = 37`).

### `firmware/config/cfg_defaults.h`

`DEF_AVG_WIN_WIND = 6` — matches `DEF_AVG_WIN_T` for OTA backward compatibility.

### `firmware/src/web_server/web_server.cpp` — 3 additions

- `GET /api/config` JSON format string: `"avg_win_wind":%d,` added after `avg_win_rh`.
- `snprintf` args: `(int)cfg.avg_win_wind,` added correspondingly.
- Limits block: `"avg_win_wind": [CFG_MIN_AVG_WIN, CFG_MAX_AVG_WIN]` added.

### `firmware/data/index.html` — Wind tab

New `slider-row admin-only` block added after `Dir excl. high`:

```html
<div class="slider-row admin-only">
  <label data-tip="...">Wind avg window (min)</label>
  <input type="range"  id="cfg-avg-win-wind-sl" step="1">
  <input type="number" id="cfg-avg-win-wind"    step="1" class="short">
  <button onclick="postCfg('wind','avg_win_wind','cfg-avg-win-wind','int')">Apply</button>
  <span class="save-ok" id="fb-avg-win-wind"></span>
</div>
```

No CSS changes — existing `.admin-only` + `body.is-admin .admin-only { display: flex; }` handles visibility.

### `firmware/data/app.js` — 2 additions

- `loadConfig()`: `setVal('cfg-avg-win-wind', cfg.avg_win_wind);`
- `linkAllSliders()` array: `'cfg-avg-win-wind'` added alongside other wind fields.

### `firmware/src/sensor_poll/sensor_poll.h`

Comment updated: T/wind/RH now use independent windows.

---

## Access control

`avg_win_wind` is **not** present in `FARMER_WIND_KEYS[]` (line 1036 of `web_server.cpp`). The existing `is_farmer_key(ns, key)` guard returns false → `403 Forbidden` for farmer sessions attempting to POST `avg_win_wind`. No new enforcement code required — the restriction is structural.

`avg_win_t` and `avg_win_rh` remain farmer-writable (unchanged).

---

## What this does NOT change

| Subsystem | Reason |
|---|---|
| `safety_monitor.cpp` | Consumes pre-averaged values from `sensor_reading_t`; window size is invisible to it |
| `climate_control.cpp` | Never reads wind averages |
| `SENSOR_HR` SD log format | ch=1 logs raw wind readings, not the sliding average |
| LCD menu (boer) | No wind-averaging screen today; `avg_win_wind` is web GUI + admin-only |
| NVS migration | `nvs_cfg_get_i32_or_default` handles the absent key on first boot; no schema version needed |
| `FARMER_WIND_KEYS[]` | Intentionally NOT modified — admin-only by exclusion |

---

## Migration note

On first boot after OTA, `avg_win_wind` is absent from NVS. `nvs_load_wind()` seeds it to `DEF_AVG_WIN_WIND = 6` in RAM. It is persisted to NVS on the first administrator config save.

**Edge case:** if an administrator previously set `avg_win_t` away from 6 (e.g. to 10 min to reduce HVAC chatter), wind averaging resets to 6 min after OTA — it does not inherit the old `avg_win_t` value. Wind safety behaviour is unchanged until the administrator explicitly reconfigures `avg_win_wind` via the web GUI Wind tab.

---

## Build artefacts

| File | Size | SHA-256 |
|---|---:|---|
| `greenhouse-controller-2.1.0.bin` | 1 360 352 B | `78ba24f5c59c4d26ea85ab9a1956ee315fd628f999d3a12390c24f67034068d4` |
| `web-assets-2.1.0.zip`            |   108 073 B | `ae43a306dfdb6c98c2ca098cb2c69e07c8a6452f84efe5f47be85127c63abce4` |
| `bootloader-2.1.0.bin`            |    22 528 B | `8919ccbb964d3cbba2491cad1c6bdb2e511299d1d706f2ee382d7f848c8e703b` |
| `partitions-2.1.0.bin`            |     3 072 B | `18fbe59ac37567be8897bc7f5266aec2ba2df85934a3b8fb9b229d8e59e7e74d` (unchanged through the 2.0.x series) |
| `firmware-2.1.0.elf`              |  12 795 KB  | (for coredump decoding) |
| `firmware-2.1.0.map`              |  10 761 KB  | (for coredump decoding) |

RAM: 18.9 % (62 064 / 327 680) — unchanged vs 2.0.3 (the new struct field is int16_t, absorbed in existing padding).
Flash: 64.8 % (1 359 941 / 2 097 152) — +424 B vs 2.0.3 for the new key constant, load/clamp/log-id/apply handlers, and JSON format additions.

---

## Verifiable post-OTA

1. `GET /api/status` → both `fw_ver` AND `asset_version` must read `2.1.0`.
2. `GET /api/config` (any authenticated session) → response includes `"avg_win_wind":6`.
3. Admin login → Wind tab → "Wind avg window" slider is visible and functional.
4. Farmer login → Wind tab → "Wind avg window" slider is **hidden** (`.admin-only` CSS).
5. Farmer-session POST `{"ns":"wind","key":"avg_win_wind","value":3}` → `403 Forbidden`.
6. Change `avg_win_t` via the Climate tab — confirm wind averaging window is unaffected (stays at its configured value).

---

## Open issues after this release

- **gh#7** — bug: serial-port WDT freeze
- **gh#27** — T15 heap-drop sampling timing
- **gh#32** — SD handling on LCD/keypad GUI
- **gh#34** — record HTTP code in SD audit row (413 upload-too-large forensics)

(gh#35 closed here.)

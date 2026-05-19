# 2.0.0-a.6.35.4 — Wind-protect-off / Humidity-ctrl-off badges in Alarms card + status JSON

Operator-visibility gap closed: the controller now signals when wind protection or humidity control has been operator-disabled via cfg. Both the local GUI's Alarms card and the public status dashboard see the state through the same `mode.flags[]` mechanism that already drives the existing wind-override / motor-alarm / sensor-fault badges.

## The gap

Pre-patch, turning either subsystem off was invisible from the outside:

- Setting `wind/wind_prot_en = 0` (via LCD config menu or `POST /api/config`) caused T3 safety_monitor to clear `EG1_BIT_WIND_OVERRIDE` if set and skip its main loop entirely. No badge appeared anywhere — the Alarms card showed "OK" and the public status dashboard had no indicator. An operator looking at the dashboard could not tell that the wind safety net was completely disabled.
- Setting `climate/rh_ctrl_en = 0` had partial visibility: the local Humidity card dimmed the RH setpoint rows (`.dimmed` class), and the canonical JSON omitted the `rh_max_active` / `rh_min_active` fields for the public dashboard. But neither surface explicitly displayed that humidity control was off — the operator had to *infer* it from the dimmed rows.

## The fix

Two new flag strings in the canonical JSON's `mode.flags[]` array:

| Flag | Condition (cfg) | Local-GUI badge text | Badge class |
|---|---|---|---|
| `wind_protect_off`  | `wind_prot_en == 0` | "Wind protect off"   | `warn` (yellow) |
| `humidity_ctrl_off` | `rh_ctrl_en == 0`   | "Humidity ctrl off"  | `info` (blue, new) |

The yellow/blue split matches the semantic difference: disabling wind protection removes a *safety net* (operator should be reminded — yellow), whereas disabling humidity control is a *routine config choice* (operator only wants temperature-driven ventilation — informational blue).

Both surfaces use the existing flag-name → badge-CSS-class mapping. The local GUI gains one new CSS class (`.badge.info { background: var(--blue); color: #fff; }`) and two new entries in `app.js::flagBadges`. The public dashboard implementer mirrors the same mapping.

`mode.current` is intentionally not affected — the controller is still in `AUTOMATIC` when either subsystem is operator-disabled. The badges convey *state* without overwriting the primary *mode* label.

### Why `wind_prot_en` and not `v_max`

The cfg has two related fields:

- `wind_prot_en` (boolean) — gates the *whole* T3 safety_monitor subsystem. When 0, T3 skips its main loop and clears `EG1_BIT_WIND_OVERRIDE` if it was set. Both speed-exceeded *and* direction-in-exclusion-zone branches are disabled.
- `v_max` (m/s) — when 0 or negative, only the speed-exceeded branch is disabled. The direction-exclusion branch keeps working.

`wind_protect_off` maps to the operator-visible "the whole subsystem is off" decision, which is `wind_prot_en`. Setting `v_max = 0` while leaving `wind_prot_en = 1` is a partial-disable case the badge does *not* cover; the direction-exclusion is still active so the subsystem is still doing something. If we ever want to surface partial states (`wind_speed_protect_off`, `wind_direction_protect_off`), that's a future extension.

## What changed

- **`firmware/src/types/app_types.h`** — added `bool wind_protect_enabled` field to `status_snapshot_t` (mirror of `rh_ctrl_enabled` placement).
- **`firmware/src/data_manager/data_manager.cpp`** — `dm_status_snapshot()` now sets `out->wind_protect_enabled = (cfg.wind_prot_en != 0)`.
- **`firmware/src/status_post/status_json.cpp`** — flag-emission loop now appends `wind_protect_off` when `!s->wind_protect_enabled` and `humidity_ctrl_off` when `!s->rh_ctrl_enabled`. Placement is after `net_backoff_active` (preserves alarm/warn ordering).
- **`firmware/data/style.css`** — new `.badge.info` class with `var(--blue)` background. The CSS variable already existed (`#2196f3`); only the `.badge.info` rule is new.
- **`firmware/data/app.js`** — `flagBadges` table extended with `wind_protect_off → "Wind protect off"` (`badge warn`) and `humidity_ctrl_off → "Humidity ctrl off"` (`badge info`).
- **`firmware/data/index.html`** — `#st-alarms` tooltip rewritten to enumerate every possible badge with its colour class. Operators hovering the Alarms card now see what every badge means.
- **`design/implementationStatusPages.md`** — flag-name → badge-class table extended (also documents the previously-undocumented `net_backoff_active`).
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.35.4`.

## Acceptance — verified on 192.168.20.160

Toggled both cfg booleans via `POST /api/config` and watched `/api/status::mode.flags` settle within ~1 s:

```
[default]      rh_ctrl_en=1, wind_prot_en=1  →  mode.flags = []
[POST rh=0]    rh_ctrl_en=0, wind_prot_en=1  →  mode.flags = ['humidity_ctrl_off']
[POST wind=0]  rh_ctrl_en=0, wind_prot_en=0  →  mode.flags = ['wind_protect_off', 'humidity_ctrl_off']
[restore]      rh_ctrl_en=1, wind_prot_en=1  →  mode.flags = []
```

Local GUI Alarms card on this unit's state (verified visually on the web UI after re-deploy):

- Both enabled → "OK" (green badge as before)
- Humidity off only → blue "Humidity ctrl off" badge
- Both off → yellow "Wind protect off" badge + blue "Humidity ctrl off" badge

Public dashboard: the new flag strings flow through the existing T14 status POST to `https://pe1mew.nl/hbwv/api.php`. The dashboard implementer needs to add the same two entries to their `FLAG_CLASS` lookup; the firmware change is complete on its side.

## Build delta vs a.6.35.3

| Metric | a.6.35.3 | a.6.35.4 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 349 136 B | **1 349 296 B** | +160 B |
| RAM static | 60 552 B | 60 553 B | +1 B |

+160 bytes: two flag-string literals (`wind_protect_off`, `humidity_ctrl_off`), one cfg field read in `dm_status_snapshot`, and the two new emission branches in the flag-loop. Final flash usage 64.3 % of the 2 MB OTA bank.

## Public-dashboard sync

The `pe1mew.nl/hbwv` dashboard's renderer uses its own `FLAG_CLASS` table to map flag strings to CSS classes. To pick up the two new flags, the dashboard side needs:

```js
// Add to assets/app.js FLAG_CLASS:
'wind_protect_off':  'warn',
'humidity_ctrl_off': 'info',
```

…plus a `.badge.info` CSS rule (any blue colour). The firmware emits the strings regardless; if the dashboard ignores them, the JSON-payload semantics are preserved and only the visual rendering on that side is missing.

## Next

Phase 7 soak continues. The new badges add another data point operators can verify daily without going into the cfg tabs — improves the soak's daily-review pass.

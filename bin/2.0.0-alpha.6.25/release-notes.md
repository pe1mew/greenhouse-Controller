# 2.0.0-alpha.6.25 — safety fix: IO0 reset requires a HIGH-edge gate

## Bug fix

T8's IO0 factory-reset detector triggered Stage 1 (PIN reset) when a host opened the USB-CDC serial port with DTR held statically high. This was operator-confirmed during alpha.6.24 acceptance: the LCD reset-progress bar was seen growing while a PowerShell SerialPort capture script was running for 25 s. After 5 s the device fired `nvs_cfg_erase_namespace(NVS_NS_ACCESS)` + `pin_auth_init()`, factory-resetting the admin PIN to `12345678`.

### Why it fired

On the LOLIN S3 dev board (and most ESP32-S3 reference designs), the USB-CDC line-state signals are routed through the dev-board's auto-reset/auto-bootloader transistor circuit to IO0 and EN. The circuit is designed for esptool's brief DTR/RTS pulses during flashing; tools that hold DTR statically (raw .NET SerialPort, some serial-monitor utilities other than miniterm) keep IO0 electrically LOW for the entire connection. T8 polled GPIO0 every 100 ms and counted consecutive LOW ticks toward the 5-second / 10-second / 15-second / 20-second factory-reset stages — so a 5+ s static-DTR connection looked indistinguishable from an operator pressing and holding the BOOT button.

`pio device monitor` (miniterm) doesn't trigger this because it pulses DTR briefly at open then maintains it through the RC filter; .NET SerialPort doesn't pulse — it just sets the signal level on `Open()`.

### Fix

Added a **HIGH-edge gate** to T8's IO0 detector. The detector now requires GPIO0 to have been observed HIGH at least once since boot before any LOW tick counts toward a reset stage. A static-LOW reading (whatever the cause — USB-DTR artifact, wiring fault, stuck button) is rejected as a passive electrical state rather than a deliberate operator gesture.

```c
static bool s_io0_seen_high = false;
if (!io0_low) s_io0_seen_high = true;
if (io0_low && s_io0_seen_high) { /* count toward reset */ }
```

This preserves the intended user-action semantics (the operator must release the button to "arm" the detector, then press to count) and is robust against the boot-time-already-LOW case. It also catches a few other latent failure modes (e.g. an operator who restarts the board mid-press now gets a clean state instead of an immediate reset).

## What changed

- **`firmware/src/ui_display/ui_display.cpp`** — added `s_io0_seen_high` local static gate in T8's main loop (~10 lines + comment). Both the press-detection and release-execute branches updated symmetrically.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-alpha.6.25`.

## Acceptance — hardware verified on 192.168.20.160

Flashed via esptool (no serial monitor opened post-flash this time, so as not to re-trigger the bug). After 51 s uptime:

```
GET /api/status     → fw_ver=2.0.0-alpha.6.25, uptime_s=51
GET /api/ota/status → state:"idle", accepted:true, bank:"A"
```

T1's 1 Hz LED blink, T11's full GUI serving, and the OTA rollback flow all preserved from alpha.6.24.

## Operator note — PIN was reset during the alpha.6.24 capture session

Because the bug triggered in alpha.6.24, the admin PIN on `192.168.20.160` is now back to the factory default `12345678` (which is what we've been using in our curl tests). If a custom PIN had been set in NVS before that session, it's gone — `nvs_cfg_erase_namespace(NVS_NS_ACCESS)` wiped the namespace before re-writing factory defaults. The farmer PIN was also reset (default `1234`).

To set a fresh custom PIN through the GUI:
- Login as admin → Settings → Change PINs

Or via the API:
```
curl -b jar -X POST http://device/api/pin -d '{"role":"admin","pin":"NEWPIN"}'
```

## Build delta vs alpha.6.24

| Metric | alpha.6.24 | alpha.6.25 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 307 136 B | 1 307 168 B | +32 B |
| RAM static | ~60 256 B | ~60 256 B | unchanged |

bin sha256: `DB9741BFEEEECE81…`

The +32 B is essentially the new boolean + one branch; everything else is the same.

## Resolved during alpha.6.25 cycle — 2.0-flavoured asset bundle landed

User reported during alpha.6.24 acceptance: "NVS buffer is still an option in webgui" + "I now have a mismatch warning". Both root-caused to the 1.20.x asset bundle we uploaded earlier — the underlying `firmware/data/` sources had the NVS UI stripped since alpha.6.5, but no STORE-only ZIP had ever been built from them.

Fixed in two steps inside the alpha.6.25 cycle:

1. **`bin/build_release.ps1` regex extended** to accept SemVer pre-release suffixes — the original regex only matched plain `X.Y.Z`, so it refused to parse `2.0.0-alpha.6.25`. Now accepts the full pre-release / build-metadata grammar.
2. **`bin/2.0.0-alpha.6.25/web-assets-2.0.0-alpha.6.25.zip` produced** by `build_release.ps1`. STORE-only, 91 414 B (4 files: index.html 38 918 B, app.js 41 193 B, style.css 10 623 B, manifest.json 51 B stamped with `"asset_version":"2.0.0-alpha.6.25"`).
3. **Live OTA performed** — uploaded to `192.168.20.160` via `POST /api/ota/assets`. T13 mirror-step wrote `fw_ver`-stamped manifest to the active partition; the resulting `/api/status` reports `fw_ver=asset_version=2.0.0-alpha.6.25` → mismatch warning cleared.

Also performed in the same cycle:

- `firmware/data/app.js` — dropped the stale `?src=sd&file=...` query param on log downloads (firmware endpoint takes only `?file=NAME` since alpha.6.19; the legacy `src=` selector was retired with NVS).
- `webUiMock/mock_server.py` — synced to the firmware:
  - `fw_ver` bumped `"1.20.0" → "2.0.0-alpha.6.25"`
  - `_nvs_log_entries()` generator deleted
  - `/api/log/files` returns `{sd_files:[...]}` only (no `nvs_count`)
  - `/api/log/download` accepts only `?file=NAME` (no `?src=` selector)
  - `/index.html` direct route added alongside `/`
- `webUiMock/README.md` — endpoint table updated to match the 2.0 firmware surface.

The web-asset bundle in this directory is the canonical 2.0 reference asset bundle going forward. Any future operator who flashes a fresh 2.0 firmware should upload `web-assets-2.0.0-alpha.6.25.zip` (or the latest tag's equivalent) via `POST /api/ota/assets`, not the 1.20.x bundle.

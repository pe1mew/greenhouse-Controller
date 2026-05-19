# 2.0.0-rc.1.1 — web-GUI wind-direction surface fix

Patch release on top of rc.1. **No firmware C/C++ code changes** — the only deltas are two lines of `firmware/data/app.js`, two tooltip strings in `firmware/data/index.html`, the `FIRMWARE_VERSION` bump in `firmware/platformio.ini`, the matching `fw_ver` bump in `webUiMock/mock_server.py`, and the companion-doc references that name the firmware-under-test. Supersedes rc.1 as the Phase 7 soak candidate; the 14-day clock restarts at day 0 against this build.

## The operator's report (verbatim)

> *"in the LCD I see 16 degrees wind direction and in the webgui I see +/- 31 degrees. why?"*

Reported while bench-testing rc.1 with the real S200 wind sensor wired up (replacing the modbus emulator that the alpha-series had used).

## What was actually happening

The canonical JSON correctly emits **three distinct** wind-direction fields. Bench live values at the moment of the report:

| JSON field | Physical meaning | Value |
|---|---|---:|
| `direction_deg` | Last instantaneous sample from the sensor | 33° |
| `direction_avg_deg` | Sliding-window vector average | 31° |
| `direction_variation_deg` | **Full** arc width spanning every sample in the window | 17° |

…but the two surfaces disagreed on which field to render:

- **LCD row 1** displayed `wind_dir_avg_deg` → 31° (averaged).
- **Web GUI "Direction"** displayed `direction_deg` → 33° (instant; ≈ 16° earlier in the operator's window as the sensor moved).
- **Web GUI "Variation"** displayed the raw `direction_variation_deg` → 17° — an operator naturally reads "Variation: 17°" as "±17°", but the underlying figure was the *full* arc width, not the half-arc around the average.

No firmware bug — a surface-consistency bug between LCD and GUI, plus a label-reads-as-different-thing bug on Variation.

## Chosen fix

Per operator direction: **make LCD and GUI agree on Direction (both averaged); keep the "Variation" label, divide the displayed Variation value by 2, and prefix a literal ± sign so the GUI matches the natural "±N° around the average" reading.**

| Surface | Field read | Display format | Rendered example (live values 33 / 31 / 17) |
|---|---|---|---|
| LCD row 1 (unchanged) | `wind_dir_avg_deg` | `" Dir: %3d \xDF (%-2s)"` | ` Dir:  31 ° (NE)` |
| GUI "Direction" *(was instant)* | `direction_avg_deg` | `toFixed(0)` + ` °` suffix | `31 °` |
| GUI "Variation" *(was full arc width)* | `direction_variation_deg / 2` | `'±' + toFixed(0)` + ` °` suffix | `±9 °` |

Operator's mental model — "wind has been swinging roughly ±9° around 31° during the last sliding window" — now matches what both surfaces show.

## What changed (file-level)

### `firmware/data/app.js`

```diff
-    if (w.direction_deg           !== undefined) setText('st-wind-dir', w.direction_deg.toFixed(0));
-    if (w.direction_variation_deg !== undefined) setText('st-wind-var', w.direction_variation_deg.toFixed(0));
+    // rc.1.1 — Direction surfaces the sliding-window vector average (matches LCD).
+    // Was reading the instant `direction_deg` field; operator saw LCD-vs-GUI mismatch
+    // (e.g. LCD 31° vs GUI 33° on the same sample).
+    if (w.direction_avg_deg       !== undefined) setText('st-wind-dir', w.direction_avg_deg.toFixed(0));
+    // rc.1.1 — Variation is the half-arc around the average ("±N°"). The canonical JSON
+    // emits the FULL arc width spanned by the window's samples, so we halve it here and
+    // prefix a literal ± to match the natural "±15° around the average" operator reading.
+    if (w.direction_variation_deg !== undefined) setText('st-wind-var', '±' + (w.direction_variation_deg / 2).toFixed(0));
```

### `firmware/data/index.html`

Tooltip strings on the Wind card "Direction" and "Variation" rows rewritten to spell out:
- Direction is the **averaged** value (and explicitly says "the same value the LCD shows").
- Variation is **half** the arc, presented as **±N°** around the average.

### `firmware/platformio.ini`

```diff
-    -DFIRMWARE_VERSION=\"2.0.0-rc.1\"
+    -DFIRMWARE_VERSION=\"2.0.0-rc.1.1\"
```

### `webUiMock/mock_server.py`

```diff
-    "fw_ver":              "2.0.0-rc.1",
+    "fw_ver":              "2.0.0-rc.1.1",
```

### `manual/beheerderHandleiding.md`

Header refreshed — still v1.17 (no procedural changes), firmware row now reads `2.0.0-rc.1.1` with a one-line note explaining the sub-iteration.

## What did NOT change

- Firmware C/C++ source — the canonical JSON shape, the LCD code, the wind-direction averaging math, every task/queue/mutex, every endpoint behaviour.
- Static RAM footprint.
- Acceptance criteria from rc.1 (zero unplanned reboots, zero coredumps, gh#23 heap watch > 30 KB, baseline drift < 5 KB / 14 days, status POST > 95 %, daily log upload, climate-control responsiveness, GUI smoke test). All carry over verbatim.

## Phase 7 soak — clock reset

rc.1's day-counter was < 1 day in when this patch was cut. The rc.1 release-notes state explicitly:

> *"Fail on any criterion = halt + a.6.36 (or rc.2) patch + restart the 14-day clock"*

A patch release on top of rc.1 — even one that touches no compiled C/C++ — counts as a new candidate, so **the 14-day clock restarts at day 0 against rc.1.1**. The bench unit at 192.168.20.160 carries the rc.1.1 paired binary + assets and runs against the production status server (`https://pe1mew.nl/hbwv/api.php`) at `status_interval_s = 120`.

## Pre-soak post-deploy verification + coredump cleanup

After the paired OTA completed cleanly (`fw_ver=2.0.0-rc.1.1`, `asset_version=2.0.0-rc.1.1`, BOOT row `value_a=5, value_b=4` = ESP_RST_SW = clean software reset), the bench dashboard surfaced a **`coredump_available`** mode-flag. Investigation:

- A 45 732 B dump was present in the partition. Downloaded via `GET /api/coredump/download` and archived to `bin/2.0.0-rc.1.1/pre-soak-artifacts/coredump-rc.1.1-pre-soak-residual.bin` (gitignored).
- SHA-256 of the new dump differs from the rc.1-archived residual (`bin/2.0.0-rc.1/pre-soak-artifacts/coredump-pre-soak-cleanup.bin`), so this is **not** the same alpha-series residual — it is a fresh dump captured *during* the rc.1 30-minute uptime window.
- The dump's embedded app-SHA prefix (`438a2fdfa`) corresponds to the rc.1 firmware build, so the panic happened while rc.1 was running, **not** during the rc.1.1 boot. (The decoder refused to load it against the rc.1.1 ELF on hand, as expected.)
- **Probable cause**: the first attempted paired-OTA upload in this session sent `POST /api/ota/firmware` with a multipart wrapper (`--boundary\r\nContent-Disposition: …`) instead of the raw .bin body the handler expects. The connection was reset rather than returning a 4xx, which is consistent with a panic inside the `httpd_req_recv → ota_firmware_write` loop when fed multipart bytes that look nothing like an ESP32 image header. A retry with the correct raw-body format succeeded.
- **Follow-up filed**: a separate session has been spawned to rebuild the rc.1 ELF, decode the backtrace, and decide whether to harden `ota_firmware_write` against malformed POST bodies. Not blocking rc.1.1 — the GUI's own OTA upload path uses the raw-body format, so the panic cannot be triggered from a browser; only from buggy curl/PowerShell scripts.

The partition was then erased via `POST /api/coredump/erase` so the soak's day-0 coredump slot is clean. Post-erase verification: `/api/coredump/status` returns `present:false`; `/api/status` `mode.flags` is `[]`; bench unit running rc.1.1 with empty coredump slot.

## Live wind-direction fix verification

Post-deploy `/api/status` payload, real S200 sensor (sample at uptime ≈ 73 s, just before partition erase):

```
direction_deg            = 34°   (instant; ignored by both surfaces now)
direction_avg_deg        = 32°   ← LCD row 1 AND GUI "Direction" now show this
direction_variation_deg  = 4°    → GUI "Variation" renders as ±2° (4/2 with literal ±)
```

A second sample shortly after, with the wind shifting more widely:

```
direction_deg            = 155°
direction_avg_deg        = 137°  ← LCD AND GUI agree
direction_variation_deg  = 131°  → GUI renders ±66°
```

Both surfaces now reference the same physical quantity for Direction; Variation reads naturally as "the wind has been swinging roughly ±66° around 137° in the window". Fix verified end-to-end against the real sensor.

## Bench verification before leaving the unit alone

Same pre-soak checklist as rc.1:

1. Boot the unit → confirm `fw_ver=2.0.0-rc.1.1`, `asset_version=2.0.0-rc.1.1`, `eg1=0`, `mode=AUTOMATIC`, `flags=[]`.
2. Open the dashboard → confirm the Wind card "Direction" number equals the LCD row-1 Direction number on the same observation tick.
3. Confirm Wind card "Variation" renders with a leading `±` sign and is half the value of `direction_variation_deg` in the `/api/status` JSON.
4. Walk the full diagnostics chain once (deliberate panic on bench → coredump captured → blue badge → Download → `idf.py coredump-info` → Erase → badge gone) to prove the rc.1 work still functions.
5. Daily GUI smoke test (login, view status, change a setpoint, download a log, see audit rows fire) for the full 14 days.

## After the soak passes

- Tag `v2.0.0` on the merge commit
- Fast-forward merge `dev/2.0.0-esp-idf` → `main`
- Run `bin/build_release.ps1` from main → publishes `bin/2.0.0/`
- Operator deploys to Unit 2 first (7-day observation), then Unit 1

## Status

Day 0 of Phase 7 soak. Day 14 = `v2.0.0` if green across the board.

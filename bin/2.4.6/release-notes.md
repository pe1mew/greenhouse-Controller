# Release 2.4.6

**Date:** 2026-09-11
**Built on:** 2.4.5
**Fixes:** [gh#52](https://github.com/pe1mew/greenhouse-Controller/issues/52) and [gh#53](https://github.com/pe1mew/greenhouse-Controller/issues/53) — two config-path defects of the same shape

## Context: `main` was re-split first

All M3 window-position-sensor work — task T17, the wire-encoder driver, the `windowPos`
component, the `SENSOR_HR ch3` / `ALARM ch6` log encodings and their parser support —
moved to the **`ropeSensor`** branch on 2026-09-11. `main` is back to shipping the
controller without it, and 2.4.6 is the first release from that `main`.

Nothing was withdrawn from the shipped product: T17 had never appeared in a release. The
2.4.5 artefact on disk predates it, and the binaries in `bin/2.4.5/` were built before
the sensor work landed.

**2.4.5 is superseded and will not be published.** It was built and push-OTA'd to FDA4
for the LCD test, but never published to ROTA (there is no `manifest-2.4.5.json`), and
FDA4 then ran `2.4.5-bench` — a *different* binary carrying T17 and an open Modbus write
route — for the whole AT-WP05 soak. So 2.4.5 has no soak history of its own. 2.4.6
carries the same LCD fix plus both config fixes, and is the artefact to soak and promote.

## gh#53 — a 200 that meant nothing

`POST /api/config` with a key the controller cannot apply returned `{"ok":true}`, wrote
the key into NVS, and applied nothing. Reproduced on FDA4 by accident while restoring
settings after the gh#51 Group C verification:

```
POST /api/config {"ns":"system","key":"poll_interval_s","value":45}   ->  200 {"ok":true}
GET  /api/config                                                      ->  "poll_interval_s": 30
```

Three consequences, none of them visible anywhere:

- the junk key **persisted across reboots** and consumed a slot in that namespace, with
  no garbage collection — only a namespace erase reclaims it;
- **`cfg_clamp()` was skipped entirely**, because it passes unknown keys straight
  through, so no range check ran;
- **no audit row and no INFO line**, because both sit inside `if (updated)`.

### Why a caller walks into it

`GET /api/config` returns **field** names, and they are not always the NVS keys:
`poll_interval_s` vs `poll_interval`, `status_interval_s` vs `status_intv_s`. Reading
the API and posting it back is the obvious way to restore settings, and it half-worked
silently. A caller looping over the GET output would write a junk key for every field
whose JSON name differs from its NVS key.

### The fix

New `dm_cfg_key_is_known(ns, key)` mirrors the four shadow ladders in
`apply_config_update()` — a key is known exactly when a ladder arm writes a
`cfg_shadow_t` field for it.

The POST handler calls it **synchronously** and returns **400
`{"ok":false,"err":"unknown ns/key"}`**. Synchronous is the whole point: the route
queues to Q4 and T4 applies it later, so the 200 has never meant more than *accepted*,
and the HTTP response is the only place a caller can see the answer.
`apply_config_update()` checks the same predicate **before** touching NVS, as defence in
depth for the LCD, which is the other Q4 producer.

The string path had the identical hole — any `str_value` key was written to NVS and
answered `ok:true`, while only `tz_str` did anything. `tz_str` is now the only string key
this route accepts.

### Deliberately not derived from the existing tables

`cfg_clamp()` and `ns_key_to_log_id()` walk nearly the same key set, but both are
near-misses: `cfg_clamp()` passes unknown keys through and also carries the four `ota_*`
keys that `/api/ota/config` owns, and `ns_key_to_log_id()` returns `LOG_PARAM_NONE` for
several genuinely known keys (`session_timeout`, `ap_timeout`, `led_*`) — so "has no log
id" does not mean "is not a key".

Collapsing all three onto one descriptor table is the right fix and was **deliberately
not attempted** in a patch release bound for production. Drift is at least *detectable*:
a key that passes the predicate but matches no ladder arm now logs an **ERROR**.

## gh#52 — a helper that was correct only where it was called

`nvs_load_mode()` restored STANDBY from NVS by only ever **setting**
`EG1_BIT_STANDBY`. It never cleared it. That is correct in its one caller — T4's boot
phase, where the event group has just been created and the bit is known-clear — and a
silent no-op anywhere else, with nothing in the name, signature or comment saying so.

Two changes, both from the issue's own ranking:

**(a)** Renamed to **`nvs_restore_standby_at_boot()`**, with the precondition in a
`@warning`. The old name put it in the `nvs_load_*` family and invited exactly the call
that could not work. It is also the only member of that family that writes an **event
group** rather than the `s_cfg` shadow — structurally different under a matching name.

**(c)** `dm_reload_all_cfg()` now restores the operating mode as its last step and
**outside MX4**. `dm_set_standby_ex()` writes NVS, posts an audit row and may post
`CMD_RECALIBRATE`; MX4 is `xSemaphoreCreateMutex()` — plain, non-recursive — so a nested
take would deadlock T4. It is routed through `dm_set_standby_ex()` rather than a bare
`xEventGroupClearBits()` because clearing STANDBY is not bookkeeping: the CLOSE_ALL sweep
is what makes the window positions agree with the mode just restored.

### The cost turned out to be zero, for a reason that arrived after the issue was filed

gh#52 ranked (c) last because it "triggers a CLOSE_ALL sweep from the reset menu" — a
real three-minute actuation on 5C88, from a menu whose distinguishing feature is *no
reboot*.

That is no longer true. **2.4.5 made `session_close()` clear STANDBY and queue a
recalibration**, and IO0 case 2 calls `session_close(false)` at `ui_display.cpp:829`,
immediately *before* `dm_reload_all_cfg()`. The sweep already happens on that path. And
`dm_set_standby_ex()` returns early when the bit already matches, so the new call costs
nothing there and does **not** sweep twice.

What (c) actually buys is that `dm_reload_all_cfg()` is correct for whatever calls it
next — which was gh#52's stated concern, not the IO0 divergence.

## Operator-visible change

**`POST /api/config` returns 400 for a key it cannot apply**, where it previously
returned 200. That includes the four `ota_*` keys: `/api/ota/config` owns them
(`rota_tds.md` R-F02/R-F03) and this route has never been able to apply them — posting
them here used to persist to NVS and take effect only on the next reload.

**No asset change, and the GUI needs none.** `post()` already returns `null` for a
non-OK response and `feedback()` renders that as **"✗ Error"** in red, so a rejected key
now shows the operator a failure where it previously showed "✓ Saved".

**Every key the bundled GUI posts was checked against the new predicate and passes** —
all 14 `climate`, 9 `motor` and 6 `wind` keys, plus `poll_interval` / `session_timeout` /
`ap_timeout` / `lat_*` / `lon_*` in `system`, and `tz_str` on the string path. A rejected
key is therefore one the GUI never sends, and one that previously did nothing.

## Verification

**NOT YET VERIFIED ON HARDWARE.** At the time of writing this release has been built
clean (`pio run -e lolin_s3`, `-Werror`, 65.9 % flash / 19.2 % RAM) and nothing more.
FDA4 is still running `2.4.5-bench` for the AT-WP05 soak.

Required before promoting to `mainstream`:

1. OTA to FDA4 and confirm **both** `fw_ver` and `asset_version` read `2.4.6`
   post-reboot.
2. **gh#53 positive:** `POST /api/config {"ns":"system","key":"poll_interval","value":45}`
   -> 200, and `GET /api/config` shows 45. A real key must still work.
3. **gh#53 negative:** the original reproduction —
   `POST /api/config {"ns":"system","key":"poll_interval_s","value":45}` -> **400**, and
   nothing written. Confirm the Motors/System tab still saves normally from the GUI.
4. **gh#53 string path:** `{"ns":"system","key":"tz_str","str_value":"CET-1CEST,M3.5.0,M10.5.0/3"}`
   -> 200; any other string key -> 400.
5. **gh#52:** LCD IO0 stage-2 reset — confirm "Defaults loaded" still tells the truth,
   the calibration runs **once**, and the unit settles at `eg1 = 0` in AUTOMATIC.
6. Soak >= overnight before `rota_release.py promote 2.4.6`.

## Upgrade notes

No NVS migration, no partition change, no payload-shape change beyond the new 400.

Anyone driving `/api/config` from a script should expect 400 on a key the controller
cannot apply, and should send **NVS key names**, not the field names `GET /api/config`
returns. The two differ for `poll_interval` and `status_intv_s`.

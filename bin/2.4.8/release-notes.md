# Release 2.4.8

**Date:** 2026-09-12
**Built on:** 2.4.7
**Fixes:** [gh#56](https://github.com/pe1mew/greenhouse-Controller/issues/56) — the IO0 factory reset kept the WiFi credentials it claimed to erase

## The defect

Reported by the operator on FDA4 while verifying 2.4.7: *"a full reset of the controller
(IO0 default setting and restart) does not erase wifi settings."*

The erase works. `nvs_cfg_erase_namespace(NVS_NS_WIFI)` removes `wifi/ssid` and
`wifi/psk`, and `/api/config` reports an empty SSID afterwards. **There is a second
copy.** This firmware never calls `esp_wifi_set_storage()`, so IDF's default
`WIFI_STORAGE_FLASH` is in force and every `esp_wifi_set_config(WIFI_IF_STA, …)` also
persisted the credentials into IDF's own NVS namespace, `nvs.net80211` — untouched by an
app-namespace erase.

The boot sequence then does this:

```
nm_wifi_init_blocking()
  reads wifi/ssid + wifi/psk        -> empty
  have_sta_creds = false            -> logs "no SSID in NVS — skip STA-connect"
  esp_wifi_set_config(STA, ...)     -> SKIPPED
  esp_wifi_start()                  -> loads nvs.net80211 and AUTO-CONNECTS
```

So the controller runs connected to a network it believes it has no credentials for.
Observed on FDA4 across two reboots and a power cycle with `wifi_ssid` empty throughout.

### What it broke

- **A relocated controller joins the old site's network** — and "Verhuizing
  kascontroller → Niveau 3 (alle locatiegebonden config wissen)" is the documented use
  case for level 3.
- **The System tab shows an empty SSID** while the unit is on that SSID; the operator
  cannot see what it is connected to.
- **It defeated the AP recovery flow it was written for.** The no-SSID branch exists so
  AP mode can come up for re-provisioning. Instead the driver rejoins the old network, so
  an operator who factory-resets *in order to* re-provision finds the unit on the old LAN
  and never sees the AP — which is exactly what happened, made worse because 2.4.6 had
  separately broken the AP toggle.

## The fix

New `nm_wifi_erase_persistent()` (`network_manager`) wraps `esp_wifi_restore()`, called
from `execute_reset_action()` cases **2 and 3** immediately after the app namespace is
erased, so both copies go together. Level 1 (PIN reset) does **not** call it — WiFi must
survive that.

`esp_wifi_restore()` alone is sufficient: the two copies only ever disagree immediately
after a reset, because anything that writes one writes the other. Clearing IDF's copy at
exactly the point the app's copy is cleared closes the gap with no change to any other
path.

Level 2's "no reboot" contract is unchanged — WiFi is reboot-to-apply by design, so the
erase lands at the next restart like every other WiFi change.

### Held back deliberately

`esp_wifi_set_storage(WIFI_STORAGE_RAM)` would remove the duplication entirely. It is
**not** in this release: with RAM storage a unit whose `wifi/ssid` is empty will not
connect at all, and a unit that was reset and never re-provisioned (because it kept
working — this bug) is in precisely that state. 5C88 is behind NAT with no push path and
no endpoint exposes the *configured* SSID, so its state cannot be confirmed remotely.
Prerequisite and options in gh#56.

## Verification

**Built clean** (`pio run -e lolin_s3`, `-Werror`, 65.9 % flash).

**To verify on FDA4 after the OTA** — both `fw_ver` and `asset_version` must read 2.4.8
post-reboot:

1. Confirm the WiFi client config is present (System tab shows the SSID).
2. **Hold IO0 to level 2** (`Reset settings?`, 10–15 s). Then:
   - `GET /api/config` → `wifi_ssid` empty, as before;
   - **and the open question for this round: does the live WiFi link drop?**
     `esp_wifi_restore()` is called while connected. A drop is acceptable (the operator
     is at the LCD, the confirmation is on the LCD) but should be *recorded* either way.
3. **Restart the unit.** It must now come up with **no** network — this is the fix. The
   AP (`Greenhouse-FDA4`) must be reachable via the LCD System menu → `1`, which is also
   a re-check of the 2.4.7 AP-toggle fix.
4. Re-provision WiFi via the AP flow (beheerder §11.1) and confirm it reconnects.
5. Repeat at **level 3** (15–20 s) — same expectation, with the reboot built in. **Read
   the warning below first.**

### Rig hazard: a level-3 reset drives M3 at the PRODUCTION travel time

Observed on FDA4 this morning, 2026-09-12, during the operator's own reset test:

```
08:55:42  Boot: esp_reset_reason = 3     <- the reset's reboot
08:55:42  M1/M2/M3 -> MOVING_CLOSE       <- boot CLOSE_ALL calibration
08:56:08  M1/M2    -> CLOSED   (26 s = 21 + 5 s margin)
08:58:38  M3       -> CLOSED  (176 s = 171 + 5 s margin)
```

The erase resets `travel_m3` to the production default of **171 s**. On the 13 s rig that
is ~163 s of the motor stalled against the end stop — and **the boot calibration fires
before anyone can re-set the value**, so the advice "re-set `travel_m3` before any M3
movement" has no window at level 3. Mitigations, in order of preference:

- **Test level 3 with M3 physically disconnected**, or
- **use level 2 (no reboot), re-set `travel_m3 = 13`, then restart** — T2 keeps its
  cached value until notified, so nothing moves in between.

Note also that **`lat_frac` / `lon_frac` restored themselves** at 09:04:56 (initiator
`System`) — T10's geolocation sync re-posts them via Q4. Motor timings have no such
self-heal, which is why `travel_m3` was the one value still wrong hours later.

### After the test round, restore the rig

`travel_m3 = 13`, `lat_frac = 225`, `lon_frac = 964`, plus the WiFi PSK and the
ROTA/status secrets (operator-entered — not readable from any endpoint). **Capture
`/api/config`, `/api/ota/config` and `/api/web` before starting**; a pre-2.4.8 capture is
in the session scratchpad as `fda4_pre248_capture.json`.

**Not promotable yet.** This release has had no soak, and gh#56's 5C88 prerequisite is
open. 2.4.7 remains the production candidate.

## Upgrade notes

No NVS migration, no partition change, no payload change. One new public function in
`network_manager.h`. After upgrading, a level-2/3 reset genuinely clears the WiFi client
configuration — the controller will **not** rejoin the previous network, and
re-provisioning via the AP flow is required. That is the intended behaviour and what the
manuals have always described.

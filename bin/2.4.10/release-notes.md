# Release 2.4.10

**Date:** 2026-09-12
**Built on:** 2.4.8
**Fixes:** [gh#60](https://github.com/pe1mew/greenhouse-Controller/issues/60) and [gh#61](https://github.com/pe1mew/greenhouse-Controller/issues/61) — two operator actions that did not do what they said

Both were found by phase 3 of the operator-path sweep, and both are pre-existing —
`pin_auth.cpp`, `pin_post_handler` and `event_logger.cpp`'s retry loop are unchanged
since `v2.3.1`, so 5C88 behaves identically today.

## gh#61 — the SD unmount was undone before the operator could reach the card

```
POST /api/sd/unmount  ->  200 {"ok":true}   mounted=false  free=0 MB
   ... 30 s, NO mount command sent ...
                          mounted=true   free=1849 MB      <- remounted itself
```

T9 uses a 60 s receive timeout whenever `s_sd_ok` is false and calls
`event_logger_sd_remount()` on each expiry, so a card inserted after boot is picked up
within a minute. Correct for the case it was written for — and wrong here, because
`s_sd_ok == false` cannot distinguish **unavailable** (absent at boot, failed, swapped)
from **released on request**.

`beheerderHandleiding.md` makes the unmount mandatory before removal and gives no
deadline:

> `:782` — *"**Verplicht voordat u een SD-kaart fysiek verwijdert**: klik **Unmount** in
> de Log-tab. Anders kunnen de laatste log-events verloren gaan of kan het bestandssysteem
> corrupt raken."*

and `:1320-1322` spells out the procedure: unmount, wait for confirmation, **then remove
the card**. Between those steps the operator opens the enclosure and finds the socket.
Sixty seconds is an optimistic budget, nothing says a clock is running, and the GUI gives
no sign when the remount happens.

**Fix:** an `s_sd_released` latch in `event_logger.cpp`, set by
`event_logger_sd_unmount()` and cleared by `event_logger_sd_remount()`. **Both** of T9's
automount call sites now check it.

> **The first attempt guarded only one of them.** T9 has two automount paths: the 60 s
> `xQueueReceive` timeout branch (`:1162`), and an elapsed-time check after event
> processing (`:1186`) that exists *because* the sensor poll keeps Q3 busy so the
> timeout never fires. A 2.4.9 bench build latched the first only, and the card
> remounted itself 30 s after a deliberate unmount. The verification step below waits
> **135 s** — more than twice the retry interval — which is what caught it. A 30 s
> wait would have passed a broken fix.

A deliberate release holds until an explicit mount request or a reboot; an absent or
failed card never sets the latch, so hot-insertion is untouched. The latch clears on a
mount request whether or not the mount succeeds — leaving it set on failure would
strand a unit whose card was released and then reinserted.

## gh#60 — `/api/pin` changed the wrong credential, and could set an unusable one

Two holes that compose:

```
POST /api/pin {"role":"bogus","pin":"1234"}   -> 200 {"ok":true}   ... changed the FARMER pin
POST /api/pin {"role":"farmer","pin":"12a4"}  -> 200 {"ok":true}   ... a pin the keypad cannot type
```

**Role fell through.** `pr = (strcmp(role_str,"admin")==0) ? ADMIN : FARMER`, with
`json_get_field()`'s return value discarded — so `"Admin"`, `"ADMIN"`, a typo or an
omitted field all became farmer. The realistic failure: an admin changes the admin PIN,
types `"Admin"`, gets `{"ok":true}`, and now the admin PIN is unchanged while the farmer
PIN is whatever they intended as the new admin PIN. Both credentials are wrong from their
point of view, and the audit row reads *farmer PIN changed* — true, but not what was asked.

**Charset was unchecked.** `pin_auth_set()` tested only `strlen()`. The LCD accepts
`'0'`–`'9'` only (`handle_pin()`), so a non-digit PIN set through the API can never be
typed at the panel. For the **admin** role that makes the LCD admin login permanently
impossible — and the LCD is the only surface for manual motor control (gh#29) and for the
IO0 reset confirmations. Recovery is then IO0 level 1 or `pin_auth_reset_admin()`, whose
own header says it *"should only be reachable through the hardware recovery procedure."*

**Fix:** exact match on both role strings with a **400** for anything else including a
missing field; a digits-only loop in `pin_auth_set()`, placed there because it is the one
choke point both surfaces share; and **400** rather than `200 {"ok":false}` for every
rejection, matching what 2.4.6 did to `/api/config`.

## Verification

**Built clean** (`pio run -e lolin_s3`, `-Werror`, 65.9 % flash).

**To verify on FDA4 after the OTA** — both `fw_ver` and `asset_version` must read 2.4.10:

### gh#60
1. `{"role":"bogus","pin":"1234"}` → **400 unknown role** (was 200 + a farmer change).
2. `{"pin":"1234"}` with **no role** → **400 unknown role**.
3. `{"role":"Admin","pin":"12345678"}` → **400** (capitalisation no longer silently
   becomes farmer).
4. `{"role":"farmer","pin":"12a4"}` → **400** (was 200).
5. `{"role":"farmer","pin":"4321"}` → 200, login with 4321 works, 1234 refused; restore
   1234 and confirm. **A real role must still work.**
6. `{"role":"admin","pin":"12345678"}` → 200. Do this one last and confirm admin login
   still works before closing the session.

### gh#61
1. `POST /api/sd/unmount` → `mounted=false`.
2. **Wait ≥120 s** — twice the retry interval — and confirm it is *still* `mounted=false`.
   That is the fix.
3. `POST /api/sd/mount` → `mounted=true`, free space back, logging resumes.
4. Confirm hot-insertion still works: with the card released, a `POST /api/sd/mount`
   must succeed, which is the same path an after-boot insertion takes.

**Not promotable.** No soak yet, and nothing is being promoted to `mainstream` pending the
wider defect review.

## Upgrade notes

No NVS migration, no partition change. One behaviour change an integrator could notice:
`POST /api/pin` now answers 400 where it answered 200, and rejects a role string that is
not exactly `farmer` or `admin`. Scripts that relied on the fall-through were changing the
farmer PIN whether they knew it or not.

Operators should know that **an unmounted card now stays unmounted** until mounted again
or the controller reboots — which is what the manual always described.

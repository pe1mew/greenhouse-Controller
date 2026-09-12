# Release 2.5.0

**Date:** 2026-09-12
**Built on:** 2.4.10
**Fixes:** [gh#57](https://github.com/pe1mew/greenhouse-Controller/issues/57) part 1 and
[gh#58](https://github.com/pe1mew/greenhouse-Controller/issues/58) — validation that was
never enforced, and failures that were never recorded

**Minor, not patch.** `LOG_PIN_AUTH` is a new log event type, which is a payload-shape
change. Both defects are pre-existing: `cfg_clamp()`, `pin_auth.cpp` and `log_type_t` are
unchanged since `v2.3.1`, so 5C88 behaves identically today.

## gh#58 — a failed PIN left no trace anywhere

Before this release, five wrong PINs on the keypad produced this much of an audit trail:

```
2026-09-12T10:26:02,SYSTEM,WEB,0,0,1,0      <- a routine T14 status POST. That is all.
```

`pin_auth.cpp` contained **no logging calls at all** — not a failed verify, not the
counter reaching the threshold, not an attempt refused during a lockout. The LCD only
painted `Wrong PIN!` on the screen. The web login wrote an `ESP_LOGW` line, which on a
unit in a greenhouse has nobody to read. And there was no event type to carry it:
`main.cpp:1313` and `:1325` had described T9 as handling `LOG_PIN_AUTH` for four minors,
but the type did not exist.

So the audit log had exactly the wrong asymmetry: `LOG_SESSION` recorded **who got in**,
and nothing recorded **who tried and failed**. The unit has a keypad in a shared building
and an AP whose factory password the manual itself warns to change, so both surfaces are
reachable by someone who should not be there.

**Fix.** A new `LOG_PIN_AUTH` type (ordinal 9, appended after `LOG_SUN`), emitted from
`pin_auth_verify()` — the one place both the LCD keypad (T8) and `POST /api/login` (T11)
pass through.

| field | meaning |
|---|---|
| `initiator` | the surface: `FARMER`/`ADMIN` = LCD keypad, `WEB` = `/api/login` |
| `channel` | role attempted: 1 = farmer, 2 = admin |
| `value_a` | 0 = failed, 1 = lockout armed, 2 = refused while locked out |
| `value_b` | failure count / lockout seconds / seconds remaining |

Three deliberate omissions, each for a reason:

- **A success emits nothing here.** It is already a `LOG_SESSION` row, so this type
  carries failures only and the two are read together.
- **The entered digits are never logged.**
- **A wrong-*length* PIN gets a console line, not an SD row.** It never touches the
  failure counter, so it can never reach the lockout and can never succeed — and giving
  it a row would hand an unauthenticated caller an unbounded way to flood the audit log.

`pin_auth_verify()` gains a required third parameter, `log_initiator_t surface`. Required
rather than defaulted, so a future third caller cannot silently lose the attribution —
the same reasoning that made `cfg_key_kind()` three-way in 2.4.7. No extra NVS write was
added: the function already writes one `int32` per call, so a brute-force attempt is
already a flash-wear path and must not become a worse one.

`log/logparser.py` learns the encoding in this same changeset, per the standing rule in
`CLAUDE.md`. `plot_daily.py` needs no change — it reads `ALARM` and `SENSOR_HR` only.

## gh#57 part 1 — eleven keys accepted any `int32`

`cfg_clamp()` covered 32 keys; the shadow ladder in `apply_config_update()` covered 46.
The eleven in the gap were clamped nowhere **and** absent from `/api/config/limits`, so
neither the server nor a client constrained them. Demonstrated live on 2.4.8:

```
POST {"ns":"climate","key":"cr_priority","value":99}  ->  200 {"ok":true}   stored: 99
POST {"ns":"climate","key":"cr_priority","value":-5}  ->  200 {"ok":true}   stored: -5
```

`cr_priority` is inert out of range (`vent_resolve_conflict()` shares `case 0:` with
`default:`). The lat/lon four are not. They feed `update_sun_times()`, which sets
`s_cfg.is_daytime`, which T6 uses to pick day or night setpoints — so an out-of-range
latitude could put the controller on **night thresholds in daylight** from one HTTP
request, the same harm shape as gh#55.

**Fix.** Bounds added to `cfg_limits.h`, enforced in `cfg_clamp()`, and published by
`/api/config/limits` (now 40 keys). Every bound is taken from the consumer, not invented:

| key | bound | why |
|---|---|---|
| `cr_priority` | 0–2 | `vent_resolve_conflict()` implements exactly 0/1/2 |
| `rh_ctrl_en`, `wind_prot_en` | 0–1 | read as `!= 0`, so any non-zero worked by luck |
| `lat_deg` / `lon_deg` | ±90 / ±180 | `update_sun_times()` assembles `deg + frac/1000.0f` |
| `lat_frac` / `lon_frac` | 0–999 | thousandths of a degree |
| `led_day_brt` / `led_nite_brt` | 0–255 | 8-bit PWM duty |
| `led_nite_from` / `led_nite_to` | 0–23 | local hours; reuses `CFG_MIN_HOUR`/`CFG_MAX_HOUR` |

Two encoding quirks found while reading the consumers, **documented in `cfg_limits.h`
rather than changed**, because changing either is a payload change:

- The coordinate fraction is unsigned. For a negative degree it moves the value *toward*
  zero (`lat_deg=-52, lat_frac=500` gives `-51.5`). Correct for the northern hemisphere,
  which is where it is used.
- `app.js` computes `Math.round(Math.abs(lat - Math.trunc(lat)) * 1000)`, so `52.9996`
  yields `frac = 1000`, which the new clamp turns into `999` — a 1-millidegree
  (about 111 m) rounding difference, versus storing an invalid value before.

## What did NOT change

**gh#57 part 2 is untouched and gh#57 stays open.** FR-S03 and FR-CF07 are both "Must"
and both say `poll_interval` is settable over **15–120 s**; `cfg_limits.h` says **30–300**
and carries a recorded rationale for the 30 s floor. An operator following the FRS cannot
set 15 s and *can* set 200 s. That is a decision, not a patch: either amend the two
requirements and record why, or change the code. Nothing in this release moves those
bounds.

## Verification

**Built clean** (`pio run -e lolin_s3`, `-Werror`), 66.0 % flash, 19.2 % RAM.

**OTA to FDA4 confirmed on both partitions** — `fw_ver = 2.5.0` *and*
`asset_version = 2.5.0` read back from `/api/status` after the reboot.

The archived binary was rebuilt and re-pushed after a comment-only docstring edit,
so the committed source, `bin/2.5.0/greenhouse-controller-2.5.0.bin` and the unit are
the same image. Post-reboot smoke test on that rebuilt image, 7/7: both versions
match, `/api/config/limits` still returns 40 keys with the new bounds, every config
value survived the reboot at its restored setting, `eg1 = 0` with the SD mounted, and
one wrong farmer PIN produced `2026-09-12T11:57:46,PIN_AUTH,WEB,1,0,0,1`.

**Static pre-flight.** `/api/config/limits` is assembled from stringified macros, so a
parenthesised `#define` or a stray comma would produce invalid JSON that only fails in
the browser. A script expanded the literal the way the preprocessor does and `json.loads`
it: 40 keys, 851 bytes, no malformed ranges. The same script cross-checks
`webUiMock/mock_server.py` against it — 40 keys each, none only on one side, none
differing.

### gh#57 part 1 — 39/39 on FDA4

- All eleven keys present in `/api/config/limits` with the bounds above; the 29
  pre-existing keys still there.
- For each of the seven keys that `GET /api/config` exposes, both bounds proven: the key
  is first moved to a base value that differs from the bound, then the out-of-range value
  is POSTed, and the **stored** value is read back. Examples: `lat_deg` `187 → 90` and
  `-187 → -90`; `lon_deg` `277 → 180` and `-277 → -180`; `lat_frac` `1096 → 999` and
  `-97 → 0`; `cr_priority` `99 → 2` and `-97 → 0`.
- **Controls:** an in-range `cr_priority = 1` is stored verbatim, so the clamp is not
  simply flattening everything; `t_max_day = 999 → 45` still behaves as it did before
  2.5.0; an unknown key still returns 400 (gh#53 intact); the NVS-only `wifi/ap_enable`
  is still accepted (the 2.4.7 three-way classifier intact).
- Every value the test moved was restored and re-read, and `/api/status` reports
  `sun: {is_daytime: true, sunrise_min: 426, sunset_min: 1199}` — plausible for Soest on
  12 September, so the latitude excursion left no residue.

**Not observable, stated rather than claimed:** `GET /api/config` never exposes the four
`led_*` fields and `ns_key_to_log_id()` returns `LOG_PARAM_NONE` for them, so there is no
API path to read their stored value back. The POSTs are accepted; the clamp itself is the
same `_CLAMP` macro in the same `else if` ladder as the seven that are proven. Confirming
it needs someone looking at the status LED.

### gh#58 — verified end to end on FDA4

Five wrong farmer PINs via `POST /api/login`, then an attempt during the lockout, then
one wrong admin PIN. These are the rows the firmware actually wrote:

```
2026-09-12T11:39:53,PIN_AUTH,WEB,1,0,0,1
2026-09-12T11:39:55,PIN_AUTH,WEB,1,0,0,2
2026-09-12T11:39:57,PIN_AUTH,WEB,1,0,0,3
2026-09-12T11:39:58,PIN_AUTH,WEB,1,0,0,4
2026-09-12T11:39:59,PIN_AUTH,WEB,1,0,1,300
2026-09-12T11:40:01,PIN_AUTH,WEB,1,0,2,298
2026-09-12T11:40:06,PIN_AUTH,WEB,2,0,0,1
```

and this is `logparser.py` reading them back:

```
2026-09-12 11:39:53  [PIN_AUTH ]  Web UI    PIN failed: farmer, failure #1  [Web UI]
2026-09-12 11:39:59  [PIN_AUTH ]  Web UI    PIN LOCKOUT ARMED: farmer locked for 300s  [Web UI]
2026-09-12 11:40:01  [PIN_AUTH ]  Web UI    PIN refused, farmer locked out, 298s remaining  [Web UI]
2026-09-12 11:40:06  [PIN_AUTH ]  Web UI    PIN failed: admin, failure #1  [Web UI]
```

Also confirmed:

- The failure counter is cleared by a correct PIN first, so the counts `1,2,3,4` are the
  real sequence and not a leftover.
- The correct PIN is **also** refused during the lockout, logged as `value_a=2` with the
  remaining seconds counting down (300 then 298).
- **Role scoping:** admin login succeeded throughout the farmer lockout, and a wrong
  admin PIN started its own counter, logged as `channel 2`.
- **The malformed-length attempt produced no row** — five farmer rows for five real
  attempts, not six.
- **Recovery:** after the 300 s window the correct farmer PIN was accepted again.
- **The success path stays silent.** Three successful logins added **no** `PIN_AUTH` row
  (7 before, 7 after) while the log demonstrably grew by 6 rows in the same window. The
  first attempt at this check compared raw line counts between two downloads without
  asserting the second download's HTTP status, and reported a false failure; counting
  `PIN_AUTH` rows directly is what settled it.

**The LCD surface is not hardware-verified.** It shares the same `pin_auth_verify()` call
and the same emission, but the `initiator` tagging (`FARMER`/`ADMIN` rather than `WEB`)
has only been exercised on the web path from here. Worth one operator check: enter two
wrong farmer PINs at the panel and confirm the rows read `FARMER`, not `WEB`.

**One side effect to note:** a control in the clamp test set `wifi/ap_enable = 0`. That
is the normal steady state for a unit on STA, but if FDA4's AP had been deliberately left
on, re-enable it from the LCD System menu.

## Upgrade notes

No NVS migration, no partition change.

**For anything that reads the audit log:** a new event type `PIN_AUTH` will appear in
`.csv` files from 2.5.0 onward. `logparser.py` handles it from this release;
`plot_daily.py` is unaffected. Older parsers will show it as `UNKNWN` rather than fail.

**For anything that calls `POST /api/config`:** eleven keys that previously stored
whatever you sent now clamp to a published range. `GET /api/config/limits` returns 40
keys instead of 29. A caller that was relying on the fall-through was storing invalid
values whether it knew it or not.

**No soak yet, and nothing is being promoted to `mainstream`.** 5C88 stays on 2.3.1
pending the wider defect review.

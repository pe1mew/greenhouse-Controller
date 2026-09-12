# Release 2.7.0

**Date:** 2026-09-12
**Built on:** 2.6.0
**Fixes:** [gh#63](https://github.com/pe1mew/greenhouse-Controller/issues/63) — drop the
misnamed `dwell_*_min` JSON aliases

**Minor, not patch.** Removing a field from a documented API response is a payload-shape
change.

## What was wrong

`GET /api/config` emitted four dwell fields where two would do:

```json
"dwell_open_s":[300,300,1500], "dwell_close_s":[0,0,600],
"dwell_open_min":[300,300,1500], "dwell_close_min":[0,0,600],
"poll_interval_s":30, "session_timeout_min":5, "ap_timeout_min":30
```

The `_min` pair was a migration alias added when `_s` became canonical in 2.4.4, and the
comment beside it said to drop it in the next minor. That was **2.5.0**. This is three
minors late.

It was worse than ordinary staleness, because **the name said minutes while the value
carried seconds** — three lines above `session_timeout_min` and `ap_timeout_min`, which
really are minutes. A client reading `"dwell_open_min":[300,300,1500]` next to
`"session_timeout_min":5` had every reason to treat all three the same way. Two of them
it could. Taking the dwell field at its word is a factor-sixty error on the parameter
that governs how long a window is held open, which was the original gh#51 Group D defect;
the JSON surface was the last place still carrying it.

## What changed

- The two `_min` entries and their six `snprintf` arguments are gone from
  `config_get_handler()`. The comment that remains explains why they must not come back.
- **`app.js` no longer falls back to the `_min` names.** Its stated reason was
  compatibility with firmware older than 2.4.4 — but **the mock was the only thing
  actually serving `_min`.** `webUiMock/mock_server.py` had never learned the `_s` names
  at all, neither in its config payload nor in its `NVS_MAP`. So the fallback was quietly
  covering a mock/firmware mismatch rather than the version skew it documented. Removing
  it without fixing the mock would have broken the dev GUI, which is precisely how that
  kind of alias survives. The mock now serves `_s`.
- `beheerderHandleiding.md` referred to `dwell_open_min` in its prose on dwell bypass;
  now `dwell_open_s`.
- `design/fixMotorTimingRefresh.md`'s open deprecation item is marked done, and the
  matching reminder is removed from `CLAUDE.md`.

Nothing else moved. `dwell_open_s` / `dwell_close_s` keep their values and meaning, the
**write** path is untouched because `POST /api/config` never accepted the `_min` spelling,
and `session_timeout_min` / `ap_timeout_min` are genuinely minutes and stay.

## Verification

**Built clean** (`pio run -e lolin_s3`, `-Werror`), 66.0 % flash, down 96 bytes.

That build is the substantive check here, not a formality. The change deleted two format
specifiers and six arguments from one `snprintf`, and an arity mismatch there would not
crash — it would emit **garbage JSON** that only shows up in a client. `-Wformat` under
`-Werror` is what proves the argument list still matches. My own attempt at a
static arity checker mis-parsed the argument list and reported a false mismatch; the
compiler settled it.

**Pushed to 2344**, the module currently fitted to the dev rig, with an explicit
`--host 192.168.20.160`. `ota_push.py` defaults to FDA4's `192.168.20.169` and FDA4 is
swapped out of the rig, so the default would have pushed to a board that is not there.

**16/16 on hardware**, with 2.6.0 as the fail-first baseline since this module ran it
minutes earlier and emitted both spellings:

- Both version fields read 2.7.0, and the unit identified itself as 2344.
- `/api/config` returns valid JSON, 34 keys in 616 bytes — so the arity is right in
  practice as well as at compile time.
- `dwell_open_min` and `dwell_close_min` are absent, and the literal string does not
  appear anywhere in the body.
- `dwell_open_s` still reads `[300, 300, 1500]` and `dwell_close_s` `[0, 0, 600]`.
- `session_timeout_min` = 5 and `ap_timeout_min` = 30 are untouched.
- A dwell write still round-trips: `dwell_open_m1` set to 301, read back, restored to 300.
- The **served** `app.js` contains no `_min` fallback and still reads `dwell_open_s`,
  which is the check that matters for an asset change — the firmware building proves
  nothing about the bundled JavaScript.
- `eg1` clear, SD mounted, `travel_s` still `[21, 21, 13]` so the rig's M3 setting
  survived the reboot.

## Upgrade notes

No NVS migration, no partition change.

**For anything parsing `GET /api/config`:** two fields are gone. If you were reading
`dwell_open_min` or `dwell_close_min`, read `dwell_open_s` / `dwell_close_s` instead —
same values, honest units. If you were reading them *as minutes*, you were already wrong
by a factor of sixty.

The bundled GUI is updated in the same release. Because assets and firmware ship as a
pair within 120 s, no mixed state persists; if a push is interrupted between the two, the
dwell fields render blank until the asset half lands, and re-pushing fixes it.

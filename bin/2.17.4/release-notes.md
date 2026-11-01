# Release 2.17.4

**Date:** 2026-11-01
**Built on:** 2.17.3
**Changes:** the session cookie no longer expires under an active operator
([gh#106](https://github.com/pe1mew/greenhouse-Controller/issues/106))

**Patch. Bug fix only:** no setting, key, log row or payload change.

> **Not soaked, not published.**

## What changed

- **The session cookie has no `Max-Age`.** It was set at login only, while the server slides its expiry on every request, so a browser dropped the cookie at login + `session_timeout_min`, however active the operator was, and the next request came back 401. On hardware that is 303 s after the login at the default timeout: anyone working in the web GUI was logged out every 5 minutes.
- **The limits that count are unchanged:** the server's expiry and the page's idle logout both stay at `session_timeout_min` without activity.
- **Closing the browser now ends the login.** The cookie lasts for the browser's session. Before, a browser reopened within the 5 minutes was still logged in. `boerHandleiding.md` says so.
- **New: `bin/at_session_cookie.py`**, with a real cookie jar. The harness's `Unit` keeps the token in a string and ignores the cookie's attributes, so no test could have seen this.

The manuals already described the timeout as idle time. This makes them true.

### Who reads and writes what

Found with grep on 2026-11-01.

| Item | Enumerated |
|---|---|
| `cookie_set_session()` callers | One, the login handler. `max_age_s` is gone. `timeout_s` still sets the server's expiry through `session_open()` |
| `Max-Age` in the firmware | Only `cookie_clear_session()`'s `Max-Age=0`, unchanged: that is what expires the cookie at logout |
| `Max-Age` elsewhere | The mock, at logout only. `bin/` and the GUI do not read it |

## Verification

| Check | Result |
|---|---|
| Release image | 1 435 200 B, sha256 `9116c278e26ebd44…`, version `2.17.4`. Assets `web-assets-2.17.4.zip`, 176 331 B, sha256 `42c7615f482bba60…`. **Built, not run on hardware** |
| Bench image on 2344 | `afd53ad13019f641…`. `fw_ver` and `asset_version` 2.17.4-bench |
| `at_session_cookie.py active`, fail-first on 2.17.3 | FAIL: 401 at call 15, 303 s after the login, cookie gone from the jar. **This reproduces the report** |
| `at_session_cookie.py`, fixed: active | PASS: 18 calls over 363 s, every one admin |
| Fixed: idle | PASS: 401 after 345 s without a call, so sessions still expire |
| Fixed: logout | PASS: the cookie is gone from the jar |
| `bin/check_cfg_desc.py` | 52 keys, 41 published, all agree |

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.

## Known limitations

- **Not soaked, not published.**
- **Checked with a cookie jar, not in a browser.** The jar applies a cookie's attributes as a browser does, but the GUI itself past 5 minutes has not been watched.
- **`heap_min_kb` 2 KB in the 2.17.3 soak:** the cause was not found.

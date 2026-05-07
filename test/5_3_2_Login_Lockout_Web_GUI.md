# 5.3.2 Login Lockout — Web GUI Test

## Purpose

Verifies the web GUI login lockout behaviour for both farmer and admin roles against test cases UT-AC-011 through UT-AC-014 (`softwareTestPlan.md` §8.3).

| ID | Description |
|----|-------------|
| UT-AC-011 | Lockout triggered after N consecutive wrong PINs |
| UT-AC-012 | Correct PIN rejected while lockout is active |
| UT-AC-013 | Lockout expires after the configured duration |
| UT-AC-014 | Failure counter resets to 0 after a successful login |

Both the **farmer** (4-digit PIN) and **admin** (8-digit PIN) roles are exercised independently.

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| Device reachable | Default `http://192.168.20.150` (overridable via `GH_DEVICE_BASE`) |
| WiFi | Device must be in AP mode or connected to the local network |
| Admin PIN known | Default `12345678` (overridable via `GH_ADMIN_PIN`) |
| Farmer PIN known | Default `1234` (overridable via `GH_FARMER_PIN`) |
| Python | 3.10 or newer |
| `requests` library | `pip install requests` |

---

## How to Run

```powershell
# 1. Install dependency (once)
python -m pip install requests

# 2. Set PINs if not using defaults
$env:GH_ADMIN_PIN  = "your-admin-pin"
$env:GH_FARMER_PIN = "your-farmer-pin"

# 3. Optionally override device address
$env:GH_DEVICE_BASE = "http://192.168.20.150"

# 4. Run from the test directory
cd test
python 5_3_2_Login_Lockout_Web_GUI.py
```

Results are written to `test/5_3_2_Login_Lockout_Web_GUI.log` and echoed to stdout.

---

## Test Parameters

The script temporarily writes the following values to NVS to keep test duration short:

| NVS key (`access` ns) | Production default | Test value | Purpose |
|-----------------------|--------------------|------------|---------|
| `lockout_max` | 5 | 3 | Trigger lockout after 3 failures |
| `lockout_secs` | 300 (5 min) | 20 | Lockout lasts 20 seconds |

**These values are restored unconditionally in a `finally` block**, even when tests fail or an exception is raised.

---

## Expected Duration

| Phase | Time |
|-------|------|
| Admin login + setup pre-clear | ~5 s |
| UT-AC-011/012 farmer | < 2 s |
| UT-AC-013 farmer (lockout wait 20+5 s) | ~25 s |
| UT-AC-014 farmer | < 2 s |
| UT-AC-011/012 admin | < 2 s |
| UT-AC-013 admin (lockout wait 20+5 s) | ~25 s |
| UT-AC-014 admin | < 2 s |
| Teardown | ~4 s |
| **Total** | **~67 s** |

---

## How the Tests Work

### UT-AC-011 — Lockout triggered

A fresh `requests.Session` (no cookie) sends `TEST_LOCKOUT_MAX` wrong PINs to `POST /api/login`.

- Attempts 1 … N−1: response must be `{"ok":false,"locked":false}` (wrong, not yet locked).
- Attempt N: response must contain `"locked":true`.

The wrong PIN is computed by incrementing every digit by 1 mod 10
(`"1234"` → `"2345"`, `"9999"` → `"0000"`). This guarantees the
computed PIN differs from any real PIN.

### UT-AC-012 — Correct PIN blocked during lockout

Called immediately after UT-AC-011, while the role is still locked.
Sends the correct PIN; expects `"locked":true` in the response.

### UT-AC-013 — Lockout expires

Waits `TEST_LOCKOUT_SECS + 5` seconds (25 s), then sends the correct PIN.
Expects `{"ok":true, "role":"..."}`.

### UT-AC-014 — Counter resets on success

1. Send N−1 wrong PINs → no lockout triggered.
2. Send correct PIN → success; failure counter resets to 0.
3. Send N−1 wrong PINs again → no lockout (counter was reset).

If the counter had **not** reset, the N−1 wrong PINs in step 3 combined with the N−1 in step 1 would total 2(N−1) failures. With N=3 that is 4 failures, exceeding the lockout threshold of 3. The absence of lockout in step 3 therefore proves the counter was reset by the successful login.

---

## Implementation Notes

### Why the admin session stays valid during admin lockout

The admin `requests.Session` used for setup/teardown holds a session cookie obtained before the lockout tests run. The lockout mechanism (`pin_auth_verify`) only blocks new `POST /api/login` calls. Cookie-based access to `/api/config` (used by `write_config`) goes through `session_validate()`, which checks the cookie — **not** the PIN lockout state. Teardown writes therefore succeed even while the admin PIN is locked.

### NVS write path for `access/lockout_max` and `access/lockout_secs`

These keys are not in T4's `cfg_shadow_t`, but `POST /api/config` routes integer values through Q4 → `apply_config_update()` in T4, which calls `nvs_cfg_set_i32(ns, key, value)` regardless of whether the key exists in the shadow. The NVS write succeeds and T4 logs:

```
Q4 key not in shadow: access/lockout_max = 3  (NVS written; shadow unchanged)
```

`pin_auth_verify()` reads `lockout_max` and `lockout_secs` fresh from NVS on every call, so the new values take effect on the very next login attempt — no restart needed.

### Lockout state in NVS

`fail_cnt_f/a` and `lockout_f/a` are NVS `int32` keys in the `access` namespace. A device power-cycle resets them. Do not power-cycle the device during the test run.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All tests passed |
| `1` | One or more tests failed |
| `2` | Admin session could not be established (check device IP and PIN) |

---

## Log File Format

```
2026-05-07 12:00:05  INFO   Admin session established
2026-05-07 12:00:05  INFO   SETUP: clearing any pre-existing lockout (lockout_secs=1, wait 3 s)
2026-05-07 12:00:10  INFO   SETUP: writing lockout_max=3, lockout_secs=20
2026-05-07 12:00:12  INFO   --- Role: farmer ---
2026-05-07 12:00:13  INFO   [UT-AC-011/farmer] PASS — Locked correctly after 3 failures
2026-05-07 12:00:13  INFO   [UT-AC-012/farmer] PASS — Correct PIN correctly rejected during active lockout
2026-05-07 12:00:13  INFO   [UT-AC-013/farmer] Waiting 25 s for lockout to expire …
2026-05-07 12:00:38  INFO   [UT-AC-013/farmer] PASS — Login accepted after lockout expiry
2026-05-07 12:00:38  INFO   [UT-AC-014/farmer] PASS — Counter reset confirmed: …
...
2026-05-07 12:01:10  INFO   SUMMARY: 8/8 passed, 0 failed
```

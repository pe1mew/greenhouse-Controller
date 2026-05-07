#!/usr/bin/env python3
"""
5_3_2_Login_Lockout_Web_GUI.py
Greenhouse Ventilation Controller — Login lockout test, web GUI

Covers: UT-AC-011, UT-AC-012, UT-AC-013, UT-AC-014
Both farmer (4-digit) and admin (8-digit) roles are tested.

See 5_3_2_Login_Lockout_Web_GUI.md for full instructions.
Results written to 5_3_2_Login_Lockout_Web_GUI.log.
"""

import os
import sys
import time
import logging
import requests

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DEVICE_BASE   = os.getenv("GH_DEVICE_BASE",   "http://192.168.20.150")
ADMIN_PIN     = os.getenv("GH_ADMIN_PIN",      "12345678")
FARMER_PIN    = os.getenv("GH_FARMER_PIN",     "1234")

# Lockout parameters written to NVS for the duration of the test.
# Restored unconditionally in teardown.
TEST_LOCKOUT_MAX   = 3   # trigger lockout after 3 failures
TEST_LOCKOUT_SECS  = 20  # lockout duration (seconds)

NVS_SETTLE_S       = 2   # wait after each POST /api/config for Q4 → T4 NVS write
EXPIRY_MARGIN_S    = 5   # extra wait beyond TEST_LOCKOUT_SECS in UT-AC-013

_DIR     = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(_DIR, "5_3_2_Login_Lockout_Web_GUI.log")

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def _make_logger() -> logging.Logger:
    log = logging.getLogger("lockout_test")
    log.setLevel(logging.DEBUG)
    fmt = logging.Formatter(
        "%(asctime)s  %(levelname)-5s  %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    fh = logging.FileHandler(LOG_PATH, mode="w", encoding="utf-8")
    fh.setFormatter(fmt)
    ch = logging.StreamHandler(sys.stdout)
    ch.setFormatter(fmt)
    log.addHandler(fh)
    log.addHandler(ch)
    return log

log = _make_logger()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def wrong_pin(correct_pin: str) -> str:
    """
    Return a PIN guaranteed to differ from correct_pin.
    Every digit is incremented by 1 modulo 10:
      "1234" -> "2345",  "9999" -> "0000",  "1230" -> "2341"
    """
    return "".join(str((int(d) + 1) % 10) for d in correct_pin)


def do_login(session: requests.Session, role: str, pin: str) -> dict:
    """POST /api/login; returns the parsed JSON body."""
    r = session.post(
        f"{DEVICE_BASE}/api/login",
        json={"role": role, "pin": pin},
        timeout=10,
    )
    r.raise_for_status()
    return r.json()


def write_config(admin_session: requests.Session, ns: str, key: str, value: int) -> None:
    """POST /api/config (integer) with the admin session."""
    r = admin_session.post(
        f"{DEVICE_BASE}/api/config",
        json={"ns": ns, "key": key, "value": value},
        timeout=10,
    )
    r.raise_for_status()
    body = r.json()
    if not body.get("ok"):
        raise RuntimeError(f"Config write {ns}/{key}={value} rejected: {body}")


def get_admin_session() -> requests.Session:
    """Create a new session and authenticate as admin."""
    s = requests.Session()
    body = do_login(s, "admin", ADMIN_PIN)
    if not body.get("ok"):
        raise RuntimeError(f"Admin login failed: {body}")
    log.info("Admin session established")
    return s

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------

def setup(admin_session: requests.Session) -> None:
    """
    Reset all lockout state, then write test parameters.

    Two independent problems require two separate reset steps:

    1. Active lockout (lockout_f/a != 0): write lockout_secs=1, wait 3 s so
       pin_auth_verify()'s expiry branch fires and clears both the expiry
       timestamp and the fail counter on the next attempt.

    2. Stale fail counter with no active lockout (lockout_f/a == 0,
       fail_cnt_f/a > 0): the expiry branch is never entered, so the counter
       persists across test runs. Explicitly zero all four NVS state keys.

    Q4 -> T4 NVS writes are asynchronous; NVS_SETTLE_S covers the latency.
    pin_auth_verify() reads all of these keys fresh from NVS on every call.
    """
    log.info("SETUP: clearing any active lockout (lockout_secs=1, wait 3 s)")
    write_config(admin_session, "access", "lockout_secs", 1)
    time.sleep(3)

    log.info("SETUP: zeroing fail counters and lockout expiry timestamps")
    write_config(admin_session, "access", "fail_cnt_f", 0)
    write_config(admin_session, "access", "fail_cnt_a", 0)
    write_config(admin_session, "access", "lockout_f",  0)
    write_config(admin_session, "access", "lockout_a",  0)

    log.info(
        f"SETUP: writing lockout_max={TEST_LOCKOUT_MAX}, "
        f"lockout_secs={TEST_LOCKOUT_SECS}"
    )
    write_config(admin_session, "access", "lockout_max",  TEST_LOCKOUT_MAX)
    write_config(admin_session, "access", "lockout_secs", TEST_LOCKOUT_SECS)
    time.sleep(NVS_SETTLE_S)


def teardown(admin_session: requests.Session) -> None:
    """Restore default lockout parameters regardless of test outcome.

    Re-authenticates before writing: the test takes ~56 s and the admin
    session cookie can expire if session_timeout_min is configured to 1 min
    or if the rolling timer was not refreshed during the long UT-AC-013 waits.
    """
    log.info("TEARDOWN: restoring lockout_max=5, lockout_secs=300")

    # Proactively refresh the admin session cookie before the config writes.
    # At this point the admin fail counter is ≤ N-2 (not locked), so re-login works.
    try:
        body = do_login(admin_session, "admin", ADMIN_PIN)
        if body.get("ok"):
            log.info("TEARDOWN: admin session refreshed")
        else:
            log.warning(f"TEARDOWN: re-login returned {body} — proceeding with existing cookie")
    except Exception as exc:
        log.warning(f"TEARDOWN: re-login attempt raised {exc} — proceeding with existing cookie")

    write_config(admin_session, "access", "lockout_max",  5)
    write_config(admin_session, "access", "lockout_secs", 300)
    time.sleep(NVS_SETTLE_S)
    log.info("TEARDOWN complete")

# ---------------------------------------------------------------------------
# Result tracker
# ---------------------------------------------------------------------------

class Results:
    def __init__(self):
        self._passed: list[str] = []
        self._failed: list[str] = []

    def record(self, test_id: str, role: str, ok: bool, detail: str = "") -> None:
        tag = f"{test_id}/{role}"
        suffix = f" — {detail}" if detail else ""
        if ok:
            self._passed.append(tag)
            log.info(f"[{tag}] PASS{suffix}")
        else:
            self._failed.append(tag)
            log.error(f"[{tag}] FAIL{suffix}")

    def print_summary(self) -> bool:
        total  = len(self._passed) + len(self._failed)
        passed = len(self._passed)
        failed = len(self._failed)
        log.info("=" * 60)
        log.info(f"SUMMARY: {passed}/{total} passed, {failed} failed")
        for tag in self._passed:
            log.info(f"  PASS  {tag}")
        for tag in self._failed:
            log.error(f"  FAIL  {tag}")
        log.info("=" * 60)
        return failed == 0

# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

def run_ac011(results: Results, role: str, correct_pin: str) -> None:
    """
    UT-AC-011 — Lockout triggered after TEST_LOCKOUT_MAX consecutive failures.

    Send TEST_LOCKOUT_MAX wrong PINs in sequence.
    - Attempts 1 .. (N-1): response must be {"ok":false,"locked":false}
    - Attempt N           : response must contain "locked":true
    """
    test_id = "UT-AC-011"
    bad = wrong_pin(correct_pin)
    s   = requests.Session()
    try:
        for attempt in range(1, TEST_LOCKOUT_MAX + 1):
            body = do_login(s, role, bad)
            if attempt < TEST_LOCKOUT_MAX:
                # Before threshold: wrong but not yet locked
                if body.get("locked"):
                    results.record(
                        test_id, role, False,
                        f"Premature lockout on attempt {attempt}/{TEST_LOCKOUT_MAX}: {body}",
                    )
                    return
            else:
                # At threshold: must be locked
                if not body.get("locked"):
                    results.record(
                        test_id, role, False,
                        f"Lockout not triggered after {attempt} failures: {body}",
                    )
                    return

        results.record(
            test_id, role, True,
            f"Locked correctly after {TEST_LOCKOUT_MAX} failures",
        )
    except Exception as exc:
        results.record(test_id, role, False, str(exc))


def run_ac012(results: Results, role: str, correct_pin: str) -> None:
    """
    UT-AC-012 — Correct PIN rejected while lockout is active.

    Precondition: role must already be locked (called immediately after run_ac011).
    Send the correct PIN; response must contain "locked":true.
    """
    test_id = "UT-AC-012"
    s = requests.Session()
    try:
        body = do_login(s, role, correct_pin)
        if body.get("locked") is True:
            results.record(
                test_id, role, True,
                "Correct PIN correctly rejected during active lockout",
            )
        else:
            results.record(
                test_id, role, False,
                f"Expected locked=true, got {body}",
            )
    except Exception as exc:
        results.record(test_id, role, False, str(exc))


def run_ac013(results: Results, role: str, correct_pin: str) -> None:
    """
    UT-AC-013 — Lockout expires after the configured duration.

    Wait TEST_LOCKOUT_SECS + EXPIRY_MARGIN_S, then send the correct PIN.
    Response must be {"ok":true, ...}.
    """
    test_id = "UT-AC-013"
    wait = TEST_LOCKOUT_SECS + EXPIRY_MARGIN_S
    log.info(f"[{test_id}/{role}] Waiting {wait} s for lockout to expire …")
    time.sleep(wait)
    s = requests.Session()
    try:
        body = do_login(s, role, correct_pin)
        if body.get("ok") is True:
            results.record(
                test_id, role, True,
                "Login accepted after lockout expiry",
            )
        else:
            results.record(
                test_id, role, False,
                f"Expected ok=true after lockout expiry, got {body}",
            )
    except Exception as exc:
        results.record(test_id, role, False, str(exc))


def run_ac014(results: Results, role: str, correct_pin: str) -> None:
    """
    UT-AC-014 — Failure counter resets to 0 after a successful login.

    Step 1: Send (N-1) wrong PINs. No lockout must occur.
    Step 2: Send the correct PIN. Counter resets to 0.
    Step 3: Send (N-1) wrong PINs again. No lockout must occur
            (would lock on attempt N-1+1=N if counter had NOT reset, but
             since reset happened, the current count is only N-1 again).
    """
    test_id = "UT-AC-014"
    bad = wrong_pin(correct_pin)
    s   = requests.Session()
    try:
        # --- First batch: N-1 failures ---
        for attempt in range(1, TEST_LOCKOUT_MAX):
            body = do_login(s, role, bad)
            if body.get("locked"):
                results.record(
                    test_id, role, False,
                    f"Unexpected lockout at attempt {attempt} (first batch): {body}",
                )
                return

        # --- Correct PIN resets the counter ---
        body = do_login(s, role, correct_pin)
        if not body.get("ok"):
            results.record(
                test_id, role, False,
                f"Correct PIN login failed (expected ok=true): {body}",
            )
            return

        # --- Second batch: N-1 failures (counter was reset) ---
        for attempt in range(1, TEST_LOCKOUT_MAX):
            body = do_login(s, role, bad)
            if body.get("locked"):
                results.record(
                    test_id, role, False,
                    f"Locked on second-batch attempt {attempt} — "
                    f"counter was NOT reset by the successful login: {body}",
                )
                return

        results.record(
            test_id, role, True,
            f"Counter reset confirmed: two batches of {TEST_LOCKOUT_MAX - 1} "
            f"failures with a successful login between them; no lockout",
        )
    except Exception as exc:
        results.record(test_id, role, False, str(exc))

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    log.info("=" * 60)
    log.info("Greenhouse Controller — Login Lockout Test")
    log.info("Test cases: UT-AC-011, UT-AC-012, UT-AC-013, UT-AC-014")
    log.info(f"Device     : {DEVICE_BASE}")
    log.info(f"Farmer PIN : {'*' * len(FARMER_PIN)} ({len(FARMER_PIN)} digits)")
    log.info(f"Admin PIN  : {'*' * len(ADMIN_PIN)} ({len(ADMIN_PIN)} digits)")
    log.info(
        f"Test params: lockout_max={TEST_LOCKOUT_MAX}, "
        f"lockout_secs={TEST_LOCKOUT_SECS} s"
    )
    log.info("=" * 60)

    results = Results()

    try:
        admin_session = get_admin_session()
    except Exception as exc:
        log.critical(f"Cannot establish admin session: {exc}")
        sys.exit(2)

    try:
        setup(admin_session)

        for role, pin in [("farmer", FARMER_PIN), ("admin", ADMIN_PIN)]:
            log.info(f"--- Role: {role} ---")
            run_ac011(results, role, pin)   # triggers lockout
            run_ac012(results, role, pin)   # verifies lockout blocks correct PIN
            run_ac013(results, role, pin)   # waits for expiry, verifies login works
            run_ac014(results, role, pin)   # verifies counter resets on success

    finally:
        teardown(admin_session)

    passed = results.print_summary()
    log.info(f"Log written to: {LOG_PATH}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()

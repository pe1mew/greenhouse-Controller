#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Teach acceptance, part 2: automatic control pauses for a teach.

WHY THIS EXISTS
---------------
Nothing paused automatic control during a teach. In AUTOMATIC, T6 could move M3
between two legs, which ends the teach with `m3_busy` at best. Operator decision
(2026-09-16): a teach puts the controller in STANDBY, and that STANDBY clears
when the admin logs out or the session times out -- the rule the LCD already
applies to manual window control.

One deliberate difference from the LCD: the teach's STANDBY is HELD, not
persisted. The LCD's STANDBY is written to NVS while the flag that lets its
session end clear it lives only in RAM, so a reboot strands the unit in
STANDBY with nothing left to clear it (gh#65). A held STANDBY and its release
both live in RAM, so a reboot ends them together. Case D is the check for that.

WHAT IT DOES
------------
Every case starts in AUTOMATIC (except C) with M3 at rest, and uses its own
admin session, because the session IS the thing under test.

  A  logout      teach -> STANDBY is on from the start and still on after
                 `done` -> log out -> STANDBY clears within --release-s and a
                 recalibration runs.
  B  timeout     session_timeout_min set to 1 -> teach -> stop using that
                 session (public /api/status only) -> STANDBY clears within
                 60 s + --release-s of the last request. The timeout is put
                 back afterwards.
  C  explicit    STANDBY set on purpose first (/api/mode) -> teach -> log out
                 -> STANDBY is STILL on: it was the operator's, not the
                 teach's. AUTOMATIC is restored afterwards.
  D  reboot      teach -> `done` with STANDBY held -> the unit reboots (a
                 web-asset upload of its own build) with the teach's session
                 still open -> after the boot STANDBY is OFF. A build that
                 persists the hold must FAIL here. The session is abandoned,
                 never logged out: a logout reaches the unit before the reboot
                 and would end the hold itself.

**THE WINDOW MOVES**: four teaches, about eight traverses on the dev rig
(~3 min of movement), plus recalibrations. Do not run it on production.
`--cases` picks a subset, e.g. `--cases A` for a quick check.

FAIL-FIRST
----------
- A build without the hold (before 2026-09-16) must FAIL case A: STANDBY is
  never set.
- A build with `DM_FAILFIRST_PERSIST_STANDBY_HOLD` defined (one line in
  `firmware/src/data_manager/data_manager.cpp`) persists the hold the way the
  LCD persists its STANDBY, and must FAIL case D.

USAGE
-----
    python bin/at_wp_teach_standby.py --host 192.168.20.169
    python bin/at_wp_teach_standby.py --host 192.168.20.169 --cases AD

Exit 0 = pass, 1 = fail, 2 = could not run the test.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import json
import os
import pathlib
import sys
import time
import urllib.request

# The script's own directory is on sys.path, so the shared helpers import as-is.
from at_wp_ramp import Unit, DEFAULT_HOST, DEFAULT_PIN
import ota_push

ACTIVE = ("arming", "traversing", "committing")
POLL_S = 0.5


def public_status(host):
    """/api/status is public: reading it does not renew any session."""
    try:
        with urllib.request.urlopen("http://%s/api/status" % host, timeout=6) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:                                          # noqa: BLE001
        return None


def flags(st):
    return ((st or {}).get("mode") or {}).get("flags") or []


def standby_on(st):
    return "standby" in flags(st)


def m3(st):
    return ((st or {}).get("windows") or {}).get("M3")


def wait_for(host, pred, limit_s, what):
    """Poll the public status until pred(status) holds. Returns (ok, seconds, last)."""
    t0 = time.time()
    st = None
    while time.time() - t0 < limit_s:
        st = public_status(host)
        if st is not None and pred(st):
            return True, time.time() - t0, st
        time.sleep(1.0)
    print("    (timed out after %d s waiting for %s; last flags %s, M3 %s)"
          % (limit_s, what, flags(st), m3(st)))
    return False, time.time() - t0, st


def quiet(st):
    """AUTOMATIC-ready: no safety state, no recalibration, M3 at rest."""
    f = flags(st)
    return ("wind_override" not in f and "motor_alarm" not in f
            and "calibrating" not in f and m3(st) in ("OPEN", "CLOSED"))


def set_mode(host, pin, mode):
    u = Unit(host, pin)
    try:
        sc, body = u._req("POST", "/api/mode", {"mode": mode})
        return sc == 200 and isinstance(body, dict) and body.get("mode") == mode
    finally:
        u.logout()


def teach(u, limit_s, host):
    """Start a teach on session u and follow it to its end.

    Returns (ok, why, standby_at_start). STANDBY is read from the PUBLIC status
    right after the start is accepted: the hold must be in place before the
    first leg can move.
    """
    sc, body = u._req("POST", "/api/diag/commission", {"action": "teach"})
    if sc != 200 or not (isinstance(body, dict) and body.get("ok")):
        return False, "teach refused: HTTP %s %s" % (sc, body), None
    st = public_status(host)
    at_start = standby_on(st)
    print("    teach started; STANDBY at start: %s (flags %s)" % (at_start, flags(st)))
    t0 = time.time()
    c = {}
    last = None
    while time.time() - t0 < limit_s:
        sc, c = u._req("GET", "/api/diag/commission")
        if sc != 200 or not isinstance(c, dict):
            return False, "GET /api/diag/commission -> HTTP %s" % sc, at_start
        key = (c.get("state"), c.get("leg"), c.get("ends"), c.get("standby_held"))
        if key != last:
            print("    t+%5.1fs %-10s leg %s ends %s standby_held %s" % ((time.time() - t0,) + key))
            last = key
        if c.get("state") not in ACTIVE:
            break
        time.sleep(POLL_S)
    if c.get("state") != "done":
        return False, "teach ended %s (reason %s)" % (c.get("state"), c.get("run_reason")), at_start
    return True, "", at_start


def not_held(ok, why):
    """The decisive failure comes first: a teach that ran in AUTOMATIC may then
    also have been disturbed by T6, which is a consequence, not the finding."""
    return "STANDBY was not set when the teach started" + (
        "" if ok else " (and the teach then failed: %s)" % why)


def case_a(a, limit):
    print("\n[A] logout releases the teach's STANDBY")
    u = Unit(a.host, a.pin)
    try:
        ok, why, at_start = teach(u, limit, a.host)
        if at_start is False:
            return False, not_held(ok, why)
        if not ok:
            return False, why
        time.sleep(3.0)
        st = public_status(a.host)
        if not standby_on(st):
            return False, "STANDBY cleared by itself after the teach (flags %s)" % flags(st)
        print("    after done, still logged in: STANDBY on (held)")
    finally:
        u.logout()
    print("    logged out")
    ok, secs, st = wait_for(a.host, lambda s: not standby_on(s), a.release_s, "STANDBY to clear")
    if not ok:
        return False, "STANDBY still on %d s after logout" % a.release_s
    print("    STANDBY cleared %.0f s after logout (flags %s)" % (secs, flags(st)))
    ok, secs, st = wait_for(a.host, lambda s: "calibrating" in flags(s), 15, "the recalibration")
    cal = ok or ((st or {}).get("mode") or {}).get("current") == "WINDOW_CAL"
    if not cal:
        return False, "no recalibration seen after the release"
    print("    recalibration running (as the LCD session end does)")
    return True, ""


def case_b(a, limit):
    print("\n[B] session timeout releases the teach's STANDBY")
    admin = Unit(a.host, a.pin)
    try:
        old = (admin.cfg() or {}).get("session_timeout_min")
        if not old:
            return False, "cannot read session_timeout_min"
        if admin.post_cfg("system", "session_timeout", 1) != 200:
            return False, "cannot set session_timeout_min = 1"
        admin.settle_cfg("session_timeout_min", 1)
    finally:
        admin.logout()
    try:
        u = Unit(a.host, a.pin)          # opened AFTER the change: 60 s timeout
        ok, why, at_start = teach(u, limit, a.host)
        last_use = time.time()
        if at_start is False:
            u.logout()
            return False, not_held(ok, why)
        if not ok:
            u.logout()
            return False, why
        # Walk away: this session is never used again, and never logged out.
        u.cookie = None
        print("    session abandoned; watching the public status only")
        ok, _secs, st = wait_for(a.host, lambda s: not standby_on(s), 60 + a.release_s + 10,
                                 "STANDBY to clear")
        idle = time.time() - last_use
        if not ok:
            return False, "STANDBY still on %.0f s after the session was last used" % idle
        if idle < 55:
            return False, ("STANDBY cleared only %.0f s after the last request -- before the "
                           "60 s timeout could have run out" % idle)
        print("    STANDBY cleared %.0f s after the session was last used (timeout 60 s)" % idle)
        return True, ""
    finally:
        admin = Unit(a.host, a.pin)
        try:
            admin.post_cfg("system", "session_timeout", int(old))
            admin.settle_cfg("session_timeout_min", int(old))
            print("    session_timeout_min restored to %s" % old)
        finally:
            admin.logout()


def case_c(a, limit):
    print("\n[C] an operator's own STANDBY is left alone")
    if not set_mode(a.host, a.pin, "standby"):
        return False, "could not set STANDBY via /api/mode"
    try:
        u = Unit(a.host, a.pin)
        try:
            ok, why, at_start = teach(u, limit, a.host)
            if not ok:
                return False, why
            sc, c = u._req("GET", "/api/diag/commission")
            if isinstance(c, dict) and c.get("standby_held"):
                return False, "the teach claims to hold an operator's STANDBY"
        finally:
            u.logout()
        time.sleep(a.release_s)
        st = public_status(a.host)
        if not standby_on(st):
            return False, "logging out cleared the operator's own STANDBY"
        print("    %d s after logout STANDBY is still on: correct, it was the operator's" % a.release_s)
        return True, ""
    finally:
        set_mode(a.host, a.pin, "automatic")
        print("    AUTOMATIC restored")


def case_d(a, limit):
    print("\n[D] a reboot ends the hold (gh#65 must not repeat)")
    u = Unit(a.host, a.pin)
    rebooting = False
    try:
        ok, why, at_start = teach(u, limit, a.host)
        if at_start is False:
            return False, not_held(ok, why)
        if not ok:
            return False, why
        st = public_status(a.host)
        if not standby_on(st):
            return False, "STANDBY not held before the reboot"
        ver = st["system"]["fw_ver"]
        here = os.path.dirname(os.path.abspath(__file__))
        bin_path = pathlib.Path(a.bin or os.path.join(here, ver, "greenhouse-controller-%s.bin" % ver))
        _b, zip_path, _v = ota_push.derive_artifacts(bin_path)
        if not zip_path.exists():
            return False, "no %s to reboot the unit with" % zip_path
        up0 = st["system"]["uptime_s"]
        print("    rebooting by uploading %s (its own web assets); the teach's session "
              "stays open" % zip_path.name)
        cookie = ota_push.login(a.host, a.pin)
        rebooting = True
        ota_push.post_bytes(a.host, "/api/ota/assets", zip_path.read_bytes(), cookie,
                            "application/zip", timeout=120)
        # The unit extracts for a few seconds before it reboots. If the hold is
        # already gone now, the reboot cannot be what ended it.
        st2 = public_status(a.host)
        if (st2 is not None and st2["system"]["uptime_s"] >= up0
                and not standby_on(st2)):
            return False, ("the hold ended BEFORE the reboot (flags %s), so this run "
                           "cannot show what a reboot does" % flags(st2))
    finally:
        if rebooting:
            # Abandoned, NOT logged out. A logout here reaches the unit before
            # it reboots and releases the hold itself, so the case would pass
            # without testing the reboot at all -- which is exactly what the
            # first version did, until its fail-first run passed (2026-09-16).
            u.cookie = None
        else:
            u.logout()
    ok, _s, st = wait_for(a.host, lambda s: s["system"]["uptime_s"] < up0, 120, "the reboot")
    if not ok:
        return False, "the unit did not reboot"
    ok, _s, st = wait_for(a.host, lambda s: s["system"]["uptime_s"] >= 30, 90, "30 s of uptime")
    print("    back up: uptime %s s, flags %s" % (st["system"]["uptime_s"], flags(st)))
    if standby_on(st):
        return False, ("STANDBY survived the reboot with no session left to release it "
                       "-- the gh#65 trap")
    print("    STANDBY is off after the reboot: the hold ended with the session")
    return True, ""


CASES = {"A": case_a, "B": case_b, "C": case_c, "D": case_d}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--cases", default="ABCD")
    ap.add_argument("--release-s", type=int, default=45,
                    help="how long a release may take after the session ends "
                         "(T17 checks at every reading: every 30 s at rest)")
    ap.add_argument("--bin", help="greenhouse-controller-<ver>.bin whose web assets "
                                  "case D uploads (default: bin/<the unit's fw_ver>/)")
    a = ap.parse_args()

    st = public_status(a.host)
    if st is None:
        print("no answer from %s" % a.host)
        return 2
    sysb = st.get("system") or {}
    print("unit %s  fw %s  mode %s %s  M3 %s"
          % (sysb.get("unit_id"), sysb.get("fw_ver"), (st.get("mode") or {}).get("current"),
             flags(st), m3(st)))
    u = Unit(a.host, a.pin)
    try:
        sc, _c = u._req("GET", "/api/diag/commission")
        if sc != 200:
            print("GET /api/diag/commission -> HTTP %s: not a commissioning build" % sc)
            return 2
        cfg = u.cfg()
        travel = ((cfg.get("travel_s") or [0, 0, 171])[2]) if isinstance(cfg, dict) else 171
    finally:
        u.logout()
    limit = 3 * (travel + 5 + 2) + 30
    print("travel_m3 %s s -> teach limit %d s" % (travel, limit))

    results = []
    for name in a.cases.upper():
        if name not in CASES:
            print("unknown case %s" % name)
            return 2
        if standby_on(public_status(a.host)):
            print("\nSTANDBY is on before case %s -- switching to AUTOMATIC first" % name)
            set_mode(a.host, a.pin, "automatic")
        ok, _s, st = wait_for(a.host, lambda s: quiet(s) and not standby_on(s), limit,
                              "a quiet AUTOMATIC unit with M3 at rest")
        if not ok:
            print("case %s: could not start -- the unit did not settle" % name)
            return 2
        time.sleep(3.0)
        ok, why = CASES[name](a, limit)
        print("  case %s: %s" % (name, "PASS" if ok else "FAIL -- " + why))
        results.append((name, ok))

    print("\n--- verdict ---")
    for name, ok in results:
        print("  %s %s" % (name, "PASS" if ok else "FAIL"))
    passed = all(ok for _n, ok in results)
    print("RESULT: %s" % ("PASS" if passed else "FAIL"))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())

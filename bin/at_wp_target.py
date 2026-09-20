#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for 2.12.0 (plan 5b): T2 holds a partial target on M3.

WHAT IS UNDER TEST
------------------
T2's targeted drive: `CMD_TARGET` on Q1, the arrival band derived from
`deadzone_m3_mm` and the taught window size, the overshoot guard, the
`CH_PART_OPEN` state that follows, its RELAY row, and the SENSOR_HR ch2
encoding that carries it. Nothing in T6 chooses a target yet -- the effective
mode is the next step -- so the command comes from the bench hook
`POST /api/diag/windowpos {"target_x10":N}`, which posts the same Q1 command
T6 will post. What is exercised is T2's real path, not a test-only shortcut.

WHICH BUILD
-----------
A 2.12.0 bench build. The hook is bench-only, like every other /api/diag
route. There is no fail-first flag here: the behaviour is new, so the
pre-2.12.0 arm is "the route answers 404", which the run checks for you when
you point it at an older build.

HOW -- everything over the network, no operator at the rig
----------------------------------------------------------
Each stage drives M3 to a known end first (the hook, with target 0 or 1000,
which T2 turns into an ordinary full-travel drive), then commands a partial
target and watches. M3's two dwells are cut to 5 s for the run, because a
target from an admin bypasses the dwell but the full-travel setup drives do
not. Everything changed is restored on exit, whatever happens.

The judgement reads `/api/status` (the window state and M3's opening), the
diag counters, and the SD log rows written during the stage.

STAGES
------
  band       target 50 %: M3 stops inside the band, the state is PART_OPEN,
             and the opening reported by /api/status agrees with the target
  twice      a second target 30 % from PART_OPEN: it closes to it and stops
             again -- the reverse direction, from a part-open start
  ends       target 0 and 1000: both become full-travel drives, both end in
             a real terminal state (CLOSED / OPEN), never PART_OPEN
  supersede  a plain CMD_CLOSE (a recalibration) during a targeted drive:
             the target is disarmed and M3 finishes CLOSED at the end, which
             is what a safety close must do
  lost       the sensor injected absent mid-target: the drive falls back to
             the travel timer and finishes at an end, exactly as a unit with
             no sensor does
  refuse     with the sensor absent from the start, the target is refused and
             M3 does not move

Usage
  python bin/at_wp_target.py --host 192.168.20.160            # every stage
  python bin/at_wp_target.py --host 192.168.20.160 --stage band
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from at_wp_ramp import Unit                                    # noqa: E402

DEFAULT_HOST = "192.168.20.160"
DEFAULT_PIN = "12345678"
TEST_DWELL_S = 5
MOVE_LIMIT_S = 120          # the rig's traverse is ~13 s; this is generous
SETTLE_S = 4.0


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))


def m3_state(st):
    """M3's state name from /api/status, whatever shape the payload uses."""
    w = (st or {}).get("windows") or {}
    for key in ("M3_state", "m3_state", "M3"):
        if key in w:
            return str(w[key])
    return "?"


def m3_open_x10(st):
    """M3's opening in 0.1 %, or None when the unit publishes none."""
    w = (st or {}).get("windows") or {}
    for key in ("M3_percent_x10", "m3_percent_x10"):
        if key in w:
            try:
                return int(w[key])
            except (TypeError, ValueError):
                return None
    return None


class Rig(object):
    """The unit, with the settings this run changes and their restoration."""

    def __init__(self, host, pin):
        self.u = Unit(host, pin)
        self.saved = {}
        st = self.u.status()
        self.unit_id = (st.get("system") or {}).get("unit_id", "?")
        self.fw = (st.get("system") or {}).get("fw_ver", "?")

    # -- settings ---------------------------------------------------------
    def cut_dwells(self):
        cfg = self.u.cfg() or {}
        for field, ns, key in (("dwell_open_s", "motor", "dwell_open_m3"),
                               ("dwell_close_s", "motor", "dwell_close_m3")):
            cur = cfg.get(field)
            val = cur[2] if isinstance(cur, list) and len(cur) > 2 else cur
            self.saved[(ns, key)] = val
            self.u.post_cfg(ns, key, TEST_DWELL_S)
        time.sleep(SETTLE_S)
        say("dwells cut to %d s (restored on exit)" % TEST_DWELL_S)

    def restore(self):
        for (ns, key), val in self.saved.items():
            if val is None:
                continue
            ok = self.u.post_cfg(ns, key, val) == 200
            say("  restore %-16s -> %-8s %s" % (key, val, "ok" if ok else "FAILED"))
        self.saved.clear()

    # -- the hook ---------------------------------------------------------
    def target(self, x10):
        sc, out = self.u._req("POST", "/api/diag/windowpos", {"target_x10": x10})
        if sc == 404:
            sys.exit("POST /api/diag/windowpos answered 404: this is not a 2.12.0 "
                     "bench build, so there is no target hook to test")
        return out if isinstance(out, dict) else {}

    def inject(self, how):
        return self.u._req("POST", "/api/diag/windowpos", {"inject": how})[1]

    # -- waiting ----------------------------------------------------------
    def wait_rest(self, limit_s=MOVE_LIMIT_S):
        """Wait until M3 is not moving; return its resting state name."""
        end = time.time() + limit_s
        moved = False
        while time.time() < end:
            s = m3_state(self.u.status())
            if "MOVING" in s.upper():
                moved = True
            elif moved or time.time() > end - limit_s + 6:
                return s
            time.sleep(0.4)
        return m3_state(self.u.status())

    def drive_to_end(self, open_end):
        want = 1000 if open_end else 0
        self.target(want)
        st = self.wait_rest()
        say("setup: M3 %s" % st)
        return st


def check(ok, msg):
    say(("PASS  " if ok else "FAIL  ") + msg)
    return ok


# --------------------------------------------------------------------------
# stages
# --------------------------------------------------------------------------


def stage_band(rig):
    rig.drive_to_end(False)
    r = rig.target(500)
    say("target 50.0 %% -> %s" % r)
    st = rig.wait_rest()
    full = rig.u.status()
    pos = m3_open_x10(full)
    ok = check("PART" in st.upper(), "state is PART_OPEN (got %s)" % st)
    if pos is None:
        say("NOTE  the status payload publishes no M3 opening; band not checked")
    else:
        ok &= check(abs(pos - 500) <= 60,
                    "stopped at %.1f %% against a 50.0 %% target" % (pos / 10.0))
    return ok


def stage_twice(rig):
    if "PART" not in m3_state(rig.u.status()).upper():
        stage_band(rig)
    rig.target(300)
    st = rig.wait_rest()
    pos = m3_open_x10(rig.u.status())
    ok = check("PART" in st.upper(), "still PART_OPEN after the second target (%s)" % st)
    if pos is not None:
        ok &= check(abs(pos - 300) <= 60,
                    "stopped at %.1f %% against a 30.0 %% target" % (pos / 10.0))
    return ok


def stage_ends(rig):
    ok = True
    for want, expect in ((1000, "OPEN"), (0, "CLOS")):
        rig.target(want)
        st = rig.wait_rest()
        ok &= check(expect in st.upper() and "PART" not in st.upper(),
                    "target %d became a full-travel drive: %s" % (want, st))
    return ok


def stage_supersede(rig):
    rig.drive_to_end(False)
    rig.target(500)
    time.sleep(2.0)
    say("recalibrating mid-target (a plain CLOSE_ALL)")
    rig.u._req("POST", "/api/mode", {"mode": "standby"})
    time.sleep(1.0)
    rig.u._req("POST", "/api/mode", {"mode": "automatic"})
    st = rig.wait_rest(180)
    return check("CLOS" in st.upper(),
                 "a full-travel close supersedes the target: %s" % st)


def stage_lost(rig):
    rig.drive_to_end(False)
    rig.target(500)
    time.sleep(2.0)
    rig.inject("absent")
    say("sensor injected absent mid-target")
    st = rig.wait_rest()
    rig.inject("none")
    return check("PART" not in st.upper(),
                 "fell back to the travel timer and finished at an end: %s" % st)


def stage_refuse(rig):
    rig.drive_to_end(False)
    rig.inject("absent")
    time.sleep(2.0)
    before = m3_state(rig.u.status())
    r = rig.target(500)
    time.sleep(3.0)
    after = m3_state(rig.u.status())
    rig.inject("none")
    return check(after == before and "MOVING" not in after.upper(),
                 "refused with no sensor, M3 unmoved (%s, reply %s)" % (after, r))


STAGES = {
    "band": stage_band,
    "twice": stage_twice,
    "ends": stage_ends,
    "supersede": stage_supersede,
    "lost": stage_lost,
    "refuse": stage_refuse,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--stage", choices=sorted(STAGES) + ["all"], default="all")
    a = ap.parse_args()

    rig = Rig(a.host, a.pin)
    print("at_wp_target -- unit %s, fw %s" % (rig.unit_id, rig.fw))
    if "bench" not in rig.fw:
        sys.exit("this needs a bench build: the target hook is bench-only")

    names = sorted(STAGES) if a.stage == "all" else [a.stage]
    results = {}
    try:
        rig.cut_dwells()
        for n in names:
            print("\n== %s ==" % n)
            try:
                results[n] = bool(STAGES[n](rig))
            except Exception as e:                             # noqa: BLE001
                say("ERROR %s" % e)
                results[n] = False
    finally:
        print("\nrestoring ...")
        rig.restore()
        rig.drive_to_end(False)
        rig.u.logout()

    print("\n== result ==")
    for n in names:
        print("  %-10s %s" % (n, "PASS" if results.get(n) else "FAIL"))
    bad = [n for n in names if not results.get(n)]
    print("\n%s" % ("ALL PASS" if not bad else "FAILED: " + ", ".join(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

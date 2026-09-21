#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for 2.12.0: rule 1's at-end exemption is judged at the grace expiry.

THE DEFECT THIS EXISTS FOR
--------------------------
Section 12.4 rule 1 ("moving means moving") excuses a drive toward the end the
leaf already sits at: the motor's end switch cuts it and the leaf correctly
does not move. Until 2026-09-21 the excuse needed "bit 3 made AND the position
at the target end" on EVERY sample from the first one.

A mode-2 target can leave M3 at the closed end's POSITION but short of its
switch -- the encoder reads 0 about 1.2 s of travel before the closed end
sensor makes. A CLOSE from there reported a stall that was not one: the first
sample cleared the excuse, the last sub-millimetre of travel produced no rate,
and the switch made two seconds in, too late (2344, 2026-09-20 16:22,
`stall_faults` 1). Since the stranded-M3 fix (c23fa12) T6's ordinary CLOSE
reaches that state, not only a wind override, so a soak would meet it.

The rule now keeps POSITION CONTINUITY (the position never left the target
end's region) and asks for the END SENSOR AT THE VERDICT (bit 3 made at the
grace expiry), instead of both on every sample.

STAGES -- over the network, no operator. M3 must be CLOSED at the start.
------
  closed    control: a recalibration of a closed M3, nothing injected. Must be
            excused (at_end_exempt +1, stall_faults +0) -- the 2026-09-16 case
            the excuse was made for, on both builds.
  headroom  the defect: bit 3 suppressed ("noend"), a recalibration, and bit 3
            released 1.5 s into the drive -- the leaf at 0 mm meeting its
            switch late, as on 2026-09-20. Must be excused. FAILS on the
            fail-first build (-DWPOS_FAILFIRST_212=32 restores the latch).
  noswitch  the fault the excuse must not hide: bit 3 suppressed for the whole
            drive -- a leaf at 0 mm that never reaches its switch. Must be
            reported (stall_faults +1).
  short     the fault the continuity is for: M3 OPEN, a shorted wiper
            ("short": position and rate read 0 from any position), a full
            CLOSE. The position sits "at the target" all along, but the open
            end's switch releases ~1.8 s in and the closed one is 13 s away,
            so nothing is made at the verdict. Must be reported
            (stall_faults +1).

`noswitch` and `short` assert that a fault IS reported, which cannot pass
vacuously; `headroom` asserts an absence, which can, so it carries the
fail-first. Rule 2 and the drive verdict are expected to react too in
`noswitch` and `short` (an early stop, a drive not reached): those are printed,
not judged -- this test is about rule 1.

HOW
---
A recalibration is what drives a CLOSED M3 (T2 ignores a plain CLOSE there):
the script holds STANDBY and leaves it. The recalibration re-arms M3's close
dwell (300 s on the rig), so T6 cannot move M3 between stages. The drive's
start is taken from /api/status (M3 MOVING_CLOSE), which touches no bus --
polling the diag route would add Modbus reads beside T17's own.

Everything is cleared on exit: the injection, and STANDBY (leaving it once
more if needed, which is one more harmless recalibration).

Usage
  python bin/at_wp_rule1.py --host 192.168.20.160
  python bin/at_wp_rule1.py --host 192.168.20.160 --stage headroom
Exit 0 = PASS, 1 = FAIL, 2 = could not run or could not judge.
"""
import argparse
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import at_wp_ramp                                               # noqa: E402
from at_wp_ramp import Unit                                     # noqa: E402
from at_wp09 import soak_of, gate_of, m3_of, flags_of, delta   # noqa: E402
from at_wp_teach_standby import public_status                   # noqa: E402

at_wp_ramp.HTTP_TIMEOUT_S = 30

PASS, FAIL, INCONCLUSIVE = 0, 1, 2
WORD = {PASS: "PASS", FAIL: "FAIL", INCONCLUSIVE: "INCONCLUSIVE"}
RELEASE_AFTER_S = 1.5     # headroom: well inside the 5 s grace and rule 2's 3.25 s
POLL_S = 0.15
PRINTED = ("strokes", "at_end_exempt", "stall_faults", "early_stops",
           "confirmed", "not_reached", "not_judged")


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


class Rig(object):
    def __init__(self, host, pin):
        self.u = Unit(host, pin)
        g = gate_of(self.u.diag() or {})
        if "inject" not in g:
            sys.exit("GET /api/diag/windowpos has no gate.inject -- not a bench build")
        ff = g.get("failfirst_212")
        self.ff = ff if isinstance(ff, int) and not isinstance(ff, bool) else None
        self.failfirst = bool(ff)
        atexit.register(self.cleanup)

    def status(self):
        return public_status(self.u.host) or {}

    def counters(self):
        return soak_of(self.u.diag() or {})

    def inject(self, how):
        sc, out = self.u._req("POST", "/api/diag/windowpos", {"inject": how})
        if sc != 200 or not (isinstance(out, dict) and out.get("ok")):
            sys.exit("injection '%s' refused: HTTP %s %s -- this needs a 2.12.0 "
                     "bench build with the rule-1 injections" % (how, sc, out))
        say("injection -> %s" % how)

    def mode(self, standby):
        self.u._req("POST", "/api/mode", {"mode": "standby" if standby else "automatic"})

    def hook(self, x10):
        sc, out = self.u._req("POST", "/api/diag/windowpos", {"target_x10": x10})
        if sc == 404:
            sys.exit("no target hook (404): this needs a 2.12.0 bench build")

    def cleanup(self):
        try:
            self.u._req("POST", "/api/diag/windowpos", {"inject": "none"})
            if "standby" in flags_of(self.status()):
                self.mode(False)
        except Exception:                                      # noqa: BLE001
            pass


def wait_state(rig, pred, limit_s):
    """(state, seconds) once pred(M3) holds, else (last state, None)."""
    t0 = time.time()
    m = None
    while time.time() - t0 < limit_s:
        st = rig.status()
        m = m3_of(st)
        if pred(m, st):
            return m, time.time() - t0
        time.sleep(POLL_S)
    return m, None


def settled(m, st):
    return m in ("OPEN", "CLOSED") and "calibrating" not in flags_of(st)


def recal_drive(rig, release_after=None):
    """Leave STANDBY -- T2's recalibration drives M3 CLOSE -- and return the
    counter deltas once the unit has settled. With `release_after`, the
    injection is cleared that many seconds after the drive is seen to start."""
    rig.mode(True)
    time.sleep(2.0)
    c0 = rig.counters()
    rig.mode(False)
    m, secs = wait_state(rig, lambda m, st: m == "MOVING_CLOSE", 20)
    if secs is None:
        say("the recalibration never drove M3 (%s) -- inconclusive" % m)
        return None
    seen = time.time()
    say("M3 MOVING_CLOSE: the recalibration's drive has started")
    if release_after is not None:
        time.sleep(max(0.0, release_after - (time.time() - seen)))
        rig.inject("none")
        say("bit 3 released %.1f s into the drive" % (time.time() - seen))
    m, secs = wait_state(rig, settled, 90)
    if secs is None:
        say("the unit did not settle (%s) -- inconclusive" % m)
        return None
    time.sleep(2.0)
    c1 = rig.counters()
    d = dict((k, delta(c1, c0, k)) for k in PRINTED)
    say("  ".join("%s %+d" % (k, d[k]) for k in PRINTED))
    return d


def judge(ok, what_pass, what_fail):
    say(("PASS  " + what_pass) if ok else ("FAIL  " + what_fail))
    return PASS if ok else FAIL


def need_closed(rig):
    m = m3_of(rig.status())
    if m == "CLOSED":
        return True
    say("M3 is %s, not CLOSED -- closing it first" % m)
    rig.mode(True)
    time.sleep(2.0)
    rig.hook(0)
    m, secs = wait_state(rig, lambda m, st: m == "CLOSED", 40)
    return secs is not None


# --------------------------------------------------------------------------
# stages
# --------------------------------------------------------------------------


def stage_closed(rig):
    if not need_closed(rig):
        return INCONCLUSIVE
    d = recal_drive(rig)
    if d is None:
        return INCONCLUSIVE
    return judge(d["at_end_exempt"] == 1 and d["stall_faults"] == 0,
                 "a closed M3's recalibration is excused",
                 "a closed M3's recalibration was not excused (the control)")


def stage_headroom(rig):
    if not need_closed(rig):
        return INCONCLUSIVE
    rig.mode(True)
    time.sleep(2.0)
    rig.inject("noend")
    time.sleep(1.0)
    d = recal_drive(rig, release_after=RELEASE_AFTER_S)
    rig.inject("none")
    if d is None:
        return INCONCLUSIVE
    return judge(d["at_end_exempt"] == 1 and d["stall_faults"] == 0,
                 "a drive that met its switch late is excused",
                 "a drive that met its switch late was reported as a stall "
                 "(stall_faults %+d, at_end_exempt %+d)"
                 % (d["stall_faults"], d["at_end_exempt"]))


def stage_noswitch(rig):
    if not need_closed(rig):
        return INCONCLUSIVE
    rig.mode(True)
    time.sleep(2.0)
    rig.inject("noend")
    time.sleep(1.0)
    d = recal_drive(rig)
    rig.inject("none")
    if d is None:
        return INCONCLUSIVE
    return judge(d["stall_faults"] == 1 and d["at_end_exempt"] == 0,
                 "a leaf that never met its switch is reported",
                 "a leaf that never met its switch was not reported "
                 "(stall_faults %+d, at_end_exempt %+d)"
                 % (d["stall_faults"], d["at_end_exempt"]))


def stage_short(rig):
    if not need_closed(rig):
        return INCONCLUSIVE
    rig.mode(True)
    time.sleep(2.0)
    rig.hook(1000)                                   # a full-travel OPEN
    m, secs = wait_state(rig, lambda m, st: m == "OPEN", 40)
    if secs is None:
        say("M3 did not open (%s) -- inconclusive" % m)
        return INCONCLUSIVE
    time.sleep(2.0)
    rig.inject("short")
    time.sleep(1.0)
    d = recal_drive(rig)                             # the recalibration closes it
    rig.inject("none")
    if d is None:
        return INCONCLUSIVE
    return judge(d["stall_faults"] == 1 and d["at_end_exempt"] == 0,
                 "a shorted wiper closing from the open end is reported",
                 "a shorted wiper closing from the open end was not reported "
                 "(stall_faults %+d, at_end_exempt %+d)"
                 % (d["stall_faults"], d["at_end_exempt"]))


STAGES = [("closed", stage_closed), ("headroom", stage_headroom),
          ("noswitch", stage_noswitch), ("short", stage_short)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="192.168.20.160")
    ap.add_argument("--pin", default="12345678")
    ap.add_argument("--stage", choices=[n for n, _ in STAGES] + ["all"], default="all")
    a = ap.parse_args()

    rig = Rig(a.host, a.pin)
    st = rig.status()
    sysb = st.get("system") or {}
    g = gate_of(rig.u.diag() or {})
    print("at_wp_rule1 -- unit %s, fw %s, failfirst_212 %s, gate %s"
          % (sysb.get("unit_id"), sysb.get("fw_ver"), g.get("failfirst_212"),
             g.get("reason_str")))
    if rig.failfirst:
        if rig.ff is not None and rig.ff & 32:
            print("  *** fail-first bit 32: rule 1's exemption is LATCHED again.")
            print("  *** `headroom` MUST fail here; the others must still pass.")
        else:
            print("  *** a fail-first build without bit 32: not what this test is for.")
    if str(g.get("reason_str")) != "ok":
        print("the sensor gate is not ok (%s) -- not run" % g.get("reason_str"))
        return INCONCLUSIVE
    for bad in ("wind_override", "motor_alarm", "calibrating"):
        if bad in flags_of(st):
            print("the unit shows '%s' -- clear it first" % bad)
            return INCONCLUSIVE

    names = [n for n, _ in STAGES] if a.stage == "all" else [a.stage]
    results = {}
    try:
        for n, fn in STAGES:
            if n not in names:
                continue
            print("\n== %s ==" % n)
            results[n] = fn(rig)
    finally:
        print("\nleaving the rig as found ...")
        rig.cleanup()
        # `noswitch` leaves the verdict "not confirmed"; one clean
        # recalibration of the closed M3 confirms it again.
        if "noswitch" in results or "short" in results:
            try:
                if need_closed(rig):
                    recal_drive(rig)
            except Exception as e:                             # noqa: BLE001
                say("final recalibration failed: %s" % e)

    print("\n== result ==")
    for n in names:
        print("  %-9s %s" % (n, WORD.get(results.get(n), "not run")))
    bad = [n for n in names if results.get(n) == FAIL]
    inc = [n for n in names if results.get(n) in (INCONCLUSIVE, None)]
    print("\n%s" % ("ALL PASS" if not bad and not inc else
                    ("FAILED: " + ", ".join(bad)) if bad else
                    "INCONCLUSIVE: " + ", ".join(inc)))
    return FAIL if bad else (INCONCLUSIVE if inc else PASS)


if __name__ == "__main__":
    sys.exit(main())

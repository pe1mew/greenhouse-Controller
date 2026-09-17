#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#72: every drive of M3 is judged, and the sensor gate does not lie.

WHAT IT CHECKS
--------------
gh#72 changed four things in T17, and this test has one stage for each. Every
stage says PASS only on a normal bench build; on a build made with
`-DWPOS_FAILFIRST_GH72`, which restores the old behaviour, the same stage must
FAIL. Run it on both. `GET /api/diag/windowpos` reports which build it is
(`gate.failfirst_gh72`), and the verdict says so.

  flap      No operator. A sensor that answers but reports its own fault must
            keep the gate shut. Before gh#72 the 30 s re-probe checked only the
            device's identity and re-opened the gate, so the gate read "ok" most
            of the time while the fault lasted.
  stale     LCD strokes. The sensor vanishes 1.5 s into a CLOSE and comes back
            2 s into the next OPEN. The OPEN must be counted as a fresh judged
            drive (`strokes` +1), and the control mode must not be promoted in
            the middle of it. Before gh#72 a shut gate kept the CLOSE, so the
            OPEN was not counted at all.
  latejoin  LCD strokes. A drive T17 joins late must be timed from T2's
            start. Seen on 2344 (2026-09-17) after a power cycle with M3 OPEN:
            T17 came up during T2's boot recalibration, timed the CLOSE from
            its first look, and rule 2 reported a false early stop. How late
            T17 joins at boot varies (5.5 s then, 2.5 s in a later try that
            did not trip), so the stage makes the late join on purpose: the
            sensor is made absent at rest, the CLOSE starts unseen, and the
            gate re-opens 7 s into it. No early stop and no stall may follow.
  reversal  LCD strokes. A CLOSE reversed to OPEN is two drives, and each must
            be judged. With the reading stuck (a shorted wiper, simulated), both
            must report a stall (`stall_faults` +2); before gh#72 only the
            first one did (+1). Then the same keys with nothing injected: no
            stall at all, because the reversal gap must not read as one.

HOW THE FAULTS ARE MADE
-----------------------
Through the bench test hook `POST /api/diag/windowpos {"inject": ...}`. It
changes only what T17 reads: "absent" (every read times out), "fault" (the
device's wiper-open fault) and "stuck" (the position frozen, rate 0, end
sensors real). Nothing is unplugged or detached. The injection is RAM only,
and this script clears it on every exit.

THE LCD STAGES
--------------
They need the LCD manual menu for M3 (screen 6, `#`, the admin PIN, `3`). Its
session holds STANDBY, so climate control does not move the windows during the
test. The script says which key to press:
  stale     press 2 (Close); then, when the script says M3 is CLOSED, press 1
            (Open). Not when the leaf looks closed: T2 drives on for its 5 s
            margin and still reports the stroke, so an earlier press is a
            reversal, and the stage stops as INCONCLUSIVE.
  latejoin  press 1 (Open) if M3 is not OPEN; wait about a minute while the
            gate shuts; press 2 (Close) when told, then nothing more.
  reversal  M3 must start OPEN. Press 2 (Close), count slowly to eight, then
            press 1 (Open) -- twice, once with the fault and once without.
The count matters: the CLOSE must run past rule 1's grace, min(5 s,
travel_m3 / 2), or its verdict is never reached, and it must be reversed before
T2 stops it (travel_m3 + 5 s).

USAGE
-----
    python bin/at_wp_gh72.py --host 192.168.20.160 flap
    python bin/at_wp_gh72.py --host 192.168.20.160 stale
    python bin/at_wp_gh72.py --host 192.168.20.160 reversal
    python bin/at_wp_gh72.py --host 192.168.20.160 latejoin

Exit 0 = PASS, 1 = FAIL, 2 = could not run or could not judge.
Stdlib only, ASCII output.
"""

import argparse
import atexit
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from at_wp09 import (Unit, DEFAULT_PIN, LCD_MENU, LOG_OUT,       # noqa: E402
                     soak_of, gate_of, m3_of, flags_of, delta)
from at_wp_teach_standby import public_status                     # noqa: E402

PASS, FAIL, INCONCLUSIVE = 0, 1, 2
WORD = {PASS: "PASS", FAIL: "FAIL", INCONCLUSIVE: "INCONCLUSIVE"}
POLL_S = 0.25
FLAP_WATCH_S = 100       # > three 30 s probe cycles
REJOIN_AFTER_S = 7.0     # latejoin: well past the ~5.5 s the old timing needs to trip
SETTLED = ("OPEN", "CLOSED")


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def banner(lines):
    print("")
    print("  " + "=" * 72)
    for ln in lines:
        print("  >>> " + ln)
    print("  " + "=" * 72)
    sys.stdout.flush()


class Rig(object):
    """The unit, with the test hook, and a guarantee the hook is cleared."""

    def __init__(self, host, pin):
        self.u = Unit(host, pin)
        d = self.u.diag()
        g = gate_of(d)
        if "inject" not in g:
            sys.exit("GET /api/diag/windowpos has no gate.inject -- this is not a "
                     "gh#72 bench build")
        self.failfirst = bool(g.get("failfirst_gh72"))
        atexit.register(self.clear)

    def inject(self, how):
        sc, out = self.u._req("POST", "/api/diag/windowpos", {"inject": how})
        if sc != 200 or not (isinstance(out, dict) and out.get("ok")):
            sys.exit("injection '%s' refused: HTTP %s %s" % (how, sc, out))
        say("injection -> %s" % how)

    def clear(self):
        try:
            self.u._req("POST", "/api/diag/windowpos", {"inject": "none"})
        except Exception:                                      # noqa: BLE001
            pass

    def diag(self):
        return self.u.diag()

    def status(self):
        # The public route: faster than the admin session, and it renews
        # nothing, so a short CLOSED between two presses is not missed.
        return public_status(self.u.host) or {}

    def travel_s(self):
        sc, cfg = self.u._req("GET", "/api/config")
        t = (cfg.get("travel_s") or [0, 0, 0])[2] if isinstance(cfg, dict) else 0
        return int(t or 0)


def preflight(rig, need_lcd):
    st = rig.status()
    sysb = st.get("system") or {}
    d = rig.diag()
    g = gate_of(d)
    s = soak_of(d)
    print("unit %s  fw %s  build: %s" % (sysb.get("unit_id", "?"), sysb.get("fw_ver", "?"),
          "FAIL-FIRST (WPOS_FAILFIRST_GH72)" if rig.failfirst else "normal"))
    print("gate %s/%s, inject %s, M3 %s, strokes %s, redrives %s, stall_faults %s"
          % (g.get("mode_str"), g.get("reason_str"), g.get("inject"), m3_of(st),
             s.get("strokes"), s.get("redrives"), s.get("stall_faults")))
    if g.get("inject") != "none":
        rig.clear()
        time.sleep(2)
    for bad in ("wind_override", "motor_alarm", "calibrating"):
        if bad in flags_of(st):
            print("the unit shows '%s' -- clear it first" % bad)
            return False
    if str(g.get("reason_str")) != "ok":
        print("the sensor gate is not ok (%s) -- fit the sensor and switch it on first"
              % g.get("reason_str"))
        return False
    if m3_of(st) not in SETTLED:
        print("M3 is not at rest -- wait for it")
        return False
    if need_lcd and "standby" not in flags_of(st):
        banner(LCD_MENU)
        t0 = time.time()
        while time.time() - t0 < 600 and "standby" not in flags_of(rig.status()):
            time.sleep(1.0)
        if "standby" not in flags_of(rig.status()):
            print("the LCD menu's STANDBY did not appear within 10 min -- not run")
            return False
        say("the LCD menu holds STANDBY: automatic control is paused")
    return True


def wait_m3(rig, pred, limit_s):
    """(state, seconds) when pred(M3 state) first holds, or (last state, None)."""
    t0 = time.time()
    m = None
    while time.time() - t0 < limit_s:
        m = m3_of(rig.status())
        if pred(m):
            return m, time.time() - t0
        time.sleep(POLL_S)
    return m, None


def verdict(code, why, rig):
    tag = " [on the FAIL-FIRST build]" if rig.failfirst else ""
    print("\n%s%s: %s" % (WORD[code], tag, why))
    return code


# ------------------------------------------------------------------ flap
def stage_flap(rig):
    if not preflight(rig, need_lcd=False):
        return INCONCLUSIVE
    s0 = soak_of(rig.diag())
    rig.inject("fault")
    t0 = time.time()
    first_fault = None
    ok_after = 0
    samples = 0
    changes = 0
    last = None
    while time.time() - t0 < FLAP_WATCH_S:
        time.sleep(1.0)
        r = str(gate_of(rig.diag()).get("reason_str"))
        if r != last:
            say("gate reason -> %s" % r)
            changes += 1
            last = r
        if first_fault is None and r == "device_fault":
            first_fault = time.time() - t0
        if first_fault is not None:
            samples += 1
            if r == "ok":
                ok_after += 1
    s1 = soak_of(rig.diag())
    rig.inject("none")
    back = None
    for _ in range(20):
        time.sleep(0.5)
        if str(gate_of(rig.diag()).get("reason_str")) == "ok":
            back = True
            break
    pf = delta(s1, s0, "probe_fail")
    say("fault seen %s; after it, %d of %d samples read ok; probe_fail +%d; "
        "cleared -> gate ok again: %s"
        % ("%.0f s after injecting" % first_fault if first_fault is not None else "never",
           ok_after, samples, pf, "yes" if back else "NO"))
    if first_fault is None:
        return verdict(INCONCLUSIVE, "T17 never reported the injected fault", rig)
    if ok_after > 0 or pf > 1:
        return verdict(FAIL, "the gate re-opened on a sensor that still reports its own "
                             "fault (%d ok samples, probe_fail +%d)" % (ok_after, pf), rig)
    if not back:
        return verdict(FAIL, "the gate did not re-open within 10 s of clearing the fault", rig)
    return verdict(PASS, "the gate stayed shut for the whole fault and re-opened when it "
                         "cleared", rig)


# ----------------------------------------------------------------- stale
def stage_stale(rig):
    if not preflight(rig, need_lcd=True):
        return INCONCLUSIVE
    if m3_of(rig.status()) != "OPEN":
        banner(["M3 must start OPEN: press 1 (Open) on the LCD."])
        m, _ = wait_m3(rig, lambda m: m == "OPEN", 120)
        if m != "OPEN":
            return verdict(INCONCLUSIVE, "M3 did not reach OPEN", rig)
        time.sleep(2.0)

    banner(["Press 2 (Close) on the LCD, then WAIT: the next key comes when",
            "the controller reports M3 CLOSED (about 5 s after the leaf stops)."])
    m, _ = wait_m3(rig, lambda m: m == "MOVING_CLOSE", 300)
    if m != "MOVING_CLOSE":
        return verdict(INCONCLUSIVE, "no CLOSE was seen", rig)
    time.sleep(1.5)
    rig.inject("absent")
    m, _ = wait_m3(rig, lambda m: m in ("CLOSED", "MOVING_OPEN"), 120)
    if m == "MOVING_OPEN":
        return verdict(INCONCLUSIVE, "the CLOSE was reversed before the controller "
                                     "reported CLOSED; press 1 only when told", rig)
    d = rig.diag()
    say("CLOSE ended (%s); gate %s" % (m, gate_of(d).get("reason_str")))
    banner(["M3 is CLOSED now. Press 1 (Open) on the LCD."])
    if str(gate_of(d).get("reason_str")) != "no_sensor":
        return verdict(INCONCLUSIVE, "the gate did not shut on the vanished sensor", rig)
    s_mid = soak_of(d)

    m, _ = wait_m3(rig, lambda m: m == "MOVING_OPEN", 300)
    if m != "MOVING_OPEN":
        return verdict(INCONCLUSIVE, "no OPEN was seen", rig)
    time.sleep(2.0)
    rig.inject("none")
    modes = set()
    reopened = False
    t0 = time.time()
    while time.time() - t0 < 120:
        d = rig.diag()
        g = gate_of(d)
        if str(g.get("reason_str")) == "ok":
            reopened = True
        m = m3_of(rig.status())
        if m == "OPEN":
            break
        modes.add(str(g.get("mode_str")))
        time.sleep(POLL_S)
    time.sleep(2.0)
    s_end = soak_of(rig.diag())
    counted = delta(s_end, s_mid, "strokes")
    say("OPEN ended: gate re-opened mid-stroke: %s; modes seen during it: %s; "
        "strokes +%d, stall_faults +%d"
        % ("yes" if reopened else "no", ", ".join(sorted(modes)), counted,
           delta(s_end, s_mid, "stall_faults")))
    if not reopened:
        return verdict(INCONCLUSIVE, "the sensor did not come back during the OPEN", rig)
    if "position" in modes:
        return verdict(FAIL, "the mode was promoted in the middle of a stroke", rig)
    if counted != 1:
        return verdict(FAIL, "the OPEN was not counted as a fresh drive (strokes +%d): "
                             "the shut gate kept the CLOSE" % counted, rig)
    return verdict(PASS, "the OPEN was judged afresh from the moment the sensor came "
                         "back, and the mode was not promoted mid-stroke", rig)


# -------------------------------------------------------------- reversal
def one_reversal(rig, label, grace_s, inject):
    if m3_of(rig.status()) != "OPEN":
        banner(["M3 must start OPEN: press 1 (Open) on the LCD."])
        m, _ = wait_m3(rig, lambda m: m == "OPEN", 120)
        if m != "OPEN":
            return INCONCLUSIVE, "M3 did not reach OPEN", {}
        time.sleep(2.0)
    s0 = soak_of(rig.diag())
    if inject:
        rig.inject("stuck")
    banner(["%s:" % label,
            "press 2 (Close), count slowly to eight, then press 1 (Open)."])
    m, _ = wait_m3(rig, lambda m: m == "MOVING_CLOSE", 300)
    if m != "MOVING_CLOSE":
        return INCONCLUSIVE, "no CLOSE was seen", {}
    t_close = time.time()
    t_flip = None
    rested = False
    while time.time() - t_close < 90:
        m = m3_of(rig.status())
        if t_flip is None and m == "MOVING_OPEN":
            t_flip = time.time()
        if m in SETTLED:
            rested = (t_flip is None)
            if m == "OPEN" or rested:
                break
        time.sleep(POLL_S)
    time.sleep(2.0)
    s1 = soak_of(rig.diag())
    if inject:
        rig.inject("none")
    k = dict((key, delta(s1, s0, key)) for key in
             ("strokes", "redrives", "stall_faults", "at_end_exempt", "early_stops"))
    rev = (t_flip - t_close) if t_flip is not None else None
    say("%s: reversed %s; strokes +%d, redrives +%d, stall_faults +%d, "
        "at_end_exempt +%d" % (label, "%.1f s into the CLOSE" % rev if rev else "NOT",
                               k["strokes"], k["redrives"], k["stall_faults"],
                               k["at_end_exempt"]))
    if rested or rev is None:
        return INCONCLUSIVE, "M3 stopped before the reversal: press 1 sooner", k
    if rev < grace_s + 0.5:
        return INCONCLUSIVE, ("reversed %.1f s in, before the CLOSE could be judged "
                              "(grace %.1f s): count more slowly" % (rev, grace_s)), k
    if inject:
        if k["stall_faults"] >= 2:
            return PASS, "both drives reported the stuck reading", k
        return FAIL, ("only %d of the two drives reported the stuck reading: the "
                      "reversal was judged as one drive" % k["stall_faults"]), k
    if k["stall_faults"] > 0:
        return FAIL, "a healthy reversal reported a stall", k
    if k["redrives"] < 1 and not rig.failfirst:
        return FAIL, "the second drive was not recognised (redrives +0)", k
    return PASS, "no stall on a healthy reversal", k


def stage_reversal(rig):
    if not preflight(rig, need_lcd=True):
        return INCONCLUSIVE
    t = rig.travel_s()
    grace = min(5.0, t / 2.0) if t else 5.0
    say("travel_m3 %d s: reverse between %.0f s and %d s into the CLOSE" % (t, grace + 1, t + 4))
    results = []
    for label, inject in (("A, reading stuck", True), ("B, healthy", False)):
        code, why, _k = one_reversal(rig, label, grace, inject)
        print("  %s: %s -- %s" % (label, WORD[code], why))
        results.append(code)
    banner(LOG_OUT)
    if FAIL in results:
        return verdict(FAIL, "see the steps above", rig)
    if INCONCLUSIVE in results:
        return verdict(INCONCLUSIVE, "see the steps above", rig)
    return verdict(PASS, "each drive of a reversal was judged on its own, and the gap "
                         "did not read as a stall", rig)



# -------------------------------------------------------------- latejoin
def stage_latejoin(rig):
    """A drive T17 joins late is timed from T2's start (gh#72, 2026-09-17).

    The field case is a power cycle with M3 OPEN: T2's boot recalibration is
    already closing when T17 comes up. How late T17 joins depends on boot
    timing (about 5.5 s at 17:44, about 2.5 s at 18:41 on 2344), and rule 2
    only false-trips on the old timing when the join is late enough, so a
    power cycle cannot be a fail-first test. This stage makes the late join
    on purpose, through the same code: the gate is shut at rest, the CLOSE
    starts unseen, and the gate re-opens REJOIN_AFTER_S into the drive.

    The injection must go in at REST, after T17 has seen the OPEN end. Shut
    mid-stroke instead, the fail-first build keeps the old stroke through the
    shut gate, joins nothing, and the stage could not fail.
    """
    if not preflight(rig, need_lcd=True):
        return INCONCLUSIVE
    travel = rig.travel_s()
    if travel < 12:
        return verdict(INCONCLUSIVE, "travel_m3 is %d s; this stage needs >= 12 s so the "
                                     "rejoin comes well before the leaf reaches 0" % travel, rig)
    if m3_of(rig.status()) != "OPEN":
        banner(["Press 1 (Open) on the LCD. Then wait for the next key: about a minute."])
        m, _ = wait_m3(rig, lambda m: m == "OPEN", 120)
        if m != "OPEN":
            return verdict(INCONCLUSIVE, "M3 did not reach OPEN", rig)
    time.sleep(2.0)                    # T17 sees the stroke end at rest
    s0 = soak_of(rig.diag())
    rig.inject("absent")
    say("waiting for the gate to shut (two failed idle reads, 30 s apart)")
    t0 = time.time()
    while str(gate_of(rig.diag()).get("reason_str")) != "no_sensor":
        if time.time() - t0 > 90:
            return verdict(INCONCLUSIVE, "the gate did not shut within 90 s", rig)
        time.sleep(1.0)
    say("gate shut after %.0f s" % (time.time() - t0))

    banner(["Press 2 (Close) on the LCD now. Then press nothing until the end."])
    m, _ = wait_m3(rig, lambda m: m != "OPEN", 300)
    if m != "MOVING_CLOSE":
        return verdict(INCONCLUSIVE, "no CLOSE was seen (M3 %s)" % m, rig)
    t_start = time.time()
    time.sleep(REJOIN_AFTER_S)
    rig.clear()                        # a shut gate probes at once
    say("injection cleared %.1f s into the CLOSE" % (time.time() - t_start))
    rejoined_s = None
    rejoin_mm = None
    while time.time() - t_start < travel + 10:
        d = rig.diag()
        if str(gate_of(d).get("reason_str")) == "ok":
            m = m3_of(rig.status())
            if m == "MOVING_CLOSE":
                rejoined_s = time.time() - t_start
                # The GET's own direct read: the device as it is, injection or not.
                if isinstance(d.get("opening_mm_x10"), int):
                    rejoin_mm = d["opening_mm_x10"] // 10
            break
        time.sleep(POLL_S)
    m, _ = wait_m3(rig, lambda m: m in ("CLOSED", "MOVING_OPEN"), travel + 15)
    if m != "CLOSED":
        return verdict(INCONCLUSIVE, "the CLOSE did not end CLOSED (M3 %s)" % m, rig)
    time.sleep(3.0)
    s1 = soak_of(rig.diag())
    early = delta(s1, s0, "early_stops")
    stall = delta(s1, s0, "stall_faults")
    judged = delta(s1, s0, "strokes")
    say("gate re-opened %s; leaf then at %s mm; strokes +%d, early_stops +%d, "
        "stall_faults +%d" % ("%.1f s into the CLOSE" % rejoined_s if rejoined_s is not None
                              else "NOT during the CLOSE", rejoin_mm, judged, early, stall))
    if rejoined_s is None:
        return verdict(INCONCLUSIVE, "the gate did not re-open while M3 was closing", rig)
    if judged < 1:
        return verdict(INCONCLUSIVE, "T17 did not judge the drive it rejoined", rig)
    if early > 0 or stall > 0:
        return verdict(FAIL, "the rejoined CLOSE was reported as a fault (early_stops +%d, "
                             "stall_faults +%d): it was timed from the rejoin, not from "
                             "T2's start" % (early, stall), rig)
    return verdict(PASS, "the rejoined CLOSE was timed from T2's start: no false fault", rig)

STAGES = {"flap": stage_flap, "stale": stage_stale, "reversal": stage_reversal,
          "latejoin": stage_latejoin}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stage", choices=sorted(STAGES))
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    a = ap.parse_args()
    print("AT gh#72 -- stage '%s' on %s" % (a.stage, a.host))
    rig = Rig(a.host, a.pin)
    try:
        return STAGES[a.stage](rig)
    except KeyboardInterrupt:
        print("\ninterrupted")
        return INCONCLUSIVE
    finally:
        rig.clear()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT-WP02 and AT-WP03 (design/windowPositionSensorRequirements.MD §"Acceptance").

AT-WP02  Resolution and repeatability: drive to the SAME target ten times, from
         both directions. Pass: the spread is within +/-1 % of stroke, and the
         directional hysteresis is quantified rather than assumed.
AT-WP03  Endpoint agreement: a full close and a full open agree with the
         mechanical end-stops, and "closed" is distinguishable from "nearly
         closed".

These two have been the acceptance of the position sensor since the
requirements were written, and until 2.12.0 they could not run: nothing could
ask M3 to stop part-way. `POST /api/diag/windowpos {"target_x10":N}` (bench
only) is what makes them runnable, and it posts the same Q1 command T6 posts.

WHY BOTH DIRECTIONS
-------------------
The M3 flap is raised against gravity to close and paid out to open, so rope
tension differs by direction (requirements §3). Backlash and slack therefore
behave differently each way, and a repeatability figure taken from one
direction would hide exactly the asymmetry this test exists to find. Each
approach here is preceded by a drive to an END, alternating, so five arrive
from below and five from above.

READING THE RESULT
------------------
- **spread** is max - min over the ten resting positions, in % of stroke. The
  requirement is +/-1 %, i.e. a spread of 2 % or less.
- **hysteresis** is the mean of the from-below approaches minus the mean of the
  from-above ones. A number near zero means direction does not matter; a large
  one is mechanical, not electrical, and belongs in the commissioning notes.
- The **deadband** in force is printed with the result: a spread cannot be
  smaller than the band the controller stops within, so a band wider than 2 %
  makes the requirement unmeasurable rather than met. On the rig 20 mm over a
  1500 mm window is 1.3 %.

Usage
  python bin/at_wp02.py --host 192.168.20.160                  # both tests
  python bin/at_wp02.py --host 192.168.20.160 --target 500 -n 10
  python bin/at_wp02.py --host 192.168.20.160 --only wp03
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
MOVE_LIMIT_S = 180
SETTLE_S = 2.0


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))


class Rig(object):
    def __init__(self, host, pin):
        self.u = Unit(host, pin)
        st = self.u.status()
        sysb = st.get("system") or {}
        self.unit_id = sysb.get("unit_id", "?")
        self.fw = sysb.get("fw_ver", "?")
        self.saved = {}

    # -- settings ---------------------------------------------------------
    def cut_dwells(self):
        cfg = self.u.cfg() or {}
        for field, key in (("dwell_open_s", "dwell_open_m3"),
                           ("dwell_close_s", "dwell_close_m3")):
            cur = cfg.get(field)
            val = cur[2] if isinstance(cur, list) and len(cur) > 2 else cur
            self.saved[key] = val
            self.u.post_cfg("motor", key, TEST_DWELL_S)
        self.saved["min_intv_m3"] = cfg.get("min_intv_m3")
        self.u.post_cfg("motor", "min_intv_m3", 0)
        time.sleep(SETTLE_S)
        say("dwells cut to %d s, minimum interval 0 (restored on exit)" % TEST_DWELL_S)

    def restore(self):
        for key, val in self.saved.items():
            if val is None:
                continue
            ok = self.u.post_cfg("motor", key, val) == 200
            say("  restore %-16s -> %-6s %s" % (key, val, "ok" if ok else "FAILED"))
        self.saved.clear()

    def deadband_pct(self):
        """The arrival band in %, as the controller computes it."""
        cfg = self.u.cfg() or {}
        mm = cfg.get("deadzone_m3_mm")
        d = self.u.diag() or {}
        win_mm = ((d.get("commission") or {}).get("window_mm")
                  or (d.get("cal") or {}).get("window_mm"))
        if not mm or not win_mm:
            return None
        return 100.0 * float(mm) / float(win_mm)

    # -- driving ----------------------------------------------------------
    def target(self, x10):
        sc, out = self.u._req("POST", "/api/diag/windowpos", {"target_x10": x10})
        if sc == 404:
            sys.exit("POST /api/diag/windowpos answered 404 -- this needs a 2.12.0 "
                     "bench build, where the target hook exists")
        return out if isinstance(out, dict) else {}

    def pos_x10(self):
        w = (self.u.status() or {}).get("windows") or {}
        v = w.get("M3_percent_x10")
        return None if v is None else int(v)

    def state(self):
        w = (self.u.status() or {}).get("windows") or {}
        return str(w.get("M3", "?"))

    def at_end(self):
        w = (self.u.status() or {}).get("windows") or {}
        return bool(w.get("M3_at_end_sensor"))

    def wait_rest(self, limit_s=MOVE_LIMIT_S):
        end = time.time() + limit_s
        moved = False
        while time.time() < end:
            s = self.state().upper()
            if "MOVING" in s:
                moved = True
            elif moved:
                time.sleep(1.0)          # let the last sample land
                return self.state()
            time.sleep(0.3)
        return self.state()

    def go(self, x10):
        self.target(x10)
        self.wait_rest()
        return self.pos_x10()


def test_wp02(rig, target_x10, n):
    """Ten approaches, alternating direction, to one target."""
    band = rig.deadband_pct()
    say("AT-WP02: target %.1f %%, %d approaches, deadband %s"
        % (target_x10 / 10.0, n,
           ("%.2f %%" % band) if band is not None else "unknown"))

    from_below, from_above = [], []
    for i in range(n):
        below = (i % 2 == 0)
        rig.go(0 if below else 1000)            # start from a known end
        pos = rig.go(target_x10)
        if pos is None:
            say("  approach %2d: no position published -- cannot judge" % (i + 1))
            return False, {}
        (from_below if below else from_above).append(pos)
        say("  approach %2d from %-5s: %6.1f %%"
            % (i + 1, "below" if below else "above", pos / 10.0))

    allp = from_below + from_above
    spread = (max(allp) - min(allp)) / 10.0
    mb = sum(from_below) / float(len(from_below)) / 10.0 if from_below else 0.0
    ma = sum(from_above) / float(len(from_above)) / 10.0 if from_above else 0.0
    hyst = mb - ma
    mean = sum(allp) / float(len(allp)) / 10.0

    print("\n  n            : %d (%d from below, %d from above)"
          % (len(allp), len(from_below), len(from_above)))
    print("  mean         : %.1f %%  (target %.1f %%, error %+.1f %%)"
          % (mean, target_x10 / 10.0, mean - target_x10 / 10.0))
    print("  spread       : %.1f %%  (requirement: within +/-1 %%, i.e. <= 2.0)" % spread)
    print("  hysteresis   : %+.1f %%  (from below minus from above)" % hyst)
    if band is not None:
        print("  deadband     : %.2f %%  -- a spread below this is not measurable" % band)
    ok = spread <= 2.0
    if not ok and band is not None and band >= 2.0:
        print("\n  NOTE: the deadband alone is >= the requirement, so this run cannot")
        print("        demonstrate +/-1 %. Lower deadzone_m3 and repeat.")
    return ok, {"spread": spread, "hysteresis": hyst, "mean": mean}


def test_wp03(rig):
    """Endpoints: both ends agree with the mechanical end-stops."""
    say("AT-WP03: endpoints")
    ok = True
    rig.go(0)
    closed_pos, closed_end, closed_state = rig.pos_x10(), rig.at_end(), rig.state()
    say("  closed: %s, position %.1f %%, end sensor %s"
        % (closed_state, (closed_pos or 0) / 10.0, closed_end))
    ok &= bool(closed_end) and "CLOS" in closed_state.upper()

    rig.go(1000)
    open_pos, open_end, open_state = rig.pos_x10(), rig.at_end(), rig.state()
    say("  open  : %s, position %.1f %%, end sensor %s"
        % (open_state, (open_pos or 0) / 10.0, open_end))
    ok &= bool(open_end) and "OPEN" in open_state.upper()

    # "closed" must be distinguishable from "nearly closed" (FR-WP07).
    rig.go(0)
    rig.target(80)                                    # 8 %, a small opening
    rig.wait_rest()
    near_pos, near_end, near_state = rig.pos_x10(), rig.at_end(), rig.state()
    say("  near  : %s, position %.1f %%, end sensor %s"
        % (near_state, (near_pos or 0) / 10.0, near_end))
    distinct = (not near_end) and "CLOS" not in near_state.upper()
    ok &= distinct
    if not distinct:
        say("  a window 8 % open still reads as closed -- FR-WP07 is not met")
    rig.go(0)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--target", type=int, default=500, help="0.1 %% units (default 500 = 50 %%)")
    ap.add_argument("-n", type=int, default=10, help="approaches for AT-WP02")
    ap.add_argument("--only", choices=["wp02", "wp03"], help="run one of the two")
    a = ap.parse_args()

    rig = Rig(a.host, a.pin)
    print("at_wp02 -- unit %s, fw %s" % (rig.unit_id, rig.fw))
    if "bench" not in rig.fw:
        sys.exit("this needs a bench build: the target hook is bench-only")

    res = {}
    try:
        rig.cut_dwells()
        if a.only != "wp03":
            res["AT-WP02"] = test_wp02(rig, a.target, a.n)[0]
        if a.only != "wp02":
            print("")
            res["AT-WP03"] = test_wp03(rig)
    finally:
        print("\nrestoring ...")
        rig.restore()
        rig.go(0)
        rig.u.logout()

    print("\n== result ==")
    for k in sorted(res):
        print("  %-8s %s" % (k, "PASS" if res[k] else "FAIL"))
    bad = [k for k in res if not res[k]]
    print("\n%s" % ("ALL PASS" if not bad else "FAILED: " + ", ".join(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

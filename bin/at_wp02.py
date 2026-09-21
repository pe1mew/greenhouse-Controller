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

WHERE THE LEAF RESTS -- read live, after it has (2026-09-21)
-----------------------------------------------------------
The first run read /api/status, which carries T17's CACHED reading -- and T17
read "at once" after a stroke, while a targeted stop was still coasting, then
not again for 30 s. Its numbers were where the leaf was just after the cut:
up to 1.0 % short of where it came to rest. Each resting position is now a
LIVE device read (the diag route's top-level fields) RESTED_S after the stop.

**settle** checks the other half: once the settle time has passed, the
position the unit PUBLISHES must match the live one (within SETTLE_TOL_X10),
on every approach. It fails on fail-first bit 128, which restores the read at
once. The lead T2 cuts by (its `t2` block in the diag) is printed before and
after, since it is learned from these very stops.

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
RESTED_S = 2.5         # after a stop: T17's settle read comes ~1.1 s after it on the rig
SETTLE_TOL_X10 = 3     # published vs live, once settled: 0.3 % (one count is ~0.1 %)


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
            if val == TEST_DWELL_S:
                say("WARNING %s already reads %s s, the test value -- NOT recording it "
                    "as the original. Set it yourself after this run." % (key, val))
            else:
                self.saved[key] = val
            self.u.post_cfg("motor", key, TEST_DWELL_S)
        self.saved["min_intv_m3"] = cfg.get("min_intv_m3")
        self.u.post_cfg("motor", "min_intv_m3", 0)
        time.sleep(SETTLE_S)
        say("dwells cut to %d s, minimum interval 0 (restored on exit)" % TEST_DWELL_S)

    def standby(self, on):
        """Hold T6 off M3.

        T6 re-posts its current step every cycle for any window that does not
        match it, and writes no MODE row while doing so; a full-travel command
        disarms an armed target. Ten approaches to one target would otherwise
        be ten races between this harness and the controller, and the loser is
        always the harness (2026-09-20, see at_wp_target.py). STANDBY inhibits
        T6 and leaves T2's Q1 handling alone.
        """
        sc, _ = self.u._req("POST", "/api/mode",
                            {"mode": "standby" if on else "automatic"})
        time.sleep(SETTLE_S)
        say("T6 %s" % ("held in STANDBY" if on else "released to automatic"))
        return sc == 200

    def wait_gate(self, limit_s=180):
        """Wait for T17 to publish POSITION.

        Between boot and M3's first completed stroke the gate is deliberately
        TIMED, and every target is refused until it promotes at a stroke
        boundary. Provoke one if none comes.
        """
        end = time.time() + limit_s
        provoked = False
        while time.time() < end:
            gate = (self.u.diag() or {}).get("gate") or {}
            if gate.get("mode_str") == "position":
                say("gate: position control available")
                return True
            if not provoked:
                say("gate is %s -- driving M3 to promote it" % gate.get("mode_str"))
                self.go(1000)
                self.go(0)
                provoked = True
            time.sleep(1.0)
        return False

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
        if not win_mm:
            # The taught window size is published by the commissioning route,
            # not by /api/diag/windowpos -- the first run (2026-09-21) printed
            # "deadband unknown" because only the latter was asked.
            sc, c = self.u._req("GET", "/api/diag/commission")
            if sc == 200 and isinstance(c, dict):
                win_mm = c.get("window_mm")
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
        """The position the unit PUBLISHES -- T17's latest reading."""
        w = (self.u.status() or {}).get("windows") or {}
        v = w.get("M3_percent_x10")
        return None if v is None else int(v)

    def live_x10(self):
        """A FRESH device read: the diag route's top-level fields bypass T17."""
        d = self.u.diag() or {}
        v = d.get("percent_x10")
        return None if (v is None or not d.get("ok")) else int(v)

    def lead(self):
        """T2's overrun lead, (open, close) in 0.01 % and how many stops taught it."""
        t2 = (self.u.diag() or {}).get("t2") or {}
        if "lead_open_x100" not in t2:
            return None
        return (t2.get("lead_open_x100"), t2.get("lead_close_x100"),
                t2.get("learned_open"), t2.get("learned_close"))

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
        """Drive to x10 and return where the leaf RESTS, read live."""
        self.target(x10)
        self.wait_rest()
        time.sleep(RESTED_S)
        return self.live_x10()

    def go_measure(self, x10):
        """...and also what the unit publishes by then: (live, published)."""
        live = self.go(x10)
        return live, self.pos_x10()


def test_wp02(rig, target_x10, n):
    """Ten approaches, alternating direction, to one target."""
    band = rig.deadband_pct()
    say("AT-WP02: target %.1f %%, %d approaches, deadband %s"
        % (target_x10 / 10.0, n,
           ("%.2f %%" % band) if band is not None else "unknown"))

    ld = rig.lead()
    if ld is not None:
        say("  T2 lead before: open %.2f %%, close %.2f %% (learned from %s / %s stops)"
            % (ld[0] / 100.0, ld[1] / 100.0, ld[2], ld[3]))
    from_below, from_above, pub_err = [], [], []
    for i in range(n):
        below = (i % 2 == 0)
        rig.go(0 if below else 1000)            # start from a known end
        pos, pub = rig.go_measure(target_x10)
        if pos is None:
            say("  approach %2d: no live position -- cannot judge" % (i + 1))
            return False, {}
        (from_below if below else from_above).append(pos)
        if pub is not None:
            pub_err.append(abs(pub - pos))
        say("  approach %2d from %-5s: %6.1f %%   (published %s)"
            % (i + 1, "below" if below else "above", pos / 10.0,
               "%.1f %%" % (pub / 10.0) if pub is not None else "none"))
    ld = rig.lead()
    if ld is not None:
        say("  T2 lead after : open %.2f %%, close %.2f %% (learned from %s / %s stops)"
            % (ld[0] / 100.0, ld[1] / 100.0, ld[2], ld[3]))

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
    worst = max(pub_err) if pub_err else None
    settle_ok = (worst is not None and len(pub_err) == len(allp)
                 and worst <= SETTLE_TOL_X10)
    print("  published    : worst %s from the live resting position (settle: <= %.1f %%)"
          % ("%.1f %%" % (worst / 10.0) if worst is not None else "n/a",
             SETTLE_TOL_X10 / 10.0))
    return ok, {"spread": spread, "hysteresis": hyst, "mean": mean, "settle": settle_ok}


def test_wp03(rig):
    """Endpoints: both ends agree with the mechanical end-stops."""
    say("AT-WP03: endpoints")
    ok = True
    closed_pos = rig.go(0)
    closed_end, closed_state = rig.at_end(), rig.state()
    say("  closed: %s, position %.1f %%, end sensor %s"
        % (closed_state, (closed_pos or 0) / 10.0, closed_end))
    ok &= bool(closed_end) and "CLOS" in closed_state.upper()

    open_pos = rig.go(1000)
    open_end, open_state = rig.at_end(), rig.state()
    say("  open  : %s, position %.1f %%, end sensor %s"
        % (open_state, (open_pos or 0) / 10.0, open_end))
    ok &= bool(open_end) and "OPEN" in open_state.upper()

    # "closed" must be distinguishable from "nearly closed" (FR-WP07).
    rig.go(0)
    near_pos = rig.go(80)                             # 8 %, a small opening
    near_end, near_state = rig.at_end(), rig.state()
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
        rig.standby(True)
        if not rig.wait_gate():
            sys.exit("T17 never published position control: every approach below "
                     "would be refused, and not by the rule under test")
        if a.only != "wp03":
            ok02, info02 = test_wp02(rig, a.target, a.n)
            res["AT-WP02"] = ok02
            res["settle"] = bool(info02.get("settle"))
        if a.only != "wp02":
            print("")
            res["AT-WP03"] = test_wp03(rig)
    finally:
        print("\nrestoring ...")
        rig.go(0)                  # a known end before T6 takes M3 back
        rig.standby(False)
        rig.restore()
        rig.u.logout()

    print("\n== result ==")
    for k in sorted(res):
        print("  %-8s %s" % (k, "PASS" if res[k] else "FAIL"))
    bad = [k for k in res if not res[k]]
    print("\n%s" % ("ALL PASS" if not bad else "FAILED: " + ", ".join(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#104: a wind override during a run still closes M3.

THE RISK THIS EXISTS FOR
------------------------
Since 2.17.2 a run that ends early sends CMD_STOP for its own drive. A wind
override ends a run too, so T3's CMD_CLOSE_ALL and T17's CMD_STOP cross in Q1 with
M3 opening. Whichever goes first, M3 must end CLOSED. ch_stop() refuses a drive
that is not the caller's (gh#93), and this is that claim on hardware.

WHAT IT DOES
------------
Starts a run, and lowers `v_max` below the wind as phase 1's second drive begins,
20 % to about 47 %. T3 judges only on a new wind sample, 30 s apart, and that drive
lasts about 4 s on the rig, so the run is started at a moment that puts the drive
on a sample. The first attempt measures that timing, so it may miss. An override
that lands with M3 at rest proves nothing and the attempt repeats, up to ATTEMPTS.

So does one that lands as the drive ends: the status read before it still says
MOVING_OPEN, but T2 has already stopped at the target, so nothing crosses. That
passed on a build with ch_stop()'s check removed. The override must land with M3
under LAND_BELOW_PCT.

PASS = the override landed with M3 opening and short of its target, the run ended
`wind`, and M3 is CLOSED within CLOSE_LIMIT_S. `v_max` is restored and the session
ended afterwards. On the log, a stop that was refused leaves no RELAY value_a 7
row between the drive and the close, which comes 2 s later, after the reversal gap.

NEEDS WIND
----------
`v_max` cannot go below 1, so the wind average must be over MIN_WIND. This script
does NOT touch the emulator: setting it needs the operator's go. Then, by hand:

    POST http://192.168.20.226/config/sensor {"sensor":"s200","mode":0,"spd":3.0,"dir":108}

and afterwards, back to REST: {"sensor":"s200","mode":3}.

USAGE
-----
    python bin/at_char_wind.py --host 192.168.20.160

Exit 0 = pass, 1 = fail, 2 = refused or never landed. Refuses to run on 5C88.
"""

import argparse
import json
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from at_wp_ramp import Unit                                     # noqa: E402

DEFAULT_PIN = "12345678"
ATTEMPTS = 5
MIN_WIND = 1.5            # m/s, the average; v_max goes to 1, its minimum
V_LOW = 1
LEAD_S = 2.0              # the drive is under way this long before the sample
LAND_BELOW_PCT = 42.0     # the drive stops near 47 %: leave T2 time to be moving
MOVE_WAIT_S = 90.0
LAND_WAIT_S = 40.0
CLOSE_LIMIT_S = 40.0      # the reversal gap, a 13 s stroke and T2's margin
HOME_S = 180.0
WIND_ADDR = 44


def say(s):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), s), flush=True)


def public(host):
    return json.load(urllib.request.urlopen("http://%s/api/status" % host, timeout=5))


def m3(st):
    return (st.get("windows") or {}).get("M3")


def pct(st):
    v = (st.get("windows") or {}).get("M3_percent_x10")
    return None if v is None else float(v) / 10.0


def flags(st):
    return (st.get("mode") or {}).get("flags") or []


class Sampler:
    """When T5 last read the wind, from the public bus counter."""

    def __init__(self):
        self.count = None
        self.last = None
        self.period = None

    def see(self, st):
        now = time.time()
        for b in st.get("bus") or []:
            if b.get("a") == WIND_ADDR:
                if self.count is not None and b.get("ok") != self.count:
                    if self.last is not None:
                        d = now - self.last
                        if 5.0 < d < 40.0:
                            self.period = d
                    self.last = now
                self.count = b.get("ok")

    def next_after(self, t):
        """The first wind sample after t."""
        n = self.last
        while n <= t:
            n += self.period
        return n


def wait_home(host, s):
    """Back in AUTOMATIC, M3 closed, the gate open."""
    t0 = time.time()
    while time.time() - t0 < HOME_S:
        st = public(host)
        s.see(st)
        if (m3(st) == "CLOSED" and (st.get("mode") or {}).get("current") == "AUTOMATIC"
                and (st.get("windows") or {}).get("M3_pos_gate") == "ok"):
            return True
        time.sleep(0.5)
    return False


def attempt(host, pin, n, s, lead):
    """(result, seconds from the start to the move). result: True, False, or None."""
    print("--- attempt %d" % n, flush=True)
    if lead is not None:
        t_start = s.next_after(time.time() + lead + LEAD_S + 3.0) - lead - LEAD_S
        while time.time() < t_start:
            s.see(public(host))
            time.sleep(0.1)
    u = Unit(host, pin)
    lowered = False
    try:
        t0 = time.time()
        sc, r = u._req("POST", "/api/diag/commission", {"action": "characterise", "rest_s": "3"})
        if not (isinstance(r, dict) and r.get("ok")):
            say("FAIL: the run did not start: HTTP %s %s" % (sc, r))
            return False, lead

        while True:
            if time.time() - t0 > MOVE_WAIT_S:
                say("FAIL: never saw the second drive")
                return False, lead
            st = public(host)
            s.see(st)
            c = (u._req("GET", "/api/diag/commission")[1] or {}).get("char") or {}
            p = pct(st)
            if int(c.get("starts") or 0) >= 2 and m3(st) == "MOVING_OPEN" and p is not None and p > 21.0:
                break
            time.sleep(0.1)
        measured = time.time() - t0
        lowered = True
        if u.post_cfg("wind", "v_max", V_LOW) != 200:
            say("FAIL: v_max was refused")
            return False, measured
        say("M3 %s at %.1f %%, %.1f s into the run: v_max -> %d" % (m3(st), p, measured, V_LOW))

        before, t1 = st, time.time()
        while "wind_override" not in flags(st):
            if time.time() - t1 > LAND_WAIT_S:
                say("FAIL: no override %.0f s after v_max: %s" % (LAND_WAIT_S, flags(st)))
                return False, measured
            before = st
            st = public(host)
            s.see(st)
            time.sleep(0.05)
        t_land = time.time()
        was, at = m3(before), pct(before)
        say("override landed %.1f s after v_max: M3 was %s at %s %%, now %s"
            % (t_land - t1, was, at, m3(st)))

        seen = [m3(st)]
        while m3(st) != "CLOSED" and time.time() - t_land < CLOSE_LIMIT_S:
            time.sleep(0.1)
            st = public(host)
            s.see(st)
            if m3(st) != seen[-1]:
                seen.append(m3(st))
        c = (u._req("GET", "/api/diag/commission")[1] or {}).get("char") or {}
        say("M3 went %s; run %s, reason %s; %.1f s after the override"
            % (" -> ".join(str(x) for x in seen), c.get("state"), c.get("reason"), time.time() - t_land))

        if m3(st) != "CLOSED":
            say("FAIL: M3 is %s, %.0f s into a wind override" % (m3(st), CLOSE_LIMIT_S))
            return False, measured
        if c.get("reason") != "wind":
            say("FAIL: the run ended %s, not wind" % c.get("reason"))
            return False, measured
        if was != "MOVING_OPEN" or at is None or at >= LAND_BELOW_PCT:
            say("not the case under test: M3 was %s at %s %% when the override landed" % (was, at))
            return None, measured
        say("PASS: M3 was opening, the run ended on the wind, and M3 closed")
        return True, measured
    finally:
        if lowered:
            u.post_cfg("wind", "v_max", s.v_max)
            if not u.settle_cfg("v_max", s.v_max):
                say("WARNING: v_max did not read back %s" % s.v_max)
        u._req("POST", "/api/diag/commission", {"action": "abort"})
        u.logout()
        say("v_max %s restored, logged out: the unit recalibrates" % s.v_max)
        if not wait_home(host, s):
            say("WARNING: not back in AUTOMATIC with M3 closed and the gate open")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    a = ap.parse_args()

    st = public(a.host)
    y = st.get("system") or {}
    if "5C88" in str(y.get("unit_id", "")).upper():
        print("REFUSED: this is the production unit")
        return 2
    avg = float((st.get("wind") or {}).get("speed_avg_ms") or 0.0)
    print("%s fw %s, up %s s, M3 %s, wind avg %.1f m/s" % (y.get("unit_id"), y.get("fw_ver"),
                                                          y.get("uptime_s"), m3(st), avg))
    if avg < MIN_WIND:
        print("REFUSED: the wind averages %.1f m/s, under %.1f. See NEEDS WIND." % (avg, MIN_WIND))
        return 2
    u = Unit(a.host, a.pin)
    v_max = u.cfg().get("v_max")
    u.logout()
    if int(v_max) <= avg:
        print("REFUSED: v_max %s is not above the wind" % v_max)
        return 2

    s = Sampler()
    s.v_max = v_max
    t0 = time.time()
    while s.period is None and time.time() - t0 < 60.0:
        s.see(public(a.host))
        time.sleep(0.1)
    if s.period is None:
        print("REFUSED: no wind samples seen")
        return 2
    print("wind sampled every %.1f s" % s.period)

    lead = None
    for n in range(1, ATTEMPTS + 1):
        res, lead = attempt(a.host, a.pin, n, s, lead)
        if res is not None:
            print("=== %s" % ("PASS" if res else "FAIL"))
            return 0 if res else 1
    print("=== NOT LANDED: the override missed the move %d times" % ATTEMPTS)
    return 2


if __name__ == "__main__":
    sys.exit(main())

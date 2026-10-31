#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#104: a characterisation that ends early must stop M3.

THE DEFECT THIS EXISTS FOR
--------------------------
On 5C88 on 2026-10-31 a run ended on a lost sensor with M3 opening towards 20 %.
Nothing stopped the drive, and with the position lost T2 finished it on the
travel timer: 118 s of a 171 s stroke, ended only by a false wind override.

WHAT IT DOES
------------
Starts a run on the rig, waits for phase 1's 20 -> 80 % move, and ends the run
while M3 is moving:

  abort   the Abort button's request. Before the fix M3 finishes its move.
  absent  the bench injection that makes address 40 stop answering, so the run
          ends with `sensor`. Before the fix M3 runs on to the OPEN end.

PASS = M3 rests PART_OPEN within STOP_LIMIT_S of the run ending, short of its
target. Nothing is unplugged. Afterwards the injection is cleared and the session
ended, so the unit recalibrates and returns to AUTOMATIC.

Not covered: that a safety close is never stopped. T2's ch_stop() refuses a
drive that is not the caller's (gh#93), and bin/at_lcd_m3_hold.py exercises it.

USAGE
-----
    python bin/at_char_stop.py --host 192.168.20.160          # both cases
    python bin/at_char_stop.py --host 192.168.20.160 abort

Exit 0 = every case passed, 1 = a case failed, 2 = refused. Needs a bench build.
Refuses to run on 5C88.
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
STOP_LIMIT_S = 3.0        # the run's end to the relay off, with T17's read gaps
WATCH_S = 30.0            # rig stroke 13 s + T2's margin: an unstopped drive ends
MOVE_WAIT_S = 90.0
HOME_S = 180.0


def say(s):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), s), flush=True)


def public(host):
    return json.load(urllib.request.urlopen("http://%s/api/status" % host, timeout=5))


def m3(st):
    return (st.get("windows") or {}).get("M3")


def pct(st):
    v = (st.get("windows") or {}).get("M3_percent_x10")
    return None if v is None else float(v) / 10.0


def char(u):
    return (u._req("GET", "/api/diag/commission")[1] or {}).get("char") or {}


def wait_home(host):
    """Back in AUTOMATIC, M3 closed, the gate open."""
    t0 = time.time()
    while time.time() - t0 < HOME_S:
        st = public(host)
        if (m3(st) == "CLOSED" and (st.get("mode") or {}).get("current") == "AUTOMATIC"
                and (st.get("windows") or {}).get("M3_pos_gate") == "ok"):
            return True
        time.sleep(2.0)
    return False


def case(host, pin, how):
    print("--- %s" % how, flush=True)
    u = Unit(host, pin)
    injected = False
    try:
        sc, r = u._req("POST", "/api/diag/commission", {"action": "characterise", "rest_s": "3"})
        if not (isinstance(r, dict) and r.get("ok")):
            say("FAIL: the run did not start: HTTP %s %s" % (sc, r))
            return False
        say("run started")

        # Phase 1: to 20 %, rest, then 20 -> 80 %. Its second drive is the one.
        t0 = time.time()
        while True:
            if time.time() - t0 > MOVE_WAIT_S:
                say("FAIL: never saw the 20 -> 80 % move: %s" % json.dumps(char(u))[:200])
                return False
            c = char(u)
            st = public(host)
            if int(c.get("starts") or 0) >= 2 and m3(st) == "MOVING_OPEN":
                p = pct(st)
                if p is not None and p > 25.0:
                    break
            time.sleep(0.15)
        say("M3 %s at %.1f %%, starts %s: ending the run" % (m3(st), p, c.get("starts")))

        if how == "abort":
            sc, r = u._req("POST", "/api/diag/commission", {"action": "abort"})
            if sc != 200:
                say("FAIL: abort refused: HTTP %s %s" % (sc, r))
                return False
        else:
            sc, r = u._req("POST", "/api/diag/windowpos", {"inject": "absent"})
            if sc != 200:
                say("FAIL: injection refused: HTTP %s %s" % (sc, r))
                return False
            injected = True
        t_act = time.time()

        ended = None                  # when the run reported its end
        rest = None
        last_p = p
        while time.time() - t_act < WATCH_S:
            st = public(host)
            if pct(st) is not None:
                last_p = pct(st)
            if ended is None:
                c = char(u)
                if c.get("state") in ("failed", "done"):
                    ended = time.time()
                    say("run ended: %s, %.1f s after the action" % (c.get("reason"), ended - t_act))
            state = m3(st)
            if state is not None and not str(state).startswith("MOVING"):
                rest = (state, time.time() - t_act)
                break
            time.sleep(0.1)

        if rest is None:
            say("FAIL: M3 still %s after %.0f s" % (m3(public(host)), WATCH_S))
            return False
        say("M3 rests %s %.1f s after the action, last position %s %%" % (rest[0], rest[1], last_p))
        ok = rest[0] == "PART_OPEN" and rest[1] <= STOP_LIMIT_S
        say("%s: %s" % ("PASS" if ok else "FAIL", "M3 stopped at once" if ok
                        else "M3 was not stopped: it carried on to its target or the end"))
        return ok
    finally:
        if injected:
            u._req("POST", "/api/diag/windowpos", {"inject": "none"})
            say("injection cleared")
        u._req("POST", "/api/diag/commission", {"action": "abort"})
        u.logout()
        say("logged out: the unit recalibrates")
        if not wait_home(host):
            say("WARNING: not back in AUTOMATIC with M3 closed and the gate open")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("cases", nargs="*", help="abort, absent; both when left out")
    a = ap.parse_args()

    st = public(a.host)
    y = st.get("system") or {}
    if "5C88" in str(y.get("unit_id", "")).upper():
        print("REFUSED: this is the production unit")
        return 2
    if "bench" not in str(y.get("fw_ver", "")):
        print("REFUSED: needs a bench build, this is %s" % y.get("fw_ver"))
        return 2
    print("%s fw %s, up %s s, M3 %s, gate %s" % (y.get("unit_id"), y.get("fw_ver"), y.get("uptime_s"),
                                                m3(st), (st.get("windows") or {}).get("M3_pos_gate")))
    results = [(c, case(a.host, a.pin, c)) for c in (a.cases or ["abort", "absent"])]
    print("=== " + ", ".join("%s %s" % (c, "PASS" if ok else "FAIL") for c, ok in results))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())

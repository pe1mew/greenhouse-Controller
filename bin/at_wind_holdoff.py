#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for 2.18.0 (FR-W04): a wind sensor that misses a few polls closes nothing.

THE DEFECT THIS EXISTS FOR
--------------------------
T5 raised the wind fault on one failed poll, and T3 then closed the greenhouse.
Every wind fault since commissioning did that, in 0.6 to 1.6 m/s of wind. The
fault now needs the sensor unread for the whole averaging window, at least two
polls.

WHAT IT DOES
------------
The bench injection `nowind` makes T5 treat every wind read as failed and puts
nothing on the bus. T17 and the T/RH stay real.

  short  inject for one poll fewer than the window. No fault and no override may
         appear. Before the fix the first failed poll raises both.
  long   inject until the fault appears. It must come at the window, within one
         poll, the override with it. Then clear: the fault must go at the next
         good read, and the unit return to AUTOMATIC.

The T5 poll is watched through the address 44 counter. A window of `avg_win_wind`
minutes is that times 60 over `poll_interval_s` polls. At the rig's 3 minutes and
30 s that is 6 polls, so this takes about 7 minutes.

USAGE
-----
    python bin/at_wind_holdoff.py --host 192.168.20.160
    python bin/at_wind_holdoff.py --host 192.168.20.160 short

Exit 0 = pass, 1 = fail, 2 = refused. Needs a bench build. Refuses to run on 5C88.
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
STEP_S = 1.0
HOME_S = 180.0


def say(s):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), s), flush=True)


def public(host):
    return json.load(urllib.request.urlopen("http://%s/api/status" % host, timeout=5))


def flags(st):
    return (st.get("mode") or {}).get("flags") or []


def inject(host, pin, how):
    u = Unit(host, pin)
    try:
        sc, r = u._req("POST", "/api/diag/windowpos", {"inject": how})
        if sc != 200:
            sys.exit("the injection was refused: HTTP %s %s" % (sc, r))
    finally:
        u.logout()


def wait_clear(host):
    t0 = time.time()
    while time.time() - t0 < HOME_S:
        st = public(host)
        f = flags(st)
        if ("sensor_fault_wind" not in f and "wind_override" not in f
                and (st.get("mode") or {}).get("current") == "AUTOMATIC"):
            return time.time() - t0
        time.sleep(STEP_S)
    return None


def wind_ok_count(st):
    return next((b.get("ok", 0) for b in st.get("bus") or [] if b.get("a") == 44), 0)


def wait_good_read(host, poll_s):
    """The count of failed polls runs on until a good read. Without one between
    the cases, `long` starts with the failures of `short` already counted."""
    n = wind_ok_count(public(host))
    t0 = time.time()
    while time.time() - t0 < 2.0 * poll_s + 5.0:
        if wind_ok_count(public(host)) > n:
            return
        time.sleep(STEP_S)
    sys.exit("no good wind read after clearing the injection")


def short(host, pin, poll_s, win):
    hold = (win - 1) * poll_s - 5.0
    print("--- short: %.0f s, %d polls of a window of %d" % (hold, win - 1, win), flush=True)
    inject(host, pin, "nowind")
    t0 = time.time()
    try:
        while time.time() - t0 < hold:
            f = flags(public(host))
            if "sensor_fault_wind" in f or "wind_override" in f:
                say("FAIL: %s %.0f s in, inside the window" % (f, time.time() - t0))
                return False
            time.sleep(STEP_S)
    finally:
        inject(host, pin, "none")
    if wait_clear(host) is None:
        say("FAIL: not back to normal")
        return False
    say("PASS: no fault and no override in %.0f s" % hold)
    wait_good_read(host, poll_s)
    return True


def long_case(host, pin, poll_s, win):
    window_s = win * poll_s
    limit = window_s + 2.0 * poll_s
    print("--- long: the fault is due at about %d s" % window_s, flush=True)
    inject(host, pin, "nowind")
    t0 = time.time()
    fault_at = None
    try:
        while time.time() - t0 < limit:
            f = flags(public(host))
            if "sensor_fault_wind" in f:
                fault_at = time.time() - t0
                say("fault %.0f s in, flags %s" % (fault_at, f))
                break
            time.sleep(STEP_S)
        if fault_at is None:
            say("FAIL: no fault %.0f s in" % limit)
            return False
        time.sleep(poll_s + 3.0)              # T3 acts on the next sample
        f = flags(public(host))
        over = "wind_override" in f
        say("override %s" % ("set" if over else "NOT set"))
    finally:
        inject(host, pin, "none")
    took = wait_clear(host)
    if took is None:
        say("FAIL: fault or override did not clear")
        return False
    early = fault_at < window_s - poll_s
    ok = over and not early
    say("%s: fault at %.0f s of a %d s window, cleared %.0f s after the injection"
        % ("PASS" if ok else "FAIL", fault_at, window_s, took))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("cases", nargs="*", help="short, long; both when left out")
    a = ap.parse_args()

    st = public(a.host)
    y = st.get("system") or {}
    if "5C88" in str(y.get("unit_id", "")).upper():
        print("REFUSED: this is the production unit")
        return 2
    if "bench" not in str(y.get("fw_ver", "")):
        print("REFUSED: needs a bench build, this is %s" % y.get("fw_ver"))
        return 2
    u = Unit(a.host, a.pin)
    c = u.cfg()
    u.logout()
    poll_s = float(c["poll_interval_s"])
    win = max(int(int(c["avg_win_wind"]) * 60 / poll_s), 2)
    print("%s fw %s, wind window %d polls of %.0f s, flags %s"
          % (y.get("unit_id"), y.get("fw_ver"), win, poll_s, flags(st)), flush=True)
    if "sensor_fault_wind" in flags(st) or "wind_override" in flags(st):
        print("REFUSED: a wind fault or override is already active")
        return 2
    results = []
    for c_ in (a.cases or ["short", "long"]):
        results.append((c_, short(a.host, a.pin, poll_s, win) if c_ == "short"
                        else long_case(a.host, a.pin, poll_s, win)))
    print("=== " + ", ".join("%s %s" % (n, "PASS" if ok else "FAIL") for n, ok in results))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())

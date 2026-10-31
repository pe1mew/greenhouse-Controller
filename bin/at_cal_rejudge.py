#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#105: a verdict of "no device" must not outlive a sensor that answers.

THE DEFECT THIS EXISTS FOR
--------------------------
2.17.3 greys Teach and the window size while the calibration check reports
`no_device`. That verdict was only re-judged when T17's gate re-opened, so one
config read lost on a bus that otherwise works left it standing with the gate
open, and a healthy sensor could then be neither taught nor characterised.

WHAT IT DOES
------------
The bench injection `noconf` fails the calibration check's config read and leaves
T17's position reads real:

  1. inject, refresh: the verdict must read unknown / no_device, the gate ok;
  2. hold it past one idle read: it must stay, so a re-judge that fails is seen
     to fail and nothing clears the verdict on its own;
  3. clear the injection and send NO refresh: the verdict must return to valid
     from T17's next reading, within LIMIT_S.

M3 does not move. Afterwards the injection is cleared and the session ended. A
failed step sends a refresh on the way out, so the unit is not left unknown.

Not covered: the card itself, which needs a browser.

USAGE
-----
    python bin/at_cal_rejudge.py --host 192.168.20.160

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
HOLD_S = 40.0             # past IDLE_READ_MS, 30 s
LIMIT_S = 45.0            # IDLE_READ_MS and the bus's time


def say(s):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), s), flush=True)


def public(host):
    return json.load(urllib.request.urlopen("http://%s/api/status" % host, timeout=5))


def cal(u):
    d = u._req("GET", "/api/diag/commission")[1] or {}
    return d.get("verdict"), d.get("cal_reason")


def run(host, u):
    sc, r = u._req("POST", "/api/diag/windowpos", {"inject": "noconf"})
    if sc != 200 or not (isinstance(r, dict) and r.get("inject") == "noconf"):
        say("FAIL: the injection was refused: HTTP %s %s" % (sc, r))
        return False
    u._req("POST", "/api/diag/commission", {"action": "refresh"})
    v = cal(u)
    say("injected and refreshed: %s / %s" % v)
    if v != ("unknown", "no_device"):
        say("FAIL: the injection did not reach the calibration check")
        return False
    gate = (public(host).get("windows") or {}).get("M3_pos_gate")
    if gate != "ok":
        say("FAIL: the gate reads %s, so this is not the case under test" % gate)
        return False

    t0 = time.time()
    while time.time() - t0 < HOLD_S:
        if cal(u) != ("unknown", "no_device"):
            say("FAIL: the verdict left no_device while the read still fails: %s / %s" % cal(u))
            return False
        time.sleep(2.0)
    say("held %.0f s: still no_device, gate %s"
        % (HOLD_S, (public(host).get("windows") or {}).get("M3_pos_gate")))

    u._req("POST", "/api/diag/windowpos", {"inject": "none"})
    t0 = time.time()
    say("injection cleared, no refresh sent")
    while time.time() - t0 < LIMIT_S:
        v = cal(u)
        if v == ("valid", "none"):
            say("PASS: valid again %.1f s later, from T17's own reading" % (time.time() - t0))
            return True
        time.sleep(1.0)
    say("FAIL: still %s / %s %.0f s after the injection cleared" % (v[0], v[1], LIMIT_S))
    return False


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
    if "bench" not in str(y.get("fw_ver", "")):
        print("REFUSED: needs a bench build, this is %s" % y.get("fw_ver"))
        return 2
    print("%s fw %s, up %s s, gate %s" % (y.get("unit_id"), y.get("fw_ver"), y.get("uptime_s"),
                                         (st.get("windows") or {}).get("M3_pos_gate")))
    u = Unit(a.host, a.pin)
    ok = False
    try:
        if cal(u) != ("valid", "none"):
            print("REFUSED: the calibration does not read valid: %s / %s" % cal(u))
            return 2
        ok = run(a.host, u)
    finally:
        u._req("POST", "/api/diag/windowpos", {"inject": "none"})
        if not ok:
            u._req("POST", "/api/diag/commission", {"action": "refresh"})
            say("refresh sent on the way out: %s / %s" % cal(u))
        u.logout()
    print("=== %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

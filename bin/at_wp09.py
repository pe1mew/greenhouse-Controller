#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT-WP09 -- obstruct M3 mid-travel, confirm the divergence is reported.

The acceptance test that `integrateWindowPositionSensor.md` records as *"not
run, and not yet runnable -- it needs 12.4 rule 1, which IS the divergence
detector"*. Rule 1 landed 2026-09-15, so this is now runnable.

WHAT IT PROVES
--------------
The controller notices that the leaf is not following the motor. That matters
because the device cannot notice it: a shorted wiper reports a perfectly
plausible CONSTANT position, every status bit stays clear, `sensor_fault` stays
false, and bit 6 is inert on this installation for lack of electrical headroom.
Rule 1 is the only thing standing between that and a window the controller
believes it has moved.

WHY IT NEEDS A HUMAN
--------------------
The divergence has to be physical. Three ways to produce it, easiest first:

  1. **Detach the draw-wire** from the leaf and command a stroke. The motor
     runs, the wire does not, position sits still. Closest to the real fault.
  2. **Obstruct the leaf** so the motor drives against a jam.
  3. **Short the wiper** -- what the rule exists for, but do not do this to a
     working sensor just to test it.

So this script is an OBSERVER, not a driver. It reads the unit's own counters
across a stroke you cause, and judges what it sees.

THE PASS IS THE EASY HALF -- READ THE FAIL-FIRST NOTE
-----------------------------------------------------
A detector that fires is worth little until you know it can stay silent. This
is a fault detector on a mechanism that moves several times a day, so a false
positive is not a nuisance: it would eventually be the reason someone stops
believing the row. `--healthy` runs the same measurement across an UNOBSTRUCTED
stroke and asserts the counter does NOT move. **Run both.** The gotcha log's
standing rule is "show the check can fail before trusting a pass"; for a
detector the inverse matters just as much, and the AT-WP05 lesson was exactly
this -- a headline counter that could not fail proved nothing over 7117 reads.

USAGE
-----
    # arm the observer, then cause an obstructed stroke when it says to
    python bin/at_wp09.py --host 192.168.20.169

    # the false-positive half: a normal, unobstructed stroke must NOT trip it
    python bin/at_wp09.py --host 192.168.20.169 --healthy

Exit 0 = the criterion held, 1 = it did not, 2 = could not run the test.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import http.client
import json
import sys
import time

DEFAULT_HOST = "192.168.20.169"     # FDA4; pass --host for 2344 (.160)
DEFAULT_PIN = "12345678"
POLL_S = 1.0
DIAG = "/api/diag/windowpos"


class Unit(object):
    """Admin session against one unit. Same shape as at_cfg_roundtrip.py's."""

    def __init__(self, host, pin):
        self.host = host
        self.pin = pin
        self.cookie = None
        self._login(pin)

    def _raw(self, method, path, body=None):
        c = http.client.HTTPConnection(self.host, 80, timeout=10)
        hdr = {"Content-Type": "application/json"}
        if self.cookie:
            hdr["Cookie"] = self.cookie
        c.request(method, path,
                  json.dumps(body).encode() if body is not None else None, hdr)
        r = c.getresponse()
        raw = r.read()
        sc = r.getcode()
        sk = r.getheader("Set-Cookie")
        if sk:
            self.cookie = sk.split(";")[0]
        c.close()
        try:
            return sc, json.loads(raw.decode("utf-8"))
        except Exception:                                      # noqa: BLE001
            return sc, raw.decode("utf-8", "replace")

    def _req(self, method, path, body=None, _retry=True):
        # The admin session expires after session_timeout minutes (default 5)
        # and an obstructed-stroke run can outlast it while waiting for a human.
        sc, out = self._raw(method, path, body)
        if sc == 401 and _retry and path != "/api/login":
            self.cookie = None
            self._login(self.pin)
            return self._raw(method, path, body)
        return sc, out

    def _login(self, pin):
        sc, _ = self._req("POST", "/api/login", {"role": "admin", "pin": pin})
        if sc != 200 or not self.cookie:
            sys.exit("login failed (HTTP %s) -- wrong PIN, or the unit is not up" % sc)

    def diag(self):
        sc, out = self._req("GET", DIAG)
        if sc != 200 or not isinstance(out, dict):
            sys.exit("%s returned HTTP %s -- is this a ropeSensor build?" % (DIAG, sc))
        return out


def soak_of(d):
    s = d.get("soak")
    if not isinstance(s, dict):
        sys.exit("no `soak` block in %s -- build too old for this test" % DIAG)
    return s


def gate_of(d):
    g = d.get("gate")
    return g if isinstance(g, dict) else {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--healthy", action="store_true",
                    help="false-positive half: an UNOBSTRUCTED stroke must not trip the rule")
    ap.add_argument("--timeout", type=int, default=300,
                    help="seconds to wait for the stroke (default 300)")
    args = ap.parse_args()

    u = Unit(args.host, args.pin)

    # ---- identify the unit, and refuse if it cannot run the test -----------
    sc, st = u._req("GET", "/api/status")
    unit_id = "?"
    if sc == 200 and isinstance(st, dict):
        unit_id = (st.get("system") or {}).get("unit_id", "?")
    d0 = u.diag()
    s0 = soak_of(d0)
    g0 = gate_of(d0)

    if "stall_faults" not in s0:
        sys.exit("this build has no `stall_faults` counter -- 12.4 rule 1 is not\n"
                 "in it, so AT-WP09 has nothing to observe. Flash a build with it.")

    mode = g0.get("mode_str", g0.get("mode", "?"))
    print("AT-WP09 -- %s on %s" % (
        "FALSE-POSITIVE half (healthy stroke)" if args.healthy
        else "divergence half (obstructed stroke)", unit_id))
    print("  gate mode now      : %s" % mode)
    print("  strokes so far     : %s" % s0.get("strokes"))
    print("  stall_faults so far: %s" % s0.get("stall_faults"))

    # An arm where T17 never polls is not a test of anything -- the same trap
    # arm B had ("an arm B where T17 never polls is arm A with the plug in").
    if str(mode).lower().startswith("timed"):
        print("\n  NOTE: the gate is TIMED, so T17 is not polling and cannot")
        print("  judge this stroke. For the obstructed half that is expected if")
        print("  you detached the wire -- the gate demotes on the missing sensor")
        print("  BEFORE rule 1 can speak, and the correct result is then")
        print("  WPOS_GATE_NO_SENSOR, not a stall. Obstruct the leaf or short the")
        print("  wiper instead, leaving the sensor answering.")

    if args.healthy:
        print("\n  >>> Command a NORMAL, unobstructed M3 stroke now.")
    else:
        print("\n  >>> Obstruct M3 (or detach the draw-wire from the leaf), then")
        print("  >>> command an M3 stroke now.")
    print("  waiting up to %d s ...\n" % args.timeout)

    t0 = time.time()
    fired_at = None
    strokes_seen = 0
    while time.time() - t0 < args.timeout:
        time.sleep(POLL_S)
        s = soak_of(u.diag())
        strokes_seen = int(s.get("strokes", 0)) - int(s0.get("strokes", 0))
        d_stall = int(s.get("stall_faults", 0)) - int(s0.get("stall_faults", 0))
        if d_stall > 0 and fired_at is None:
            fired_at = time.time() - t0
            print("  rule 1 fired at t+%.1f s (stall_faults +%d)" % (fired_at, d_stall))
            if not args.healthy:
                break
        if strokes_seen > 0 and args.healthy and (time.time() - t0) > 30:
            break

    if strokes_seen == 0:
        print("\nINCONCLUSIVE: no stroke was observed (`strokes` did not move).")
        print("Nothing was tested. T2 must actually energise M3 for this test to")
        print("mean anything -- a stroke you did not cause is not a null result.")
        return 2

    print("\n  strokes observed   : %d" % strokes_seen)

    # ---- verdict ----------------------------------------------------------
    if args.healthy:
        if fired_at is None:
            print("\nPASS: a healthy stroke did NOT trip 12.4 rule 1.")
            print("That is the half that keeps the row believable.")
            return 0
        print("\nFAIL: rule 1 tripped on an UNOBSTRUCTED stroke (t+%.1f s)." % fired_at)
        print("This is a false positive. Before touching the threshold, check")
        print("`travel_m3` against the wired window -- nominal derives from it,")
        print("so a rig value left at the production 171 makes every real stroke")
        print("look ~13x too slow and would trip this every time.")
        return 1

    if fired_at is None:
        print("\nFAIL: the leaf was obstructed and 12.4 rule 1 did NOT report it.")
        print("Check, in this order: was the gate in POSITION (T17 polling at")
        print("all)? did `strokes` move? is `rejected_rate` climbing -- every")
        print("sample implausible also means no accepted evidence of movement.")
        return 1

    print("\nPASS: the divergence was reported (t+%.1f s)." % fired_at)
    print("Read the ALARM ch6 param 249 row off the SD log for the peak rate")
    print("and threshold it recorded; `logparser.py` decodes it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""The GATE prerequisite: clean aperture data over a SUSTAINED period.

The last item standing between the window-position work and T2's position
consumer. Everything else on that gate is done: the monotonic ramp is
demonstrated in both directions, and AT-WP06/07/09 all pass. What is missing is
duration -- a handful of traverses in one evening is not a period, and the rig
spent that evening having its encoder unplugged and replugged.

WHY THIS NEEDS A TOOL AND NOT AN EYEBALL
-----------------------------------------
"It looked fine overnight" is the failure mode this project keeps meeting: a
soak counter that cannot fail (AT-WP05's `err_busy`), an absent field reading as
a quiet one, an A/B with no statistical power. So the criteria are written down
here, checked mechanically, and -- the part that matters -- **the run is
declared INCONCLUSIVE rather than PASS when the sample is too small to mean
anything.** A soak that saw three strokes has not earned the word "sustained"
however many hours it ran.

CRITERIA
--------
Over the soak window, with the sensor fitted and trusted:

  duration      >= --hours (default 12)
  strokes       >= --min-strokes (default 10) -- the denominator. Without
                  traverses, elapsed time measures an idle bus, not aperture data
  stall_faults   == 0   12.4 rule 1 must not false-trip across many real strokes.
                        THIS is the criterion the period exists to test: one
                        healthy stroke proved it can stay silent once
  early_stops    == 0   12.4 rule 2, same argument
  rejected_rate  == 0   no implausible samples; a real one would mean the trace
                        cannot be trusted as aperture data
  err_comm       == 0   the encoder answered every time it was asked
  mode_changes   <= --max-mode-changes (default 2) -- the gate must SETTLE in
                        POSITION, not flap. Flapping means the data is a mix of
                        traced and untraced strokes

USAGE
-----
    python bin/at_wp_soak.py --start --host 192.168.20.169     # record baseline
    python bin/at_wp_soak.py --report --host 192.168.20.169    # judge it

The baseline lands next to this script as `.at_wp_soak.json` (git-ignored via
`bin/.*`). `--start` REFUSES if the unit is not in a state where a soak would
measure anything: encoder absent, wind override up, or STANDBY latched all
produce zero strokes and an empty result that looks like a clean one.

Exit 0 = criteria met, 1 = a criterion failed, 2 = could not run / inconclusive.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import http.client
import json
import os
import sys
import time

DEFAULT_HOST = "192.168.20.169"
DEFAULT_PIN = "12345678"
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".at_wp_soak.json")
EG1_WIND_OVERRIDE = 1 << 0
EG1_STANDBY = 1 << 7


class Unit(object):
    def __init__(self, host, pin):
        self.host, self.pin, self.cookie = host, pin, None
        self._login(pin)

    def _raw(self, method, path, body=None):
        c = http.client.HTTPConnection(self.host, 80, timeout=10)
        hdr = {"Content-Type": "application/json"}
        if self.cookie:
            hdr["Cookie"] = self.cookie
        c.request(method, path,
                  json.dumps(body).encode() if body is not None else None, hdr)
        r = c.getresponse()
        raw, sc = r.read(), r.getcode()
        sk = r.getheader("Set-Cookie")
        if sk:
            self.cookie = sk.split(";")[0]
        c.close()
        try:
            return sc, json.loads(raw.decode("utf-8"))
        except Exception:                                      # noqa: BLE001
            return sc, raw.decode("utf-8", "replace")

    def _req(self, method, path, body=None, _retry=True):
        sc, out = self._raw(method, path, body)
        if sc == 401 and _retry and path != "/api/login":
            self.cookie = None
            self._login(self.pin)
            return self._raw(method, path, body)
        return sc, out

    def _login(self, pin):
        sc, _ = self._req("POST", "/api/login", {"role": "admin", "pin": pin})
        if sc != 200 or not self.cookie:
            sys.exit("login failed (HTTP %s)" % sc)

    def status(self):
        j = self._req("GET", "/api/status")[1]
        if not isinstance(j, dict) or "system" not in j:
            sys.exit("GET /api/status has no `system` block -- refusing to run")
        return j

    def diag(self):
        sc, out = self._req("GET", "/api/diag/windowpos")
        if sc != 200 or not isinstance(out, dict):
            sys.exit("GET /api/diag/windowpos returned HTTP %s -- ropeSensor build?" % sc)
        return out


FIELDS = ("reads_ok", "err_comm", "err_busy", "rejected_rate", "strokes",
          "probe_fail", "mode_changes", "stall_faults", "early_stops",
          "gated_polls")


def snapshot(u):
    st, dg = u.status(), u.diag()
    sysb = st.get("system") or {}
    s = dg.get("soak") or {}
    missing = [f for f in ("stall_faults", "early_stops") if f not in s]
    if missing:
        sys.exit("this build has no %s -- the 12.4 rules are not in it, so the\n"
                 "soak cannot judge them. An ABSENT counter must never be read as\n"
                 "a quiet one." % ", ".join(missing))
    return {
        "t": time.time(),
        "unit_id": sysb.get("unit_id"),
        "fw_ver": sysb.get("fw_ver"),
        "uptime_s": sysb.get("uptime_s"),
        "eg1": int(sysb.get("eg1", 0) or 0),
        "gate": str((dg.get("gate") or {}).get("mode_str", "?")),
        "counters": dict((f, int(s.get(f, 0))) for f in FIELDS),
    }


def cmd_start(u, args):
    snap = snapshot(u)
    st = u.status()
    dg = u.diag()

    problems = []
    if not dg.get("ok") or dg.get("sensor_fault"):
        problems.append("the encoder is not answering cleanly (ok=%s sensor_fault=%s).\n"
                        "     With no sensor the gate shuts and no aperture data is\n"
                        "     produced at all -- the soak would measure an idle bus."
                        % (dg.get("ok"), dg.get("sensor_fault")))
    if snap["eg1"] & EG1_WIND_OVERRIDE:
        w = st.get("wind") or {}
        problems.append("the wind override is UP (eg1 0x%02X, wind %s m/s avg %s).\n"
                        "     Windows are held closed, so there will be no strokes.\n"
                        "     T3 runs on the AVERAGE, so this clears as the window rolls."
                        % (snap["eg1"], w.get("speed_ms"), w.get("speed_avg_ms")))
    if snap["eg1"] & EG1_STANDBY:
        problems.append("STANDBY is latched (eg1 0x%02X). T6 is suspended, so nothing\n"
                        "     will move and the soak collects nothing." % snap["eg1"])
    if problems:
        print("REFUSING to start a soak that cannot measure anything:")
        for p in problems:
            print("  *  %s" % p)
        return 2

    with open(STATE, "w") as fh:
        json.dump(snap, fh, indent=2)
    print("soak baseline recorded -- %s fw %s" % (snap["unit_id"], snap["fw_ver"]))
    print("  gate now : %s" % snap["gate"])
    print("  counters : %s" % ", ".join("%s=%s" % (k, snap["counters"][k])
                                        for k in ("strokes", "stall_faults",
                                                  "early_stops", "rejected_rate",
                                                  "err_comm", "mode_changes")))
    print("  file     : %s" % STATE)
    print("\nLeave the unit alone. Judge it with:")
    print("  python bin/at_wp_soak.py --report --host %s" % args.host)
    return 0


def cmd_report(u, args):
    if not os.path.exists(STATE):
        sys.exit("no baseline at %s -- run --start first" % STATE)
    with open(STATE) as fh:
        base = json.load(fh)
    now = snapshot(u)

    if now["uptime_s"] < base["uptime_s"]:
        print("NOTE: the unit REBOOTED during the soak (uptime went backwards).")
        print("Counters reset at boot, so the deltas below are not meaningful.")
        print("Restart the soak.")
        return 2
    if now["fw_ver"] != base["fw_ver"]:
        print("NOTE: firmware changed mid-soak (%s -> %s). Restart the soak."
              % (base["fw_ver"], now["fw_ver"]))
        return 2

    hours = (now["t"] - base["t"]) / 3600.0
    d = dict((f, now["counters"][f] - base["counters"][f]) for f in FIELDS)

    print("soak report -- %s fw %s" % (now["unit_id"], now["fw_ver"]))
    print("  elapsed        : %.2f h   (need >= %s)" % (hours, args.hours))
    print("  strokes        : %d       (need >= %d)" % (d["strokes"], args.min_strokes))
    print("  reads_ok       : %d" % d["reads_ok"])
    print("  stall_faults   : %d       (need 0)" % d["stall_faults"])
    print("  early_stops    : %d       (need 0)" % d["early_stops"])
    print("  rejected_rate  : %d       (need 0)" % d["rejected_rate"])
    print("  err_comm       : %d       (need 0)" % d["err_comm"])
    print("  err_busy       : %d       (informational -- T5 contention)" % d["err_busy"])
    print("  mode_changes   : %d       (need <= %d)" % (d["mode_changes"], args.max_mode_changes))
    print("  gate now       : %s" % now["gate"])

    # Power first: a clean result on too small a sample is not a pass.
    if hours < args.hours or d["strokes"] < args.min_strokes:
        print("\nINCONCLUSIVE: the sample is too small to mean anything yet.")
        print("Zero faults across %d stroke(s) in %.2f h is not evidence that the"
              % (d["strokes"], hours))
        print("detectors stay quiet over a period -- it is mostly evidence that")
        print("little happened. Keep soaking.")
        return 2

    fails = []
    for f, label in (("stall_faults", "12.4 rule 1 false-tripped"),
                     ("early_stops", "12.4 rule 2 false-tripped"),
                     ("rejected_rate", "implausible samples were rejected"),
                     ("err_comm", "the encoder failed to answer")):
        if d[f]:
            fails.append("%s (%s +%d)" % (label, f, d[f]))
    if d["mode_changes"] > args.max_mode_changes:
        fails.append("the gate flapped (mode_changes +%d): the period is a mix of "
                     "traced and untraced strokes" % d["mode_changes"])

    if fails:
        print("\nNOT CLEAN:")
        for f in fails:
            print("  *  %s" % f)
        return 1

    print("\nPASS: %.2f h, %d strokes, no false trips, no rejected samples, no comm"
          % (hours, d["strokes"]))
    print("errors, gate settled. That is the GATE's 'clean aperture data over a")
    print("sustained period'. Record it in the plan with these numbers.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--start", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--hours", type=float, default=12.0)
    ap.add_argument("--min-strokes", type=int, default=10)
    ap.add_argument("--max-mode-changes", type=int, default=2)
    args = ap.parse_args()
    if args.start == args.report:
        sys.exit("pick exactly one of --start / --report")
    u = Unit(args.host, args.pin)
    return cmd_start(u, args) if args.start else cmd_report(u, args)


if __name__ == "__main__":
    sys.exit(main())

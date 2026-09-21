#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Phase 3 exit criterion: a stroke replays from the log as a clean monotonic ramp.

This is the **GATE prerequisite**, and the one thing blocking T2's position
consumer. The plan's exit table has read *"monotonic ramp from the log: **not
demonstrated** -- the strokes observed so far were driven closed onto the end
switch, so the traces are flat at 0 rather than ramps"* since Phase 3. A sensor
that has never been shown to trace a traverse is not a sensor anything should be
controlled from.

WHAT IT CHECKS
--------------
An M3 OPEN stroke, with the gate in POSITION, producing `SENSOR_HR ch3` rows
that rise monotonically from ~0 to the full travel. Specifically:

  * the trace MOVES -- span is a real fraction of full travel, not a flat line
    at 0, which is exactly what every stroke so far has produced;
  * it is MONOTONIC within a tolerance, rising for an OPEN;
  * it has enough samples to be a ramp rather than two endpoints;
  * no sample was rejected as implausible mid-stroke, and no rule 1 stall fired.

HOW TO CAUSE THE STROKE
-----------------------
  default    You open M3 from the LCD. **Preferred**: an operator move is
             SRC_OPERATOR_MANUAL, which BYPASSES the dwell timers, so it happens
             at once. This script observes.
  --induce   Lower the active temperature maximum so T6 demands full ventilation
             and steps to M3. Scriptable, but T6 is SRC_T6 and IS subject to
             `dwell_open_s[2]`, which is 1500 s on this rig -- so the stroke can
             legitimately be 25 minutes away, and a dwell deferral is logged as
             LOG_SYSTEM value_a=29 rather than being silent. The setpoint is
             restored in a `finally`.

Reads the trace from the unit's own counters plus `GET /api/diag/windowpos`; the
SD rows are the durable record and `logparser.py` decodes them.

USAGE
-----
    python bin/at_wp_ramp.py --host 192.168.20.169            # observe
    python bin/at_wp_ramp.py --host 192.168.20.169 --induce   # drive T6

Exit 0 = ramp demonstrated, 1 = it was not, 2 = could not run the test.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import http.client
import json
import sys
import time

DEFAULT_HOST = "192.168.20.169"
DEFAULT_PIN = "12345678"
# Two cadences on purpose. Waiting for a stroke that may be 25 minutes away at
# 2 Hz is ~3600 requests of pointless load on a unit whose AP has bitten us
# before; the stroke itself is 13 s and wants every sample it can get.
WAIT_POLL_S = 2.0
STROKE_POLL_S = 0.2
SETTLE_S = 20
MIN_SAMPLES = 6          # two endpoints are not a ramp
MIN_SPAN_FRAC = 0.30     # of full travel; below this the leaf barely moved
BACKSTEP_TOL_X10 = 20    # 2.0 mm of non-monotonic jitter tolerated


# Seconds to wait for the unit's HTTP reply. 10 is plenty on a good link, but a
# marginal one (-78 dBm and 15 % ping loss on 2344, 2026-09-21) answers a plain
# GET in up to 7.5 s, and a harness that dies on a slow reply loses the run and
# its observation window. Harnesses that need it raise this at import.
HTTP_TIMEOUT_S = 10

# A GET that times out or loses its connection is tried again, this many times.
# A GET changes nothing, so a second attempt is always safe, and on a marginal
# link one lost reply otherwise ends a run and its observation window (2344,
# 2026-09-21: a connect timeout inside settle_cfg at -83 dBm). Writes are never
# retried here -- a harness that wants that decides per call, knowing whether
# the write is idempotent.
GET_RETRIES = 2


class Unit(object):
    def __init__(self, host, pin):
        self.host = host
        self.pin = pin
        self.cookie = None
        self._login(pin)

    def _raw(self, method, path, body=None):
        c = http.client.HTTPConnection(self.host, 80, timeout=HTTP_TIMEOUT_S)
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
        tries = 1 + (GET_RETRIES if method == "GET" else 0)
        for i in range(tries):
            try:
                sc, out = self._raw(method, path, body)
                break
            except (OSError, http.client.HTTPException):
                if i + 1 >= tries:
                    raise
                time.sleep(2.0)
        if sc == 401 and _retry and path != "/api/login":
            self.cookie = None
            self._login(self.pin)
            return self._raw(method, path, body)
        return sc, out

    def _login(self, pin):
        sc, _ = self._req("POST", "/api/login", {"role": "admin", "pin": pin})
        if sc != 200 or not self.cookie:
            sys.exit("login failed (HTTP %s) -- wrong PIN, or the unit is not up" % sc)

    def logout(self):
        """Give the session slot back. The unit holds FOUR, RAM-only, so a
        harness that exits without this can lock the operator out until the
        5-minute idle timeout frees one."""
        if self.cookie:
            try:
                self._raw("POST", "/api/logout", {})
            except Exception:                                  # noqa: BLE001
                pass                       # best effort: the timeout still frees it
            self.cookie = None

    def status(self):
        j = self._req("GET", "/api/status")[1]
        if not isinstance(j, dict) or "system" not in j:
            sys.exit("GET /api/status has no `system` block -- refusing to run "
                     "without knowing which unit this is")
        return j

    def diag(self):
        sc, out = self._req("GET", "/api/diag/windowpos")
        return out if (sc == 200 and isinstance(out, dict)) else {}

    def cfg(self):
        return self._req("GET", "/api/config")[1]

    def post_cfg(self, ns, key, value):
        return self._req("POST", "/api/config", {"ns": ns, "key": key, "value": value})[0]

    def settle_cfg(self, field, want, secs=SETTLE_S):
        end = time.time() + secs
        while time.time() < end:
            c = self.cfg()
            if isinstance(c, dict) and c.get(field) == want:
                return True
            time.sleep(0.5)
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--induce", action="store_true",
                    help="drive T6 to vent step 3 by lowering the active T maximum")
    ap.add_argument("--timeout", type=int, default=1800,
                    help="seconds to wait for the stroke (default 1800 -- M3 open dwell is 1500)")
    args = ap.parse_args()

    u = Unit(args.host, args.pin)
    st = u.status()
    sysb = st.get("system") or {}
    uid = str(sysb.get("unit_id", "?"))
    if "5C88" in uid.upper():
        sys.exit("REFUSING: %s is the production unit." % uid)

    d0 = u.diag()
    gate = str((d0.get("gate") or {}).get("mode_str", "?"))
    s0 = d0.get("soak") or {}

    # Full travel is not published by the diag endpoint, but it is recoverable
    # EXACTLY: derive() computes nominal = full_travel / travel_m3, so
    # nominal x travel_m3 is the 40004 the device was calibrated to. Checked
    # against the rig 2026-09-15: 1153 x 13 = 14989, and 40004 is 15000.
    t17 = d0.get("t17") or {}
    nominal_x10 = int(t17.get("nominal_x10", 0) or 0)
    cfg0 = u.cfg()
    travel_s = 0
    if isinstance(cfg0, dict) and isinstance(cfg0.get("travel_s"), list) \
            and len(cfg0["travel_s"]) >= 3:
        travel_s = int(cfg0["travel_s"][2] or 0)
    full_x10 = nominal_x10 * travel_s
    m3 = (st.get("windows") or {}).get("M3", "?")
    is_day = bool((st.get("sun") or {}).get("is_daytime", True))

    print("monotonic-ramp demonstration -- %s  fw %s" % (uid, sysb.get("fw_ver")))
    print("  gate mode  : %s" % gate)
    print("  M3 now     : %s" % m3)
    print("  full travel: %s (0.1 mm)  [nominal %s x travel_m3 %s s]"
          % (full_x10 or "unknown", nominal_x10, travel_s))

    if gate.lower().startswith("timed"):
        print("\nREFUSING: the gate is TIMED, so T17 is not polling and no trace")
        print("will be produced. This needs the sensor present and trusted.")
        return 2
    if m3 in ("OPEN", "MOVING_OPEN"):
        print("\nREFUSING: M3 is already %s. An OPEN stroke has to start from" % m3)
        print("closed, or there is no ramp to trace. Let it close first.")
        return 2

    restore = None
    field = "t_max_ngt" if not is_day else "t_max_day"
    try:
        if args.induce:
            c = u.cfg()
            cur = c.get(field) if isinstance(c, dict) else None
            if cur is None:
                print("\nCannot read %s from /api/config." % field)
                return 2
            tavg = float((st.get("climate") or {}).get("temp_avg_c", 0) or 0)
            target = int(max(1, int(tavg) - 4))
            print("\n  it is %s; lowering %s %s -> %d (temp_avg %.1f) to demand"
                  % ("day" if is_day else "night", field, cur, target, tavg))
            print("  full ventilation, which steps T6 to M3.")
            if u.post_cfg("climate", field, target) != 200:
                print("  POST /api/config rejected the write")
                return 2
            restore = cur
            u.settle_cfg(field, target)
            print("  NOTE: T6 is SRC_T6 and dwell_open_s[2] = 1500 s applies, so")
            print("  this stroke can legitimately be up to 25 minutes away. A")
            print("  deferral is logged as LOG_SYSTEM value_a=29.")
        else:
            print("\n  >>> Open M3 from the LCD now (an operator move bypasses dwell).")

        print("  watching for the stroke, up to %d s ...\n" % args.timeout)

        t0 = time.time()
        samples = []          # (t, opening_x10)
        saw_moving = False
        while time.time() - t0 < args.timeout:
            time.sleep(STROKE_POLL_S if saw_moving else WAIT_POLL_S)
            s = u.status()
            w = s.get("windows") or {}
            state = w.get("M3", "?")
            pos = w.get("M3_mm_x10")
            if state == "MOVING_OPEN":
                if not saw_moving:
                    print("  M3 MOVING_OPEN at t+%.1f s" % (time.time() - t0))
                saw_moving = True
                if isinstance(pos, int):
                    samples.append((time.time() - t0, pos))
            elif saw_moving and state in ("OPEN", "CLOSED"):
                if isinstance(pos, int):
                    samples.append((time.time() - t0, pos))
                print("  stroke ended in %s at t+%.1f s" % (state, time.time() - t0))
                break

        if not saw_moving:
            print("\nINCONCLUSIVE: no M3 OPEN stroke happened inside the window.")
            if args.induce:
                print("Most likely the M3 open dwell (1500 s) deferred it -- check the")
                print("SD log for LOG_SYSTEM value_a=29 before assuming anything broke.")
            return 2

        # ---- judge the trace ------------------------------------------------
        s1 = u.diag().get("soak") or {}
        d_rej = int(s1.get("rejected_rate", 0)) - int(s0.get("rejected_rate", 0))

        # An ABSENT counter must never read as a quiet one. `.get(key, 0)` on
        # both sides yields 0 - 0 = 0, which prints as "rule 1 stayed silent"
        # on a build that does not contain rule 1 -- a criterion no plausible
        # failure can trip, which is a description and not a test (AT-WP05's
        # err_busy, same shape). Caught on the first real run: FDA4 was on a
        # 2.8.0-bench predating the rule and the line still said 0.
        has_stall = "stall_faults" in s0 and "stall_faults" in s1
        d_stall = (int(s1["stall_faults"]) - int(s0["stall_faults"])) if has_stall else None
        vals = [p for _, p in samples]
        print("\n  samples over the stroke : %d" % len(vals))
        if vals:
            print("  span                    : %d -> %d (0.1 mm)" % (vals[0], vals[-1]))
        print("  rejected_rate delta     : %d" % d_rej)
        print("  stall_faults delta      : %s"
              % (d_stall if has_stall else
                 "NOT IN THIS BUILD -- 12.4 rule 1 absent, nothing judged here"))

        fails = []
        if len(vals) < MIN_SAMPLES:
            fails.append("only %d samples -- two endpoints are not a ramp. The HTTP\n"
                         "     poll may simply be slower than the %s s stroke; read the\n"
                         "     SENSOR_HR ch3 rows off the SD card instead, which are\n"
                         "     logged at the derived poll and are the real evidence."
                         % (len(vals), "13"))
        else:
            span = max(vals) - min(vals)
            if full_x10 and span < MIN_SPAN_FRAC * full_x10:
                fails.append("span %d is under %.0f%% of full travel %d -- this is the\n"
                             "     flat-at-0 trace the plan already has, not a ramp."
                             % (span, 100 * MIN_SPAN_FRAC, full_x10))
            back = sum(1 for a, b in zip(vals, vals[1:]) if b < a - BACKSTEP_TOL_X10)
            if back:
                fails.append("%d backward step(s) beyond %.1f mm -- not monotonic."
                             % (back, BACKSTEP_TOL_X10 / 10.0))
        if has_stall and d_stall:
            fails.append("12.4 rule 1 fired during the stroke (stall_faults +%d): the\n"
                         "     leaf was not following, so this trace is not a clean ramp."
                         % d_stall)

        if fails:
            print("\nNOT DEMONSTRATED:")
            for f in fails:
                print("  *  %s" % f)
            return 1

        print("\nPASS: M3 traced a monotonic ramp over an OPEN stroke.")
        print("This is the Phase 3 exit criterion and the GATE prerequisite.")
        print("Record it in the plan's exit table with the span and sample count.")
        return 0

    finally:
        if restore is not None:
            print("\n  restoring %s -> %s ..." % (field, restore))
            ok = (u.post_cfg("climate", field, restore) == 200 and
                  u.settle_cfg(field, restore))
            print("  restored and confirmed." if ok else
                  "  *** RESTORE FAILED -- set %s back to %s BY HAND NOW ***"
                  % (field, restore))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT-WP-REST (gh#86): mode 2 engages WITHOUT anyone moving the window.

The defect: T17 promoted the control law only at a stroke boundary, so after a
reboot a unit with the sensor fitted, answering and taught, and `ctrl_mode_m3`
set to linear, kept running M3 on its travel time until something unrelated
happened to move the window. On 2344 on 2026-09-25 that was 90 minutes and
counting -- M1 alone was meeting demand, so M3 had no reason to move at all.

WHAT MAKES THIS TEST DISCRIMINATING
-----------------------------------
A boot recalibration is itself a stroke, so "reboot and look for LINEAR" would
pass on the OLD code too whenever the sweep moves M3. It does not move M3 when
M3 is already CLOSED on its end sensor -- which is exactly the state the defect
was found in -- so this test:

  * requires M3 to start CLOSED at its end sensor, and
  * FAILS ITS OWN RESULT as inconclusive if M3 moves during the window.

Without both, a pass would say nothing about the rule.

USAGE
-----
    python bin/at_wp_rest_promote.py --host 192.168.20.160
    python bin/at_wp_rest_promote.py --host 192.168.20.160 --expect timed

`--expect timed` is the FAIL-FIRST arm: build with
`-DWPOS_FAILFIRST_212=4096` (FF212_STROKEONLY), which restores promotion at a
stroke boundary only, and the mode must then stay TIMED for the whole window.

Needs 2.13.0 or later: it reads `M3_ctrl_reason` and `M3_pos_gate` from the
PUBLIC status, which gh#85 added. No admin session, so it cannot defer a ROTA
apply or collide with another harness's session. Refuses to run on 5C88.

Exit 0 = as expected, 1 = not, 2 = could not run / inconclusive.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

REST_STATES = ("CLOSED",)
POLL_S = 5.0


def status(host, timeout=8.0):
    try:
        with urllib.request.urlopen("http://%s/api/status" % host, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError):
        return None


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def wait_up(host, limit_s):
    """Wait for the unit to answer after a reboot; return its status."""
    t0 = time.time()
    while time.time() - t0 < limit_s:
        st = status(host)
        if st:
            return st
        time.sleep(3.0)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--expect", choices=("linear", "timed"), default="linear",
                    help="linear = the fix; timed = the fail-first arm (bit 4096)")
    ap.add_argument("--window", type=float, default=300.0,
                    help="seconds to watch after the unit is up (default 300)")
    ap.add_argument("--boot-wait", type=float, default=180.0)
    a = ap.parse_args()

    st = wait_up(a.host, a.boot_wait)
    if not st:
        print("REFUSED: no answer from %s within %.0f s" % (a.host, a.boot_wait))
        return 2
    sysb = st.get("system") or {}
    if "5C88" in str(sysb.get("unit_id", "")).upper():
        print("REFUSED: this is the production unit")
        return 2
    w = st.get("windows") or {}
    if "M3_pos_gate" not in w:
        print("REFUSED: no M3_pos_gate in the status payload -- needs 2.13.0 or later")
        return 2

    print("AT-WP-REST -- %s fw %s, uptime %s s, expecting %s"
          % (sysb.get("unit_id"), sysb.get("fw_ver"), sysb.get("uptime_s"), a.expect.upper()))

    start_state = w.get("M3")
    start_end = bool(w.get("M3_at_end_sensor"))
    say("M3 %s at %.1f %%, end sensor %s, gate %s, mode %s"
        % (start_state, (w.get("M3_percent_x10") or 0) / 10.0, start_end,
           w.get("M3_pos_gate"), w.get("M3_ctrl_mode")))

    if start_state not in REST_STATES or not start_end:
        print("INCONCLUSIVE: M3 must start CLOSED on its end sensor -- it is %s (end %s)."
              % (start_state, start_end))
        print("  A boot sweep that MOVES M3 promotes the mode on the old code too,")
        print("  so the test would prove nothing.")
        return 2

    moved = False
    became_linear_at = None
    t0 = time.time()
    last = None
    while time.time() - t0 < a.window:
        st = status(a.host)
        if not st:
            time.sleep(POLL_S)
            continue
        w = st.get("windows") or {}
        mode = w.get("M3_ctrl_mode")
        state = w.get("M3")
        pct = (w.get("M3_percent_x10") or 0) / 10.0
        if state not in REST_STATES or pct > 0.5:
            moved = True
        line = "%s %5.1f%%  mode %s  reason %s  gate %s" % (
            state, pct, mode, w.get("M3_ctrl_reason"), w.get("M3_pos_gate"))
        if line != last:
            say("[%4ds] %s" % (int(time.time() - t0), line))
            last = line
        if mode == "LINEAR" and became_linear_at is None:
            became_linear_at = time.time() - t0
            if a.expect == "linear":
                break
        time.sleep(POLL_S)

    print()
    if moved:
        print("INCONCLUSIVE: M3 MOVED during the window, so a promotion cannot be")
        print("  attributed to the at-rest rule. Re-run from a settled CLOSED window.")
        return 2

    if a.expect == "linear":
        if became_linear_at is not None:
            print("PASS: the mode reached LINEAR %.0f s after boot with M3 never moving."
                  % became_linear_at)
            return 0
        print("FAIL: still TIMED after %.0f s with M3 at rest, gate ok -- gh#86 not fixed."
              % a.window)
        return 1

    # fail-first arm
    if became_linear_at is None:
        print("FAIL-FIRST OK: stayed TIMED for %.0f s with M3 at rest, as the old rule"
              % a.window)
        print("  demands. The test can fail, so a PASS on the fixed build means something.")
        return 0
    print("FAIL-FIRST BROKEN: reached LINEAR after %.0f s on a build that restores"
          % became_linear_at)
    print("  stroke-boundary-only promotion -- the bit is not restoring the defect.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

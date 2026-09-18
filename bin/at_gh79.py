#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for gh#79: a wind override that starts during a long recalibration must not be lost.

WHAT IT CHECKS
--------------
While T2 runs its blocking CLOSE_ALL sweep (boot, STANDBY exit, motor-alarm
clearance) it does not read Q1. Until 2.9.2:
  - T6 kept posting through the sweep: its inhibit mask lacked
    EG1_BIT_CALIBRATING, so it queued up to three commands per wake into the
    8-deep Q1;
  - T3 posted the override's CMD_CLOSE_ALL once, non-blocking, unchecked.
A wind override that began after the queue had filled could therefore be
DROPPED. gh#79 predicted that T2 would then run T6's stale OPENs after the
sweep, so that M1 and M2 -- which have no close dwell -- opened while the
override was active, with nothing to close them until the wind dropped.

THE FIRST FAIL-FIRST RUN CORRECTED THE ISSUE (2026-09-18, 2.9.1)
-----------------------------------------------------------------
M1 and M2 did NOT open. The SD log showed T6's stale OPENs were queued, but
T2 deferred all three on the dwell timer: it timed every command drained
after the blocking sweep with the clock from BEFORE it, so each dwell looked
up to 176 s longer ("26 s remaining" for M1/M2 = their travel, "476 s" for M3
= sweep + close dwell). That stale clock masked the harm by accident; 2.9.2
refreshes it, which is safe only because T6 now pauses during a sweep.

The masking holds only for the sweep this test starts, the STANDBY exit: T2
runs it inside the Q1 drain, whose clock was read before it. The boot sweep
runs before T2's loop, and the motor-alarm recovery re-reads the clock after
its sweep, so on 2.9.1 those two let the stale OPENs run and the predicted
harm stands there (inferred from the code; this test cannot start either).

So the discriminating evidence is the SD log, not the windows alone:

  PASS  no `LOG_SYSTEM 29` OPEN-deferral row at the sweep's end (T6 posted
        nothing into the blocked queue), AND M1 and M2 stay closed after the
        sweep while the override is active.
  FAIL  such rows exist (T6 posted during the sweep -- 3 on 2.9.1), OR M1 or
        M2 opens while the override is active.

Run it on 2.9.1 first: it must FAIL there, by the deferral rows. On 2.9.2 it
must PASS.

HOW -- everything over the network, no operator at the rig
----------------------------------------------------------
 1. Make T6 want all three windows open. `cr_priority` -> 2 (the larger demand
    wins) makes a humidity demand of step 3 decide; if that is not enough, the
    active temperature maximum is lowered too. Wait until all three are OPEN.
 2. Lengthen the sweep: `travel_m3` -> 171, so the recalibration lasts 176 s.
 3. Start a recalibration: POST /api/mode "standby", then "automatic".
 4. RAISE_AT_S into the sweep -- after at least three T6 wakes, so a pre-2.9.2
    T6 has filled Q1 -- raise the override with the direction exclusion arc
    around the measured wind direction. No wind is needed at the emulator.
 5. Confirm the override arrived WHILE the sweep was still running, then watch
    M1 and M2 for WATCH_S after the sweep ends.
 Finally, always: restore `travel_m3`, `cr_priority`, the temperature maximum,
 the arc, and AUTOMATIC -- in that order, so T6 cannot reopen anything before
 the override clears.

EXPECTED SIDE EFFECT -- not a fault
-----------------------------------
With `travel_m3` at 171 on the rig's 13 s window, T17 sees M3 move about 13x
faster than nominal, rejects the samples as implausible and reports a rule-1
stall (ALARM ch6 param 249) for the sweep's M3 drive. That is this test's
doing. Do not start a soak baseline until the stage has finished and
`travel_m3` is back.

USAGE
-----
    python bin/at_gh79.py --host 192.168.20.160

Exit 0 = PASS, 1 = FAIL, 2 = could not run or could not judge.
Stdlib only, ASCII output.
"""

import argparse
import atexit
import csv
import http.client
import io
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from at_wp_ramp import Unit                                   # noqa: E402
from at_wp_teach_standby import public_status                 # noqa: E402

PASS, FAIL, INCONCLUSIVE = 0, 1, 2
WORD = {PASS: "PASS", FAIL: "FAIL", INCONCLUSIVE: "INCONCLUSIVE"}
DEFAULT_PIN = "12345678"
LONG_TRAVEL_S = 171          # production's value: a 176 s sweep
MARGIN_S = 5                 # T2's fixed drive margin
WATCH_S = 90                 # after the sweep ends
ARC_HALF_DEG = 60
T_MIN = {"t_max_day": 15, "t_max_ngt": 10}   # cfg_limits.h


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def flags(st):
    return ((st or {}).get("mode") or {}).get("flags") or []


def wins(st):
    w = (st or {}).get("windows") or {}
    return w.get("M1"), w.get("M2"), w.get("M3")


def verdict(code, why):
    print("\n%s: %s" % (WORD[code], why))
    return code


def iso(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(epoch))


def log_rows(u, host, unit, boot_epoch, lo_epoch, hi_epoch):
    """Rows from the live SD log between two moments, or None if not found.

    GET /api/log/files lists at most SD_MAX_FILES (30) names in directory
    order and silently drops the rest, which on a full card hides the newest
    file. So first probe the name the current boot's file must carry --
    <unit>_<YYYYMMDDhhmmss>.csv, stamped a second or so after boot -- and
    only then fall back to the listing."""
    def fetch(name):
        c = http.client.HTTPConnection(host, 80, timeout=120)
        try:
            c.request("GET", "/api/log/download?file=" + name, headers={"Cookie": u.cookie})
            r = c.getresponse()
            body = r.read()
            return body.decode("utf-8", "replace") if r.status == 200 and body else None
        finally:
            c.close()

    names = ["%s_%s.csv" % (unit, time.strftime("%Y%m%d%H%M%S", time.localtime(boot_epoch + dt)))
             for dt in range(0, 20)]
    sc, files = u._req("GET", "/api/log/files")
    listed = [n for n in ((files or {}).get("sd_files") or []) if n.startswith(unit + "_")] \
        if isinstance(files, dict) else []
    names += sorted(listed, key=lambda n: re.sub(r"\D", "", n)[-14:], reverse=True)[:3]
    lo, hi = iso(lo_epoch), iso(hi_epoch)
    seen = set()
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        raw = fetch(name)
        if not raw:
            continue
        rows = [r for r in csv.reader(io.StringIO(raw)) if len(r) >= 7 and r[0][:2] == "20"]
        if rows and rows[-1][0] >= hi:
            return [r for r in rows if lo <= r[0] <= hi], name
    return None, None


class Restore(object):
    """Every change this stage makes, undone on any exit -- only what was
    actually changed (each restore is an audit row), and in a fixed order:
    travel first (the rig's real window is 13 s), then what makes T6 want the
    windows open, and the exclusion arc LAST, so the override still holds T6
    off while its wishes are put back."""

    ORDER = {"travel_m3": 0, "cr_priority": 1, "t_max_day": 2, "t_max_ngt": 2,
             "dir_excl_low": 3, "dir_excl_high": 4}

    def __init__(self, u):
        self.u = u
        self.steps = {}      # key -> (ns, value, settle_field, settle_value)

    def remember(self, ns, key, value, field, want):
        """Call BEFORE changing `key`; the first call for a key wins."""
        self.steps.setdefault(key, (ns, value, field, want))

    def run(self):
        if not self.steps:
            return
        print("\n  restoring ...")
        ok_all = True
        for key in sorted(self.steps, key=lambda k: self.ORDER.get(k, 9)):
            ns, value, field, want = self.steps[key]
            label = key
            ok = self.u.post_cfg(ns, key, value) == 200 and self.u.settle_cfg(field, want)
            ok_all = ok_all and ok
            print("    %-26s -> %-10s %s" % (label, value, "ok" if ok else "*** FAILED ***"))
        self.steps = {}
        sc, _ = self.u._req("POST", "/api/mode", {"mode": "automatic"})
        print("    %-26s -> %-10s %s" % ("mode", "automatic", "ok" if sc == 200 else "*** FAILED ***"))
        if not ok_all:
            print("  *** RESTORE INCOMPLETE -- check travel_m3 FIRST: the rig's window is 13 s ***")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    a = ap.parse_args()
    print("AT gh#79 -- a wind override during a long recalibration, on %s" % a.host)

    u = Unit(a.host, a.pin)
    atexit.register(u.logout)
    rs = Restore(u)
    atexit.register(rs.run)

    # ---------------------------------------------------------------- preflight
    st = public_status(a.host) or {}
    sysb = st.get("system") or {}
    uid = str(sysb.get("unit_id", "?"))
    if "5C88" in uid.upper():
        sys.exit("REFUSING: %s is the production unit." % uid)
    print("unit %s  fw %s  assets %s" % (uid, sysb.get("fw_ver"), sysb.get("asset_version")))
    cfg = u.cfg()
    if not isinstance(cfg, dict):
        return verdict(INCONCLUSIVE, "GET /api/config failed")
    if flags(st):
        return verdict(INCONCLUSIVE, "the unit shows %s -- clear it first" % flags(st))
    if any(m not in ("OPEN", "CLOSED") for m in wins(st)):
        return verdict(INCONCLUSIVE, "a window is moving or unknown: %s" % (wins(st),))
    if not cfg.get("wind_prot_en"):
        return verdict(INCONCLUSIVE, "wind protection is disabled")
    wind = st.get("wind") or {}
    if wind.get("direction_avg_deg") is None:
        return verdict(INCONCLUSIVE, "no wind direction reading to aim the arc at")
    poll = int(cfg.get("poll_interval_s") or 30)
    travel = list(cfg.get("travel_s") or [])
    if len(travel) != 3:
        return verdict(INCONCLUSIVE, "travel_s unreadable: %s" % (travel,))
    sweep_s = LONG_TRAVEL_S + MARGIN_S
    raise_at = max(100, int(3.3 * poll))
    if raise_at + poll + 15 >= sweep_s:
        return verdict(INCONCLUSIVE, "poll interval %d s leaves no room to raise the override "
                                     "inside a %d s sweep" % (poll, sweep_s))
    day = bool((st.get("sun") or {}).get("is_daytime", True))
    tmax_key = "t_max_day" if day else "t_max_ngt"
    print("poll %d s | travel %s | %s %s | cr_priority %s | arc %s..%s | wind %s m/s from %s deg"
          % (poll, travel, tmax_key, cfg.get(tmax_key), cfg.get("cr_priority"),
             cfg.get("dir_excl_low"), cfg.get("dir_excl_high"),
             wind.get("speed_avg_ms"), wind.get("direction_avg_deg")))

    # ------------------------------------------- 1. T6 wants all three open
    say("making T6 want all three windows open: cr_priority -> 2")
    rs.remember("climate", "cr_priority", cfg.get("cr_priority"), "cr_priority", cfg.get("cr_priority"))
    if u.post_cfg("climate", "cr_priority", 2) != 200 or not u.settle_cfg("cr_priority", 2):
        return verdict(INCONCLUSIVE, "could not set cr_priority")
    t0 = time.time()
    lowered = False
    while time.time() - t0 < 420:
        m = wins(public_status(a.host))
        if all(x == "OPEN" for x in m):
            break
        if not lowered and time.time() - t0 > 90 and not all(x in ("OPEN", "MOVING_OPEN") for x in m):
            say("humidity alone is not enough: lowering %s to %d as well"
                % (tmax_key, T_MIN[tmax_key]))
            rs.remember("climate", tmax_key, cfg.get(tmax_key), tmax_key, cfg.get(tmax_key))
            u.post_cfg("climate", tmax_key, T_MIN[tmax_key])
            u.settle_cfg(tmax_key, T_MIN[tmax_key])
            lowered = True
        time.sleep(1.0)
    m = wins(public_status(a.host))
    if not all(x == "OPEN" for x in m):
        return verdict(INCONCLUSIVE, "T6 did not open all three windows within 7 min (%s): "
                                     "raise the emulated temperature or humidity" % (m,))
    say("all three windows OPEN")

    # ------------------------------------------------ 2. a long sweep
    long_travel = [travel[0], travel[1], LONG_TRAVEL_S]
    say("lengthening the sweep: travel_m3 -> %d (the sweep lasts %d s)" % (LONG_TRAVEL_S, sweep_s))
    rs.remember("motor", "travel_m3", travel[2], "travel_s", travel)
    if u.post_cfg("motor", "travel_m3", LONG_TRAVEL_S) != 200 or \
            not u.settle_cfg("travel_s", long_travel):
        return verdict(INCONCLUSIVE, "could not set travel_m3")

    # ------------------------------------------------ 3. start a recalibration
    say("recalibration: POST /api/mode standby, then automatic")
    u._req("POST", "/api/mode", {"mode": "standby"})
    time.sleep(2.0)
    u._req("POST", "/api/mode", {"mode": "automatic"})
    t_sweep = None
    t1 = time.time()
    while time.time() - t1 < 20:
        if "calibrating" in flags(public_status(a.host)):
            t_sweep = time.time()
            break
        time.sleep(0.25)
    if t_sweep is None:
        return verdict(INCONCLUSIVE, "no calibration sweep started after leaving STANDBY")
    say("sweep running; raising the override %d s in (after >= 3 T6 wakes)" % raise_at)

    # ------------------------------------------------ 4. raise the override
    while time.time() - t_sweep < raise_at:
        time.sleep(0.5)
    d = int((public_status(a.host) or {}).get("wind", {}).get("direction_avg_deg") or
            wind.get("direction_avg_deg"))
    lo, hi = (d - ARC_HALF_DEG) % 360, (d + ARC_HALF_DEG) % 360
    say("override: exclusion arc %d..%d around %d deg" % (lo, hi, d))
    rs.remember("wind", "dir_excl_low", cfg.get("dir_excl_low"), "dir_excl_low", cfg.get("dir_excl_low"))
    rs.remember("wind", "dir_excl_high", cfg.get("dir_excl_high"), "dir_excl_high", cfg.get("dir_excl_high"))
    u.post_cfg("wind", "dir_excl_high", hi)
    u.post_cfg("wind", "dir_excl_low", lo)
    during_sweep = None
    t2 = time.time()
    while time.time() - t2 < poll + 20:
        s = public_status(a.host) or {}
        if "wind_override" in flags(s):
            during_sweep = "calibrating" in flags(s)
            break
        time.sleep(0.25)
    if during_sweep is None:
        return verdict(INCONCLUSIVE, "the override did not appear within %d s" % (poll + 20))
    say("override active %.0f s into the sweep; sweep still running: %s"
        % (time.time() - t_sweep, "yes" if during_sweep else "NO"))
    if not during_sweep:
        return verdict(INCONCLUSIVE, "the override arrived after the sweep ended, so the queue "
                                     "was never tested")

    # ------------------------------------------------ 5. watch
    while "calibrating" in flags(public_status(a.host)):
        if time.time() - t_sweep > sweep_s + 60:
            return verdict(INCONCLUSIVE, "the sweep did not end")
        time.sleep(0.25)
    t_sweep_end = time.time()
    say("sweep ended %.0f s after it began; watching M1 and M2 for %d s" %
        (t_sweep_end - t_sweep, WATCH_S))
    opened = []
    t3 = time.time()
    while time.time() - t3 < WATCH_S:
        s = public_status(a.host) or {}
        m1, m2, m3 = wins(s)
        if "wind_override" in flags(s):
            for name, state in (("M1", m1), ("M2", m2)):
                if state in ("OPEN", "MOVING_OPEN") and name not in [o[0] for o in opened]:
                    opened.append((name, state, round(time.time() - t3, 1)))
                    say("%s is %s under the override, %.1f s after the sweep" %
                        (name, state, time.time() - t3))
        time.sleep(0.5)
    s = public_status(a.host) or {}
    say("end of watch: M1 %s, M2 %s, M3 %s, flags %s" % (wins(s) + (flags(s),)))

    # ------------------------------------------------ 6. the SD evidence
    boot_epoch = time.time() - int((s.get("system") or {}).get("uptime_s", 0))
    rows, fname = log_rows(u, a.host, uid, boot_epoch, t_sweep_end - 5, t_sweep_end + 20)
    if rows is None:
        return verdict(INCONCLUSIVE, "the SD log covering the sweep's end was not found, so "
                                     "whether T6 posted during the sweep is unknown")
    deferrals = [r for r in rows if r[1] == "SYSTEM" and r[5] == "29"]
    say("SD %s, sweep end %s: %d dwell-deferral row(s) %s"
        % (fname, iso(t_sweep_end), len(deferrals),
           ["M%s %+d s" % (r[3], int(r[6])) for r in deferrals]))

    if opened:
        return verdict(FAIL, "%s opened while the wind override was active"
                             % " and ".join(o[0] for o in opened))
    if deferrals:
        return verdict(FAIL, "T6 posted into Q1 while T2 was blocked in the sweep: %d stale "
                             "command(s) reached T2 afterwards and were deferred on the dwell "
                             "timer. The windows stayed closed only because of that" % len(deferrals))
    return verdict(PASS, "T6 posted nothing during the sweep, and M1 and M2 stayed closed "
                         "under an override raised mid-sweep")


if __name__ == "__main__":
    sys.exit(main())

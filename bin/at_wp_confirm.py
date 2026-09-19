#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for 2.10.0 (plan 5d): the verdict on every M3 drive, the travel check,
bit 4, and gh#78's rule 2.

WHICH BUILD
-----------
Every stage PASSES only on a normal 2.10.0 bench build, and must FAIL on the
fail-first build (-DWPOS_FAILFIRST_292, which restores 2.9.2's behaviour: no
verdicts, no travel check, the old rule 2, bit 4 ignored, start-up readings
used as evidence). GET /api/diag/windowpos reports which build it is
(gate.failfirst_292), and the verdict line says so. Run each stage on both.

HOW -- everything over the network, no operator at the rig
----------------------------------------------------------
M3 is moved by T6 and by recalibrations:
  - OPEN:  the active temperature maximum -> its minimum, and cr_priority -> 2
           (the larger demand wins), so T6 asks for step 3;
  - CLOSE: the maximum -> its top value, and cr_priority -> 0 (temperature
           first), so T6 steps down;
  - a recalibration: POST /api/mode standby, then automatic.
M3's two dwells are cut to 5 s for the run, so T6 is not held back for 25
minutes (a dwell already running keeps its old length, gh#51, so the first
move may wait for it). Everything changed is restored on exit, whatever
happens: travel_m3 first, because the rig's window is 13 s.

The judgement reads the diag counters and the SD log rows written during the
stage: ALARM ch6 params 249 (rule 1), 250 (rule 2), 251 (the verdict) and 252
(the travel check).

STAGES
------
  healthy     full OPEN, full CLOSE: both confirmed after a full traverse,
              no travel warning, no badge
  notreached  travel_m3 5 (a 10 s drive, ~12 s traverse): the OPEN is not
              reached and raises m3_not_confirmed; with 13 restored the next
              CLOSE is confirmed and clears it
  short       travel_m3 10: OPEN and CLOSE confirmed with the 'too short'
              warning each way; with 13 restored both clear
  long        travel_m3 171: OPEN and CLOSE confirmed with 'much longer than
              needed'; with 13 restored both clear. Rejected samples and a
              rule-1 row are EXPECTED here: T17 derives its rate limit from
              the wrong value
  lost        the sensor absent 3 s into an OPEN: the drive is not judged
              (sensor), and it still finishes on T2's timer
  reversal    OPEN, then a recalibration ~5 s in: the OPEN is not judged
              (interrupted), the close from part-way is confirmed, and rule 2
              stays silent (gh#78, part 1)
  atend       a recalibration of a closed M3: confirmed, already at the end
  race        M3 at rest OPEN, the position forced to read 0, then a CLOSE:
              rule 2 trips (gh#78, part 2)
  ends        both end sensors reported 3 s into an OPEN: the gate shuts with
              reason end_sensors and the drive is not judged

USAGE
-----
    python bin/at_wp_confirm.py --host 192.168.20.160 healthy
    python bin/at_wp_confirm.py --host 192.168.20.160 all

Exit 0 = PASS, 1 = FAIL, 2 = could not run or could not judge.
Stdlib only, ASCII output. Refuses to run on 5C88.
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
RIG_TRAVEL_S = 13
MOVE_LIMIT_S = 420          # a running 300 s dwell, a 30 s T6 wake, the drive
TEST_DWELL_S = 5
SETTLE_S = 4.0              # after a drive ends, before reading the log

# param 251, value_a magnitude
CONFIRMED_FULL, CONFIRMED, NOT_REACHED, NOT_JUDGED = 1, 2, 3, 4
NOTJ = {1: "interrupted", 2: "not at target", 3: "sensor", 4: "both ends", 5: "no reading"}


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def flags(st):
    return ((st or {}).get("mode") or {}).get("flags") or []


def m3(st):
    return ((st or {}).get("windows") or {}).get("M3")


class Restore(object):
    """Everything a stage changes, undone on exit in a fixed order: travel first
    (the rig's window is 13 s), then what steers T6, then the dwells."""

    ORDER = {"travel_m3": 0, "cr_priority": 1, "t_max_day": 2, "t_max_ngt": 2,
             "dwell_open_m3": 3, "dwell_close_m3": 3}

    def __init__(self, u):
        self.u = u
        self.steps = {}

    def remember(self, ns, key, value, field, want):
        self.steps.setdefault(key, (ns, value, field, want))

    def run(self):
        if not self.steps:
            return
        print("\n  restoring ...")
        ok_all = True
        for key in sorted(self.steps, key=lambda k: self.ORDER.get(k, 9)):
            ns, value, field, want = self.steps[key]
            ok = self.u.post_cfg(ns, key, value) == 200 and self.u.settle_cfg(field, want)
            ok_all = ok_all and ok
            print("    %-16s -> %-8s %s" % (key, value, "ok" if ok else "*** FAILED ***"))
        self.steps = {}
        if not ok_all:
            print("  *** RESTORE INCOMPLETE -- check travel_m3 FIRST: the rig's window is 13 s ***")


class Rig(object):
    def __init__(self, host, pin, log_file=None):
        self.host = host
        self.log_file = log_file
        self.u = Unit(host, pin)
        atexit.register(self.u.logout)
        self.rs = Restore(self.u)
        atexit.register(self.rs.run)
        atexit.register(self.clear)
        g = self.gate()
        if "failfirst_292" not in g:
            sys.exit("GET /api/diag/windowpos has no gate.failfirst_292 -- this is not a "
                     "2.10.0 bench build")
        self.failfirst = bool(g.get("failfirst_292"))
        self.cfg0 = self.u.cfg()
        if not isinstance(self.cfg0, dict):
            sys.exit("GET /api/config failed")

    # --- device views -------------------------------------------------------
    def status(self):
        return public_status(self.host) or {}

    def diag(self):
        return self.u.diag() or {}

    def gate(self):
        g = self.diag().get("gate")
        return g if isinstance(g, dict) else {}

    def soak(self):
        s = self.diag().get("soak")
        return s if isinstance(s, dict) else {}

    def inject(self, how):
        sc, out = self.u._req("POST", "/api/diag/windowpos", {"inject": how})
        if sc != 200 or not (isinstance(out, dict) and out.get("ok")):
            sys.exit("injection '%s' refused: HTTP %s %s" % (how, sc, out))
        say("injection -> %s" % how)

    def clear(self):
        try:
            self.u._req("POST", "/api/diag/windowpos", {"inject": "none"})
        except Exception:                                      # noqa: BLE001
            pass

    # --- configuration ------------------------------------------------------
    def set_cfg(self, ns, key, value, field=None, want=None):
        """Change one key, remembering its original for the restore."""
        field = field or key
        orig = self.cfg0.get(field)
        self.rs.remember(ns, key, self._orig_value(key, orig), field, orig)
        if want is None:
            want = value
        if self.u.post_cfg(ns, key, value) != 200 or not self.u.settle_cfg(field, want):
            sys.exit("could not set %s/%s = %s" % (ns, key, value))

    @staticmethod
    def _orig_value(key, orig):
        # travel_s / dwell_*_s are arrays in GET /api/config; the key is M3's.
        if isinstance(orig, list):
            return orig[2]
        return orig

    def travel(self, secs):
        t = list(self.cfg0.get("travel_s") or [0, 0, 0])
        t[2] = secs
        self.set_cfg("motor", "travel_m3", secs, "travel_s", t)
        say("travel_m3 -> %d" % secs)

    def dwells_short(self):
        o = list(self.cfg0.get("dwell_open_s") or [0, 0, 0])
        c = list(self.cfg0.get("dwell_close_s") or [0, 0, 0])
        if o[2] != TEST_DWELL_S:
            o2 = list(o); o2[2] = TEST_DWELL_S
            self.set_cfg("motor", "dwell_open_m3", TEST_DWELL_S, "dwell_open_s", o2)
        if c[2] != TEST_DWELL_S:
            c2 = list(c); c2[2] = TEST_DWELL_S
            self.set_cfg("motor", "dwell_close_m3", TEST_DWELL_S, "dwell_close_s", c2)

    def tmax_key(self):
        day = bool((self.status().get("sun") or {}).get("is_daytime", True))
        return "t_max_day" if day else "t_max_ngt"

    def limits(self, key):
        sc, lim = self.u._req("GET", "/api/config/limits")
        lo_hi = lim.get(key) if isinstance(lim, dict) else None
        if not (isinstance(lo_hi, list) and len(lo_hi) == 2):
            sys.exit("no limits for %s" % key)
        return lo_hi

    # --- moving M3 ----------------------------------------------------------
    def want(self, opened):
        k = self.tmax_key()
        lo, hi = self.limits(k)
        self.set_cfg("climate", k, lo if opened else hi)
        self.set_cfg("climate", "cr_priority", 2 if opened else 0)

    def wait_m3(self, target, limit_s=MOVE_LIMIT_S):
        t0 = time.time()
        seen_move = False
        while time.time() - t0 < limit_s:
            s = m3(self.status())
            if s and s.startswith("MOVING"):
                seen_move = True
            if s == target and seen_move:
                return time.time() - t0
            time.sleep(0.5)
        return None

    def move(self, opened, limit_s=MOVE_LIMIT_S):
        target = "OPEN" if opened else "CLOSED"
        if m3(self.status()) == target:
            say("M3 already %s" % target)
            return 0.0
        self.want(opened)
        say("waiting for T6 to drive M3 %s" % target)
        took = self.wait_m3(target, limit_s)
        if took is None:
            sys.exit("M3 did not reach %s within %d s (now %s)" % (target, limit_s,
                                                                    m3(self.status())))
        say("M3 %s after %.0f s" % (target, took))
        time.sleep(SETTLE_S)
        return took

    def recalibrate(self):
        say("recalibration: POST /api/mode standby, then automatic")
        self.u._req("POST", "/api/mode", {"mode": "standby"})
        time.sleep(1.5)
        self.u._req("POST", "/api/mode", {"mode": "automatic"})

    # --- the SD log ---------------------------------------------------------
    @staticmethod
    def clock():
        """Now, as the ISO local time the log rows carry. The PC's clock, not the
        unit's `time_iso`: that is T4's snapshot, up to 60 s stale, and would
        let the previous stage's last rows into this one. Both are NTP-synced
        and in the same time zone."""
        return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(time.time() - 1))

    def _download(self, name):
        c = http.client.HTTPConnection(self.host, 80, timeout=120)
        try:
            c.request("GET", "/api/log/download?file=" + name,
                      headers={"Cookie": self.u.cookie or ""})
            r = c.getresponse()
            body = r.read()
        finally:
            c.close()
        return body if (r.status == 200 and body) else None

    def rows_since(self, iso_from):
        """ALARM ch6 rows (params 248..252) written at or after iso_from.

        Finding the current file is harder than it should be:
          - GET /api/log/files lists at most 30 names in directory order and
            hides the rest, so the newest file may not be listed;
          - the name is the clock at boot, and if NTP had not synced by then
            the clock came from the DS1307 -- 5 h 42 min off on 2344 on
            2026-09-19, so a name derived from the real boot time missed it.
        So: --log-file if given; else the boot time by the PC's clock; else the
        newest listed names. Once found, the name is kept for the run."""
        st = self.status()
        sysb = st.get("system") or {}
        unit = str(sysb.get("unit_id", ""))
        names = [self.log_file] if self.log_file else []
        # The PC's clock minus the uptime, as bin/at_gh79.py does: `ts_unix` is
        # T4's snapshot and up to 60 s stale, which would miss the file name.
        boot = int(time.time()) - int(sysb.get("uptime_s", 0))
        names += ["%s_%s.csv" % (unit, time.strftime("%Y%m%d%H%M%S", time.localtime(boot + dt)))
                  for dt in range(-3, 20)]
        sc, files = self.u._req("GET", "/api/log/files")
        listed = [n for n in ((files or {}).get("sd_files") or [])
                  if isinstance(files, dict) and n.startswith(unit + "_")]
        names += sorted(listed, key=lambda n: re.sub(r"\D", "", n)[-14:], reverse=True)[:3]
        for name in names:
            body = self._download(name)
            if body is None:
                continue
            self.log_file = name
            out = []
            for row in csv.reader(io.StringIO(body.decode("utf-8", "replace"))):
                if len(row) < 7 or row[0][:2] != "20" or row[0] < iso_from:
                    continue
                if row[1] == "ALARM" and row[3] == "6":
                    out.append((row[0], int(row[4]), int(row[5]), int(row[6])))
            return out
        sys.exit("could not find this boot's SD log file (tried %d names): pass --log-file"
                 % len(names))


def verdicts(rows):
    return [(t, va, vb) for (t, p, va, vb) in rows if p == 251]


def show(rows):
    for t, p, va, vb in rows:
        if p in (249, 250, 251, 252):
            print("      %s  param %d  a=%d  b=%d" % (t[11:], p, va, vb))


def judge(rig, ok, why):
    tag = " [on the FAIL-FIRST build]" if rig.failfirst else ""
    code = PASS if ok else FAIL
    print("\n%s%s: %s" % (WORD[code], tag, why))
    return code


def delta(s1, s0, key):
    return int(s1.get(key, 0) or 0) - int(s0.get(key, 0) or 0)


# ---------------------------------------------------------------- the stages
def ensure_closed(rig):
    """M3 closed, AND T6 wanting it so: a stage that ended with T6 wanting M3
    open (the reversal's recalibration closes M3 while that wish stands) would
    otherwise reopen it under the next stage -- the first `all` run's `atend`
    recalibrated an OPEN in progress instead of a closed M3."""
    rig.want(False)
    if m3(rig.status()) != "CLOSED":
        rig.move(False)
    time.sleep(SETTLE_S)


def ensure_open(rig):
    rig.want(True)
    if m3(rig.status()) != "OPEN":
        rig.move(True)
    time.sleep(SETTLE_S)


def stage_healthy(rig):
    ensure_closed(rig)
    t0, s0 = rig.clock(), rig.soak()
    rig.move(True)
    rig.move(False)
    rows, s1 = rig.rows_since(t0), rig.soak()
    show(rows)
    v = verdicts(rows)
    full_open = [x for x in v if x[1] == CONFIRMED_FULL]
    full_close = [x for x in v if x[1] == -CONFIRMED_FULL]
    travel_rows = [r for r in rows if r[1] == 252]
    st = rig.status()
    bad_flags = [f for f in flags(st) if f.startswith("m3_")]
    ok = (full_open and full_close and not travel_rows and not bad_flags
          and delta(s1, s0, "not_reached") == 0)
    return judge(rig, bool(ok),
                 "OPEN confirmed-full %s, CLOSE confirmed-full %s, travel rows %d, "
                 "flags %s, confirmed +%d, not_reached +%d"
                 % ([x[2] for x in full_open], [x[2] for x in full_close], len(travel_rows),
                    bad_flags, delta(s1, s0, "confirmed"), delta(s1, s0, "not_reached")))


def stage_notreached(rig):
    ensure_closed(rig)
    rig.travel(5)
    t0, s0 = rig.clock(), rig.soak()
    rig.move(True)
    st_after = rig.status()
    rows_open = rig.rows_since(t0)
    rig.travel(RIG_TRAVEL_S)
    rig.move(False)
    rows, s1 = rig.rows_since(t0), rig.soak()
    show(rows)
    v = verdicts(rows_open)
    not_reached = [x for x in v if x[1] == NOT_REACHED]
    flagged = "m3_not_confirmed" in flags(st_after)
    cleared = "m3_not_confirmed" not in flags(rig.status())
    closes = [x for x in verdicts(rows) if x[1] in (-CONFIRMED, -CONFIRMED_FULL)]
    ok = not_reached and flagged and cleared and closes
    return judge(rig, bool(ok),
                 "OPEN not reached %s (opening 0.1 %%), badge raised %s, CLOSE confirmed %s, "
                 "badge cleared %s, not_reached +%d"
                 % ([x[2] for x in not_reached], flagged, [x[2] for x in closes], cleared,
                    delta(s1, s0, "not_reached")))


def travel_stage(rig, secs, want_state, label):
    ensure_closed(rig)
    rig.travel(secs)
    t0 = rig.clock()
    rig.move(True, MOVE_LIMIT_S + secs)
    rig.move(False, MOVE_LIMIT_S + secs)
    st_warn = rig.status()
    rows_warn = rig.rows_since(t0)
    rig.travel(RIG_TRAVEL_S)
    rig.move(True)
    rig.move(False)
    rows = rig.rows_since(t0)
    show(rows)
    raised = [r for r in rows_warn if r[1] == 252 and r[2] // 1000 == want_state]
    dirs = set("CLOSE" if r[3] < 0 else "OPEN" for r in raised)
    cleared = [r for r in rows if r[1] == 252 and r[2] // 1000 == 0]
    flag = "m3_travel_short" if want_state == 1 else "m3_travel_long"
    was_flagged = flag in flags(st_warn)
    now_clear = flag not in flags(rig.status())
    ok = dirs == {"OPEN", "CLOSE"} and was_flagged and len(cleared) >= 2 and now_clear
    return judge(rig, bool(ok),
                 "'%s' raised for %s, flag %s then cleared %s, cleared rows %d"
                 % (label, sorted(dirs) or "nothing", was_flagged, now_clear, len(cleared)))


def stage_short(rig):
    return travel_stage(rig, 10, 1, "too short")


def stage_long(rig):
    print("  NOTE: rejected samples and a rule-1 row are expected in this stage")
    return travel_stage(rig, 171, 2, "much longer than needed")


def during_open(rig, act, after_s=3.0):
    """Start an OPEN and call act() after_s into the drive."""
    ensure_closed(rig)
    rig.want(True)
    t0 = time.time()
    while time.time() - t0 < MOVE_LIMIT_S:
        if m3(rig.status()) == "MOVING_OPEN":
            break
        time.sleep(0.2)
    else:
        sys.exit("M3 did not start opening")
    time.sleep(after_s)
    act()


def stage_lost(rig):
    t0, s0 = rig.clock(), rig.soak()
    during_open(rig, lambda: rig.inject("absent"))
    took = rig.wait_m3("OPEN", 60)
    rig.clear()
    time.sleep(35)                          # the gate re-probes every 30 s
    rows, s1 = rig.rows_since(t0), rig.soak()
    show(rows)
    nj = [x for x in verdicts(rows) if x[1] == NOT_JUDGED and x[2] == 3]
    ok = nj and took is not None
    return judge(rig, bool(ok),
                 "OPEN not judged (sensor) %s, drive finished on the timer %s, "
                 "gate now %s" % (len(nj), took is not None, rig.gate().get("reason_str")))


REVERSE_AFTER_S = 2.0      # into the OPEN: with the 1.5 s STANDBY hop, a close from ~20 %


def stage_reversal(rig):
    """gh#78, part 1: the recalibration must reverse M3 early, so that its
    close starts well below ~58 % open. From higher up the old rule 2 does not
    reach ~0 inside half the traverse and cannot show its false trip -- the
    first run, reversing ~6.5 s in, left the fail-first build silent too."""
    t0, s0 = rig.clock(), rig.soak()
    during_open(rig, rig.recalibrate, after_s=REVERSE_AFTER_S)
    rig.wait_m3("CLOSED", 90)
    time.sleep(SETTLE_S)
    rows, s1 = rig.rows_since(t0), rig.soak()
    show(rows)
    v = verdicts(rows)
    interrupted = [x for x in v if x[1] == NOT_JUDGED and x[2] == 1]
    part_close = [x for x in v if x[1] == -CONFIRMED]
    early = delta(s1, s0, "early_stops")
    ok = interrupted and part_close and early == 0
    return judge(rig, bool(ok),
                 "OPEN interrupted %d, part-way CLOSE confirmed %s, early_stops +%d "
                 "(gh#78: must be 0)" % (len(interrupted), [x[2] for x in part_close], early))


def stage_atend(rig):
    ensure_closed(rig)
    time.sleep(SETTLE_S)
    t0, s0 = rig.clock(), rig.soak()
    rig.recalibrate()
    time.sleep(30)                          # the sweep: 26 s on the rig
    rows, s1 = rig.rows_since(t0), rig.soak()
    show(rows)
    at_end = [x for x in verdicts(rows) if x[1] == -CONFIRMED and x[2] == 0]
    ok = at_end and delta(s1, s0, "stall_faults") == 0 and delta(s1, s0, "early_stops") == 0
    return judge(rig, bool(ok),
                 "CLOSE confirmed at the end %d, at_end_exempt +%d, stall +%d, early +%d"
                 % (len(at_end), delta(s1, s0, "at_end_exempt"), delta(s1, s0, "stall_faults"),
                    delta(s1, s0, "early_stops")))


def stage_race(rig):
    ensure_open(rig)
    time.sleep(SETTLE_S)
    t0, s0 = rig.clock(), rig.soak()
    rig.inject("race")
    rig.move(False)
    rig.clear()
    rows, s1 = rig.rows_since(t0), rig.soak()
    show(rows)
    early = delta(s1, s0, "early_stops")
    return judge(rig, early >= 1,
                 "a full CLOSE with the position reading 0 throughout: early_stops +%d "
                 "(gh#78: must trip)" % early)


def stage_ends(rig):
    t0 = rig.clock()
    during_open(rig, lambda: rig.inject("ends"))
    time.sleep(1.5)
    reason = rig.gate().get("reason_str")
    fault = "sensor_fault_position" in flags(rig.status())
    took = rig.wait_m3("OPEN", 60)
    rig.clear()
    time.sleep(35)
    rows = rig.rows_since(t0)
    show(rows)
    nj = [x for x in verdicts(rows) if x[1] == NOT_JUDGED and x[2] == 4]
    ok = reason == "end_sensors" and fault and nj and took is not None
    return judge(rig, bool(ok),
                 "gate reason %s, sensor_fault_position %s, OPEN not judged (bit 4) %d, "
                 "drive finished %s, gate now %s"
                 % (reason, fault, len(nj), took is not None, rig.gate().get("reason_str")))


STAGES = [("healthy", stage_healthy), ("notreached", stage_notreached),
          ("short", stage_short), ("long", stage_long), ("lost", stage_lost),
          ("reversal", stage_reversal), ("atend", stage_atend), ("race", stage_race),
          ("ends", stage_ends)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--log-file", help="the current SD log file, when its name cannot be "
                                       "found (a boot before NTP synced names it by the DS1307)")
    ap.add_argument("stage", choices=[n for n, _ in STAGES] + ["all"])
    a = ap.parse_args()

    st = public_status(a.host) or {}
    sysb = st.get("system") or {}
    uid = str(sysb.get("unit_id", "?"))
    if "5C88" in uid.upper():
        sys.exit("REFUSING: %s is the production unit." % uid)
    if not sysb.get("ntp_synced"):
        print("the unit's clock is not NTP-synced -- its log rows carry the DS1307's time "
              "and cannot be matched to this run. Wait for the sync (retried every 5 min).")
        return INCONCLUSIVE
    rig = Rig(a.host, a.pin, a.log_file)
    g = rig.gate()
    print("AT 2.10.0 confirm -- unit %s fw %s, build %s, gate %s/%s, M3 %s"
          % (uid, sysb.get("fw_ver"), "FAIL-FIRST (WPOS_FAILFIRST_292)" if rig.failfirst
             else "normal", g.get("mode_str"), g.get("reason_str"), m3(st)))
    for bad in ("wind_override", "motor_alarm", "calibrating", "standby"):
        if bad in flags(st):
            print("the unit shows '%s' -- clear it first" % bad)
            return INCONCLUSIVE
    if str(g.get("reason_str")) != "ok":
        print("the sensor gate is not ok (%s) -- fit the sensor and switch it on first"
              % g.get("reason_str"))
        return INCONCLUSIVE
    if int((rig.cfg0.get("travel_s") or [0, 0, 0])[2]) != RIG_TRAVEL_S:
        print("travel_m3 is %s, not the rig's %d -- set it first"
              % ((rig.cfg0.get("travel_s") or [None] * 3)[2], RIG_TRAVEL_S))
        return INCONCLUSIVE
    rig.dwells_short()

    names = [n for n, _ in STAGES] if a.stage == "all" else [a.stage]
    results = []
    for name, fn in STAGES:
        if name not in names:
            continue
        print("\n--- stage %s ---" % name)
        results.append((name, fn(rig)))
    if len(results) > 1:
        print("\nsummary:")
        for name, code in results:
            print("  %-10s %s" % (name, WORD[code]))
    return max(code for _, code in results)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Teach acceptance: the M3 teach commits from EITHER end.

WHY THIS EXISTS
---------------
The first teach drove one traverse from a parked end. The sensor captures an
end only when its end sensor MAKES after arming, so that teach could only ever
capture the far end and never committed on its own. It looked fine from OPEN,
because T6 happened to reopen the window afterwards and made the second
capture by chance; from CLOSED it failed. Operator requirement (2026-09-16):
**direction must be irrelevant during any teach.** This test proves that on the
rig: two teaches that START AT OPPOSITE ENDS must both commit.

WHAT IT DOES
------------
  1. Teach from wherever M3 is now.
  2. Move M3 to the other end by starting a teach and ABORTING it as soon as
     its first leg has made an end sensor. T2 finishes that stroke, so M3 ends
     at the far end -- and the abort path is exercised on the way (it must
     leave nothing armed and the calibration unchanged).
  3. Teach again, now from the other end.

Each teach PASSES when it reaches `done` with 2 ends made, verdict VALID, bit 5
clear, a span >= 30 % of the ADC, it did not trip 12.4 rule 1 or 2, and T17's
`orphan_aborts` did not move -- our own teach, or our own abort, must never be
taken for an orphaned one.
The run PASSES when both teaches pass AND they started at different ends.
Each teach pauses automatic control (it holds STANDBY until this script's admin
session ends; `bin/at_wp_teach_standby.py` tests that), so T6 cannot move M3
between legs. The start end is read at each start anyway.

**THE WINDOW MOVES**: about five traverses on the dev rig (~2 min). Do not run
it on production without meaning to: there a traverse is ~3 minutes.

Not covered here: a teach started PART-WAY, because nothing on the web API can
stop M3 mid-travel. It is covered by the same code path as an end start whose
first leg moves (the leaf is off an end sensor when the leg begins).

USAGE
-----
    python bin/at_wp_teach.py --host 192.168.20.169

Exit 0 = pass, 1 = fail, 2 = could not run the test.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import sys
import time

# The script's own directory is on sys.path, so the shared client imports as-is.
from at_wp_ramp import Unit, DEFAULT_HOST, DEFAULT_PIN

POLL_S = 0.25
REST_S = 3.0              # M3 must be still this long before a run starts
ACTIVE = ("arming", "traversing", "committing")
SPAN_MIN_PCT = 30


def m3_state(u):
    return ((u.status().get("windows") or {}).get("M3")) or "?"


def comm(u):
    sc, c = u._req("GET", "/api/diag/commission")
    if sc != 200 or not isinstance(c, dict):
        sys.exit("GET /api/diag/commission -> HTTP %s: not a commissioning build, "
                 "or not an admin session" % sc)
    return c


def counters(u):
    c = u.diag().get("soak") or {}
    return {k: c.get(k) for k in ("stall_faults", "early_stops", "strokes", "orphan_aborts")}


def wait_rest(u, limit_s):
    """Wait until M3 has been OPEN or CLOSED for REST_S; return that state."""
    t_end = time.time() + limit_s
    since, last = None, None
    while time.time() < t_end:
        st = m3_state(u)
        if st in ("OPEN", "CLOSED"):
            if st != last:
                since, last = time.time(), st
            elif time.time() - since >= REST_S:
                return st
        else:
            since, last = None, None
        time.sleep(0.5)
    return None


def where(u):
    d = u.diag()
    return "T2 %s | sensor %.1f mm, end sensor %s" % (
        m3_state(u), (d.get("opening_mm_x10") or 0) / 10.0, d.get("at_end_sensor"))


def run_teach(u, limit_s, abort_after_first_end=False):
    """Start a teach and follow it. Returns a result dict."""
    before = comm(u)
    cnt0 = counters(u)
    start = m3_state(u)
    print("  start: %s" % where(u))
    print("  calibration before: %s..%s  verdict %s"
          % (before.get("taught_closed"), before.get("taught_open"), before.get("verdict")))
    sc, body = u._req("POST", "/api/diag/commission", {"action": "teach"})
    if sc != 200 or not (isinstance(body, dict) and body.get("ok")):
        c = comm(u)
        return {"ok": False, "start": start,
                "why": "refused: HTTP %s, state %s, reason %s"
                       % (sc, c.get("state"), c.get("run_reason"))}
    t0 = time.time()
    last, c = None, {}
    aborted = False
    while time.time() - t0 < limit_s:
        c = comm(u)
        key = (c.get("state"), c.get("leg"), c.get("dir"), c.get("ends"),
               c.get("teach_armed"), c.get("verdict"))
        if key != last:
            print("  t+%5.1fs %-10s leg %s dir %-5s ends %s armed %-5s verdict %s"
                  % ((time.time() - t0,) + key))
            last = key
        if abort_after_first_end and not aborted and (c.get("ends") or 0) >= 1:
            u._req("POST", "/api/diag/commission", {"action": "abort"})
            aborted = True
            print("  t+%5.1fs ABORT sent after the first end sensor" % (time.time() - t0))
            continue
        if c.get("state") not in ACTIVE:
            break
        time.sleep(POLL_S)
    dur = time.time() - t0
    # Let the verdict follow bit 5 after a disarm (the unit re-judges within a
    # few prompt readings), then read everything once more.
    time.sleep(3.0)
    after = comm(u)
    live = u.diag()
    cnt1 = counters(u)
    res = {
        "start": start, "dur": dur, "aborted": aborted,
        "state": after.get("state"), "reason": after.get("run_reason"),
        "legs": after.get("leg"), "ends": after.get("ends"),
        "verdict": after.get("verdict"), "cal_reason": after.get("cal_reason"),
        "armed_live": live.get("teach_armed"),
        "before": (before.get("taught_closed"), before.get("taught_open")),
        "after": (after.get("taught_closed"), after.get("taught_open")),
        "span_pct": after.get("span_pct"),
        "stall": (cnt0["stall_faults"], cnt1["stall_faults"]),
        "early": (cnt0["early_stops"], cnt1["early_stops"]),
        "orphan": (cnt0["orphan_aborts"], cnt1["orphan_aborts"]),
    }
    # Our own teach, and our own abort, must never be mistaken for an orphan.
    # (The first cut logged the operator abort as one; see commission.cpp.)
    orphan_ok = res["orphan"][0] is not None and res["orphan"][0] == res["orphan"][1]
    if aborted:
        res["ok"] = (res["state"] == "idle" and res["armed_live"] is False
                     and res["after"] == res["before"] and orphan_ok)
        res["why"] = "" if res["ok"] else (
            "abort left state %s, bit 5 %s, cal %s->%s, orphan_aborts %s->%s"
            % (res["state"], res["armed_live"], res["before"], res["after"],
               res["orphan"][0], res["orphan"][1]))
        return res
    fails = []
    if res["state"] != "done":
        fails.append("state %s (reason %s)" % (res["state"], res["reason"]))
    if res["ends"] != 2:
        fails.append("ends %s" % res["ends"])
    if res["verdict"] != "valid":
        fails.append("verdict %s (%s)" % (res["verdict"], res["cal_reason"]))
    if res["armed_live"] is not False:
        fails.append("bit 5 still %s" % res["armed_live"])
    if (res["span_pct"] or 0) < SPAN_MIN_PCT:
        fails.append("span %s %%" % res["span_pct"])
    if res["stall"][0] != res["stall"][1] or res["early"][0] != res["early"][1]:
        fails.append("rule 1/2 tripped: stall %s->%s early %s->%s"
                     % (res["stall"] + res["early"]))
    if not orphan_ok:
        fails.append("orphan_aborts %s->%s (our own teach taken for an orphan, "
                     "or a firmware without the counter)" % res["orphan"])
    res["ok"] = not fails
    res["why"] = "; ".join(fails)
    return res


def show(tag, r):
    print("  %s: %s" % (tag, "PASS" if r.get("ok") else "FAIL -- " + r.get("why", "")))
    if "dur" in r:
        print("    started %s, %.1f s, legs %s, ends %s, calibration %s..%s -> %s..%s (%s %% of range)"
              % (r["start"], r["dur"], r["legs"], r["ends"],
                 r["before"][0], r["before"][1], r["after"][0], r["after"][1], r["span_pct"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    a = ap.parse_args()

    u = Unit(a.host, a.pin)
    try:
        s = u.status()
        sysb = s.get("system") or {}
        print("unit %s  fw %s  assets %s" % (sysb.get("unit_id"), sysb.get("fw_ver"),
                                            sysb.get("asset_version")))
        cfg = u.cfg()
        travel = ((cfg.get("travel_s") or [0, 0, 171])[2]) if isinstance(cfg, dict) else 171
        # three legs of T2's full stroke, twice over, plus arming and settling
        limit = 3 * (travel + 5 + 2) * 2 + 30
        print("travel_m3 %s s -> run limit %d s" % (travel, limit))
        comm(u)                                    # refuses early on a release build

        if not wait_rest(u, limit):
            print("M3 did not come to rest -- not teaching")
            return 2

        print("\n[1] teach from where M3 is")
        r1 = run_teach(u, limit)
        show("teach 1", r1)

        print("\n[2] move M3 to the other end: teach, aborted after its first end sensor")
        if not wait_rest(u, limit):
            print("M3 did not come to rest -- stopping")
            return 2
        rf = run_teach(u, limit, abort_after_first_end=True)
        show("abort", rf)
        end_now = wait_rest(u, limit)
        print("  M3 now %s" % end_now)

        print("\n[3] teach from the other end")
        r2 = run_teach(u, limit)
        show("teach 2", r2)

        print("\n--- verdict ---")
        opposite = {r1.get("start"), r2.get("start")} == {"OPEN", "CLOSED"}
        print("teaches started at opposite ends : %s (%s, %s)"
              % ("yes" if opposite else "NO", r1.get("start"), r2.get("start")))
        ok = r1.get("ok") and r2.get("ok") and rf.get("ok") and opposite
        print("RESULT: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        u.logout()
        print("(logged out)")


if __name__ == "__main__":
    sys.exit(main())

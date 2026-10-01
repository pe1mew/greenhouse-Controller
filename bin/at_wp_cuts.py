#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""The AT-WP02 scatter study: split each targeted M3 stop into what
extrapolation could remove and what it could not (2026-10-01).

WHY
---
AT-WP02 passes or fails by chance on the rig: each stop scatters by
sigma 0.55-0.75 %, so ten stops span ~2-2.5 % against the 2.0 % allowed
(design/integrateWindowPositionSensor.md section 0, item 4). The proposed cure
is for T2 to extrapolate between T17's readings instead of cutting on the
first sample past its aim. That can only remove the part of the error that
comes from WHEN that sample was taken. The leaf's run-on after the cut is out
of its reach. So before building it, measure the split.

The bench build keeps a log of every targeted stop (T2, MODBUS_BENCH only):
GET /api/diag/windowpos?cuts. Run bin/at_wp02.py for the stops, then this.

EACH STOP, IN % OF STROKE (+ = further in the direction of travel)
  sample   cut - aim          how far past the aim the sample T2 cut on landed.
                              T17 reads about every 180 ms on the rig and the
                              encoder publishes every 100 ms, so this runs from
                              0 to about two windows' travel (~2 %)
  age      v x cut_age        how far the leaf moved between that sample and
                              the moment T2 acted (T2 ticks every 20 ms)
  run-on   rest - cut - age   how far the leaf went after the relay was cut
  error    rest - want        = sample + age + run-on - lead
v is the leaf's speed from the two samples before the cut (the device's own
rate when there is no earlier sample). `rest` is the first reading sampled a
second or more after the cut: T17's settle read, the one T2 learns its lead
from.

THE PREDICTION
Today's scatter is taken from the overrun past the aim (sample + age +
run-on), which the lead does not enter, so the lead's learning in the first
stops after a boot does not inflate it. The error as measured is printed too.
An extrapolating T2 would cut within one of its own 20 ms ticks of the aim:
sample + age becomes ~uniform(0, v x 20 ms) and the run-on stays. So each
direction's scatter with extrapolation is the run-on's sigma with that tick
added in quadrature. Ten stops drawn from a normal distribution span about
3.08 sigma on average; that is the figure to hold against AT-WP02's 2.0 %.

USAGE
  python bin/at_wp_cuts.py --host 192.168.20.160 --save cuts.json   # fetch, merge, report
  python bin/at_wp_cuts.py --load cuts.json                          # report only
  add --want 500 to judge only the stops to one target (at_wp02 uses 50 %)

The saved file keeps every fetch: records are keyed by the boot they came
from, so fetching after each run loses nothing when the ring (32) wraps.
"""
import argparse
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

DEFAULT_PIN = "12345678"
T2_TICK_MS = 20.0
RANGE_OF_TEN = 3.078          # E[max - min] of ten draws from N(0, 1)


def fetch(host, pin):
    from at_wp_ramp import Unit                 # noqa: E402 (only needed to fetch)
    u = Unit(host, pin)
    try:
        st = u.status() or {}
        sysb = st.get("system") or {}
        if "bench" not in str(sysb.get("fw_ver", "")):
            sys.exit("needs a bench build: the stop log is bench-only")
        sc, log = u._req("GET", "/api/diag/windowpos?cuts")
        if sc != 200 or not isinstance(log, dict) or "cuts" not in log:
            sys.exit("GET /api/diag/windowpos?cuts gave no stop log (HTTP %s): "
                     "a bench build from before 2026-10-01?" % sc)
        sc2, com = u._req("GET", "/api/diag/commission")
        window_mm = com.get("window_mm") if (sc2 == 200 and isinstance(com, dict)) else None
    finally:
        u.logout()
    boot = int(round((time.time() - float(sysb.get("uptime_s", 0))) / 10.0) * 10)
    return {"unit": sysb.get("unit_id"), "fw": sysb.get("fw_ver"), "boot": boot,
            "window_mm": window_mm, "seq": log.get("seq"), "cuts": log.get("cuts") or []}


def merge(store, got):
    """Key every record by (unit, boot, n); a later fetch may complete one."""
    recs = dict(((r["unit"], r["boot"], r["n"]), r) for r in store.get("records", []))
    new = 0
    for c in got["cuts"]:
        r = dict(c, unit=got["unit"], boot=got["boot"], fw=got["fw"],
                 window_mm=got["window_mm"])
        k = (r["unit"], r["boot"], r["n"])
        if k not in recs:
            new += 1
        if k not in recs or (r.get("rested") and not recs[k].get("rested")):
            recs[k] = r
    store["records"] = sorted(recs.values(), key=lambda r: (r["unit"], r["boot"], r["n"]))
    return new


def split(r):
    """One stop in % of stroke, or None when it cannot be judged."""
    if not r.get("rested") or not r.get("window_mm"):
        return None
    w = float(r["window_mm"])
    pct = lambda mm_x10: mm_x10 / w * 10.0          # 0.1 mm -> % of the window
    sgn = 1.0 if r["o"] else -1.0
    cut, rest = pct(r["cut_mm"]), pct(r["rest_mm"])
    if r.get("prev", -1) >= 0 and r.get("prev_dt", 0) > 0:
        v = sgn * (cut - pct(r["prev_mm"])) / float(r["prev_dt"])      # %/ms
        v_src = "samples"
    else:
        v = sgn * (r["cut_rate"] / 10.0) / w * 100.0 / 1000.0
        v_src = "device"
    aim, want = r["aim"] / 10.0, r["want"] / 10.0
    s = {
        "n": r["n"], "o": r["o"], "want": want, "v": v, "v_src": v_src,
        "dt": r.get("prev_dt", 0), "age_ms": r["cut_age"],
        "sample": sgn * (cut - aim),
        "age": v * r["cut_age"],
        "lead": sgn * (want - aim),
        "error": sgn * (rest - want),
    }
    s["runon"] = sgn * (rest - cut) - s["age"]
    # how far the reported percent strays from the mm it came from
    s["pct_check"] = abs(r["cut"] / 10.0 - cut)
    return s


def mean(xs):
    return sum(xs) / float(len(xs)) if xs else float("nan")


def sd(xs):
    if len(xs) < 2:
        return float("nan")
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def corr(xs, ys):
    if len(xs) < 3:
        return float("nan")
    mx, my = mean(xs), mean(ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    return sxy / (sx * sy) if sx and sy else float("nan")


def pooled(groups):
    """sigma about each group's own mean, pooled over the groups."""
    num = sum((len(g) - 1) * sd(g) ** 2 for g in groups if len(g) > 1)
    den = sum(len(g) - 1 for g in groups if len(g) > 1)
    return math.sqrt(num / den) if den else float("nan")


def report(store, want=None):
    rows = [split(r) for r in store.get("records", [])]
    skipped = sum(1 for x in rows if x is None)
    rows = [x for x in rows if x is not None and (want is None or abs(x["want"] - want / 10.0) < 0.05)]
    print("stops judged: %d%s  (not rested or no window size: %d)"
          % (len(rows), "" if want is None else " to %.1f %%" % (want / 10.0), skipped))
    if not rows:
        return 1
    worst_pct = max(x["pct_check"] for x in rows)
    if worst_pct > 0.15:
        print("WARNING: percent and mm disagree by up to %.2f %% -- is window_mm right?" % worst_pct)

    print("\n  n   dir    v %/s  dt ms  age ms   sample    age   run-on   lead    error")
    for x in rows:
        print("%4d  %-5s %6.2f  %5d  %6d   %6.2f  %5.2f   %6.2f  %5.2f   %6.2f%s"
              % (x["n"], "open" if x["o"] else "close", x["v"] * 1000.0, x["dt"],
                 x["age_ms"], x["sample"], x["age"], x["runon"], x["lead"], x["error"],
                 "" if x["v_src"] == "samples" else "  (v from the device)"))

    groups = {}
    for x in rows:
        groups.setdefault("open" if x["o"] else "close", []).append(x)
    print("\nper direction (mean / sigma, % of stroke):")
    print("  dir     n   v %/s   sample          age             run-on          error")
    for d in ("open", "close"):
        g = groups.get(d, [])
        if not g:
            continue
        col = lambda k: (mean([x[k] for x in g]), sd([x[k] for x in g]))
        print("  %-5s %3d  %6.2f   %5.2f / %4.2f   %5.2f / %4.2f   %5.2f / %4.2f   %+5.2f / %4.2f"
              % ((d, len(g), mean([x["v"] for x in g]) * 1000.0) + col("sample") + col("age")
                 + col("runon") + col("error")))

    gl = [g for g in groups.values() if g]
    err_meas = pooled([[x["error"] for x in g] for g in gl])
    # The overrun past the aim does not involve the lead, so its sigma is the
    # scatter at a settled lead even while the lead is still learning.
    err_now = pooled([[x["sample"] + x["age"] + x["runon"] for x in g] for g in gl])
    q_now = pooled([[x["sample"] + x["age"] for x in g] for g in gl])
    run = pooled([[x["runon"] for x in g] for g in gl])
    tick = mean([x["v"] for x in rows]) * T2_TICK_MS / math.sqrt(12.0)
    err_ext = math.sqrt(run ** 2 + tick ** 2)
    all_err = [x["error"] for x in rows]

    print("\nsigma, about each direction's own mean (the lead takes the means away):")
    print("  error as measured             %.2f %%   (the lead still learns for a few stops"
          " after a boot)" % err_meas)
    print("  error at a settled lead       %.2f %%   ten stops span ~%.2f %%"
          % (err_now, RANGE_OF_TEN * err_now))
    print("    of which sample + age       %.2f %%   (what extrapolation removes)" % q_now)
    print("    of which run-on             %.2f %%   (what it cannot)" % run)
    print("  error with extrapolation      %.2f %%   ten stops span ~%.2f %%  (run-on + a 20 ms tick)"
          % (err_ext, RANGE_OF_TEN * err_ext))
    print("  all stops about one mean      %.2f %%   (as AT-WP02 sees them, hysteresis included;"
          " spread %.2f %%)" % (sd(all_err), max(all_err) - min(all_err)))
    print("\nrun-on against speed: r = %.2f;  against the sample's overshoot: r = %.2f"
          % (corr([x["runon"] for x in rows], [x["v"] for x in rows]),
             corr([x["runon"] for x in rows], [x["sample"] for x in rows])))
    if math.isnan(err_now) or math.isnan(err_ext):
        print("too few stops per direction to estimate a sigma: run more")
        return 1
    print("expected ten-stop spread: now %.2f %%, with extrapolation %.2f %% "
          "(AT-WP02 allows 2.0 %%)" % (RANGE_OF_TEN * err_now, RANGE_OF_TEN * err_ext))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host")
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--save", help="merge the fetch into this JSON file")
    ap.add_argument("--load", help="report from this JSON file without fetching")
    ap.add_argument("--want", type=int, help="only stops to this target, 0.1 %% units")
    a = ap.parse_args()

    if a.load:
        with open(a.load, encoding="utf-8") as fh:
            store = json.load(fh)
    else:
        if not a.host:
            sys.exit("give --host to fetch, or --load FILE")
        store = {}
        if a.save and os.path.exists(a.save):
            with open(a.save, encoding="utf-8") as fh:
                store = json.load(fh)
        got = fetch(a.host, a.pin)
        new = merge(store, got)
        print("unit %s, fw %s: %d stops since boot, %d kept by the unit, %d new here"
              % (got["unit"], got["fw"], got["seq"] or 0, len(got["cuts"]), new))
        if a.save:
            with open(a.save, "w", encoding="utf-8") as fh:
                json.dump(store, fh, indent=1)
    return report(store, a.want)


if __name__ == "__main__":
    sys.exit(main())

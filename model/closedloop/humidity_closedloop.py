"""humidity_closedloop.py -- what cr_priority does to the greenhouse, mode 1 and mode 2.

The closed loop (primary plant, 5C88's logged weather, 2026-06-05..09-16,
today's firmware emulation) under each priority, in both modes, and on the
manual's tomato row. It prints the heat the priority costs by day and the
cold it costs by night, from the same runs. Prints the tables in
humidityControl.md, "Mode 2, and what each priority costs":

    python model/closedloop/humidity_closedloop.py        (about 20 minutes)

Humidity comes from the log (--rh-from-log): the plant's humidity model is too
weak to decide on. That is the main limitation. The logged RH came from a house
whose windows did what 5C88 did, not what the simulated house does, so once a
simulated house shuts, its humidity input no longer answers. humidity_audit.py's
shut-house section is why that is a fair proxy: in a warm shut house RH moved
by -1 % per 20 minutes, while absolute humidity rose.

The plant is calibrated to about 42 degC -- the baseline's peak is 42.6
against 42.2 logged. A run that goes past that is extrapolating, and its
"hottest" figure says only that it left the observed range.
"""
import sys
import json
import bisect
from datetime import timedelta
from argparse import Namespace
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import closed_loop as cl
import dataset
import law_compare as lc
import logdata

MODE2 = ["wpos_fitted_m3=1", "ctrl_mode_m3=1"]
# The manual's tomato row (boer and beheerder, Klimaat-startinstellingen per
# gewas): T day 18-26, night 16-18; RH day 60-75, night 65-80; CR-prio RH.
TOMATO = ["t_max_day=26", "t_max_ngt=18", "rh_max_day=75", "rh_max_ngt=80",
          "rh_min_day=60", "rh_min_ngt=65"]
RUNS = [
    ("mode 1, p0", "stepped", ["cr_priority=0"]),
    ("mode 1, p1", "stepped", ["cr_priority=1"]),
    ("mode 2, p0", "graded", MODE2 + ["cr_priority=0"]),
    ("mode 2, p1", "graded", MODE2 + ["cr_priority=1"]),
    ("mode 2, p2", "graded", MODE2 + ["cr_priority=2"]),
    ("tomato, p0", "graded", MODE2 + TOMATO + ["cr_priority=0"]),
    ("tomato, p1", "graded", MODE2 + TOMATO + ["cr_priority=1"]),
]
T_HOT = 31


def one(ds, lo, hi, plant, model, sets):
    args = Namespace(model=model, warmup_h=6.0, rh_from_log=True,
                     calibrator_hold=False, openness="state", t3="sim",
                     daynight="sim", firmware="current", config=None,
                     set=list(sets), plant2=str(plant),
                     m3_span_mm=1500, m3_airflow_exp=1.0)
    params = json.loads(Path(plant).read_text())["params"]
    recs, act, ctl = cl.run_closed_loop(ds, lo, hi, "two", params, args,
                                        cl.schedule_from_args(args))
    starts = [c.n.starts - b.starts for c, b in zip(act.ch, act.n0)]
    starts += [ctl.dropped - ctl.n0[0], ctl.deferred - ctl.n0[1],
               (act.ch[2].n.run_ms - act.n0[2].run_ms) / 60000.0]
    # law_compare's "mean opening" reads o3_log, which only a linear-M3 run
    # records; mode 1's M3 is open or shut, and that is all it needs.
    for r in recs:
        r.setdefault("o3_log", 1.0 if cl._code(r["bm_log"], 2) != 0 else 0.0)
    return recs, starts


def by_day(recs):
    """Heat, and the hours the law shut the house while the temperature asked to vent."""
    step_h = (recs[1]["t"] - recs[0]["t"]).total_seconds() / 3600.0
    span = (recs[-1]["t"] - recs[0]["t"]).total_seconds() / 86400.0
    c = Counter()
    longest = run = 0.0
    day_max = {}
    for r in recs:
        st, s, t = r.get("step_t"), r.get("step"), r["T_sim"]
        d = r["t"].date()
        day_max[d] = max(day_max.get(d, -99.0), t)
        if st is not None and s is not None and st > 0 and s == 0:
            c["dry"] += step_h
            if st >= 2:
                c["dry_m3"] += step_h
            run += step_h
            longest = max(longest, run)
        else:
            run = 0.0
        for lim in (33, 35):
            if t >= lim:
                c[lim] += step_h
    v = list(day_max.values())
    return {
        "hours >= 33 degC a day": c[33] / span,
        "hours >= 35 degC a day": c[35] / span,
        "hottest simulated, degC": max(v),
        "mean daily max, degC": sum(v) / len(v),
        "dry-close hours a day": c["dry"] / span,
        "  of it with T at step 2+": c["dry_m3"] / span,
        "longest dry-close episode, h": longest,
    }


def by_night(recs, is_day):
    step_h = (recs[1]["t"] - recs[0]["t"]).total_seconds() / 3600.0
    span = (recs[-1]["t"] - recs[0]["t"]).total_seconds() / 86400.0
    mins = defaultdict(lambda: 99.0)
    c = Counter()
    for r in recs:
        if is_day(r["t"]):
            continue
        t = r["T_sim"]
        night = (r["t"] - timedelta(hours=12)).date()
        mins[night] = min(mins[night], t)
        for lim in (14, 12):
            if t < lim:
                c[lim] += step_h
        if cl._code(r["bm_sim"], 2) != 0:
            c["m3"] += step_h
    v = list(mins.values())
    return {
        "mean nightly minimum, degC": sum(v) / len(v),
        "coldest night, degC": min(v),
        "night hours below 14 degC a day": c[14] / span,
        "night hours below 12 degC a day": c[12] / span,
        "M3 open at night, h a day": c["m3"] / span,
    }


def table(cols, keys, title):
    width = max(len(k) for k in keys) + 2
    print("")
    print(title)
    head = "%-*s" % (width, "")
    for label, _ in cols:
        head += " %11s" % label
    print(head)
    for k in keys:
        line = "%-*s" % (width, k)
        for _, m in cols:
            line += " %11s" % lc.fmt(m.get(k))
        print(line)


def main():
    plant = lc.PLANTS["primary"]
    ds = dataset.build()
    start, end = cl._parse_local("2026-06-05"), cl._parse_local("2026-09-16", end=True)
    lo, hi = bisect.bisect_left(ds.t, start), bisect.bisect_left(ds.t, end)
    log = logdata.load_sd_logs([str(HERE.parent / "campaign-summer-2026" / "*.log")])
    print("=== cr_priority in the closed loop: primary plant, %s .. %s, RH from the log ==="
          % (ds.t[lo], ds.t[hi - 1]))

    cols, north = [], None
    for label, model, sets in RUNS:
        recs, starts = one(ds, lo, hi, plant, model, sets)
        if north is None:
            north = lc.north_days(lc.full_days(recs))
        m = lc.metrics(recs, starts, "bm_sim", T_HOT, north)
        m.update(by_day(recs))
        m.update(by_night(recs, log.is_day))
        cols.append((label, m))
        print("  ran %-11s %-8s %s" % (label, model, " ".join(sets)), flush=True)

    table(cols, ["hours >= %d degC a day" % T_HOT, "hours >= 33 degC a day",
                 "hours >= 35 degC a day", "hottest simulated, degC",
                 "mean daily max, degC", "daytime mean, degC",
                 "dry-close hours a day", "  of it with T at step 2+",
                 "longest dry-close episode, h", "swing, degC",
                 "M3 drives a day", "M3 open hours a day", "M1+M2 drives a day"],
          "-- by day --")
    table(cols, ["mean nightly minimum, degC", "coldest night, degC",
                 "night hours below 14 degC a day", "night hours below 12 degC a day",
                 "M3 open at night, h a day"],
          "-- by night --")


if __name__ == "__main__":
    main()

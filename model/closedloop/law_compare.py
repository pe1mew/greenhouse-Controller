"""
law_compare.py -- a mode 2 law against the stepped law: the summer, both plants, the airflow range.

    python model/closedloop/law_compare.py [--law graded] [--start 2026-06-05] [--end 2026-09-16]
                                          [--airflow 1,0.5,2] [--plants primary,second]
                                          [--define NAME=VALUE ...]

--define tunes a candidate without editing it: each NAME's #define in
drivers/ventModel/src is replaced in a copy built under build/variants/.

For each M3 airflow exponent and each adopted plant, it closes the loop twice
over the same logged weather, both with today's firmware (--firmware current):

  stepped   the binary law, M3 on its timer (mode 1)
  LAW       the candidate, M3 with its wire sensor (wpos_fitted_m3 = 1, mode 2)

and prints, beside what 5C88 logged, what the greenhouse would have felt and
what the motors would have done:

  hours >= 31 degC      a day, at or above where the stepped law opens M3
  daytime mean          08-19 h
  swing                 the campaign's: the median over days of the T range
                        between successive M3 openings (closed_loop.reproduce),
                        and split by north wind (M3's side) and other wind
  fluctuation           the median over days of the daytime spread of T around
                        its own 60-min average: the limit cycle, whether or
                        not M3 ever shuts
  M3 drives, openings   a day; contract s.7 asks for the drives
  M3 motor minutes      a day, relay on: a short partial move costs a start
                        but little running time, so both are shown
  M3 open hours         a day, not shut; contract s.5 asks for M3's open time
  M3 mean opening       percent of the stroke, over the whole period
  M1+M2 drives          a day
  targets dropped,      a day: within the deadband of M3 at rest, or inside
  deferred              the minimum interval (contract s.5); mode 2 only

The airflow exponent (--m3-airflow-exp) is how much air a part-open M3 lets
through: its opening to that power. It is unmeasured (plan s.5c), so a mode 2
verdict must hold across the range, not at 1 alone. The logged column does
not depend on it.

ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

from __future__ import annotations

import argparse
import bisect
import json
import statistics
import sys
from argparse import Namespace
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
for p in (HERE, HERE.parent):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import closed_loop as cl  # noqa: E402
import dataset  # noqa: E402
from ventmodel import VENT_STEPS_MAX, entry_temp_c  # noqa: E402

P2 = HERE.parent / "campaign-summer-2026" / "plant2"
PLANTS = {"primary": P2 / "plant2_summer2026_Ca2.9_tau120_tau90_ev5_dir.json",
          "second": P2 / "plant2_summer2026_Ca2.9_tau240_tau90_ev5_dir.json"}
DAY_HOURS = (8, 19)
WINDOW = 120                    # samples in 60 min at the log's 30 s


def variant_lib(defines):
    """The library with #define NAME VALUE replaced, for a tuning run: compiled
    from a temporary copy of drivers/ventModel/src, which stays as it is, into
    build/variants/ (only the DLL is kept, and .gitignore already excludes it)."""
    import hashlib
    import re
    import shutil
    import tempfile
    import ventmodel as vm
    key = hashlib.sha1("\n".join(sorted(defines)).encode()).hexdigest()[:10]
    out = vm.BUILD_DIR / "variants" / ("ventmodel_%s.dll" % key)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for f in vm.LIB_SRC.glob("*"):
            shutil.copy(f, d)
        for spec in defines:
            name, value = spec.split("=", 1)
            hits = 0
            for f in d.glob("*.cpp"):
                text, n = re.subn(r"(#define\s+%s\s+)\S+" % re.escape(name), r"\g<1>%s" % value,
                                  f.read_text(encoding="utf-8"))
                if n:
                    f.write_text(text, encoding="utf-8")
                    hits += n
            if hits != 1:
                raise SystemExit("--define %s: %d #define lines named %s in "
                                 "drivers/ventModel/src" % (spec, hits, name))
        vm.build_dll(out, sorted(d.glob("*.cpp")) + [vm.FFI_SRC], include_dirs=[d], force=True)
    return vm.VentLib(out)


def run(ds, lo, hi, plant, law, fitted, flow):
    args = Namespace(model=law, warmup_h=6.0, rh_from_log=False, calibrator_hold=False,
                     openness="state", t3="sim", daynight="sim", firmware="current",
                     config=None, set=["wpos_fitted_m3=1"] if fitted else None,
                     plant2=str(plant), m3_span_mm=1500, m3_min_move_s=0, m3_airflow_exp=flow)
    params = json.loads(Path(plant).read_text())["params"]
    recs, act, ctl = cl.run_closed_loop(ds, lo, hi, "two", params, args,
                                        cl.schedule_from_args(args))
    starts = [c.n.starts - b.starts for c, b in zip(act.ch, act.n0)]
    return recs, starts + [ctl.dropped - ctl.n0[0], ctl.deferred - ctl.n0[1],
                           (act.ch[2].n.run_ms - act.n0[2].run_ms) / 60000.0]


def full_days(recs):
    by_day = {}
    for r in recs:
        by_day.setdefault(r["t"].date(), []).append(r)
    return {d: rs for d, rs in sorted(by_day.items()) if len(rs) >= 2000}


def north_days(days):
    """North: at least half of the day's logged M3-open time had wind from 315-45 deg."""
    out = {}
    for d, rs in days.items():
        m3 = [r for r in rs if cl._code(r["bm_log"], 2) != 0]
        if m3 and d >= cl.WIND_VALID_FROM.date():
            out[d] = 100.0 * sum(1 for r in m3 if r["wind_dir"] >= 315 or r["wind_dir"] < 45) \
                / len(m3) >= 50
    return out


def fluctuation(rs, tk):
    """Daytime spread of T around its centred 60-min average, degC."""
    t = np.array([r[tk] for r in rs if DAY_HOURS[0] <= r["t"].hour < DAY_HOURS[1]])
    if len(t) < 3 * WINDOW:
        return None
    avg = np.convolve(t, np.ones(WINDOW) / WINDOW, mode="valid")
    core = t[WINDOW // 2: WINDOW // 2 + len(avg)]
    return float(np.std(core - avg))


def metrics(recs, starts, key, t_m3, north):
    """What one run -- or the log, key 'bm_log' -- did, per day."""
    tk = "T_sim" if key == "bm_sim" else "T_log"
    days = full_days(recs)
    span = (recs[-1]["t"] - recs[0]["t"]).total_seconds() / 86400.0
    per = [cl.day_metrics(rs, key, t_m3) for rs in days.values()]

    def med(xs):
        xs = [x for x in xs if x is not None]
        return statistics.median(xs) if xs else None

    swing = {d: m["swing"] for d, m in zip(days, per)}
    day_t = [r[tk] for r in recs if DAY_HOURS[0] <= r["t"].hour < DAY_HOURS[1]]
    codes = [cl._code(r[key], 2) for r in recs]
    m3_open = sum(1 for a, b in zip(codes, codes[1:]) if a == 0 and b in (1, 2))
    if starts is None:
        # The log: drives from its bitmask. M1 and M2 travel 26 s, so a 30-s
        # sample often sees them only shut, then open: that is a drive too.
        starts = []
        for ch in range(3):
            c = [cl._code(r[key], ch) for r in recs]
            starts.append(sum(1 for a, b in zip(c, c[1:])
                              if (b in (1, 3) and b != a) or {a, b} == {0, 2}))
        step_min = (recs[1]["t"] - recs[0]["t"]).total_seconds() / 60.0
        starts += [None, None,
                   sum(1 for r in recs if cl._code(r[key], 2) in (1, 3)) * step_min]
    opening = (sum(r["o3_log"] for r in recs) / len(recs) if key == "bm_log"
               else sum(r["pos_m3"] for r in recs) / len(recs))
    step_h = (recs[1]["t"] - recs[0]["t"]).total_seconds() / 3600.0
    return {
        "hours >= %d degC a day" % t_m3: sum(m["h_above"] for m in per) / len(per),
        "daytime mean, degC": sum(day_t) / len(day_t),
        "swing, degC": med(swing.values()),
        "  north wind days": med(s for d, s in swing.items() if north.get(d) is True),
        "  other wind days": med(s for d, s in swing.items() if north.get(d) is False),
        "fluctuation, degC": med(fluctuation(rs, tk) for rs in days.values()),
        "M3 drives a day": starts[2] / span,
        "M3 motor minutes a day": starts[5] / span,
        "M3 openings a day": m3_open / span,
        "M3 open hours a day": sum(1 for c in codes if c != 0) * step_h / span,
        "M3 mean opening, %": 100.0 * opening,
        "M1+M2 drives a day": (starts[0] + starts[1]) / span,
        "targets dropped a day": None if starts[3] is None else starts[3] / span,
        "targets deferred a day": None if starts[4] is None else starts[4] / span,
    }


def fmt(x):
    return "-" if x is None else ("%.1f" % x if abs(x) < 100 else "%.0f" % x)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--law", default="graded")
    ap.add_argument("--start", default="2026-06-05")
    ap.add_argument("--end", default="2026-09-16")
    ap.add_argument("--airflow", default="1,0.5,2",
                    help="M3 airflow exponents to run (default 1,0.5,2)")
    ap.add_argument("--plants", default="primary,second")
    ap.add_argument("--define", action="append", default=[], metavar="NAME=VALUE",
                    help="run a variant with this #define of the law's source replaced "
                         "(repeatable), e.g. M3_HOLD_MS=300000u; built in build/variants/")
    args = ap.parse_args(argv)
    flows = [float(x) for x in args.airflow.split(",")]
    plants = [(p, PLANTS[p]) for p in args.plants.split(",")]
    if args.define:
        import ventmodel
        ventmodel._LIB = variant_lib(args.define)

    ds = dataset.build()
    start, end = cl._parse_local(args.start), cl._parse_local(args.end, end=True)
    lo, hi = bisect.bisect_left(ds.t, start), bisect.bisect_left(ds.t, end)
    sched = cl.schedule_from_args(Namespace(config=None, set=None))
    s = sched.at(ds.t[lo])
    t_m3 = entry_temp_c(VENT_STEPS_MAX, s.t_max_day, s.hyst_t)

    print("=== law_compare: %s (mode 2, linear M3) against stepped (mode 1), today's firmware ==="
          % args.law)
    print("  %s .. %s; plants %s; the settings are 5C88's; M3 airflow = opening ** exp"
          % (ds.t[lo], ds.t[hi - 1], args.plants))
    if args.define:
        print("  a variant of the law: %s" % ", ".join(args.define))
    logged = None
    for flow in flows:
        cols, north = [], None
        for pname, plant in plants:
            for law, fitted in (("stepped", False), (args.law, True)):
                recs, starts = run(ds, lo, hi, plant, law, fitted, flow)
                if north is None:
                    north = north_days(full_days(recs))
                if logged is None and fitted:
                    logged = metrics(recs, None, "bm_log", t_m3, north)
                cols.append(("%s %s" % (pname, law), metrics(recs, starts, "bm_sim", t_m3,
                                                             north)))
                print("  ran %-8s %-8s airflow %g" % (pname, law, flow), flush=True)
        names = ["logged"] + [c[0] for c in cols]
        print("\n  M3 airflow exponent %g%s" % (flow, "  (proportional, as fitted)"
                                                if flow == 1.0 else ""))
        print("  %-26s" % "" + "".join("%17s" % n for n in names))
        for k in logged:
            print("  %-26s" % k + "%17s" % fmt(logged[k])
                  + "".join("%17s" % fmt(c[1][k]) for c in cols))
    nn = sum(1 for v in north.values() if v)
    print("\n  north wind days: %d, other wind days: %d (wind valid from %s)"
          % (nn, len(north) - nn, cl.WIND_VALID_FROM.date()))
    return 0


if __name__ == "__main__":
    sys.exit(main())

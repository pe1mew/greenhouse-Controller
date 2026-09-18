"""
refit.py -- fit the two-node plant to 5C88's summer, judged on days it never saw.

    python model/closedloop/refit.py fit [--direction] [--door1 closed|mask] [--quick]
    python model/closedloop/refit.py show ARTIFACT.json

What is fitted, and on what
---------------------------
The two-node plant (plant2_kernel.c) against the logged indoor temperature
and absolute humidity. Inputs: the windows' openness reconstructed from the
RELAY rows, the doors, and the outdoor weather interpolated between LoRa
uplinks (dataset.py). It is still an open-loop fit -- the windows are the
logged ones -- but the plant now has a fast air node, so the fit can follow
the swings the controller makes instead of smoothing them away. Whether the
result then reproduces the controller's behaviour is a separate, harder
test: closed_loop.py reproduce --plant2 ARTIFACT.json.

Split: every fourth calendar day (day index % 4 == 3) is held out, so both
halves span the whole season -- crop, sun angle, wind regimes. Loss: MSE of
the air node against T_in, plus MSE of AH in g/m3 against AH_in, over
samples with fresh outdoor data and outside the first day after each
restart (the structure node starts from a guess). Door 1's state after its
sensor's last report (2026-08-16 09:57 local, "closed") is an assumption:
--door1 mask drops those samples instead of assuming the door stayed shut.

Optimiser: differential evolution in a transformed space (log10 for the
parameters that span decades), seeded so a rerun gives the same fit, then a
bounded L-BFGS-B polish. The adopted single-node artifact is scored on
exactly the same samples, for comparison.

ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from datetime import date
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
for p in (HERE, HERE.parent):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import dataset  # noqa: E402
import plant2  # noqa: E402
from logdata import CAMPAIGN  # noqa: E402
from plant import ADOPTED, Plant, rh_from_ah  # noqa: E402

# (name, lo, hi, log-scaled)
FIT = [
    ("Ca_MJ",    0.2,   30.0,  True),
    ("Cs_MJ",    1.0,   2000.0, True),
    ("Gas",      10.0,  60000.0, True),
    ("Gso",      0.0,   20000.0, False),
    ("UA0",      10.0,  20000.0, True),
    ("ach_m1",   0.0,   40.0,  False),
    ("ach_m3",   0.0,   60.0,  False),
    ("ach_door", 0.0,   40.0,  False),
    ("ka",       0.0,   10.0,  False),
    ("ks",       0.0,   10.0,  False),
    ("ach_inf",  0.0,   10.0,  False),
    ("e0",       0.0,   0.05,  False),
    ("e1",       0.0,   2e-6,  False),
]
DIRECTION = ("m3_ww", 0.0, 60.0, False)
W_AH = 1.0            # 1 g/m3 of AH error weighs as much as 1 degC
BURN_IN_S = 86400.0


def space(spec):
    return [(np.log10(lo), np.log10(hi)) if lg else (lo, hi) for _, lo, hi, lg in spec]


def decode(x, spec):
    return {name: (10.0 ** v if lg else v) for v, (name, _, _, lg) in zip(x, spec)}


def encode(params, spec):
    return np.array([np.log10(params[name]) if lg else params[name]
                     for name, _, _, lg in spec])


def masks(ds, door1_policy):
    day = ds.day_index()
    held_out = (day % 4) == 3
    since = np.zeros(len(ds))
    last = ds.t_s[0]
    for i in range(len(ds)):
        if ds.restart[i]:
            last = ds.t_s[i]
        since[i] = ds.t_s[i] - last
    usable = ~ds.stale & (since >= BURN_IN_S)
    if door1_policy == "mask":
        usable &= ds.door1_known
    return usable & ~held_out, usable & held_out


def score(Ta, AH, ds, m):
    e = Ta[m] - ds.T_in[m]
    ea = (AH[m] - ds.AH_in[m]) * 1000.0
    rh = rh_from_ah(AH[m], Ta[m])
    return {
        "n": int(m.sum()),
        "T_rmse": float(np.sqrt(np.mean(e * e))),
        "T_bias": float(np.mean(e)),
        "T_p95": float(np.percentile(np.abs(e), 95)),
        "T_within1_pct": float(100.0 * np.mean(np.abs(e) <= 1.0)),
        "AH_rmse_g": float(np.sqrt(np.mean(ea * ea))),
        "RH_rmse": float(np.sqrt(np.mean((rh - ds.RH_in[m]) ** 2))),
    }


M3_RESPONSE_MIN = (5, 10, 15, 25)


def m3_response(ds, T):
    """Median change of T after every daytime (08-19 h) M3 command, open loop.

    The sharpest test of a plant against the limit cycle: the logged T_in
    moves -0.3/-1.3/-2.0/-2.3 degC at 5/10/15/25 min after M3 starts to open,
    and a plant that cannot follow that cannot reproduce the cycle. Model and
    log see the same lux and outdoor temperature, so whatever the sun does
    around an M3 command is on both sides of the comparison.
    Returns {"opens": [...], "closes": [...], "n_opens": n, "n_closes": n}.
    """
    from firmware import CH_MOVING_CLOSE, CH_MOVING_OPEN, RELAY_TO_CH
    grid = np.arange(0, 30.5, 0.5)
    pick = [int(m * 2) for m in M3_RESPONSE_MIN]
    out = {}
    for kind, key in ((CH_MOVING_OPEN, "opens"), (CH_MOVING_CLOSE, "closes")):
        curves = []
        for ts, ch, v in ds.log.relay:
            if ch != 2 or RELAY_TO_CH.get(v) != kind or not (8 <= ts.hour < 19):
                continue
            i0 = int(np.searchsorted(ds.t_s, (ts - dataset.EPOCH).total_seconds()))
            if i0 + 60 >= len(ds) or ds.stale[i0]:
                continue
            seg = slice(i0, i0 + 60)
            if np.any(np.diff(ds.t_s[seg]) > 90):
                continue
            curves.append(np.interp(grid, (ds.t_s[seg] - ds.t_s[i0]) / 60.0, T[seg] - T[i0]))
        med = np.median(np.array(curves), axis=0)
        out[key] = [float(med[k]) for k in pick]
        out["n_" + key] = len(curves)
    return out


def m3_event_indices(ds):
    """Sample index of every daytime (08-19 h) M3 command, opening or closing."""
    from firmware import CH_MOVING_CLOSE, CH_MOVING_OPEN, RELAY_TO_CH
    idx = []
    for ts, ch, v in ds.log.relay:
        if ch == 2 and RELAY_TO_CH.get(v) in (CH_MOVING_OPEN, CH_MOVING_CLOSE) \
                and 8 <= ts.hour < 19:
            i0 = int(np.searchsorted(ds.t_s, (ts - dataset.EPOCH).total_seconds()))
            if i0 < len(ds):
                idx.append(i0)
    return np.array(sorted(set(idx)), dtype=np.int64)


def print_response(label, r):
    print("  %-26s opens %s   closes %s"
          % (label, " ".join("%+5.1f" % x for x in r["opens"]),
             " ".join("%+5.1f" % x for x in r["closes"])))


def run_single_node(ds, artifact=ADOPTED):
    """The adopted artifact over the same Dataset, the calibrator's way:
    a window counts as open unless its logged state is CLOSED."""
    pl = Plant.from_json(artifact)
    Ta = np.empty(len(ds))
    AH = np.empty(len(ds))
    for i in range(len(ds)):
        if ds.restart[i]:
            pl.reset(ds.T_in[i], ds.AH_in[i])
            Ta[i], AH[i] = pl.T, pl.AH
            continue
        bm = int(ds.bm[i])
        o = tuple(0.0 if ((bm >> (2 * k)) & 3) == 0 else 1.0 for k in range(3))
        Ta[i], _ = pl.step(o, ds.T_out[i], ds.RH_out[i], ds.lux[i])
        AH[i] = pl.AH
    return Ta, AH


def describe(params):
    vrc = plant2.V_FIXED * plant2.RHO_AIR * plant2.CP_AIR / 3600.0
    ua_closed = params["UA0"]
    ua_open = params["UA0"] + (2 * params["ach_m1"] + params["ach_m3"]) * vrc
    ca = params["Ca_MJ"] * 1e6
    lines = [
        "  air node        Ca %.2f MJ/K  (air alone: %.2f MJ/K at V %.0f m3)"
        % (params["Ca_MJ"], plant2.V_FIXED * 1.2 * 1005 / 1e6, plant2.V_FIXED),
        "  structure node  Cs %.0f MJ/K, coupled by Gas %.0f W/K, to outside Gso %.0f W/K"
        % (params["Cs_MJ"], params["Gas"], params["Gso"]),
        "  house shut      UA0 %.0f W/K (cover %.0f + air exchange ach_inf %.2f /h)"
        % (ua_closed, plant2.cover_ua(params), params["ach_inf"]),
        "  windows         ach_m1 = ach_m2 %.2f /h, ach_m3 %.2f /h%s, a door %.2f /h"
        % (params["ach_m1"], params["ach_m3"],
           (" + %.2f /h x cos(windward angle)" % params["m3_ww"]) if params.get("m3_ww") else "",
           params["ach_door"]),
        "  all open        UA %.0f W/K" % ua_open,
        "  air time const  %.1f min shut, %.1f min all open (Ca / (UA + Gas))"
        % (ca / (ua_closed + params["Gas"]) / 60, ca / (ua_open + params["Gas"]) / 60),
        "  sun at 30 klux  %.1f kW to air, %.1f kW to structure"
        % (params["ka"] * 30, params["ks"] * 30),
        "  moisture        E = %.1f kg/h + %.1f kg/h per 10 klux"
        % (params["e0"] * 3600, params["e1"] * 1e4 * 3600),
    ]
    return "\n".join(lines)


def fit(args):
    from scipy.optimize import differential_evolution, minimize

    t0 = time.time()
    ds = dataset.build()
    train, val = masks(ds, args.door1)
    print("=== refit: two-node plant, %s .. %s ===" % (ds.t[0], ds.t[-1]))
    print("  samples: %d train, %d held out (every 4th day)%s"
          % (train.sum(), val.sum(),
             "; door 1 after 2026-08-16 masked" if args.door1 == "mask"
             else "; door 1 assumed shut after 2026-08-16"))

    fixed = {}
    for item in args.fix or []:
        name, _, value = item.partition("=")
        fixed[name] = float(value)
    spec = [s for s in FIT + ([DIRECTION] if args.direction else []) if s[0] not in fixed]
    if fixed:
        print("  fixed: %s" % ", ".join("%s = %g" % kv for kv in fixed.items()))
    prep = plant2.Prepared(ds, horizon_s=args.horizon_min * 60.0)
    weight = np.ones(len(ds))
    if args.event_weight > 0:
        # Anchor the air node at every daytime M3 command and weigh the half
        # hour after it: the fit must then reproduce the response the limit
        # cycle is made of, not only the season's slow balance.
        ev = m3_event_indices(ds)
        code = prep.restart.copy()
        code[ev] = np.where(code[ev] == 1, 1, 2)
        prep.restart[:] = code
        for i0 in ev:
            weight[i0 + 1:i0 + 61] += args.event_weight
        print("  objective: a free run, re-anchored at %d daytime M3 commands, the 30 min "
              "after each weighted x%g" % (len(ev), 1 + args.event_weight))
    elif args.horizon_min:
        print("  objective: %d-minute predictions (air and humidity re-anchored to the "
              "measurement every %d min, structure node free)" % (args.horizon_min,
                                                                 args.horizon_min))
    else:
        print("  objective: a free run over the season")
    Tm, AHm = prep.Tm[train], prep.AHm[train] * 1000.0
    wt = weight[train]
    wsum = float(wt.sum())

    def loss(x):
        params = dict(decode(x, spec), **fixed)
        if plant2.cover_ua(params) < 0.0:
            return 1e3 - plant2.cover_ua(params) * 1e-2
        try:
            Ta, _, AH = plant2.run(params, None, prep=prep)
        except ValueError:
            return 1e6
        e = Ta[train] - Tm
        ea = AH[train] * 1000.0 - AHm
        v = float(np.sum(wt * e * e) / wsum + W_AH * np.mean(ea * ea))
        return v if np.isfinite(v) else 1e6

    bounds = space(spec)
    res = differential_evolution(loss, bounds, seed=42, popsize=(8 if args.quick else 15),
                                 maxiter=(40 if args.quick else 200), tol=1e-7,
                                 mutation=(0.5, 1.0), recombination=0.7, init="sobol",
                                 polish=False, updating="deferred", workers=1)
    print("  differential evolution: loss %.4f after %d generations (%.0f s)"
          % (res.fun, res.nit, time.time() - t0))
    pol = minimize(loss, res.x, method="L-BFGS-B", bounds=bounds)
    x = pol.x if pol.fun < res.fun else res.x
    print("  L-BFGS-B polish: loss %.4f" % min(pol.fun, res.fun))

    params = dict(decode(x, spec), **fixed)
    full = dict(zip(plant2.PARAMS, plant2.vector(params)))
    # Scored as a free run whatever the objective was, so every artifact is
    # compared on the same footing.
    Ta, _, AH = (a.copy() for a in plant2.run(full, ds))
    Ta1, AH1 = run_single_node(ds)
    result = {
        "two_node": {"train": score(Ta, AH, ds, train), "held_out": score(Ta, AH, ds, val)},
        "single_node_adopted": {"train": score(Ta1, AH1, ds, train),
                                "held_out": score(Ta1, AH1, ds, val)},
    }
    if args.horizon_min:
        hp = plant2.Prepared(ds, horizon_s=args.horizon_min * 60.0)
        Th, _, AHh = (a.copy() for a in plant2.run(full, None, prep=hp))
        result["two_node_%dmin_prediction" % args.horizon_min] = {
            "train": score(Th, AHh, ds, train), "held_out": score(Th, AHh, ds, val)}
    result["m3_response"] = {"logged": m3_response(ds, ds.T_in),
                             "two_node": m3_response(ds, Ta),
                             "single_node_adopted": m3_response(ds, Ta1)}
    print("\n" + describe(full))
    report(result)

    name = "plant2_summer2026%s%s%s%s%s.json" % (
        "".join("_%s%g" % (k.split("_")[0], v) for k, v in fixed.items()),
        "_h%d" % args.horizon_min if args.horizon_min else "",
        "_ev%g" % args.event_weight if args.event_weight else "",
        "_dir" if args.direction else "",
        "_door1mask" if args.door1 == "mask" else "")
    out = Path(args.out) if args.out else CAMPAIGN / "plant2" / name
    out.parent.mkdir(exist_ok=True)
    with open(out, "w") as fh:
        json.dump({
            "_comment": ("Two-node plant (model/closedloop/plant2_kernel.c) fitted by "
                         "model/closedloop/refit.py on 5C88 SD logs + lht65-20 + lds01-5/6, "
                         "%s .. %s. Every 4th day held out. Open-loop fit with the logged "
                         "windows; judge it in closed loop (closed_loop.py reproduce --plant2)."
                         % (ds.t[0].date(), ds.t[-1].date())),
            "model": "plant2",
            "fitted": date.today().isoformat(),
            "options": {"direction": args.direction, "door1": args.door1, "fixed": fixed,
                        "horizon_min": args.horizon_min, "event_weight": args.event_weight,
                        "w_ah": W_AH, "burn_in_s": BURN_IN_S},
            "params": {k: float(v) for k, v in full.items()},
            "metrics": result,
        }, fh, indent=2)
    print("\n  wrote %s" % out)
    return 0


def report(result):
    print("\n  %-26s %-9s %7s %7s %6s %8s %9s %7s"
          % ("model", "samples", "T RMSE", "bias", "p95", "+-1 C", "AH g/m3", "RH %"))
    for model in [m for m in ("single_node_adopted", "two_node") if m in result] + \
            [m for m in result if m.endswith("prediction")]:
        for part in ("train", "held_out"):
            r = result[model][part]
            print("  %-26s %-9s %7.2f %+7.2f %6.2f %7.1f%% %9.2f %7.1f"
                  % (model if part == "train" else "", part, r["T_rmse"], r["T_bias"],
                     r["T_p95"], r["T_within1_pct"], r["AH_rmse_g"], r["RH_rmse"]))
    if "m3_response" in result:
        print("\n  response to a daytime M3 command, median dT (degC) at %s min, open loop:"
              % "/".join(str(m) for m in M3_RESPONSE_MIN))
        for label, r in result["m3_response"].items():
            print_response(label, r)


def show(args):
    with open(args.artifact) as fh:
        a = json.load(fh)
    print(a["_comment"])
    print(describe(a["params"]))
    report(a["metrics"])
    return 0


def compare(args):
    """Several artifacts side by side: free-run scores and the M3 response."""
    ds = dataset.build()
    train, val = masks(ds, "closed")
    rows = [("logged", None, m3_response(ds, ds.T_in))]
    Ta1, AH1 = run_single_node(ds)
    rows.append(("single node (adopted)", score(Ta1, AH1, ds, val), m3_response(ds, Ta1)))
    for path in args.artifacts:
        with open(path) as fh:
            p = json.load(fh)["params"]
        Ta, _, AH = (a.copy() for a in plant2.run(p, ds))
        rows.append((Path(path).stem, score(Ta, AH, ds, val), m3_response(ds, Ta)))
    print("  %-34s %-15s  %-26s %-26s" % ("", "held-out T RMSE", "M3 opens: dT 5/10/15/25",
                                          "M3 closes: dT 5/10/15/25"))
    for name, sc, r in rows:
        print("  %-34s %-15s  %-26s %-26s"
              % (name, ("%.2f degC" % sc["T_rmse"]) if sc else "",
                 " ".join("%+5.1f" % x for x in r["opens"]),
                 " ".join("%+5.1f" % x for x in r["closes"])))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fit")
    p.add_argument("--direction", action="store_true",
                   help="add M3's windward term (m3_ww, north-facing wall)")
    p.add_argument("--door1", choices=("closed", "mask"), default="closed")
    p.add_argument("--horizon-min", type=int, default=0,
                   help="score N-minute predictions (re-anchor the air node every N min); "
                        "0 = a free run over the season")
    p.add_argument("--fix", action="append", metavar="NAME=VALUE",
                   help="hold a parameter at a value instead of fitting it (repeatable), "
                        "e.g. --fix Ca_MJ=2.9 for the air alone")
    p.add_argument("--event-weight", type=float, default=0.0, metavar="L",
                   help="re-anchor at every daytime M3 command and weigh the 30 min after "
                        "it (1 + L) times: fit the response the limit cycle is made of")
    p.add_argument("--quick", action="store_true", help="a small search, for testing")
    p.add_argument("--out")
    p.set_defaults(fn=fit)
    p = sub.add_parser("show")
    p.add_argument("artifact")
    p.set_defaults(fn=show)
    p = sub.add_parser("compare", help="artifacts side by side, incl. the M3 response")
    p.add_argument("artifacts", nargs="+")
    p.set_defaults(fn=compare)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())

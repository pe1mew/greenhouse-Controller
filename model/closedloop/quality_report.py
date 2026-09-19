"""
quality_report.py -- how well the adopted model reproduces 5C88, in numbers and figures.

    python model/closedloop/quality_report.py

Recomputes every number and figure in modelQuality.md: it writes the PNGs to
images/ beside this script and prints the numbers the text quotes.

Three views of the same model:
  closed loop   the law in the emulated firmware drives the plant through
                5C88's logged weather, 2026-06-05 .. 09-16 (closed_loop.py
                reproduce, T3 and day/night simulated), for the adopted pair
                and the superseded single node;
  plant alone   the plant driven by the logged window positions, scored on
                the held-out days (every 4th day), and its median response
                to an M3 command (refit.py);
  an example    two days of the closed loop, logged against simulated.
The controller-chain gates (gate-control, gate-sun, gate-wind) are
closed_loop.py commands of their own; modelQuality.md quotes them.

Takes about three minutes. ASCII-only output (Windows console is cp1252).
"""

from __future__ import annotations

import bisect
import json
import statistics
import sys
from argparse import Namespace
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
MODEL_DIR = HERE.parent
for p in (HERE, MODEL_DIR):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import closed_loop as cl  # noqa: E402
import dataset  # noqa: E402
import plant2  # noqa: E402
import refit  # noqa: E402
from plant import ADOPTED  # noqa: E402

IMAGES = HERE / "images"
P2 = MODEL_DIR / "campaign-summer-2026" / "plant2"
PLANTS = {
    "primary": P2 / "plant2_summer2026_Ca2.9_tau120_tau90_ev5_dir.json",
    "second": P2 / "plant2_summer2026_Ca2.9_tau240_tau90_ev5_dir.json",
}
START, END = datetime(2026, 6, 5), datetime(2026, 9, 17)
EXAMPLE = (datetime(2026, 8, 1), datetime(2026, 8, 3))   # a typical pair, 2 Aug held out
DOOR1_SILENT = datetime(2026, 8, 16)
GRID = np.arange(0, 30.5, 0.5)

# The validated categorical palette (dataviz reference, light mode) and chart ink
LOG, SIM, SIM2, GRAY = "#2a78d6", "#eb6834", "#1baf7a", "#898781"
INK, INK2, MUTED, GRIDC, AXISC = "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"


# --------------------------------------------------------------------------
# numbers
# --------------------------------------------------------------------------

def response_curves(ds, T):
    """refit.m3_response(), keeping the whole median curve (0..30 min)."""
    from firmware import CH_MOVING_CLOSE, CH_MOVING_OPEN, RELAY_TO_CH
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
            curves.append(np.interp(GRID, (ds.t_s[seg] - ds.t_s[i0]) / 60.0, T[seg] - T[i0]))
        out[key] = np.median(np.array(curves), axis=0)
        out["n_" + key] = len(curves)
    return out


def open_loop(ds):
    train, val = refit.masks(ds, "closed")
    groups = refit.m3_open_groups(ds)
    res = {"logged": {"response": response_curves(ds, ds.T_in),
                      "north_other": refit.by_wind(ds.T_in, groups)}}
    runs = {"single": refit.run_single_node(ds)}
    for name, path in PLANTS.items():
        Ta, _, AH = (a.copy() for a in plant2.run(json.load(open(path))["params"], ds))
        runs[name] = (Ta, AH)
    for name, (Ta, AH) in runs.items():
        res[name] = {"held_out": refit.score(Ta, AH, ds, val),
                     "response": response_curves(ds, Ta),
                     "north_other": refit.by_wind(Ta, groups),
                     "errors": Ta[val] - ds.T_in[val]}
    return res


def closed_loop(ds, kind, params, plant2_path=None):
    lo, hi = bisect.bisect_left(ds.t, START), bisect.bisect_left(ds.t, END)
    args = Namespace(model="stepped", warmup_h=6.0, rh_from_log=False, calibrator_hold=False,
                     openness="state", t3="sim", daynight="sim", firmware="5c88",
                     config=None, set=None, plant2=str(plant2_path) if plant2_path else None)
    sched = cl.schedule_from_args(args)
    recs, _, _ = cl.run_closed_loop(ds, lo, hi, kind, params, args, sched)
    s = sched.at(START)
    t_m3 = s.t_max_day + 2 * max(1, s.hyst_t // 3) + 1
    by_day = {}
    for r in recs:
        by_day.setdefault(r["t"].date(), []).append(r)
    days = []
    for day, rs in sorted(by_day.items()):
        if len(rs) < 2000:                     # partial day, as closed_loop.reproduce
            continue
        lg, sm = cl.day_metrics(rs, "bm_log", t_m3), cl.day_metrics(rs, "bm_sim", t_m3)
        err = np.array([r["T_sim"] - r["T_log"] for r in rs])
        m3_open = [r for r in rs if cl._code(r["bm_log"], 2) != 0]
        north = (100.0 * sum(1 for r in m3_open if r["wind_dir"] >= 315 or r["wind_dir"] < 45)
                 / len(m3_open)) if m3_open and day >= cl.WIND_VALID_FROM.date() else None
        days.append({"day": day, "held": rs[0]["held_out"], "log": lg, "sim": sm,
                     "rmse": float(np.sqrt(np.mean(err * err))),
                     "door": 100.0 * sum(r["door"] for r in rs) / len(rs), "north": north})
    return days, recs


def summary(days):
    def med(key, side):
        v = [d[side][key] for d in days if d[side][key] is not None]
        return (statistics.median(v), len(v)) if v else (None, 0)
    o_l = np.array([d["log"]["M3_opens"] for d in days], float)
    o_s = np.array([d["sim"]["M3_opens"] for d in days], float)
    return {"days": len(days), "opens": (o_l.sum(), o_s.sum()),
            "r": float(np.corrcoef(o_l, o_s)[0, 1]),
            "within2": int(np.sum(np.abs(o_l - o_s) <= 2)),
            "open_h": (sum(d["log"]["M3_open_h"] for d in days),
                       sum(d["sim"]["M3_open_h"] for d in days)),
            "h_above": (sum(d["log"]["h_above"] for d in days),
                        sum(d["sim"]["h_above"] for d in days)),
            "cycle": (med("cycle_min", "log"), med("cycle_min", "sim")),
            "swing": (med("swing", "log"), med("swing", "sim")),
            "rmse_median": statistics.median(d["rmse"] for d in days)}


def swing_split(days):
    def med(sel, side):
        v = [d[side]["swing"] for d in sel if d[side]["swing"] is not None]
        return statistics.median(v) if v else None
    groups = (("North wind", [d for d in days if d["north"] is not None and d["north"] >= 50]),
              ("Other wind", [d for d in days if d["north"] is not None and d["north"] < 50]),
              ("Doors shut", [d for d in days if d["door"] < 20]),
              ("A door open", [d for d in days if d["door"] >= 20]))
    return [(label, med(sel, "log"), med(sel, "sim")) for label, sel in groups]


# --------------------------------------------------------------------------
# figures
# --------------------------------------------------------------------------

def _mpl():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    have = {f.name for f in font_manager.fontManager.ttflist}
    plt.rcParams.update({
        "font.family": [f for f in ("Segoe UI", "Helvetica", "Arial") if f in have] + ["DejaVu Sans"],
        "font.size": 10, "axes.edgecolor": AXISC, "axes.linewidth": 0.8,
        "axes.labelcolor": INK2, "axes.titlecolor": INK, "axes.titlesize": 11,
        "axes.titleweight": "normal", "axes.titlelocation": "left",
        "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK2,
        "ytick.labelcolor": INK2, "legend.frameon": False, "legend.fontsize": 9,
        "figure.facecolor": "white", "axes.facecolor": "white", "savefig.dpi": 150,
        "lines.solid_capstyle": "round", "lines.dash_capstyle": "round",
    })
    return plt


def _axes(ax, ygrid=True):
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    if ygrid:
        ax.grid(axis="y", color=GRIDC, linewidth=0.8)
        ax.set_axisbelow(True)


def fig_openings(plt, days, path):
    """Per day: simulated against logged M3 openings, fitted days gray, held-out blue."""
    fig, ax = plt.subplots(figsize=(6.2, 5.2))
    _axes(ax, ygrid=False)
    ax.grid(color=GRIDC, linewidth=0.8)
    ax.set_axisbelow(True)
    rng = np.random.default_rng(7)
    for held, color, marker, label in ((False, GRAY, "o", "fitted day"),
                                       (True, LOG, "^", "held-out day (every 4th)")):
        sel = [d for d in days if d["held"] == held]
        x = np.array([d["log"]["M3_opens"] for d in sel], float) + rng.uniform(-.18, .18, len(sel))
        y = np.array([d["sim"]["M3_opens"] for d in sel], float) + rng.uniform(-.18, .18, len(sel))
        ax.scatter(x, y, s=42 if held else 34, c=color, marker=marker, edgecolors="white",
                   linewidths=1.2, label=label, zorder=3 if held else 2)
    ax.plot([-0.5, 13.5], [-0.5, 13.5], color=MUTED, linewidth=0.8, zorder=1)
    ax.text(12.2, 13.0, "simulated = logged", color=INK2, fontsize=9, ha="right")
    ax.set(xlim=(-0.5, 13.5), ylim=(-0.5, 13.5), xlabel="Logged M3 openings per day",
           ylabel="Simulated M3 openings per day")
    ax.set_xticks(range(0, 14, 2))
    ax.set_yticks(range(0, 14, 2))
    ax.legend(loc="upper left")
    ax.set_title("M3 openings per day, 102 days (small jitter so equal days stay visible)")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def fig_timeline(plt, days, path):
    """7-day mean of the daily openings through the summer, logged against simulated."""
    fig, ax = plt.subplots(figsize=(9, 3.6))
    _axes(ax)
    d = [x["day"] for x in days]
    lg = np.array([x["log"]["M3_opens"] for x in days], float)
    sm = np.array([x["sim"]["M3_opens"] for x in days], float)
    k = np.ones(7) / 7.0
    roll = lambda v: np.convolve(np.pad(v, 3, mode="edge"), k, mode="valid")  # noqa: E731
    ax.plot(d, roll(lg), color=LOG, linewidth=1.8, label="logged")
    ax.plot(d, roll(sm), color=SIM, linewidth=1.8, dashes=(5, 2.5), label="simulated")
    ax.axvline(DOOR1_SILENT.date(), color=MUTED, linewidth=0.8)
    ax.text(DOOR1_SILENT.date() + timedelta(days=1), 9.3,
            "door 1's sensor silent from here:\nits state is assumed shut", color=INK2,
            fontsize=9, va="top")
    ax.set(ylim=(0, 10), ylabel="M3 openings per day (7-day mean)")
    ax.legend(loc="upper left", ncol=2)
    ax.set_title("M3 openings through the summer")
    fig.autofmt_xdate(rotation=0, ha="center")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def fig_swing(plt, split, path):
    fig, ax = plt.subplots(figsize=(6.2, 3.8))
    _axes(ax)
    x = np.arange(len(split))
    w = 0.34
    for off, idx, color, label in ((-w / 2 - 0.02, 1, LOG, "logged"),
                                   (w / 2 + 0.02, 2, SIM, "simulated")):
        vals = [s[idx] for s in split]
        bars = ax.bar(x + off, vals, width=w, color=color, label=label, zorder=2)
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v + 0.06, "%.1f" % v, ha="center",
                    va="bottom", color=INK2, fontsize=9)
    ax.set_xticks(x, [s[0] for s in split])
    ax.tick_params(axis="x", length=0)
    ax.set(ylim=(0, 4.6), ylabel="Median swing inside a cycle, °C")
    ax.legend(loc="upper right", ncol=2)
    ax.set_title("The swing by condition")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def fig_response(plt, ol, path):
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.9), sharex=True)
    series = (("single", GRAY, 1.3, (None, None), "single node (superseded)"),
              ("second", SIM2, 1.8, (1.2, 2.2), "second plant (adopted)"),
              ("primary", SIM, 1.8, (5, 2.5), "primary plant (adopted)"),
              ("logged", LOG, 1.8, (None, None), "logged"))
    for ax, key, title in ((axes[0], "opens", "After M3 opens (median of %d)"),
                           (axes[1], "closes", "After M3 closes (median of %d)")):
        _axes(ax)
        ax.axhline(0, color=AXISC, linewidth=0.8)
        for name, color, lw, dash, label in series:
            y = ol[name]["response"][key]
            kw = {"dashes": dash} if dash[0] else {}
            ax.plot(GRID, y, color=color, linewidth=lw, label=label, **kw)
        ax.set(xlim=(0, 30), xlabel="Minutes after the command", ylabel="Change in T, °C")
        ax.set_title(title % ol["logged"]["response"]["n_" + key])
    axes[0].set_ylim(-2.8, 0.8)
    axes[1].set_ylim(-0.5, 2.3)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles[::-1], labels[::-1], loc="lower center", ncol=4,
               bbox_to_anchor=(0.5, -0.01))
    fig.tight_layout(rect=(0, 0.07, 1, 1))
    fig.savefig(path)
    plt.close(fig)


def fig_errors(plt, ol, path):
    fig, ax = plt.subplots(figsize=(6.8, 3.6))
    _axes(ax)
    bins = np.arange(-8.0, 8.01, 0.25)
    for name, color, lw, dash, label in (
            ("single", GRAY, 1.4, None, "single node (superseded), RMSE %.2f °C"),
            ("primary", SIM, 1.8, None, "primary plant (adopted), RMSE %.2f °C")):
        e = np.clip(ol[name]["errors"], -7.99, 7.99)
        h, _ = np.histogram(e, bins=bins)
        ax.step(bins[:-1], 100.0 * h / h.sum(), where="post", color=color, linewidth=lw,
                label=label % ol[name]["held_out"]["T_rmse"])
    ax.axvline(0, color=AXISC, linewidth=0.8)
    ax.set(xlim=(-8, 8), xlabel="Simulated minus logged temperature, °C "
                                "(the end bins hold everything beyond ±8)",
           ylabel="Share of samples, %")
    ax.legend(loc="upper right")
    ax.set_title("Error on the held-out days, open loop (every 4th day, 68 987 samples)")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def fig_example(plt, recs, path):
    import matplotlib.dates as mdates
    sel = [r for r in recs if EXAMPLE[0] <= r["t"] < EXAMPLE[1]]
    t = [r["t"] for r in sel]
    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 4.6), sharex=True,
                                  gridspec_kw={"height_ratios": [4, 1]})
    _axes(ax)
    ax.plot(t, [r["T_out"] for r in sel], color=GRAY, linewidth=1.1, label="outdoor")
    ax.plot(t, [r["T_sim"] for r in sel], color=SIM, linewidth=1.5, dashes=(5, 2.5),
            label="simulated")
    ax.plot(t, [r["T_log"] for r in sel], color=LOG, linewidth=1.5, label="logged")
    ax.set(ylabel="°C", ylim=(5, 36))
    h, lab = ax.get_legend_handles_labels()
    ax.legend(h[::-1], lab[::-1], loc="upper left", ncol=3)
    ax.set_title("Closed loop on 1 and 2 August 2026 (2 August held out)")
    for side in ("top", "right", "left"):
        ax2.spines[side].set_visible(False)
    lg = np.array([cl._code(r["bm_log"], 2) != 0 for r in sel], float)
    sm = np.array([cl._code(r["bm_sim"], 2) != 0 for r in sel], float)
    ax2.fill_between(t, 1.1, 1.1 + 0.8 * lg, step="post", color=LOG, alpha=0.85, linewidth=0)
    ax2.fill_between(t, 0.1, 0.1 + 0.8 * sm, step="post", color=SIM, alpha=0.85, linewidth=0)
    ax2.set_yticks([0.5, 1.5], ["M3 simulated", "M3 logged"])
    ax2.tick_params(axis="y", length=0)
    ax2.set_ylim(0, 2)
    ax2.xaxis.set_major_locator(mdates.HourLocator(byhour=(0, 6, 12, 18)))
    ax2.xaxis.set_major_formatter(mdates.DateFormatter("%d %b %H:%M"))
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


# --------------------------------------------------------------------------

def main():
    ds = dataset.build()
    print("open loop ...")
    ol = open_loop(ds)
    print("closed loop: primary, second, single node ...")
    cls = {}
    for name, path in PLANTS.items():
        cls[name] = closed_loop(ds, "two", json.load(open(path))["params"], path)
    cls["single"] = closed_loop(ds, "single", json.load(open(ADOPTED)))
    days, recs = cls["primary"]

    IMAGES.mkdir(exist_ok=True)
    plt = _mpl()
    fig_openings(plt, days, IMAGES / "closedloop_openings_per_day.png")
    fig_timeline(plt, days, IMAGES / "closedloop_openings_timeline.png")
    fig_swing(plt, swing_split(days), IMAGES / "closedloop_swing_by_condition.png")
    fig_response(plt, ol, IMAGES / "plant_m3_response.png")
    fig_errors(plt, ol, IMAGES / "plant_heldout_error.png")
    fig_example(plt, recs, IMAGES / "closedloop_example_2026-08-01.png")
    print("wrote the figures to %s" % IMAGES)

    def f(x, fmt="%.1f"):
        return "-" if x is None else fmt % x
    print("\n=== closed loop, %s .. %s ===" % (START.date(), (END - timedelta(days=1)).date()))
    for name in ("primary", "second", "single"):
        for label, sel in (("all", cls[name][0]),
                           ("held out", [d for d in cls[name][0] if d["held"]]),
                           ("before 08-16", [d for d in cls[name][0]
                                             if d["day"] < DOOR1_SILENT.date()]),
                           ("from 08-16", [d for d in cls[name][0]
                                           if d["day"] >= DOOR1_SILENT.date()])):
            s = summary(sel)
            print("  %-8s %-12s %3d days | openings %4.0f / %4.0f r %.2f within2 %3d | open h "
                  "%4.0f / %4.0f | h>=31 %4.0f / %4.0f | cycle %s / %s (%d / %d days) | swing "
                  "%s / %s | rmse med %.2f"
                  % (name, label, s["days"], s["opens"][0], s["opens"][1], s["r"], s["within2"],
                     s["open_h"][0], s["open_h"][1], s["h_above"][0], s["h_above"][1],
                     f(s["cycle"][0][0], "%.0f"), f(s["cycle"][1][0], "%.0f"),
                     s["cycle"][0][1], s["cycle"][1][1], f(s["swing"][0][0]),
                     f(s["swing"][1][0]), s["rmse_median"]))
    print("  swing split (primary):", ", ".join("%s %s / %s" % (lb, f(a), f(b))
                                                for lb, a, b in swing_split(days)))
    bias = {}
    for r in recs:
        h = r["t"].hour
        band = "night" if (h >= 22 or h < 6) else ("day" if 10 <= h < 18 else "shoulder")
        bias.setdefault(band, []).append(r["T_sim"] - r["T_log"])
    print("  closed-loop bias:", ", ".join("%s %+.2f" % (k, np.mean(v)) for k, v in bias.items()))

    print("\n=== plant alone, held-out days ===")
    for name in ("primary", "second", "single"):
        h = ol[name]["held_out"]
        r = ol[name]["response"]
        pick = [int(m * 2) for m in (5, 10, 15, 25)]
        print("  %-8s T RMSE %.2f bias %+.2f p95 %.2f within1 %.1f%% RH RMSE %.1f | opens %s | "
              "closes %s | north/other at 25 min %+.2f / %+.2f"
              % (name, h["T_rmse"], h["T_bias"], h["T_p95"], h["T_within1_pct"], h["RH_rmse"],
                 " ".join("%+.1f" % r["opens"][i] for i in pick),
                 " ".join("%+.1f" % r["closes"][i] for i in pick), *ol[name]["north_other"]))
    r = ol["logged"]["response"]
    print("  logged                                             | opens %s | closes %s | "
          "north/other at 25 min %+.2f / %+.2f"
          % (" ".join("%+.1f" % r["opens"][i] for i in pick),
             " ".join("%+.1f" % r["closes"][i] for i in pick), *ol["logged"]["north_other"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

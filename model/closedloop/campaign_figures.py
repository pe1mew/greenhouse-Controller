"""
campaign_figures.py -- the figures in campaignResults_summer2026.md and
thermalProfileCampaign.md §9.12 that come from neither refit.py nor
closed_loop.py, recomputed on correctly timed data.

    python model/closedloop/campaign_figures.py [--only F2 F3 ...]

One section per finding (F*) or campaign step (NS9), plus the plants' ladder:

  F2      the forced M3-only tests of 2026-07-04 and 2026-07-11, and the
          all-closed re-heat between the Jul 11 windows
  F3      slopes before and after daytime window openings (the event study)
  F4      the hottest days: the house's state at the daily maximum
  F5      wind directions over the wind-valid era, overall and by month
  F11     M3-only-open minutes under windward (315-45 deg) wind, by date
  F12     the indoor LoRa sensors against the controller's sensor
  NS9     all-open temperature excess per 10 klux, by wind speed and sector
  LADDER  the fitted plants' heat loss per ventilation step, and M3 against
          a roof window in every plant2 artifact (no dataset needed)

Everything is read through dataset.py, so the LoRa rows are converted from
UTC (lora_time.py) and the 2026-07-09 morning is masked. F5 and F11 use the
SD logs alone; the others join the LoRa data. Times are the local time the
SD logs use.

ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

from __future__ import annotations

import argparse
import glob
import json
import sys
from collections import Counter, defaultdict
from datetime import date
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
for p in (HERE, HERE.parent):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import dataset  # noqa: E402
from firmware import CH_MOVING_CLOSE, CH_MOVING_OPEN, RELAY_TO_CH  # noqa: E402
from plant import CP_AIR, RHO_AIR  # noqa: E402

DAYTIME = (8, 19)                 # hours, as refit.m3_response
FORCED_TEST_DAYS = (date(2026, 7, 4), date(2026, 7, 11))
HOT_DAY_C = 34.0
WINDY_MS = 1.0
SECT8 = ("N", "NE", "E", "SE", "S", "SW", "W", "NW")
SECT4 = (("N 315-45", 315, 45), ("E 45-135", 45, 135), ("S 135-225", 135, 225), ("W 225-315", 225, 315))
SPEED_BINS = ((0, 0.5), (0.5, 1), (1, 1.5), (1.5, 2), (2, 3), (3, 9))
ADOPTED = ("plant2_summer2026_Ca2.9.json", "plant2_summer2026_dir.json")
LHT_INDOOR = {
    "LHT65-02 (1/4)": "lht65_02_2026-06-01_2026-09-17.csv",
    "LHT65-03 (3/4)": "lht65_03_2026-06-01_2026-09-17.csv",
}


def state(bm, ch):
    """Channel state from the logged bitmask: 0 CLOSED, 1 MOVING_OPEN, 2 OPEN, 3 MOVING_CLOSE."""
    return (int(bm) >> (2 * ch)) & 3


def windward(deg):
    d = deg % 360
    return d >= 315 or d < 45


def index_at(ds, ts):
    return int(np.searchsorted(ds.t_s, (ts - dataset.EPOCH).total_seconds()))


def runs(ds, idx, pred, min_s=600):
    """[first, last] index pairs of the stretches of idx where pred(i) holds, min_s or longer."""
    spans, cur = [], None
    for i in idx:
        if pred(i):
            cur = [i, i] if cur is None else [cur[0], i]
        elif cur is not None:
            spans.append(cur)
            cur = None
    if cur is not None:
        spans.append(cur)
    return [(a, b) for a, b in spans if ds.t_s[b] - ds.t_s[a] >= min_s]


def f2_forced_tests(ds):
    """Each M3-only opening of 10 min or more (M1, M2 closed by the bitmask) on the test days."""
    print("== F2  forced M3-only tests (excess = inside minus outside)")
    by_day = defaultdict(list)
    for i, t in enumerate(ds.t):
        if t.date() in FORCED_TEST_DAYS:
            by_day[t.date()].append(i)
    for day in FORCED_TEST_DAYS:
        bms = ds.bm
        spans = runs(ds, by_day[day], lambda i: state(bms[i], 2) in (1, 2)
                     and state(bms[i], 0) == 0 and state(bms[i], 1) == 0)
        for a, b in spans:
            seg = np.arange(a, b + 1)
            pre = a - 1
            dT = ds.T_in[seg] - ds.T_out[seg]
            dAH = 1000.0 * (ds.AH_in[seg] - ds.AH_out[seg])
            k = int(np.argmax(dAH <= 0.5)) if np.any(dAH <= 0.5) else None
            lo = np.searchsorted(ds.t_s, ds.t_s[a] - 7200)
            hi = np.searchsorted(ds.t_s, ds.t_s[a] + 7200)
            print("  %s %s-%s (%d min)" % (day, ds.t[a].strftime("%H:%M"), ds.t[b].strftime("%H:%M"),
                                          round((ds.t_s[b] - ds.t_s[a]) / 60)))
            print("    T excess  before %+.1f  end %+.1f  min %+.1f degC"
                  % (ds.T_in[pre] - ds.T_out[pre], dT[-1], dT.min()))
            print("    AH excess before %+.1f  end %+.1f  min %+.1f g/m3; first <= 0.5 after %s min"
                  % (1000.0 * (ds.AH_in[pre] - ds.AH_out[pre]), dAH[-1], dAH.min(),
                     "-" if k is None else round((ds.t_s[a + k] - ds.t_s[a]) / 60)))
            print("    wind %.0f deg (p10-p90 %.0f-%.0f), %.1f m/s (p10-p90 %.1f-%.1f);"
                  " lux %.0f (%.0f-%.0f)"
                  % (np.median(ds.wind_dir[seg]), np.percentile(ds.wind_dir[seg], 10),
                     np.percentile(ds.wind_dir[seg], 90), np.median(ds.wind_ms[seg]),
                     np.percentile(ds.wind_ms[seg], 10), np.percentile(ds.wind_ms[seg], 90),
                     np.median(ds.lux[seg]), ds.lux[seg].min(), ds.lux[seg].max()))
            print("    door 1 open %.0f %% (within 2 h either side %.0f %%), door 2 open %.0f %%"
                  % (100 * np.mean(ds.door1[seg] > 0), 100 * np.mean(ds.door1[lo:hi] > 0),
                     100 * np.mean(ds.door2[seg] > 0)))
        if len(spans) < 2:
            continue
        # The all-closed stretches between the forced windows: how fast the
        # house re-heats after a flush (thermalProfileCampaign.md §9.10 finding 3).
        between = [i for i in by_day[day] if spans[0][1] < i < spans[-1][0]]
        for a, b in runs(ds, between, lambda i: int(bms[i]) & 0x3F == 0):
            seg = np.arange(a, b + 1)
            print("  %s %s-%s all closed (%d min): T_in %.1f -> %.1f, %+.1f degC per 30 min;"
                  " lux %.0f (%.0f-%.0f)"
                  % (day, ds.t[a].strftime("%H:%M"), ds.t[b].strftime("%H:%M"),
                     round((ds.t_s[b] - ds.t_s[a]) / 60), ds.T_in[a], ds.T_in[b],
                     (ds.T_in[b] - ds.T_in[a]) / ((ds.t_s[b] - ds.t_s[a]) / 1800.0),
                     np.median(ds.lux[seg]), ds.lux[seg].min(), ds.lux[seg].max()))


def f3_event_study(ds):
    """Median slope 15 min before -> 3..18 min after each daytime opening command."""
    print("== F3  event study: median slope 15 min before -> 3-18 min after the command, %d-%d h"
          % DAYTIME)

    def slope(x, i0, a, b):
        ia = np.searchsorted(ds.t_s, ds.t_s[i0] + a * 60)
        ib = np.searchsorted(ds.t_s, ds.t_s[i0] + b * 60)
        if ia <= 0 or ib >= len(ds) or ib <= ia or ds.t_s[ib] - ds.t_s[ia] < 300:
            return None
        return (x[ib] - x[ia]) / ((ds.t_s[ib] - ds.t_s[ia]) / 3600.0)

    groups = defaultdict(list)
    for ts, ch, v in ds.log.relay:
        if RELAY_TO_CH.get(v) != CH_MOVING_OPEN or not (DAYTIME[0] <= ts.hour < DAYTIME[1]):
            continue
        i0 = index_at(ds, ts)
        if i0 >= len(ds) or i0 < 60 or ds.stale[i0]:
            continue
        o = ds.pos[i0 - 1]
        if ch == 2 and o[0] > 0.99 and o[1] > 0.99:
            key = "M3 opens on M1+M2"
        elif ch == 1 and o[2] < 0.01:
            key = "M2 opens, M3 shut"
        elif ch == 0 and o[1] < 0.01 and o[2] < 0.01:
            key = "M1 opens, all shut"
        else:
            continue
        row = []
        for x, scale in ((ds.T_in, 1.0), (ds.AH_in, 1000.0), (ds.lux, 1.0)):
            pre, post = slope(x, i0, -15, 0), slope(x, i0, 3, 18)
            row.append(None if pre is None or post is None else (pre * scale, post * scale))
        if all(r is not None for r in row):
            groups[key].append(row)
    for key in ("M3 opens on M1+M2", "M2 opens, M3 shut", "M1 opens, all shut"):
        g = groups[key]

        def med(k, j):
            return np.median([r[k][j] for r in g])

        print("  %-19s n=%3d | T %+5.1f -> %+5.1f degC/h (swing %+5.1f) | AH %+5.1f -> %+5.1f g/m3/h"
              " | lux %+6.0f -> %+6.0f lux/h"
              % (key, len(g), med(0, 0), med(0, 1), med(0, 1) - med(0, 0), med(1, 0), med(1, 1),
                 med(2, 0), med(2, 1)))


def f4_hot_days(ds):
    print("== F4  the hottest days: the state at each day's logged maximum")
    days = defaultdict(list)
    for i, t in enumerate(ds.t):
        days[t.date()].append(i)
    hot = []
    for d, idx in days.items():
        i = max(idx, key=lambda k: ds.T_in[k])
        if ds.T_in[i] >= HOT_DAY_C:
            hot.append((ds.T_in[i], ds.T_out[i], ds.lux[i],
                        all(ds.pos[i, k] > 0.99 for k in range(3)),
                        bool(ds.door1[i] or ds.door2[i])))
    print("  %d days logged; %d reach %.0f degC; at the maximum all three windows open on %d,"
          " a door open on %d" % (len(days), len(hot), HOT_DAY_C, sum(h[3] for h in hot),
                                  sum(h[4] for h in hot)))
    print("  median at the maximum: T_in %.1f, T_out %.1f, excess %.1f degC, lux %.0f k"
          % (np.median([h[0] for h in hot]), np.median([h[1] for h in hot]),
             np.median([h[0] - h[1] for h in hot]), np.median([h[2] for h in hot]) / 1000))


def f5_wind(ds):
    sel = [i for i in range(len(ds))
           if ds.t[i] >= dataset.WIND_VALID_FROM and ds.wind_ms[i] >= WINDY_MS]
    print("== F5  wind at >= %.1f m/s, %s .. %s (n=%d)"
          % (WINDY_MS, dataset.WIND_VALID_FROM, ds.t[-1], len(sel)))
    c8 = Counter(SECT8[int(((ds.wind_dir[i] % 360) + 22.5) // 45) % 8] for i in sel)
    print("  from 315-45 deg (M3's windward side): %.1f %%"
          % (100.0 * sum(windward(ds.wind_dir[i]) for i in sel) / len(sel)))
    print("  8 sectors: " + "  ".join("%s %.1f" % (k, 100.0 * c8[k] / len(sel)) for k in SECT8))
    by_month = defaultdict(list)
    for i in sel:
        by_month[ds.t[i].strftime("%Y-%m")].append(windward(ds.wind_dir[i]))
    print("  from 315-45 deg by month: " + "  ".join(
        "%s %.0f %%" % (m, 100.0 * np.mean(v)) for m, v in sorted(by_month.items())))


def f11_windward_minutes(ds):
    print("== F11 M3-only OPEN minutes (M1, M2 closed) under 315-45 deg wind, by date")
    by_date = defaultdict(float)
    for i in range(len(ds)):
        if ds.t[i] < dataset.WIND_VALID_FROM:
            continue
        bm = ds.bm[i]
        if state(bm, 2) == 2 and state(bm, 0) == 0 and state(bm, 1) == 0 \
                and windward(ds.wind_dir[i]):
            by_date[ds.t[i].date()] += 0.5
    for d in sorted(by_date):
        print("  %s  %5.1f min" % (d, by_date[d]))
    print("  total %.1f min on %d days" % (sum(by_date.values()), len(by_date)))


def f12_indoor_lora(ds):
    """LHT65-02/-03 hang mid-width at 1/4 and 3/4 of the length; the FG6485A is in the centre."""
    sensors = {name: dataset._read_lora(dataset.CAMPAIGN / fn, ("airTemperature", "soilTemperature"))
               for name, fn in LHT_INDOOR.items()}

    def fg_at(t):
        x = (t - dataset.EPOCH).total_seconds()
        i = int(np.searchsorted(ds.t_s, x))
        if 0 < i < len(ds) and abs(ds.t_s[i] - x) <= 60:
            return ds.T_in[i]
        return None

    print("== F12 indoor LoRa air T minus the controller's T")
    for name, rows in sensors.items():
        day, night = [], []
        for t, ta, _soil in rows:
            f = fg_at(t)
            if f is None:
                continue
            if 10 <= t.hour < 17:
                day.append(ta - f)
            elif t.hour >= 22 or t.hour < 5:
                night.append(ta - f)
        print("  %-15s 10-17 h median %+.2f   22-05 h median %+.2f degC"
              % (name, np.median(day), np.median(night)))

    bins = ((0, 10), (10, 20), (20, 30), (30, 40))
    for kind, label in ((CH_MOVING_OPEN, "M3 opens"), (CH_MOVING_CLOSE, "M3 closes")):
        cmds = [ts for ts, ch, v in ds.log.relay
                if ch == 2 and RELAY_TO_CH.get(v) == kind and DAYTIME[0] <= ts.hour < DAYTIME[1]]
        print("  %s (daytime): median change from the last reading before the command,"
              " at 0-10/10-20/20-30/30-40 min" % label)
        for name, rows in sensors.items():
            rt = [r[0] for r in rows]
            acc = {b: ([], []) for b in bins}
            for te in cmds:
                k = int(np.searchsorted(rt, te)) - 1
                if k < 0 or (te - rt[k]).total_seconds() > 700:
                    continue
                before = fg_at(rt[k])
                if before is None:
                    continue
                for j in range(k + 1, min(k + 6, len(rows))):
                    m = (rt[j] - te).total_seconds() / 60
                    f = fg_at(rt[j])
                    for b in bins:
                        if b[0] <= m < b[1] and f is not None:
                            acc[b][0].append(rows[j][1] - rows[k][1])
                            acc[b][1].append(f - before)
            print("    %-15s LoRa %s | controller %s  (n=%d)"
                  % (name, " ".join("%+5.1f" % np.median(acc[b][0]) for b in bins),
                     " ".join("%+5.1f" % np.median(acc[b][1]) for b in bins), len(acc[bins[1]][0])))


def ns9_wind_speed(ds):
    """Does ventilation grow with wind speed? A crude, model-free check.

    With all three windows open, both doors shut and the sun above 20 klux,
    (T_in - T_out) per 10 klux falls as ventilation rises. Transients and the
    structure's stored heat blur it, so read trends, not levels.
    """
    open3 = np.all(ds.o > 0.99, axis=1)
    hours = np.array([t.hour for t in ds.t])
    m = (open3 & ~ds.stale & ds.wind_valid & (ds.door1 + ds.door2 == 0)
         & (hours >= 10) & (hours < 16) & (ds.lux > 20000))
    exc = np.full(len(ds), np.nan)
    exc[m] = (ds.T_in[m] - ds.T_out[m]) / (ds.lux[m] / 1e4)
    print("== NS9 all three open, doors shut, 10-16 h, > 20 klux: (T_in - T_out) per 10 klux,"
          " median (n=%d)" % m.sum())
    for lo, hi in SPEED_BINS:
        s = m & (ds.wind_ms >= lo) & (ds.wind_ms < hi)
        if s.sum() > 50:
            print("  wind %.1f-%.1f m/s: n %5d  %.2f degC" % (lo, hi, s.sum(), np.median(exc[s])))
    d = ds.wind_dir % 360
    for name, lo, hi in SECT4:
        sector = (d >= lo) | (d < hi) if lo > hi else (d >= lo) & (d < hi)
        s = m & sector & (ds.wind_ms >= WINDY_MS)
        days = len({ds.t[i].date() for i in np.flatnonzero(s)})
        if s.sum() > 50:
            print("  from %-9s at >= %.1f m/s: n %5d on %2d days  %.2f degC"
                  % (name, WINDY_MS, s.sum(), days, np.median(exc[s])))


def ladder(_ds=None):
    """Heat loss to outside per ventilation step, from the fitted parameters alone.

    UA(step) = UA0 + (sum of the open windows' ach) * V*rho*cp/3600 -- the air
    node's loss to outside (cover, leaks, windows), without its coupling Gas
    to the structure node. Step 1 = M1, 2 = M1+M2, 3 = all three.
    """
    print("== LADDER heat loss to outside per step, kW/K (and the factor over the step below)")
    root = HERE.parent / "campaign-summer-2026" / "plant2"
    for fn in ADOPTED:
        p = json.load(open(root / fn))["params"]
        vrc = p["V"] * RHO_AIR * CP_AIR / 3600.0
        ua = [p["UA0"]]
        for ach in (p["ach_m1"], p["ach_m2"], p["ach_m3"]):
            ua.append(ua[-1] + ach * vrc)
        row = "  ".join("%d: %.1f" % (k, u / 1000) + ("" if k == 0 else " (x%.2f)" % (u / ua[k - 1]))
                        for k, u in enumerate(ua))
        print("  %-30s %s" % (fn[len("plant2_summer2026"):-5] or "(free run)", row))
        if p.get("m3_ww"):
            full = ua[2] + (p["ach_m3"] + p["m3_ww"]) * vrc
            print("  %-30s 3, wind straight onto M3's wall: %.1f (x%.2f)" % ("", full / 1000, full / ua[2]))
    print("  M3 against one roof window, and M3's share of the window ventilation with all open:")
    for f in sorted(glob.glob(str(root / "*.json"))):
        p = json.load(open(f))["params"]
        name = Path(f).stem[len("plant2_summer2026"):] or "(free run)"
        m3 = p["ach_m3"]
        print("  %-12s ach_m1 %5.2f  ach_m3 %5.2f /h  ratio %4.1f  share %3.0f %%"
              % (name, p["ach_m1"], m3, m3 / p["ach_m1"], 100 * m3 / (m3 + p["ach_m1"] + p["ach_m2"]))
              + ("" if not p.get("m3_ww") else "  (wind onto the wall: %.2f /h, ratio %.1f, share %.0f %%)"
                 % (m3 + p["m3_ww"], (m3 + p["m3_ww"]) / p["ach_m1"],
                    100 * (m3 + p["m3_ww"]) / (m3 + p["m3_ww"] + p["ach_m1"] + p["ach_m2"]))))


SECTIONS = {
    "F2": f2_forced_tests,
    "F3": f3_event_study,
    "F4": f4_hot_days,
    "F5": f5_wind,
    "F11": f11_windward_minutes,
    "F12": f12_indoor_lora,
    "NS9": ns9_wind_speed,
    "LADDER": ladder,
}
NO_DATASET = {"LADDER"}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    ap.add_argument("--only", nargs="+", choices=sorted(SECTIONS), help="sections to print")
    args = ap.parse_args(argv)
    chosen = [n for n in SECTIONS if args.only is None or n in args.only]
    ds = None
    if any(n not in NO_DATASET for n in chosen):
        ds = dataset.build()
        print("dataset: %d samples, %s .. %s" % (len(ds), ds.t[0], ds.t[-1]))
    for name in chosen:
        SECTIONS[name](ds)
    return 0


if __name__ == "__main__":
    sys.exit(main())

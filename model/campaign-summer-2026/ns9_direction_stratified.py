"""NS-9 step 1 — wind-direction-stratified per-segment ACH estimation.

Method
------
For every continuous window-state segment (>= MIN_SEG_MIN minutes) in the
merged calibration input, fit the segment's TOTAL ach by locally simulating
the single-node model from the segment's measured initial T_in:

    C_eff dT/dt = k_solar*lux - P_transp - UA(ach)*(T_in - T_out)

(k_solar, c_eff, transp from the adopted freem3 artifact; grid scan on ach).
The M3 increment is then  ach_m3 = ach_total - ach_inf - ach_m1*(n_m1 + n_m2).
Segments are classified by median wind direction into 4 bins relative to the
north-wall M3 window:  N/windward 315-45, E/cross 45-135, S/leeward 135-225,
W/cross 225-315.

Constraints
-----------
- Wind measurements valid from 2026-06-19 12:00 only (vane commissioning);
  all earlier rows are excluded from this analysis.
- Option-A door exclusion applied by default: a segment is used when >= 70 %
  of its rows are calibration_valid. Counts with the filter relaxed are
  reported for the windward bin (the Jul 11 forced tests fall on a farm
  Saturday, like Jul 4).
- Known bias: the single-node model omits structure->air reheat after a
  flush, so fitted ach in high-exchange segments is an UNDERestimate.

Usage:  python model/campaign-summer-2026/ns9_direction_stratified.py
"""
import csv
import math
from datetime import datetime
from pathlib import Path

HERE   = Path(__file__).parent
CAL_IN = HERE / "calibration_input_2026-06-04_2026-07-12.csv"

WIND_VALID_FROM = datetime(2026, 6, 19, 12, 0, 0)   # vane commissioning
MIN_SEG_MIN     = 15
VALID_FRAC      = 0.70
ACH_GRID        = [x / 50.0 for x in range(1, 601)]   # 0.02 .. 12.0 /h

# adopted freem3 stage-1 parameters
K_SOLAR = 0.0847
C_EFF   = 2.894e6
P_TRANSP = 0.0003 * 2.45e6
ACH_INF = 0.203
ACH_M1  = 0.166
V, RHO, CP = 2900.0, 1.2, 1005.0

def ua(ach):
    return ach / 3600.0 * V * RHO * CP

def ch_open(bm, i):
    return ((bm >> (2 * i)) & 3) in (1, 2)

def dir_bin(deg):
    if deg >= 315 or deg < 45:   return "N  (windward)"
    if deg < 135:                return "E  (cross)"
    if deg < 225:                return "S  (leeward)"
    return "W  (cross)"

rows = []
with open(CAL_IN) as f:
    for r in csv.DictReader(f):
        try:
            rows.append((
                datetime.strptime(r["timestamp"], "%Y-%m-%dT%H:%M:%S"),
                float(r["T_in_C"]), float(r["T_out_C"]), float(r["lux"]),
                int(r["bitmask"]), float(r["wind_ms"]), float(r["wind_dir_deg"]),
                r["calibration_valid"] == "1",
            ))
        except (ValueError, KeyError):
            continue

rows = [r for r in rows if r[0] >= WIND_VALID_FROM]
print(f"rows with valid wind (>= {WIND_VALID_FROM}): {len(rows)}")

# --- segment the timeline by (M1,M2,M3) open-state ---
def state(bm):
    return (ch_open(bm, 0), ch_open(bm, 1), ch_open(bm, 2))

segments = []
seg = [rows[0]]
for r in rows[1:]:
    gap = (r[0] - seg[-1][0]).total_seconds() > 120
    if state(r[4]) != state(seg[-1][4]) or gap:
        segments.append(seg)
        seg = [r]
    else:
        seg.append(r)
segments.append(seg)

def fit_ach(seg):
    best = (None, 1e9)
    for ach in ACH_GRID:
        T = seg[0][1]; err = 0.0; n = 0
        for i in range(1, len(seg)):
            dt_s = (seg[i][0] - seg[i-1][0]).total_seconds()
            _, _, T_out, lux, *_ = seg[i-1]
            T += (K_SOLAR * lux - P_TRANSP - ua(ach) * (T - T_out)) / C_EFF * dt_s
            err += (T - seg[i][1]) ** 2; n += 1
        rmse = math.sqrt(err / n)
        if rmse < best[1]:
            best = (ach, rmse)
    return best

# --- per-segment fits, bucketed ---
from collections import defaultdict
buckets = defaultdict(list)          # (state_label, dir_bin) -> [ach_m3 estimates]
buckets_all = defaultdict(list)      # same, door filter relaxed

for seg in segments:
    dur_min = (seg[-1][0] - seg[0][0]).total_seconds() / 60.0
    if dur_min < MIN_SEG_MIN or len(seg) < 20:
        continue
    m1, m2, m3 = state(seg[0][4])
    if not m3:
        continue                     # M3-open segments only (target of NS-9 step 1)
    dirs = sorted(x[6] for x in seg)
    spds = sorted(x[5] for x in seg)
    db   = dir_bin(dirs[len(dirs) // 2])
    label = f"M3+{'M1' if m1 else ''}{'M2' if m2 else ''}" if (m1 or m2) else "M3 only"
    ach_tot, rmse = fit_ach(seg)
    ach_m3 = ach_tot - ACH_INF - ACH_M1 * (m1 + m2)
    valid_frac = sum(1 for x in seg if x[7]) / len(seg)
    rec = (ach_m3, dur_min, spds[len(spds) // 2], rmse)
    buckets_all[(label, db)].append(rec)
    if valid_frac >= VALID_FRAC:
        buckets[(label, db)].append(rec)

def report(title, bk):
    print(f"\n=== {title} ===")
    print(f"{'state':10} {'wind bin':14} {'segs':>4} {'min':>6} {'med v':>5}  "
          f"{'ach_m3 med':>10} {'IQR':>13}")
    for key in sorted(bk):
        recs = bk[key]
        a = sorted(x[0] for x in recs)
        mins = sum(x[1] for x in recs)
        v = sorted(x[2] for x in recs)[len(recs) // 2]
        q1, q3 = a[len(a) // 4], a[(3 * len(a)) // 4]
        print(f"{key[0]:10} {key[1]:14} {len(recs):4d} {mins:6.0f} {v:5.1f}  "
              f"{a[len(a)//2]:10.2f} [{q1:5.2f},{q3:5.2f}]")

report("Option-A door filter (>=70 % valid rows per segment)", buckets)
report("door filter RELAXED (all segments)", buckets_all)

# --- method control: M3-CLOSED segments should recover ~ach_inf + k*ach_m1 ---
print("\n=== method control (M3 closed; fitted total ach vs model prediction) ===")
ctl = defaultdict(list)
for seg in segments:
    dur_min = (seg[-1][0] - seg[0][0]).total_seconds() / 60.0
    if dur_min < MIN_SEG_MIN or len(seg) < 20:
        continue
    m1, m2, m3 = state(seg[0][4])
    if m3:
        continue
    if sum(1 for x in seg if x[7]) / len(seg) < VALID_FRAC:
        continue
    ach_tot, rmse = fit_ach(seg)
    pred = ACH_INF + ACH_M1 * (m1 + m2)
    ctl[(m1, m2)].append(ach_tot - pred)
for key in sorted(ctl):
    a = sorted(ctl[key])
    print(f"  M1={key[0]:d} M2={key[1]:d}: {len(a):3d} segs, "
          f"median fit-minus-model {a[len(a)//2]:+.2f} /h")

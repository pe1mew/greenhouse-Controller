"""Event study: what actually happens when M3 opens vs when M2 opens (control).

For every transition where a channel goes CLOSED -> MOVING_OPEN/OPEN, measure
T_in / RH_in / lux / T_out slopes in the 15 min BEFORE vs the window AFTER
(+3 min settle for motor travel, to +18 min). If M3 causes fast cooling, the
post-open T slope should turn sharply negative RELATIVE TO the pre slope, and
more so than for the M2 control events, after accounting for lux changes.
"""
import csv, math
from datetime import datetime

CSV = r"C:\Users\drasv\github\greenhouse-Controller\model\campaign-summer-2026\calibration_input_2026-06-04_2026-07-04.csv"

rows = []
with open(CSV) as f:
    for r in csv.DictReader(f):
        try:
            rows.append((
                datetime.strptime(r["timestamp"], "%Y-%m-%dT%H:%M:%S"),
                float(r["T_in_C"]), float(r["RH_in_pct"]),
                float(r["T_out_C"]), float(r["lux"]),
                int(r["bitmask"]), float(r["wind_ms"]),
            ))
        except ValueError:
            pass

def ch_state(bm, i):          # 2-bit field: 0=CLOSED 1=MOV_OPEN 2=OPEN 3=MOV_CLOSE
    return (bm >> (2 * i)) & 3

def ch_open(bm, i):           # open-ish = moving-open or open
    return ch_state(bm, i) in (1, 2)

def slope(pts):               # least-squares slope in unit/hour
    if len(pts) < 6: return None
    n = len(pts); xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    mx = sum(xs)/n; my = sum(ys)/n
    den = sum((x-mx)**2 for x in xs)
    if den == 0: return None
    return sum((x-mx)*(y-my) for x, y in zip(xs, ys)) / den * 3600.0

def window(rows, i0, t0, lo_s, hi_s, col):
    out = []
    j = i0
    while j >= 0 and (rows[j][0]-t0).total_seconds() >= lo_s:
        j -= 1
    for k in range(max(j,0), len(rows)):
        dt = (rows[k][0]-t0).total_seconds()
        if dt > hi_s: break
        if lo_s <= dt <= hi_s:
            out.append((dt, rows[k][col]))
    return out

def collect(events_ch, require_prev_open=None, forbid_ch_open=None):
    """Transitions of channel events_ch from CLOSED to MOV_OPEN/OPEN."""
    ev = []
    for i in range(1, len(rows)):
        t0, *_ = rows[i]
        bm_prev, bm = rows[i-1][5], rows[i][5]
        if ch_state(bm_prev, events_ch) == 0 and ch_state(bm, events_ch) in (1, 2):
            if (rows[i][0]-rows[i-1][0]).total_seconds() > 120: continue  # log gap
            if require_prev_open is not None and not all(ch_open(bm_prev, c) for c in require_prev_open): continue
            if forbid_ch_open is not None and any(ch_open(bm_prev, c) for c in forbid_ch_open): continue
            # daytime only (lux>5000) to match the user's observation
            if rows[i][4] < 5000: continue
            ev.append(i)
    return ev

def analyse(name, events):
    res = []
    for i in events:
        t0 = rows[i][0]
        pre  = {c: window(rows, i, t0, -900,  -30, col) for c, col in
                [("T",1),("RH",2),("lux",4),("Tout",3)]}
        post = {c: window(rows, i, t0,  180, 1080, col) for c, col in
                [("T",1),("RH",2),("lux",4),("Tout",3)]}
        s = {}
        ok = True
        for c in pre:
            sp, so = slope(pre[c]), slope(post[c])
            if sp is None or so is None: ok = False; break
            s[c+"_pre"], s[c+"_post"] = sp, so
        if not ok: continue
        s["T_lvl"]   = rows[i][1]; s["dT_oi"] = rows[i][1]-rows[i][3]
        s["lux_lvl"] = rows[i][4]; s["t"] = t0
        res.append(s)
    def med(key):
        v = sorted(x[key] for x in res)
        return v[len(v)//2] if v else float("nan")
    print(f"\n=== {name}  (n={len(res)}) ===")
    print(f"  T_in slope   pre {med('T_pre'):+6.2f} -> post {med('T_post'):+6.2f} C/h   (delta {med('T_post')-med('T_pre'):+.2f})")
    print(f"  RH_in slope  pre {med('RH_pre'):+6.2f} -> post {med('RH_post'):+6.2f} %/h")
    print(f"  lux slope    pre {med('lux_pre'):+8.0f} -> post {med('lux_post'):+8.0f} lux/h  (median lux at event {med('lux_lvl'):.0f})")
    print(f"  T_out slope  pre {med('Tout_pre'):+6.2f} -> post {med('Tout_post'):+6.2f} C/h")
    print(f"  median T_in-T_out at event: {med('dT_oi'):.1f} C")
    return res

# M3 opens on top of M1+M2 (the normal step-3 escalation)
m3ev = collect(2, require_prev_open=[0, 1])
# control: M2 opens on top of M1 (step-2 escalation) with M3 closed
m2ev = collect(1, require_prev_open=[0], forbid_ch_open=[2])
# extra control: M1 opens from all-closed (step-1)
m1ev = collect(0, forbid_ch_open=[1, 2])

r3 = analyse("M3 opens (M1+M2 already open -> 0b111)", m3ev)
r2 = analyse("CONTROL: M2 opens (M1 open, M3 closed -> 0b011)", m2ev)
r1 = analyse("CONTROL: M1 opens (all closed -> 0b001)", m1ev)

# Model-implied slope change at the median M3 event, for both ach_m3 hypotheses
C_EFF = 2.894e6; V=2900.0; RHO=1.2; CP=1005.0
def ua(ach): return ach/3600.0*V*RHO*CP
med_dT  = sorted(x["dT_oi"] for x in r3)[len(r3)//2]
for label, dach in [("measured ach_m3=0.05", 0.05), ("area-scaled ach_m3=1.32", 1.32)]:
    dslope = -ua(dach)*med_dT/C_EFF*3600.0
    print(f"\n  model-implied EXTRA T slope from opening M3 ({label}): {dslope:+.2f} C/h at dT={med_dT:.1f} C")

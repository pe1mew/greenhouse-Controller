"""July 11 forced M3-only test analysis (3x ~28 min, cloudy, M1/M2 closed).

Per segment (all-closed controls + M3-only windows):
- T_in / RH_in trajectory, T_out, lux, wind speed+direction
- Best-fit ACH by simulating C_eff*dT/dt = k_solar*lux - P_transp - UA(ACH)*(T_in-T_out)
  over the segment from its measured starting T_in (grid scan on ACH).
The all-closed segments calibrate the method (expect ~ach_inf=0.20/h).
"""
import csv, math
from datetime import datetime

LOG = r"C:\Users\drasv\github\greenhouse-Controller\model\campaign-summer-2026\2026-07-12_003904.log"
OUT = r"C:\Users\drasv\AppData\Local\Temp\claude\C--Users-drasv-github-greenhouse-Controller\09480fa7-f3f9-4406-bc23-b36a4c3e9128\scratchpad\lht65_jul11.csv"

# adopted stage-1 params (freem3 artifact)
K_SOLAR = 0.0847; C_EFF = 2.894e6; TRANSP = 0.0003; LV = 2.45e6
V = 2900.0; RHO = 1.2; CP = 1005.0
P_TRANSP = TRANSP * LV

def ua(ach): return ach / 3600.0 * V * RHO * CP

# --- indoor rows ---
tin = []   # (dt, T, RH)
wind = []  # (dt, speed, dir)
with open(LOG, encoding='utf-8', errors='replace') as f:
    for line in f:
        p = line.strip().split(',')
        if len(p) < 7 or p[1] != 'SENSOR_HR' or not p[0].startswith('2026-07-11'):
            continue
        dt = datetime.strptime(p[0], '%Y-%m-%dT%H:%M:%S')
        if p[3] == '0':
            tin.append((dt, int(p[5]) / 10.0, float(p[6])))
        elif p[3] == '1':
            wind.append((dt, int(p[5]) / 10.0, float(p[6])))

# --- outdoor rows (forward-fill later) ---
outd = []
with open(OUT) as f:
    for r in csv.DictReader(f):
        dt = datetime.strptime(r['dateTime'], '%Y-%m-%d %H:%M:%S')
        outd.append((dt, float(r['T_out_C']), float(r['RH_out_pct']), float(r['lux'])))

def out_at(dt):
    best = None
    for o in outd:
        if o[0] <= dt: best = o
        else: break
    return best

def seg_rows(t0s, t1s):
    t0 = datetime.strptime('2026-07-11T' + t0s, '%Y-%m-%dT%H:%M:%S')
    t1 = datetime.strptime('2026-07-11T' + t1s, '%Y-%m-%dT%H:%M:%S')
    rows = [(dt, T, RH) for dt, T, RH in tin if t0 <= dt <= t1]
    w = [(s, d) for dt, s, d in wind if t0 <= dt <= t1]
    return rows, w

def fit_ach(rows):
    """Grid-scan ACH; simulate T from rows[0], RMSE against measured."""
    best = (None, 1e9)
    for ach in [x / 100.0 for x in range(2, 801, 2)]:
        T = rows[0][1]; err = 0.0; n = 0
        for i in range(1, len(rows)):
            dt_s = (rows[i][0] - rows[i-1][0]).total_seconds()
            o = out_at(rows[i-1][0])
            if o is None: continue
            _, T_out, _, lux = o
            dTdt = (K_SOLAR * lux - P_TRANSP - ua(ach) * (T - T_out)) / C_EFF
            T += dTdt * dt_s
            err += (T - rows[i][1]) ** 2; n += 1
        rmse = math.sqrt(err / n)
        if rmse < best[1]: best = (ach, rmse)
    return best

def ah(T, RH):  # absolute humidity g/m3 (Magnus)
    es = 6.112 * math.exp(17.62 * T / (243.12 + T))
    return 216.7 * (RH / 100.0 * es) / (273.15 + T)

SEGS = [
    ("CONTROL all-closed pre-W1", "09:30:00", "10:06:00"),
    ("W1  M3-only",               "10:09:30", "10:36:30"),
    ("CONTROL all-closed W1-W2",  "10:39:30", "11:14:30"),
    ("W2  M3-only",               "11:17:30", "11:45:00"),
    ("CONTROL all-closed W2-W3",  "11:47:30", "12:17:00"),
    ("W3  M3-only",               "12:21:30", "12:48:30"),
]

print(f"{'segment':28} {'T_in start->end':>16} {'RH_in':>12} {'T_out':>6} {'lux':>6} "
      f"{'wind':>4} {'dir':>4}  {'fit ACH/h':>9} {'rmse':>5}")
for name, a, b in SEGS:
    rows, w = seg_rows(a, b)
    if len(rows) < 10:
        print(f"{name:28}  insufficient rows"); continue
    o0 = out_at(rows[len(rows)//2][0])
    ws = sorted(x[0] for x in w); wd = sorted(x[1] for x in w)
    ach, rmse = fit_ach(rows)
    print(f"{name:28} {rows[0][1]:6.1f} -> {rows[-1][1]:5.1f}  "
          f"{rows[0][2]:4.0f}->{rows[-1][2]:3.0f}%  {o0[1]:5.1f}  {o0[3]:6.0f} "
          f"{ws[len(ws)//2]:4.1f} {wd[len(wd)//2]:4.0f}  {ach:9.2f} {rmse:5.2f}")

# extra: AH balance during windows
print("\nAbsolute humidity (g/m3): indoor vs outdoor at window mid-points")
for name, a, b in SEGS:
    if not name.startswith('W'): continue
    rows, _ = seg_rows(a, b)
    mid = rows[len(rows)//2]; o = out_at(mid[0])
    print(f"  {name}: AH_in start {ah(rows[0][1], rows[0][2]):.1f} -> end {ah(rows[-1][1], rows[-1][2]):.1f};  AH_out {ah(o[1], o[2]):.1f}")

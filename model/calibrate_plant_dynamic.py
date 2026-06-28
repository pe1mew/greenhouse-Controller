"""
calibrate_plant_dynamic.py

Fit a per-channel additive ACH model against the summer-2026 campaign data
and evaluate against a held-out validation week (AC-9 / AC-10).

Parameterisation (7 parameters):
    k_solar         W per outdoor lux
    c_eff_mj_per_c  MJ/°C effective heat capacity
    transpiration   kg/s crop moisture load
    ach_inf         /h  baseline infiltration (all channels CLOSED)
    ach_m1          /h  incremental ACH when M1 is non-CLOSED
    ach_m2          /h  incremental ACH when M2 is non-CLOSED
    ach_m3          /h  incremental ACH when M3 is non-CLOSED

Total ACH at any instant:
    ach_total = ach_inf + ach_m1*m1_open + ach_m2*m2_open + ach_m3*m3_open

where m{k}_open = 1 if channel k has state != CLOSED (0) in the 16-bit bitmask.
EG1 override bits (12=WIND_OVERRIDE, 13=MOTOR_ALARM) are read but do NOT change
the ACH — the bitmask sub-row already encodes the resulting window state.

Advantages over the binary model (calibrate_plant_campaign.py):
  - M1-only ventilation (small 21-step window, ~low ACH) is distinguished from
    M1+M2+M3 full ventilation (adds large 171-step M3 panel, ~high ACH).
  - The M3 contribution is isolated so simulation.py's per-channel ach_roof/
    ach_wall fields can be calibrated directly from the fit.
  - Better generalisation: settings changes that alter the bitmask distribution
    (e.g. dwell tuning) are modelled at the per-channel level.

Train/validation split:
    Training:   Jun 4–18 (days 0–14)  — used for DE + NM optimisation
    Validation: Jun 19–25 (days 15–21) — held out; used only for AC-9 / AC-10

Usage:
    python model/calibrate_plant_dynamic.py [--plot] [--fast]
    python model/calibrate_plant_dynamic.py --plot --fast   # smoke-test
"""

from __future__ import annotations

import argparse
import csv
import json
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from scipy.optimize import differential_evolution, minimize
from scipy.signal import lfilter

ROOT   = Path(__file__).parent
CAL_IN = ROOT / "campaign-summer-2026" / "calibration_input_2026-06-04_2026-06-25.csv"
OLD_PLANT     = ROOT / "plant_calibrated.json"
BINARY_PLANT  = ROOT / "campaign-summer-2026" / "plant_calibrated_summer2026.json"
OUT_PLANT     = ROOT / "campaign-summer-2026" / "plant_calibrated_dynamic_summer2026.json"
OUT_PNG       = ROOT / "campaign-summer-2026" / "calibration_dynamic_summer2026.png"

# Validation split: Jun 19 00:00 Amsterdam local (UTC+2 in June) = Jun 18 22:00 UTC
VAL_SPLIT_UTC = datetime(2026, 6, 18, 22, 0, 0, tzinfo=timezone.utc).timestamp()

V       = 2400.0
RHO_AIR = 1.2
CP_AIR  = 1005.0
DT_S    = 30.0
C_EFF_AIR_FLOOR = V * RHO_AIR * CP_AIR / 1e6   # 2.89 MJ/°C

# Bitmask channel bit positions (see §5.1 of thermalProfileCampaign.md)
# bits 1..0 = M1, bits 3..2 = M2, bits 5..4 = M3; each: 0=CLOSED,1=MOVING_OPEN,2=OPEN,3=MOVING_CLOSE
CH_SHIFT = [0, 2, 4]   # bit shifts for M1, M2, M3

PARAM_NAMES = ("k_solar", "c_eff_mj_per_c", "transpiration_kg_s",
               "ach_inf", "ach_m1", "ach_m2", "ach_m3")
PARAM_BOUNDS = [
    (1e-4, 2.0),              # k_solar  W/lux
    (C_EFF_AIR_FLOOR, 200.0), # c_eff
    (0.0,  0.10),             # transpiration  kg/s
    (0.02, 10.0),             # ach_inf  (background, all closed)
    (0.0,  30.0),             # ach_m1   (incremental per M1)
    (0.0,  30.0),             # ach_m2   (incremental per M2)
    # M3 lower bound > 0: M3 has travel_m3=171 vs M1/M2 travel=21 (~8x larger
    # panel area), so it must contribute *some* incremental ventilation. Without
    # this floor the optimizer collapses ach_m3 to 0 because M3 appears almost
    # exclusively in combination with M1+M2, making the three-way interaction
    # nearly collinear with the M1+M2 two-way interaction.
    (0.05, 60.0),             # ach_m3   (incremental per M3, larger panel)
]


# ── psychrometrics ───────────────────────────────────────────────────────────
def ah_from_rh(rh, t):
    es = 611.2 * np.exp((17.62 * t) / (t + 243.12))
    return 2.166e-3 * (rh / 100.0 * es) / (t + 273.15)

def rh_from_ah(ah, t):
    es = 611.2 * np.exp((17.62 * t) / (t + 243.12))
    return np.clip(100.0 * ah * (t + 273.15) / 2.166e-3 / es, 0.0, 100.0)


# ── first-order integrator ───────────────────────────────────────────────────
def piecewise_lag(x, seg_starts, seg_alphas, x0):
    n = len(x)
    y = np.empty(n)
    state = float(x0)
    for i in range(len(seg_alphas)):
        s, e = int(seg_starts[i]), int(seg_starts[i + 1])
        if e <= s:
            continue
        a = float(seg_alphas[i])
        if not (0.0 < a < 1.0):
            return np.full(n, np.nan)
        zi = np.array([state - a * x[s]])
        seg, _ = lfilter([a], [1.0, -(1.0 - a)], x[s:e], zi=zi)
        y[s:e] = seg
        state = float(seg[-1])
    return y


def channel_open(bm_raw: np.ndarray, ch: int) -> np.ndarray:
    """True if channel ch (0=M1, 1=M2, 2=M3) is non-CLOSED in the raw bitmask."""
    return ((bm_raw >> CH_SHIFT[ch]) & 0x3) != 0


def vent_mask_from_bitmask(bm_raw: np.ndarray) -> np.ndarray:
    """Collapse 16-bit raw bitmask to 3-bit ventilation mask (bit k = M{k+1} non-CLOSED)."""
    return (channel_open(bm_raw, 0).astype(np.uint8)
            | (channel_open(bm_raw, 1).astype(np.uint8) << 1)
            | (channel_open(bm_raw, 2).astype(np.uint8) << 2))


def build_segments(vent_mask: np.ndarray):
    """Segment start indices and per-segment vent_mask (3-bit) when vent_mask changes."""
    transitions = np.flatnonzero(np.diff(vent_mask.astype(np.int16)) != 0) + 1
    seg_starts   = np.concatenate(([0], transitions, [len(vent_mask)])).astype(np.int64)
    seg_vent_mask = vent_mask[seg_starts[:-1]]
    return seg_starts, seg_vent_mask


def seg_ach_from_params(params: np.ndarray, seg_vent: np.ndarray) -> np.ndarray:
    """Compute per-segment total ACH (/h) from the 7-param vector."""
    _, _, _, ach_inf, ach_m1, ach_m2, ach_m3 = params
    m1 = (seg_vent >> 0) & 1
    m2 = (seg_vent >> 1) & 1
    m3 = (seg_vent >> 2) & 1
    return ach_inf + ach_m1 * m1 + ach_m2 * m2 + ach_m3 * m3


def simulate(params: np.ndarray,
             T_out: np.ndarray, RH_out: np.ndarray, lux: np.ndarray,
             seg_starts: np.ndarray, seg_ach_per_h: np.ndarray):
    k_solar, c_eff_mj, transp, *_ = params
    c_eff_J = c_eff_mj * 1e6

    seg_ach_s = seg_ach_per_h / 3600.0
    seg_at    = DT_S * seg_ach_s * V * RHO_AIR * CP_AIR / c_eff_J
    seg_ah    = DT_S * seg_ach_s

    row_ach_s = np.repeat(seg_ach_s, np.diff(seg_starts).astype(int))
    throughput = row_ach_s * V * RHO_AIR * CP_AIR

    T_eq  = T_out + k_solar * lux / throughput
    AH_eq = ah_from_rh(RH_out, T_out) + transp / (row_ach_s * V)

    T_in  = piecewise_lag(T_eq,  seg_starts, seg_at,  T_out[0])
    AH_in = np.maximum(piecewise_lag(AH_eq, seg_starts, seg_ah,
                                     ah_from_rh(RH_out[0:1], T_out[0:1])[0]), 0.0)
    return T_in, rh_from_ah(AH_in, T_in)


def loss_fn(params, T_out, RH_out, lux, T_ref, RH_ref, mask,
            seg_starts, seg_vent, w_rh=0.5):
    if np.any(np.array(params) < 0):
        return 1e9
    # Enforce ach_inf > 0 strictly (avoid div-by-zero in throughput)
    if params[3] < 0.01:
        return 1e9
    seg_ach = seg_ach_from_params(params, seg_vent)
    T_sim, RH_sim = simulate(params, T_out, RH_out, lux, seg_starts, seg_ach)
    if not np.all(np.isfinite(T_sim)):
        return 1e9
    if not mask.any():
        return 1e9
    return (float(np.mean((T_sim[mask] - T_ref[mask]) ** 2))
            + w_rh * float(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2)))


# ── loader (shared with calibrate_plant_campaign.py) ────────────────────────
def load_calibration_input(path):
    ts, T_in, RH_in, T_out, RH_out, lux_arr, bitmask, valid = \
        [], [], [], [], [], [], [], []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                dt   = datetime.strptime(r["timestamp"], "%Y-%m-%dT%H:%M:%S")
                t_in  = float(r["T_in_C"])
                rh_in = float(r["RH_in_pct"])
                t_out = float(r["T_out_C"])    if r["T_out_C"]    not in ("", "None") else None
                rh_out= float(r["RH_out_pct"]) if r["RH_out_pct"] not in ("", "None") else None
                lux   = float(r["lux"])         if r["lux"]         not in ("", "None") else None
                bm    = int(r["bitmask"])        if r["bitmask"]     not in ("", "None") else None
                v     = int(r["calibration_valid"])
            except (ValueError, KeyError):
                continue
            if t_out is None or rh_out is None or lux is None or bm is None:
                continue
            # Convert local naive timestamp to UTC unix seconds (Amsterdam = UTC+2 in June)
            ts.append(dt.timestamp() - 7200.0)
            T_in.append(t_in);   RH_in.append(rh_in)
            T_out.append(t_out); RH_out.append(rh_out)
            lux_arr.append(lux); bitmask.append(bm)
            valid.append(v)
    order = np.argsort(ts)
    return (np.array(ts)[order],
            np.array(T_in)[order],  np.array(RH_in)[order],
            np.array(T_out)[order], np.array(RH_out)[order],
            np.array(lux_arr)[order],
            np.array(bitmask, dtype=np.int32)[order],
            np.array(valid, dtype=bool)[order])


def resample_to_grid(ts, *arrays, dt=DT_S, max_gap=1800.0):
    t0, t1 = ts[0], ts[-1]
    n = int((t1 - t0) / dt) + 1
    grid = t0 + np.arange(n) * dt
    out = [grid]
    for arr in arrays:
        out.append(np.interp(grid, ts, arr.astype(float)))
    idx     = np.clip(np.searchsorted(ts, grid), 1, len(ts) - 1)
    nearest = np.minimum(grid - ts[idx - 1], ts[idx] - grid)
    data_mask = nearest <= max_gap
    return tuple(out), data_mask


def ffill_bool(ts_src, bool_src, grid):
    out = np.zeros(len(grid), dtype=bool)
    j = 0
    for i, t in enumerate(grid):
        while j + 1 < len(ts_src) and ts_src[j + 1] <= t:
            j += 1
        out[i] = bool(bool_src[j])
    return out


def old_binary_params(plant_json: Path, lux_max: float):
    with open(plant_json) as f:
        p = json.load(f)
    ach_closed = 0.5  # ACH_INF from simulation.py
    ach_open   = ach_closed + p["ach_roof"] + p["ach_wall"]
    k_solar    = p["solar_peak_w"] / lux_max if lux_max > 0 else 0.5
    return np.array([k_solar, p["c_eff_mj_per_c"], p["transpiration_kg_s"],
                     ach_closed, ach_open])


def rmse_rh_stats(T_sim, RH_sim, T_ref, RH_ref, mask):
    rmse_T  = float(np.sqrt(np.mean((T_sim[mask]  - T_ref[mask])  ** 2)))
    rmse_RH = float(np.sqrt(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2)))
    err_T   = T_sim[mask] - T_ref[mask]
    pct95   = float(np.percentile(np.abs(err_T), 95))
    bias_T  = float(np.mean(err_T))
    within1 = float(np.mean(np.abs(err_T) <= 1.0) * 100)
    return rmse_T, rmse_RH, pct95, bias_T, within1


# ── main ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--fast", action="store_true",
                    help="Reduced DE budget (quick smoke-test)")
    args = ap.parse_args()

    # ── load and grid ────────────────────────────────────────────────────────
    print(f"Loading {CAL_IN.name} ...")
    ts, T_in, RH_in, T_out, RH_out, lux, bitmask_raw, valid = \
        load_calibration_input(CAL_IN)
    n_total = len(ts)
    print(f"  {n_total} rows  /  {valid.sum()} valid ({valid.sum()*100//n_total}%)")

    print("Resampling to 30 s grid ...")
    (grid, gT_in, gRH_in, gT_out, gRH_out, glux, gbm_f), data_mask = resample_to_grid(
        ts, T_in, RH_in, T_out, RH_out, lux, bitmask_raw.astype(float))
    gbm_raw = gbm_f.astype(np.int32)

    # Forward-fill calibration_valid onto grid
    valid_grid  = ffill_bool(ts, valid, grid)
    full_mask   = valid_grid & data_mask   # all valid rows

    # Train / validation split
    train_mask = full_mask & (grid < VAL_SPLIT_UTC)
    val_mask   = full_mask & (grid >= VAL_SPLIT_UTC)
    n_train    = train_mask.sum()
    n_val      = val_mask.sum()
    n_grid     = len(grid)
    print(f"  Grid: {n_grid} points  |  train mask: {n_train} "
          f"({n_train*100//n_grid}%)  |  val mask: {n_val} "
          f"({n_val*100//n_grid}%)")
    print(f"  Validation split: Jun 19–25 "
          f"(grid[val_mask][0] = "
          f"{datetime.utcfromtimestamp(grid[val_mask][0]).strftime('%Y-%m-%d %H:%M') if val_mask.any() else 'N/A'} UTC)")

    # Bitmask -> per-channel open flags and ventilation segments
    vent_mask   = vent_mask_from_bitmask(gbm_raw)
    seg_starts, seg_vent = build_segments(vent_mask)
    n_seg = len(seg_vent)
    n_zero  = (seg_vent == 0).sum()
    n_nz    = n_seg - n_zero
    print(f"  Bitmask segments: {n_seg} total  |  {n_zero} all-closed  |  {n_nz} any-open")

    # Per-vent-mask coverage stats (training set only)
    vm_counts = {v: int(np.sum((vent_mask == v) & train_mask)) for v in range(8)}
    print("  Vent-mask coverage in training set (30 s rows):")
    for vm, cnt in sorted(vm_counts.items()):
        label = f"0b{vm:03b}  M1={'1' if vm&1 else '0'} M2={'1' if vm&2 else '0'} M3={'1' if vm&4 else '0'}"
        hours = cnt * DT_S / 3600
        print(f"    {label}  {cnt:6d} rows  ({hours:.1f} h)")

    lux_max = float(glux.max())

    # ── evaluate spring-2026 model (binary, old params) ─────────────────────
    from calibrate_plant_campaign import (simulate as sim_binary,
                                           build_segments as build_seg_binary)
    any_open  = (vent_mask > 0)
    bin_starts, bin_is_open = build_seg_binary(any_open)
    old_p = old_binary_params(OLD_PLANT, lux_max)
    old_seg_ach = np.where(bin_is_open, old_p[4], old_p[3])
    T_old, RH_old = sim_binary(old_p, gT_out, gRH_out, glux, bin_starts, bin_is_open)
    # (binary sim params = [k_solar, c_eff, transp, ach_closed, ach_open])

    print("\nSpring-2026 model on summer data:")
    for split_name, mask in [("train (Jun 4-18)", train_mask), ("val   (Jun 19-25)", val_mask)]:
        if not mask.any():
            continue
        r = rmse_rh_stats(T_old, RH_old, gT_in, gRH_in, mask)
        print(f"  {split_name}:  T RMSE {r[0]:.2f} C  RH RMSE {r[1]:.2f}%  "
              f"95th-pct err {r[2]:.2f} C  within±1C {r[4]:.1f}%")

    # ── evaluate binary summer-2026 model if available ───────────────────────
    if BINARY_PLANT.exists():
        with open(BINARY_PLANT) as f:
            bp = json.load(f)
        # Use the stored fit values (ach_closed_per_hr / ach_open_per_hr) when
        # present; the roof/wall split loses ach_closed because it uses ACH_INF=0.5
        # as baseline, which differs from the actual fitted ach_closed.
        if "ach_closed_per_hr" in bp and "k_solar_w_per_lux" in bp:
            bin_new_p = np.array([
                bp["k_solar_w_per_lux"],
                bp["c_eff_mj_per_c"],
                bp["transpiration_kg_s"],
                bp["ach_closed_per_hr"],
                bp["ach_open_per_hr"],
            ])
        else:
            bin_new_p = np.array([
                bp["solar_peak_w"] / lux_max,
                bp["c_eff_mj_per_c"],
                bp["transpiration_kg_s"],
                0.5,
                0.5 + bp["ach_roof"] + bp["ach_wall"],
            ])
        T_bin, RH_bin = sim_binary(bin_new_p, gT_out, gRH_out, glux, bin_starts, bin_is_open)
        print("\nBinary summer-2026 model:")
        for split_name, mask in [("train (Jun 4-18)", train_mask), ("val   (Jun 19-25)", val_mask)]:
            if not mask.any():
                continue
            r = rmse_rh_stats(T_bin, RH_bin, gT_in, gRH_in, mask)
            print(f"  {split_name}:  T RMSE {r[0]:.2f} C  RH RMSE {r[1]:.2f}%  "
                  f"95th-pct err {r[2]:.2f} C  within±1C {r[4]:.1f}%")

    # ── fit dynamic model ────────────────────────────────────────────────────
    print("\nFitting dynamic model on training set (Jun 4-18) ...")
    fn = lambda p: loss_fn(p, gT_out, gRH_out, glux, gT_in, gRH_in,
                            train_mask, seg_starts, seg_vent)

    popsize = 10 if args.fast else 25
    maxiter = 100 if args.fast else 400
    res_de = differential_evolution(
        fn, bounds=PARAM_BOUNDS,
        maxiter=maxiter, popsize=popsize, seed=42,
        polish=False, tol=1e-4,
        mutation=(0.5, 1.0), recombination=0.7, init="sobol",
        workers=1)
    print(f"  DE done: loss={res_de.fun:.4f}  iters={res_de.nit}")

    res_nm = minimize(fn, x0=res_de.x, method="Nelder-Mead",
                      options={"xatol": 1e-6, "fatol": 1e-5, "maxiter": 12000})
    new_p = res_nm.x
    print(f"  NM done: loss={res_nm.fun:.4f}")

    new_seg_ach = seg_ach_from_params(new_p, seg_vent)
    T_new, RH_new = simulate(new_p, gT_out, gRH_out, glux, seg_starts, new_seg_ach)

    print("\nNew parameters (dynamic model, summer-2026 training fit):")
    for n, v in zip(PARAM_NAMES, new_p):
        print(f"  {n:<25} {v:.5f}")
    k_solar, c_eff, transp, ach_inf, ach_m1, ach_m2, ach_m3 = new_p
    print(f"\n  Implied ACH by bitmask:")
    for vm in range(8):
        m1, m2, m3 = (vm>>0)&1, (vm>>1)&1, (vm>>2)&1
        label = f"0b{vm:03b}  M1={m1} M2={m2} M3={m3}"
        ach   = ach_inf + ach_m1*m1 + ach_m2*m2 + ach_m3*m3
        cnt   = vm_counts.get(vm, 0)
        print(f"    {label}  ACH={ach:.3f} /h   ({cnt} train rows)")

    # ── residual table ───────────────────────────────────────────────────────
    print(f"\n{'':25} {'train (Jun 4-18)':>20} {'val (Jun 19-25)':>20}")
    print("=" * 70)
    for label, T_sim, RH_sim in [
            ("spring-2026 model",  T_old, RH_old),
            ("binary summer-2026", T_bin if BINARY_PLANT.exists() else T_old, RH_bin if BINARY_PLANT.exists() else RH_old),
            ("dynamic summer-2026", T_new, RH_new),
    ]:
        if label == "binary summer-2026" and not BINARY_PLANT.exists():
            continue
        train_r = rmse_rh_stats(T_sim, RH_sim, gT_in, gRH_in, train_mask) if train_mask.any() else (None,)*5
        val_r   = rmse_rh_stats(T_sim, RH_sim, gT_in, gRH_in, val_mask)   if val_mask.any()   else (None,)*5
        def fmt(r):
            if r[0] is None:
                return "  n/a"
            return f"T={r[0]:.2f}C RH={r[1]:.1f}% ±1C={r[4]:.0f}%"
        print(f"  {label:<23}  {fmt(train_r):>22}  {fmt(val_r):>22}")
    print("=" * 70)
    if val_mask.any():
        rval = rmse_rh_stats(T_new, RH_new, gT_in, gRH_in, val_mask)
        ac9_pass = rval[2] <= 1.0 and rval[4] >= 95.0
        print(f"\n  AC-9 check (validation week): 95th-pct |err| = {rval[2]:.2f} C  "
              f"within±1C = {rval[4]:.1f}%   -> {'PASS' if ac9_pass else 'FAIL (target: <=1.0 C and >=95%)'}")
        print(f"  AC-10 check: RH RMSE on validation = {rval[1]:.2f}%  "
              f"(target <= 5%)  -> {'PASS' if rval[1] <= 5.0 else 'FAIL'}")

    # ── write output plant JSON ───────────────────────────────────────────────
    plant = {
        "_comment": (
            f"Dynamic per-channel calibration by calibrate_plant_dynamic.py "
            f"on summer-2026 campaign (Jun 4-18 training set, Option A door exclusion). "
            f"k_solar={k_solar:.4f} W/lux, c_eff={c_eff:.2f} MJ/degC, "
            f"ach_inf={ach_inf:.3f} /h, ach_m1={ach_m1:.3f} /h, "
            f"ach_m2={ach_m2:.3f} /h, ach_m3={ach_m3:.3f} /h. "
            f"Validation T RMSE: "
            f"{rmse_rh_stats(T_new, RH_new, gT_in, gRH_in, val_mask)[0]:.2f} C."
            if val_mask.any() else ""
        ),
        "volume_m3":          V,
        "ach_inf":            round(ach_inf, 4),
        "ach_m1":             round(ach_m1, 4),
        "ach_m2":             round(ach_m2, 4),
        "ach_m3":             round(ach_m3, 4),
        "ach_roof":           round(ach_m1, 4),    # simulation.py compat: ach_roof ~ ach_m1
        "ach_wall":           round(ach_m3, 4),    # simulation.py compat: ach_wall ~ ach_m3
        "transpiration_kg_s": round(transp, 6),
        "solar_peak_w":       round(k_solar * lux_max, 0),
        "c_eff_mj_per_c":     round(c_eff, 3),
    }
    with open(OUT_PLANT, "w") as f:
        json.dump(plant, f, indent=2)
    print(f"\nWrote {OUT_PLANT.relative_to(ROOT)}")

    # ── comparison plot ───────────────────────────────────────────────────────
    if not args.plot:
        print("(pass --plot to generate calibration_dynamic_summer2026.png)")
        return

    try:
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates
        import matplotlib.patches as mpatches
        from matplotlib.lines import Line2D

        invalid_grid = ~valid_grid & data_mask
        dts = [datetime.utcfromtimestamp(t) for t in grid]
        val_start_dt = datetime.utcfromtimestamp(VAL_SPLIT_UTC)

        fig, axes = plt.subplots(4, 1, figsize=(17, 14), sharex=True)
        fig.suptitle(
            "Plant model calibration — per-channel dynamic model (summer-2026)\n"
            f"Training: Jun 4–18  |  Validation: Jun 19–25  "
            f"(AC-9 target: ±1 °C ≥ 95% of valid samples)",
            fontsize=12, fontweight="bold"
        )

        def shade_axes(ax):
            ax.fill_between(dts, 0, 1, where=invalid_grid,
                            transform=ax.get_xaxis_transform(),
                            color="#e8a0a0", alpha=0.3)
            ax.axvline(val_start_dt, color="purple", lw=1.2, ls="--", alpha=0.6)

        # Panel 1 — Temperature
        ax = axes[0]
        shade_axes(ax)
        ax.plot(dts, gT_out, color="0.65", lw=0.6, label="T_out")
        ax.plot(dts, gT_in,  "b-", lw=0.9, alpha=0.75, label="T_in measured")
        ax.plot(dts, T_old,  color="salmon", lw=0.9, ls="--",
                label=f"spring-2026 (RMSE train {rmse_rh_stats(T_old,RH_old,gT_in,gRH_in,train_mask)[0]:.2f}C / val {rmse_rh_stats(T_old,RH_old,gT_in,gRH_in,val_mask)[0]:.2f}C)")
        if BINARY_PLANT.exists():
            ax.plot(dts, T_bin, color="orange", lw=0.9, ls="-.",
                    label=f"binary fit (RMSE train {rmse_rh_stats(T_bin,RH_bin,gT_in,gRH_in,train_mask)[0]:.2f}C / val {rmse_rh_stats(T_bin,RH_bin,gT_in,gRH_in,val_mask)[0]:.2f}C)")
        ax.plot(dts, T_new,  "g-", lw=1.1,
                label=f"dynamic fit  (RMSE train {rmse_rh_stats(T_new,RH_new,gT_in,gRH_in,train_mask)[0]:.2f}C / val {rmse_rh_stats(T_new,RH_new,gT_in,gRH_in,val_mask)[0]:.2f}C)")
        ax.set_ylabel("Temperature [C]")
        ax.legend(fontsize=7, loc="upper right", ncol=2)
        ax.grid(alpha=0.2)

        # Panel 2 — T residuals
        ax = axes[1]
        shade_axes(ax)
        ax.axhline(0, color="k", lw=0.7)
        ax.axhline( 1, color="g", lw=0.5, ls=":")
        ax.axhline(-1, color="g", lw=0.5, ls=":")
        ax.plot(dts, T_old - gT_in, color="salmon", lw=0.6, alpha=0.7, label="spring err")
        if BINARY_PLANT.exists():
            ax.plot(dts, T_bin - gT_in, color="orange", lw=0.6, alpha=0.7, label="binary err")
        ax.plot(dts, T_new - gT_in, "g-", lw=0.7, alpha=0.85, label="dynamic err")
        ax.set_ylabel("T error [C]\n(sim − meas)")
        ax.set_ylim(-15, 15)
        ax.legend(fontsize=7, loc="upper right")
        ax.grid(alpha=0.2)

        # Panel 3 — RH
        ax = axes[2]
        shade_axes(ax)
        ax.plot(dts, gRH_in,  "b-", lw=0.9, alpha=0.75, label="RH measured")
        ax.plot(dts, RH_old, color="salmon", lw=0.9, ls="--", label="spring-2026")
        if BINARY_PLANT.exists():
            ax.plot(dts, RH_bin, color="orange", lw=0.9, ls="-.", label="binary fit")
        ax.plot(dts, RH_new,  "g-", lw=1.1, label="dynamic fit")
        ax.set_ylabel("RH [%]")
        ax.set_ylim(0, 105)
        ax.legend(fontsize=7, loc="upper right", ncol=2)
        ax.grid(alpha=0.2)

        # Panel 4 — per-channel open state + lux
        ax = axes[3]
        ax2 = ax.twinx()
        shade_axes(ax)
        colors = {"M1": "#2980b9", "M2": "#27ae60", "M3": "#e67e22"}
        for i, ch in enumerate(["M1", "M2", "M3"]):
            ch_open = channel_open(gbm_raw, i).astype(float)
            # offset slightly so traces don't overlap
            ax.step(dts, ch_open * (1 - 0.07*i), where="post",
                    color=colors[ch], lw=1.2, label=ch, alpha=0.85)
        ax.set_ylabel("Channel open (0/1)")
        ax.set_ylim(-0.1, 1.3)
        ax2.fill_between(dts, 0, glux / 1000, alpha=0.2, color="#e6a817", step="post")
        ax2.set_ylabel("Outdoor lux [k]", color="#e6a817")
        ax2.tick_params(axis="y", labelcolor="#e6a817")
        ax.legend(fontsize=7, loc="upper left", ncol=3)
        ax.set_xlabel("Date (UTC)")
        ax.grid(alpha=0.2, axis="x")

        loc = mdates.AutoDateLocator()
        axes[-1].xaxis.set_major_locator(loc)
        axes[-1].xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc))
        fig.autofmt_xdate(rotation=0, ha="center")

        # Annotation: validation split line
        for ax in axes:
            ax.axvline(val_start_dt, color="purple", lw=1.2, ls="--", alpha=0.6)
        axes[0].annotate("val split →", xy=(val_start_dt, axes[0].get_ylim()[1]),
                         xytext=(-30, -14), textcoords="offset points",
                         fontsize=7, color="purple")

        plt.tight_layout(rect=[0, 0, 1, 0.95])
        fig.savefig(OUT_PNG, dpi=130, bbox_inches="tight")
        plt.close(fig)
        print(f"Saved {OUT_PNG.relative_to(ROOT)}")
    except ImportError:
        print("matplotlib not available — skipping plot")


if __name__ == "__main__":
    main()

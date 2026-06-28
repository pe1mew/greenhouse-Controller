"""
calibrate_plant_campaign.py

Fit plant-model parameters from the summer-2026 merged calibration input
(prepare_calibration_input.py output) and compare against the previous
spring-2026 calibration stored in plant_calibrated.json.

Key differences from calibrate_plant.py:
  - Input: the merged 30 s calibration CSV (door1_open, door2_open,
    calibration_valid, bitmask already joined)
  - Schedule: bitmask > 0 drives open/closed (not a fixed 10:00-18:00 clock)
  - Exclusion: only calibration_valid==1 rows enter the loss function;
    the integrator still runs over the full time series so temperature
    state is never reset across door-open gaps.
  - Comparison: evaluates both old and new parameters over the full
    22-day series and writes a 5-panel overlay PNG.

Usage:
    python model/calibrate_plant_campaign.py [--plot] [--fast]

    --plot   always generate comparison PNG (default: only if fit improves)
    --fast   reduced DE budget for a quick smoke-test run
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from datetime import datetime
from pathlib import Path

import numpy as np
from scipy.optimize import differential_evolution, minimize
from scipy.signal import lfilter

ROOT   = Path(__file__).parent
CAL_IN = ROOT / "campaign-summer-2026" / "calibration_input_2026-06-04_2026-06-25.csv"
OLD_PLANT = ROOT / "plant_calibrated.json"
OUT_PLANT = ROOT / "campaign-summer-2026" / "plant_calibrated_summer2026.json"
OUT_PNG   = ROOT / "campaign-summer-2026" / "calibration_compare_summer2026.png"

V       = 2400.0
RHO_AIR = 1.2
CP_AIR  = 1005.0
DT_S    = 30.0
C_EFF_AIR_FLOOR = V * RHO_AIR * CP_AIR / 1e6   # 2.89 MJ/degC

PARAM_BOUNDS = [
    (1e-4, 2.0),               # k_solar  W/lux
    (C_EFF_AIR_FLOOR, 200.0),  # c_eff_mj_per_c
    (0.0,  0.10),              # transpiration_kg_s
    (0.05, 30.0),              # ach_closed /h
    (0.05, 60.0),              # ach_open   /h
]
PARAM_NAMES = ("k_solar", "c_eff_mj_per_c", "transpiration_kg_s",
               "ach_closed_per_hr", "ach_open_per_hr")


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


def simulate(params, T_out, RH_out, lux, seg_starts, seg_is_open):
    k_solar, c_eff_mj, transp, ach_closed, ach_open = params
    c_eff_J = c_eff_mj * 1e6

    seg_ach     = np.where(seg_is_open, ach_open, ach_closed) / 3600.0
    seg_at      = DT_S * seg_ach * V * RHO_AIR * CP_AIR / c_eff_J
    seg_ah      = DT_S * seg_ach

    row_ach = np.repeat(seg_ach, np.diff(seg_starts).astype(int))
    throughput = row_ach * V * RHO_AIR * CP_AIR

    T_eq  = T_out + k_solar * lux / throughput
    AH_eq = ah_from_rh(RH_out, T_out) + transp / (row_ach * V)

    T_in  = piecewise_lag(T_eq,  seg_starts, seg_at, T_out[0])
    AH_in = np.maximum(piecewise_lag(AH_eq, seg_starts, seg_ah,
                                     ah_from_rh(RH_out[0:1], T_out[0:1])[0]), 0.0)
    return T_in, rh_from_ah(AH_in, T_in)


def build_segments(is_open):
    transitions = np.flatnonzero(np.diff(is_open.astype(np.int8)) != 0) + 1
    seg_starts  = np.concatenate(([0], transitions, [len(is_open)])).astype(np.int64)
    return seg_starts, is_open[seg_starts[:-1]]


def loss_fn(params, T_out, RH_out, lux, T_ref, RH_ref, mask,
            seg_starts, seg_is_open, w_rh=0.5):
    if np.any(np.array(params) <= 0):
        return 1e9
    T_sim, RH_sim = simulate(params, T_out, RH_out, lux, seg_starts, seg_is_open)
    if not np.all(np.isfinite(T_sim)):
        return 1e9
    if not mask.any():
        return 1e9
    return (float(np.mean((T_sim[mask] - T_ref[mask]) ** 2))
            + w_rh * float(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2)))


# ── data loader ──────────────────────────────────────────────────────────────
def load_calibration_input(path):
    ts, T_in, RH_in, T_out, RH_out, lux_arr, bitmask, valid = \
        [], [], [], [], [], [], [], []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                dt = datetime.strptime(r["timestamp"], "%Y-%m-%dT%H:%M:%S")
                t_in  = float(r["T_in_C"])
                rh_in = float(r["RH_in_pct"])
                t_out = float(r["T_out_C"])   if r["T_out_C"]    not in ("", "None") else None
                rh_out= float(r["RH_out_pct"]) if r["RH_out_pct"] not in ("", "None") else None
                lux   = float(r["lux"])        if r["lux"]         not in ("", "None") else None
                bm    = int(r["bitmask"])       if r["bitmask"]     not in ("", "None") else None
                v     = int(r["calibration_valid"])
            except (ValueError, KeyError):
                continue
            if t_out is None or rh_out is None or lux is None or bm is None:
                continue
            ts.append(dt.timestamp())
            T_in.append(t_in); RH_in.append(rh_in)
            T_out.append(t_out); RH_out.append(rh_out)
            lux_arr.append(lux); bitmask.append(bm)
            valid.append(v)
    order = np.argsort(ts)
    return (np.array(ts)[order],
            np.array(T_in)[order], np.array(RH_in)[order],
            np.array(T_out)[order], np.array(RH_out)[order],
            np.array(lux_arr)[order],
            np.array(bitmask)[order],
            np.array(valid, dtype=bool)[order])


def resample_to_grid(ts, *arrays, dt=DT_S, max_gap=1800.0):
    t0, t1 = ts[0], ts[-1]
    n = int((t1 - t0) / dt) + 1
    grid = t0 + np.arange(n) * dt
    out = [grid]
    for arr in arrays:
        out.append(np.interp(grid, ts, arr.astype(float)))
    # Staleness mask: within max_gap of a real sample
    idx   = np.clip(np.searchsorted(ts, grid), 1, len(ts) - 1)
    nearest = np.minimum(grid - ts[idx - 1], ts[idx] - grid)
    mask  = nearest <= max_gap
    return tuple(out), mask


def interp_bool(ts, bool_arr, grid):
    """Forward-fill a boolean array onto grid."""
    out = np.zeros(len(grid), dtype=bool)
    j = 0
    for i, t in enumerate(grid):
        while j + 1 < len(ts) and ts[j + 1] <= t:
            j += 1
        out[i] = bool(bool_arr[j])
    return out


# ── old-parameters derivation ────────────────────────────────────────────────
def old_params_from_plant_json(path, lux_max):
    with open(path) as f:
        p = json.load(f)
    ach_extra  = p["ach_roof"] + p["ach_wall"]
    ach_closed = 0.5                           # ACH_INF hardcoded in simulation.py
    ach_open   = ach_closed + ach_extra
    k_solar    = p["solar_peak_w"] / lux_max if lux_max > 0 else 0.5
    return np.array([k_solar, p["c_eff_mj_per_c"],
                     p["transpiration_kg_s"], ach_closed, ach_open])


# ── main ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--fast", action="store_true",
                    help="Reduced DE budget (quick smoke-test)")
    args = ap.parse_args()

    print(f"Loading {CAL_IN.name} ...")
    ts, T_in, RH_in, T_out, RH_out, lux, bitmask, valid = \
        load_calibration_input(CAL_IN)
    n_total = len(ts)
    n_valid = valid.sum()
    print(f"  {n_total} rows total  /  {n_valid} valid ({n_valid*100//n_total}%)")
    print(f"  T_in  range: {T_in.min():.1f} .. {T_in.max():.1f} C")
    print(f"  T_out range: {T_out.min():.1f} .. {T_out.max():.1f} C")
    print(f"  lux   range: {int(lux.min())} .. {int(lux.max())}")

    # Resample to regular 30 s grid
    print("Resampling to 30 s grid ...")
    (grid, gT_in, gRH_in, gT_out, gRH_out, glux, gbm_f), data_mask = resample_to_grid(
        ts, T_in, RH_in, T_out, RH_out, lux, bitmask.astype(float))
    gbm = gbm_f > 0.5   # boolean: any window open

    # Valid mask: grid points near a calibration_valid==1 row
    valid_mask_on_raw = valid & data_mask[:len(valid)] if len(valid) <= len(data_mask) else valid
    # Forward-fill calibration_valid flag onto grid
    valid_grid = interp_bool(ts, valid, grid)
    fit_mask   = valid_grid & data_mask

    n_fit = fit_mask.sum()
    print(f"  Grid: {len(grid)} points  /  {n_fit} in fit mask ({n_fit*100//len(grid)}%)")

    seg_starts, seg_is_open = build_segments(gbm)

    # ── load old parameters ──────────────────────────────────────────────────
    lux_max = float(glux.max())
    old_p   = old_params_from_plant_json(OLD_PLANT, lux_max)
    print(f"\nOld parameters (plant_calibrated.json / spring-2026):")
    for n, v in zip(PARAM_NAMES, old_p):
        print(f"  {n:<25} {v:.5f}")

    # Evaluate old params on the full grid
    T_old, RH_old = simulate(old_p, gT_out, gRH_out, glux, seg_starts, seg_is_open)
    rmse_T_old  = float(np.sqrt(np.mean((T_old[fit_mask]  - gT_in[fit_mask])  ** 2)))
    rmse_RH_old = float(np.sqrt(np.mean((RH_old[fit_mask] - gRH_in[fit_mask]) ** 2)))
    print(f"  -> on summer-2026 valid rows:  T RMSE {rmse_T_old:.2f} C  "
          f"RH RMSE {rmse_RH_old:.2f} %")

    # ── calibrate on summer-2026 data ────────────────────────────────────────
    print("\nFitting on summer-2026 data (Option A: door-open rows excluded) ...")
    fn = lambda p: loss_fn(p, gT_out, gRH_out, glux, gT_in, gRH_in,
                           fit_mask, seg_starts, seg_is_open)

    popsize = 8 if args.fast else 20
    maxiter = 80 if args.fast else 300
    res_de = differential_evolution(
        fn, bounds=PARAM_BOUNDS,
        maxiter=maxiter, popsize=popsize, seed=42,
        polish=False, tol=1e-4,
        mutation=(0.5, 1.0), recombination=0.7, init="sobol",
        workers=1)
    print(f"  DE done: loss={res_de.fun:.4f}  iters={res_de.nit}")

    res_nm = minimize(fn, x0=res_de.x, method="Nelder-Mead",
                      options={"xatol": 1e-6, "fatol": 1e-5, "maxiter": 10000})
    new_p = res_nm.x
    print(f"  NM done: loss={res_nm.fun:.4f}")

    T_new, RH_new = simulate(new_p, gT_out, gRH_out, glux, seg_starts, seg_is_open)
    rmse_T_new  = float(np.sqrt(np.mean((T_new[fit_mask]  - gT_in[fit_mask])  ** 2)))
    rmse_RH_new = float(np.sqrt(np.mean((RH_new[fit_mask] - gRH_in[fit_mask]) ** 2)))

    print(f"\nNew parameters (summer-2026 campaign fit):")
    for n, v in zip(PARAM_NAMES, new_p):
        print(f"  {n:<25} {v:.5f}")
    print(f"  -> on summer-2026 valid rows:  T RMSE {rmse_T_new:.2f} C  "
          f"RH RMSE {rmse_RH_new:.2f} %")

    # ── parameter comparison table ───────────────────────────────────────────
    print(f"\n{'':25} {'spring-2026':>14} {'summer-2026':>14}  {'change':>10}")
    print("-" * 70)
    for name, old, new in zip(PARAM_NAMES, old_p, new_p):
        chg = (new - old) / abs(old) * 100 if old != 0 else float("inf")
        print(f"  {name:<23} {old:>14.5f} {new:>14.5f}  {chg:>+9.1f}%")
    print("-" * 70)
    print(f"  {'T  RMSE (valid rows)':<23} {rmse_T_old:>14.2f} {rmse_T_new:>14.2f}  "
          f"{(rmse_T_new-rmse_T_old)/rmse_T_old*100:>+9.1f}%")
    print(f"  {'RH RMSE (valid rows)':<23} {rmse_RH_old:>14.2f} {rmse_RH_new:>14.2f}  "
          f"{(rmse_RH_new-rmse_RH_old)/rmse_RH_old*100:>+9.1f}%")

    # ── write calibrated plant JSON ──────────────────────────────────────────
    k_solar, c_eff, transp, ach_cl, ach_op = new_p
    ach_extra = max(0.0, ach_op - ach_cl)
    plant = {
        "_comment": (f"Calibrated by calibrate_plant_campaign.py on summer-2026 "
                     f"campaign data (Option A door exclusion). "
                     f"k_solar={k_solar:.4f} W/lux, c_eff={c_eff:.2f} MJ/degC, "
                     f"ach_open={ach_op:.2f} /h, ach_closed={ach_cl:.2f} /h. "
                     f"T RMSE {rmse_T_new:.2f} C vs old {rmse_T_old:.2f} C."),
        "volume_m3":          V,
        "ach_roof":           round(ach_extra * (8.0  / 56.0), 3),
        "ach_wall":           round(ach_extra * (40.0 / 56.0), 3),
        "transpiration_kg_s": round(transp, 6),
        "solar_peak_w":       round(k_solar * lux_max, 0),
        "c_eff_mj_per_c":     round(c_eff, 3),
    }
    with open(OUT_PLANT, "w") as f:
        json.dump(plant, f, indent=2)
    print(f"\nWrote {OUT_PLANT.relative_to(ROOT)}")

    # ── comparison plot ───────────────────────────────────────────────────────
    if args.plot or rmse_T_new < rmse_T_old:
        try:
            import matplotlib.pyplot as plt
            import matplotlib.dates as mdates
            import matplotlib.patches as mpatches

            # Forward-fill door open flags for shading
            door_open = interp_bool(ts, ~valid & (bitmask == bitmask), grid)
            # Actually: door open = calibration_valid==0 AND outdoor data was fresh
            # Approximate: any row where calibration_valid=0 near the grid point
            invalid_grid = ~valid_grid & data_mask

            dts = [datetime.utcfromtimestamp(t) for t in grid]

            fig, axes = plt.subplots(4, 1, figsize=(16, 14), sharex=True)
            fig.suptitle(
                "Plant model calibration — spring-2026 vs summer-2026 campaign\n"
                f"(Option A: door-open rows excluded from fit; "
                f"fit on {n_fit} / {len(grid)} grid points)",
                fontsize=12, fontweight="bold"
            )

            # Panel 1: Temperature
            ax = axes[0]
            ax.fill_between(dts, 0, 1, where=invalid_grid,
                            transform=ax.get_xaxis_transform(),
                            color="#e8a0a0", alpha=0.35, label="door open (excluded)")
            ax.fill_between(dts, 0, 1, where=~data_mask,
                            transform=ax.get_xaxis_transform(),
                            color="#cccccc", alpha=0.5, label="outdoor stale")
            ax.plot(dts, gT_out, color="0.6", lw=0.7, label="T_out")
            ax.plot(dts, gT_in,  "b-", lw=0.9, alpha=0.8, label="T_in measured")
            ax.plot(dts, T_old,  "r--", lw=1.1, label=f"spring-2026 model  (RMSE {rmse_T_old:.2f} C)")
            ax.plot(dts, T_new,  "g-",  lw=1.1, label=f"summer-2026 model  (RMSE {rmse_T_new:.2f} C)")
            ax.set_ylabel("Temperature [C]")
            ax.legend(fontsize=7, loc="upper right", ncol=3)
            ax.grid(alpha=0.2)

            # Panel 2: Residuals
            ax = axes[1]
            ax.fill_between(dts, 0, 1, where=invalid_grid,
                            transform=ax.get_xaxis_transform(),
                            color="#e8a0a0", alpha=0.35)
            ax.axhline(0, color="k", lw=0.7)
            ax.plot(dts, T_old - gT_in, "r-", lw=0.7, alpha=0.7,
                    label=f"spring err  bias {np.mean(T_old[fit_mask]-gT_in[fit_mask]):+.2f} C")
            ax.plot(dts, T_new - gT_in, "g-", lw=0.7, alpha=0.8,
                    label=f"summer err  bias {np.mean(T_new[fit_mask]-gT_in[fit_mask]):+.2f} C")
            ax.set_ylabel("T error [C]\n(sim − meas)")
            ax.set_ylim(-15, 15)
            ax.legend(fontsize=7, loc="upper right")
            ax.grid(alpha=0.2)

            # Panel 3: RH
            ax = axes[2]
            ax.fill_between(dts, 0, 1, where=invalid_grid,
                            transform=ax.get_xaxis_transform(),
                            color="#e8a0a0", alpha=0.35)
            ax.plot(dts, gRH_in,  "b-",  lw=0.9, alpha=0.8, label="RH_in measured")
            ax.plot(dts, RH_old,  "r--", lw=1.0,
                    label=f"spring-2026  RMSE {rmse_RH_old:.1f}%")
            ax.plot(dts, RH_new,  "g-",  lw=1.0,
                    label=f"summer-2026  RMSE {rmse_RH_new:.1f}%")
            ax.set_ylabel("RH [%]")
            ax.set_ylim(0, 105)
            ax.legend(fontsize=7, loc="upper right", ncol=2)
            ax.grid(alpha=0.2)

            # Panel 4: bitmask + lux
            ax = axes[3]
            ax2 = ax.twinx()
            ax.fill_between(dts, 0, 1, where=invalid_grid,
                            transform=ax.get_xaxis_transform(),
                            color="#e8a0a0", alpha=0.35, label="door open")
            ax.step(dts, gbm.astype(float), where="post", color="#2980b9",
                    lw=1.2, label="bitmask > 0 (any window open)")
            ax.set_ylabel("Window open (any)")
            ax.set_ylim(-0.1, 1.4)
            ax2.fill_between(dts, 0, glux / 1000, alpha=0.25, color="#e6a817",
                             step="post")
            ax2.set_ylabel("Outdoor lux [k]", color="#e6a817")
            ax2.tick_params(axis="y", labelcolor="#e6a817")
            ax.legend(fontsize=7, loc="upper left")
            ax.set_xlabel("Date (local)")
            ax.grid(alpha=0.2, axis="x")

            loc = mdates.AutoDateLocator()
            axes[-1].xaxis.set_major_locator(loc)
            axes[-1].xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc))
            fig.autofmt_xdate(rotation=0, ha="center")
            plt.tight_layout(rect=[0, 0, 1, 0.95])
            fig.savefig(OUT_PNG, dpi=130, bbox_inches="tight")
            plt.close(fig)
            print(f"Saved {OUT_PNG.relative_to(ROOT)}")
        except ImportError:
            print("matplotlib not available — skipping plot")


if __name__ == "__main__":
    main()

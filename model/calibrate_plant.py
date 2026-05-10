#!/usr/bin/env python3
"""
Fit plant-model parameters in `settings.json` / `settings_optimised.json`
against the live indoor sensors in `srcData/`.

Open-loop model: assumes the windows were CLOSED for the duration of the
archive (i.e. ACH = ach_inf only). Solar gain is taken from the outdoor
sensor's measured lumosity rather than the synthetic NOAA solar model, so
cloudy and clear days are handled equally. The model integrates a
first-order thermal/moisture lag (matching `simulation.plant_step`) and is
fit by minimising mean-squared error against the indoor T (and RH)
recorded by LHT65-03 in kas 1 (and LHT65-02 in kas 2).

Parameters fit (per indoor sensor):
    k_solar         W per outdoor-lux  (replaces solar_peak_w)
    c_eff_mj_per_c  MJ/°C effective heat capacity
    transpiration_kg_s  crop moisture load
    ach_inf         per-hour background infiltration

The volume, ACH per vent, RHO_AIR, CP_AIR are held fixed at their settings
values (these are physical constants of the building, not crop-dependent).

Usage:
    python calibrate_plant.py            # fit both sensors over full archive
    python calibrate_plant.py --plot     # also save a fit-vs-measured PNG

Output:
    Console: per-sensor best-fit parameter table.
    calibrate_plant_<sensor>.png   (only with --plot)
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np
from scipy.optimize import minimize, differential_evolution
from scipy.signal  import lfilter

ROOT = Path(__file__).parent
SRC  = ROOT / "srcData"

OUTDOOR = SRC / "greenhouseClimate-lht65-20_2026-03-17_to_2026-05-07.csv"
INDOOR  = {
    # LHT65-03 (kas 1) was excluded after the first fit revealed
    # ach_closed > ach_open for that compartment, suggesting the 10:00-18:00
    # schedule does not apply there. The calibration now uses kas 2 only.
    "LHT65-02_kas2": SRC / "greenhouseClimate-LHT65-02_2026-03-17_to_2026-05-07.csv",
}

# Physical constants (held fixed during the fit)
V       = 2400.0    # m³ greenhouse air volume
RHO_AIR = 1.2       # kg/m³
CP_AIR  = 1005.0    # J/kg/°C

DT_S    = 60.0      # resampling step for the integrator [s]


# ── Psychrometrics ─────────────────────────────────────────────────────────
def ah_from_rh(rh_pct: np.ndarray, t_c: np.ndarray) -> np.ndarray:
    """Magnus saturation vapour pressure → absolute humidity [kg/m³]."""
    es = 611.2 * np.exp((17.62 * t_c) / (t_c + 243.12))
    e  = (rh_pct / 100.0) * es
    return 2.166e-3 * (e / (t_c + 273.15))   # kg/m³


def rh_from_ah(ah: np.ndarray, t_c: np.ndarray) -> np.ndarray:
    es = 611.2 * np.exp((17.62 * t_c) / (t_c + 243.12))
    e  = ah * (t_c + 273.15) / 2.166e-3
    rh = 100.0 * e / es
    return np.clip(rh, 0.0, 100.0)


# ── CSV loader ──────────────────────────────────────────────────────────────
@dataclass
class Series:
    t: np.ndarray   # unix seconds (sorted, deduped)
    T: np.ndarray   # °C
    RH: np.ndarray  # %
    lux: Optional[np.ndarray]  # lux (None for indoor sensors)


def load(path: Path, has_lux: bool) -> Series:
    ts, T, H, L = [], [], [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                dt = datetime.strptime(r["dateTime"], "%Y-%m-%d %H:%M:%S")
                t  = float(r["airTemperature"])
                h  = float(r["airHumidity"])
            except (KeyError, ValueError):
                continue
            ts.append(dt.replace(tzinfo=timezone.utc).timestamp())
            T.append(t); H.append(h)
            if has_lux:
                lx_raw = r.get("lumosity", "")
                L.append(float(lx_raw) if lx_raw not in (None, "", "NULL") else math.nan)
    order = np.argsort(ts)
    t_arr = np.array(ts)[order]
    # deduplicate equal timestamps (keep first)
    keep = np.concatenate(([True], np.diff(t_arr) > 0))
    t_arr = t_arr[keep]
    T_arr = np.array(T)[order][keep]
    H_arr = np.array(H)[order][keep]
    L_arr = (np.array(L)[order][keep]) if has_lux else None
    return Series(t=t_arr, T=T_arr, RH=H_arr, lux=L_arr)


def resample(series: Series, t_grid: np.ndarray,
             max_gap_s: float = 3600.0):
    """Linear interpolation onto t_grid. Returns (T, RH, lux, mask) where
    mask is True only on grid points whose nearest source samples are
    within max_gap_s — so long-gap regions can be excluded from the fit
    without confusing the integrator."""
    T  = np.interp(t_grid, series.t, series.T)
    RH = np.interp(t_grid, series.t, series.RH)
    if series.lux is not None:
        lux_clean = np.where(np.isnan(series.lux), 0.0, series.lux)
        L = np.interp(t_grid, series.t, lux_clean)
    else:
        L = None

    # Build a "trustworthy" mask: True iff the grid time is within max_gap_s
    # of an actual sample on either side.
    idx = np.searchsorted(series.t, t_grid)
    idx = np.clip(idx, 1, len(series.t) - 1)
    left  = series.t[idx - 1]
    right = series.t[idx]
    nearest = np.minimum(t_grid - left, right - t_grid)
    mask = (nearest <= max_gap_s) & (t_grid >= series.t[0]) & (t_grid <= series.t[-1])
    return T, RH, L, mask


# ── Plant model (closed-windows, lux-driven solar) ─────────────────────────
def _piecewise_first_order_lag(x: np.ndarray, alphas: np.ndarray,
                                seg_starts: np.ndarray, seg_alphas: np.ndarray,
                                x0: float) -> np.ndarray:
    """
    Run y[i] = (1-alpha[i]) y[i-1] + alpha[i] x[i] where alpha[i] is
    piecewise constant. seg_starts is the array of segment-start indices
    (length S+1, last entry = len(x)); seg_alphas is the alpha for each
    segment (length S). Each segment is integrated with scipy.signal.lfilter,
    state propagated across segment boundaries.
    """
    n = len(x)
    y = np.empty(n)
    state = x0
    for i in range(len(seg_alphas)):
        s, e = seg_starts[i], seg_starts[i + 1]
        if e <= s:
            continue
        a = float(seg_alphas[i])
        if a >= 1.0 or a <= 0.0:
            # Outside the stable Euler regime — return sentinel.
            return np.full(n, np.nan)
        a_coef = 1.0 - a
        # zi convention: y[0]_seg = alpha x[s] + a_coef * y[-1]_seg.
        # Choose so that y[0]_seg = state (the previous segment's last value).
        zi = np.array([state - a * x[s]])
        seg, _zf = lfilter([a], [1.0, -a_coef], x[s:e], zi=zi)
        y[s:e] = seg
        state = float(seg[-1])
    return y


def simulate(params: np.ndarray,
             T_out: np.ndarray, RH_out: np.ndarray,
             lux: np.ndarray, dt_s: float,
             seg_starts: np.ndarray,
             seg_is_open: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    """
    Integrate first-order thermal & moisture lag with a known ACH schedule.

    params = [k_solar, c_eff_mj, transp_kg_s, ach_closed, ach_open]
    seg_starts/seg_is_open describe the precomputed open/closed schedule
    (windows ∈ {open, closed} blocks). When ACH varies per segment so do
    air_throughput, T_eq, AH_eq and the integrator's alpha.
    """
    k_solar, c_eff_mj, transp, ach_closed, ach_open = params

    seg_ach = np.where(seg_is_open, ach_open, ach_closed)        # h^-1 per segment
    # Per-segment derived scalars
    seg_ach_per_s     = seg_ach / 3600.0
    seg_air_throughput = seg_ach_per_s * V * RHO_AIR * CP_AIR    # W/degC
    c_eff_J            = c_eff_mj * 1e6
    seg_alpha_T  = dt_s * seg_air_throughput / c_eff_J
    seg_alpha_AH = dt_s * seg_ach_per_s

    # Per-row ACH (for computing equilibrium targets)
    ach_per_s_row = np.where(
        np.repeat(seg_is_open, np.diff(seg_starts).astype(int)),
        ach_open / 3600.0, ach_closed / 3600.0
    )
    air_throughput_row = ach_per_s_row * V * RHO_AIR * CP_AIR

    Qs    = k_solar * lux
    T_eq  = T_out + Qs / air_throughput_row
    AH_out_arr = ah_from_rh(RH_out, T_out)
    AH_eq = AH_out_arr + transp / (ach_per_s_row * V)

    T_in  = _piecewise_first_order_lag(T_eq,  None, seg_starts, seg_alpha_T,  T_out[0])
    AH_in = _piecewise_first_order_lag(AH_eq, None, seg_starts, seg_alpha_AH, AH_out_arr[0])
    AH_in = np.maximum(AH_in, 0.0)
    RH_in = rh_from_ah(AH_in, T_in)
    return T_in, RH_in


# ── Fit harness ─────────────────────────────────────────────────────────────
def loss(params: np.ndarray,
         T_out: np.ndarray, RH_out: np.ndarray, lux: np.ndarray,
         T_ref: np.ndarray, RH_ref: np.ndarray, mask: np.ndarray,
         seg_starts: np.ndarray, seg_is_open: np.ndarray,
         w_rh: float, dt_s: float) -> float:
    if np.any(params <= 0):
        return 1e9
    T_sim, RH_sim = simulate(params, T_out, RH_out, lux, dt_s,
                             seg_starts, seg_is_open)
    if not np.all(np.isfinite(T_sim)) or not np.all(np.isfinite(RH_sim)):
        return 1e9
    if not mask.any():
        return 1e9
    err_T  = float(np.mean((T_sim[mask]  - T_ref[mask])  ** 2))
    err_RH = float(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2))
    return err_T + w_rh * err_RH


# Physical bounds for the five parameters:
#   k_solar  [W/lux]
#   c_eff_mj_per_c  [MJ/degC]  — must be >= air mass V*rho*cp/1e6 (2.89)
#   transpiration_kg_s [kg/s]
#   ach_closed [/h]   — windows closed (night).
#   ach_open   [/h]   — windows open (day, 10:00–18:00 schedule).
C_EFF_AIR_FLOOR_MJ = V * RHO_AIR * CP_AIR / 1e6   # 2.89 MJ/degC
PARAM_BOUNDS = [
    (1e-4, 1.0),
    (C_EFF_AIR_FLOOR_MJ, 200.0),
    (0.0,  0.05),
    (0.1,  60.0),
    (0.1,  60.0),
]
PARAM_NAMES = ("k_solar", "c_eff_mj_per_c", "transpiration_kg_s",
               "ach_closed_per_hr", "ach_open_per_hr")

# Daily ventilation schedule (local time as recorded by the sensor).
# The user's greenhouse runs the windows on a fixed day-time schedule:
# opened at 10:00, closed at 18:00. Hours [WIN_OPEN_H, WIN_CLOSE_H) = open.
WIN_OPEN_H  = 10
WIN_CLOSE_H = 18


def build_schedule(t_grid: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    """
    Compute the open/closed schedule on a given timestamp grid.

    Returns (seg_starts, seg_is_open):
      seg_starts[k] is the index in t_grid where segment k begins
                    (seg_starts[-1] = len(t_grid))
      seg_is_open[k] is True iff segment k is in the windows-open window.
    """
    hours = ((t_grid % 86400) / 3600.0)
    is_open = (hours >= WIN_OPEN_H) & (hours < WIN_CLOSE_H)
    transitions = np.flatnonzero(np.diff(is_open.astype(np.int8)) != 0) + 1
    seg_starts = np.concatenate(([0], transitions, [len(t_grid)])).astype(np.int64)
    seg_is_open = is_open[seg_starts[:-1]]
    return seg_starts, seg_is_open


def fit_one_sensor(name: str, indoor_path: Path,
                   outdoor: Series, plot: bool) -> dict:
    indoor = load(indoor_path, has_lux=False)

    # Common time window where both sensors have data
    t0 = max(outdoor.t[0], indoor.t[0])
    t1 = min(outdoor.t[-1], indoor.t[-1])
    n  = int((t1 - t0) // DT_S) + 1
    grid = t0 + np.arange(n) * DT_S

    T_out, RH_out, lux,  m_out = resample(outdoor, grid)
    T_in_ref, RH_in_ref, _, m_in = resample(indoor, grid)
    mask = m_out & m_in   # only score where both sensors have a sample within 1 h

    seg_starts, seg_is_open = build_schedule(grid)

    coverage = mask.mean()
    print(f"  Common grid: {n} steps ({n*DT_S/86400:.1f} d); "
          f"trustworthy coverage {coverage*100:.0f}%; "
          f"{int(seg_is_open.sum())} open / {int((~seg_is_open).sum())} closed segments")

    # Global search over the bounded box, then a bounded local refine.
    de_loss = lambda p: loss(p, T_out, RH_out, lux, T_in_ref, RH_in_ref,
                              mask, seg_starts, seg_is_open, 0.5, DT_S)
    res_de = differential_evolution(
        de_loss, bounds=PARAM_BOUNDS,
        maxiter=200, popsize=20, seed=1,
        polish=False, tol=1e-4, mutation=(0.5, 1.0), recombination=0.7,
        init="sobol")

    res_nm = minimize(de_loss, x0=res_de.x, method="Nelder-Mead",
                      bounds=PARAM_BOUNDS,
                      options={"xatol": 1e-6, "fatol": 1e-5, "maxiter": 8000})
    k_solar, c_eff, transp, ach_closed, ach_open = res_nm.x

    # Final residuals (masked)
    T_sim, RH_sim = simulate(
        np.array([k_solar, c_eff, transp, ach_closed, ach_open]),
        T_out, RH_out, lux, DT_S, seg_starts, seg_is_open)
    rmse_T  = float(np.sqrt(np.mean((T_sim[mask]  - T_in_ref[mask])  ** 2)))
    rmse_RH = float(np.sqrt(np.mean((RH_sim[mask] - RH_in_ref[mask]) ** 2)))
    bias_T  = float(np.mean(T_sim[mask]  - T_in_ref[mask]))
    bias_RH = float(np.mean(RH_sim[mask] - RH_in_ref[mask]))

    print(f"\n=== {name} ===")
    print(f"  k_solar         = {k_solar:.5f}  W per outdoor lux")
    print(f"  c_eff_mj_per_c  = {c_eff:.2f}    MJ/degC")
    print(f"  transpiration   = {transp:.5f}  kg/s")
    print(f"  ach_closed      = {ach_closed:.3f}  /hour  (windows closed, "
          f"{WIN_CLOSE_H:02d}:00..{WIN_OPEN_H:02d}:00)")
    print(f"  ach_open        = {ach_open:.3f}  /hour  (windows open, "
          f"{WIN_OPEN_H:02d}:00..{WIN_CLOSE_H:02d}:00)")
    print(f"  --- residuals (sim - measured), masked to {mask.sum()*DT_S/86400:.1f} days of valid coverage ---")
    print(f"  T   RMSE = {rmse_T:.2f} degC  bias = {bias_T:+.2f} degC")
    print(f"  RH  RMSE = {rmse_RH:.2f} %     bias = {bias_RH:+.2f} %")
    print(f"  T_in:  measured {T_in_ref[mask].min():.1f}..{T_in_ref[mask].max():.1f} degC  |  sim {T_sim[mask].min():.1f}..{T_sim[mask].max():.1f} degC")

    # Conversion hint: solar_peak_w in synthetic model
    # max-lux x k_solar ~ noon-peak Q_solar
    suggested_solar_peak_w = float(np.nanmax(lux) * k_solar)
    print(f"  -> equivalent solar_peak_w (synthetic model) ~ {suggested_solar_peak_w:.0f} W")

    if plot:
        try:
            import matplotlib.pyplot as plt
            fig, axes = plt.subplots(3, 1, figsize=(12, 9), sharex=True)
            t_plot = (grid - grid[0]) / 86400.0   # days from start
            axes[0].plot(t_plot, T_out, lw=0.8, color="0.5", label="outdoor T")
            axes[0].plot(t_plot, T_in_ref, "b-",  lw=0.9, label=f"measured T  ({name})")
            axes[0].plot(t_plot, T_sim,    "r--", lw=0.9, label="simulated T (closed)")
            axes[0].set_ylabel("Temperature [°C]")
            axes[0].legend(fontsize=8); axes[0].grid(alpha=0.3)
            axes[1].plot(t_plot, RH_out,  lw=0.8, color="0.5", label="outdoor RH")
            axes[1].plot(t_plot, RH_in_ref, "b-",  lw=0.9, label="measured RH")
            axes[1].plot(t_plot, RH_sim,    "r--", lw=0.9, label="simulated RH")
            axes[1].set_ylabel("RH [%]"); axes[1].set_ylim(0, 105)
            axes[1].legend(fontsize=8); axes[1].grid(alpha=0.3)
            axes[2].plot(t_plot, lux, "y-", lw=0.6, label="outdoor lumosity")
            axes[2].set_ylabel("Lux"); axes[2].set_xlabel("Days from archive start")
            axes[2].legend(fontsize=8); axes[2].grid(alpha=0.3)
            fig.suptitle(f"Plant calibration — {name}  "
                         f"(T RMSE {rmse_T:.2f} °C, RH RMSE {rmse_RH:.1f} %)",
                         fontsize=11)
            png = ROOT / f"calibrate_plant_{name}.png"
            fig.tight_layout()
            fig.savefig(png, dpi=130)
            plt.close(fig)
            print(f"  Saved {png.name}")
        except ImportError:
            print("  (matplotlib not available — skip plot)")

    return {
        "name":              name,
        "k_solar_w_per_lux": k_solar,
        "c_eff_mj_per_c":    c_eff,
        "transpiration_kg_s": transp,
        "ach_closed_per_hr": ach_closed,
        "ach_open_per_hr":   ach_open,
        "rmse_T":            rmse_T,
        "rmse_RH":           rmse_RH,
        "suggested_solar_peak_w": suggested_solar_peak_w,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plot", action="store_true", help="save fit-vs-measured PNGs")
    args = ap.parse_args()

    print(f"Loading outdoor archive: {OUTDOOR.name}")
    outdoor = load(OUTDOOR, has_lux=True)
    print(f"  {len(outdoor.t)} samples, {(outdoor.t[-1]-outdoor.t[0])/86400:.1f} days")

    fits = []
    for name, p in INDOOR.items():
        fits.append(fit_one_sensor(name, p, outdoor, plot=args.plot))

    print("\n=== Summary ===")
    print(f"{'sensor':<18} {'k_solar':>10} {'c_eff':>8} {'transp':>9} {'ach_close':>9} {'ach_open':>9} {'T RMSE':>8} {'RH RMSE':>8}")
    for f in fits:
        print(f"{f['name']:<18} {f['k_solar_w_per_lux']:>10.5f} "
              f"{f['c_eff_mj_per_c']:>8.2f} {f['transpiration_kg_s']:>9.5f} "
              f"{f['ach_closed_per_hr']:>9.3f} {f['ach_open_per_hr']:>9.3f} "
              f"{f['rmse_T']:>8.2f} {f['rmse_RH']:>8.2f}")

    # Write a calibrated plant model JSON and a settings JSON that references
    # it. The plant section was split out of settings.json so the controller
    # config and physical greenhouse description can be versioned/reused
    # independently.
    avg = lambda key: float(np.mean([f[key] for f in fits]))
    ach_closed_avg = avg("ach_closed_per_hr")
    ach_open_avg   = avg("ach_open_per_hr")
    # Default split: M1 + M2 = 16 h^-1, M3 = 40 h^-1 -> M3 share = 40/56 = 5/7.
    # ach_extra = ach_open - ach_closed is the additional airflow attributable
    # to opening all vents; we distribute it between roof and wall vents in
    # the original 16:40 ratio so simulation.py's per-channel ach_roof /
    # ach_wall stays meaningful.
    ach_extra    = max(0.0, ach_open_avg - ach_closed_avg)
    ach_roof_fit = ach_extra * (8.0 / 56.0)
    ach_wall_fit = ach_extra * (40.0 / 56.0)

    plant_payload = {
        "_comment": (f"Calibrated by calibrate_plant.py against srcData/ "
                     f"(scheduled-window model, lux-driven solar; sensors: "
                     f"{', '.join(f['name'] for f in fits)}). Per-sensor "
                     f"k_solar={[round(float(f['k_solar_w_per_lux']),4) for f in fits]} W/lux, "
                     f"c_eff={[round(float(f['c_eff_mj_per_c']),2) for f in fits]} MJ/degC, "
                     f"ach_open={[round(float(f['ach_open_per_hr']),2) for f in fits]} /h, "
                     f"ach_closed={[round(float(f['ach_closed_per_hr']),2) for f in fits]} /h. "
                     f"simulation.py uses ACH_INF=0.5 as a constant; consider "
                     f"raising to {ach_closed_avg:.2f} (the fit ach_closed) "
                     f"for full fidelity."),
        "volume_m3":           V,
        "ach_roof":             round(ach_roof_fit, 2),
        "ach_wall":             round(ach_wall_fit, 2),
        "transpiration_kg_s":   round(avg("transpiration_kg_s"), 5),
        "solar_peak_w":         round(avg("suggested_solar_peak_w"), 0),
        "c_eff_mj_per_c":       round(avg("c_eff_mj_per_c"), 2),
    }

    settings_payload = {
        "_comment": ("Greenhouse Controller simulation -- generated by "
                     "calibrate_plant.py. Controller config matches "
                     "settings_optimised.json; the plant section is split "
                     "out into plant_calibrated.json (referenced via "
                     "plant_file)."),
        "_note": (f"Calibrated against the live greenhouse with a known "
                  f"window schedule: open {WIN_OPEN_H:02d}:00 .. "
                  f"{WIN_CLOSE_H:02d}:00, closed otherwise. "
                  f"ach_closed ~ {ach_closed_avg:.2f} /h, "
                  f"ach_open ~ {ach_open_avg:.2f} /h."),
        "climate": {
            "t_max_day":  28, "t_max_ngt":  20,
            "rh_min_day": 50, "rh_max_day": 75,
            "rh_min_ngt": 55, "rh_max_ngt": 80,
            "hyst_t":      5, "hyst_rh":     5,
            "rh_ctrl_en":  1, "cr_priority": 0,
            "avg_win_t":   6, "avg_win_rh":  5,
        },
        "wind": {
            "wind_prot_en": 1, "v_max": 6,
            "dir_excl_low": 0, "dir_excl_high": 0,
        },
        "motor": {
            "travel_m1": 21, "travel_m2": 21, "travel_m3": 171,
            "dwell_open_m1": 300, "dwell_open_m2": 300, "dwell_open_m3": 300,
            "dwell_close_m1": 0, "dwell_close_m2": 0, "dwell_close_m3": 0,
        },
        "system": {
            "poll_interval": 60,
            "lat_deg": 52, "lat_frac": 0,
            "lon_deg":  5, "lon_frac": 0,
        },
        "plant_file": "plant_calibrated.json",
    }

    import json
    plant_path    = ROOT / "plant_calibrated.json"
    settings_path = ROOT / "settings_calibrated.json"
    with open(plant_path, "w") as f:
        json.dump(plant_payload, f, indent=2)
    with open(settings_path, "w") as f:
        json.dump(settings_payload, f, indent=2)
    print(f"\nWrote {plant_path.name} and {settings_path.name}")
    print(f"  Run: python simulation.py <input> {settings_path.name}")


if __name__ == "__main__":
    main()

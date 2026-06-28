"""
calibrate_plant_constrained.py

Constrained 6-parameter plant calibration embodying two physical priors
accepted after the full per-bitmask identification study (2026-06-27):

  Prior 1 — M1 = M2:  ach_m2 = ach_m1
    M1 and M2 are identical 21-step roof windows (same geometry, same travel).
    Only 15 rows of M2-alone data exist in the 21-day campaign — not enough
    to identify ach_m2 independently. Equality by construction is the right
    choice; the data cannot refute it.

  Prior 2 — M3 dominates when open:  ach_m3 >> ach_m1
    M3 is a 171-step ridge panel (~8x M1's travel and area). When M3 is open,
    M1/M2's incremental contribution is ~1/(1+8) = 11 % of the M3-driven flow
    — accepted as a small correction, modelled explicitly but not dominant.
    This does NOT mean M3's contribution is dropped; it means M3 dominates the
    ventilation in any state where it is open.

Six free parameters:  k_solar, c_eff_mj_per_c, transpiration_kg_s,
                       ach_inf, ach_m1, ach_m3
Constraint (not fitted):  ach_m2 = ach_m1

Two-stage fit:
  Stage 1 — M3-closed states (vent_mask & 0b100 == 0):
      0b000  18903 rows    0b001  4326 rows
      0b010     15 rows    0b011   905 rows   total 24149 training rows
      k_solar, c_eff, transp, ach_inf, ach_m1 fitted here.
      0b011 rows provide independent constraint on ach_m1 via ach(0b011)
      = ach_inf + 2*ach_m1 (because ach_m2 = ach_m1).

  Stage 2 — M3-open states (vent_mask & 0b100 != 0):
      0b100   4 rows    0b101  362 rows
      0b110  20 rows    0b111 1775 rows   total 2161 training rows
      ach_m3 fitted with Stage 1 params locked and ach_m2 = ach_m1.
      The 1775 rows of M1+M2+M3 (0b111) — 14.8 h of full-ventilation during
      peak solar events — provide the primary signal: the model must use a
      large enough ach_m3 to explain why T_in stays near setpoint despite high
      lux, given the known solar-gain coefficient from Stage 1.
      Lower bound: ach_m3 >= ach_m1_stage1 (M3 must contribute at least as
      much as M1 given its larger panel area).

Firmware limitation:
    The staged opening sequence (M1 -> M1+M2 -> M1+M2+M3) means M3 is never
    opened in isolation during normal operation (0b100 has only 4 training rows).
    There is no admin test mode to bypass the staged sequence. ach_m3 must be
    identified indirectly from M3-combined states, dominated by 0b111.
    See thermalProfileCampaign.md section 9.7 for full discussion.

Usage:
    python model/calibrate_plant_constrained.py [--plot] [--fast]
"""

from __future__ import annotations

import argparse
import json
from datetime import datetime
from pathlib import Path

import numpy as np
from scipy.optimize import differential_evolution, minimize

import sys
sys.path.insert(0, str(Path(__file__).parent))
from calibrate_plant_dynamic import (
    load_calibration_input, resample_to_grid, ffill_bool,
    vent_mask_from_bitmask, build_segments, simulate,
    rmse_rh_stats, channel_open, ah_from_rh, rh_from_ah,
    CAL_IN, BINARY_PLANT, OLD_PLANT,
    VAL_SPLIT_UTC, V, RHO_AIR, CP_AIR, DT_S, C_EFF_AIR_FLOOR,
)
from calibrate_plant_campaign import (
    simulate as sim_binary, build_segments as build_seg_binary,
)

OUT_PLANT = Path(__file__).parent / "campaign-summer-2026" / "plant_calibrated_constrained_summer2026.json"
OUT_PNG   = Path(__file__).parent / "campaign-summer-2026" / "calibration_constrained_summer2026.png"

S1_NAMES  = ("k_solar", "c_eff_mj_per_c", "transpiration_kg_s", "ach_inf", "ach_m1")
S1_BOUNDS = [
    (1e-4, 2.0),
    (C_EFF_AIR_FLOOR, 200.0),
    (0.0,  0.10),
    (0.02, 10.0),
    (0.0,  30.0),
]


def seg_ach_c6(s1_params, ach_m3, seg_vent):
    """Per-segment ACH for the constrained 6-param model (ach_m2 = ach_m1)."""
    _, _, _, ach_inf, ach_m1 = s1_params
    m1 = (seg_vent >> 0) & 1
    m2 = (seg_vent >> 1) & 1
    m3 = (seg_vent >> 2) & 1
    return ach_inf + ach_m1 * (m1 + m2) + ach_m3 * m3


def simulate_c6(s1_params, ach_m3, T_out, RH_out, lux, seg_starts, seg_vent):
    seg_ach = seg_ach_c6(s1_params, ach_m3, seg_vent)
    k_solar, c_eff, transp, ach_inf, ach_m1 = s1_params
    dummy7 = np.array([k_solar, c_eff, transp, ach_inf, ach_m1, ach_m1, ach_m3])
    return simulate(dummy7, T_out, RH_out, lux, seg_starts, seg_ach)


def enforce_bounds(params, bounds):
    return any(p < lo or p > hi for p, (lo, hi) in zip(params, bounds))


def loss_s1(p, T_out, RH_out, lux, T_ref, RH_ref, mask, seg_starts, seg_vent, w_rh=0.5):
    if enforce_bounds(p, S1_BOUNDS):
        return 1e9
    if p[3] < 0.01:
        return 1e9
    T_sim, RH_sim = simulate_c6(p, 0.0, T_out, RH_out, lux, seg_starts, seg_vent)
    if not np.all(np.isfinite(T_sim)):
        return 1e9
    return (float(np.mean((T_sim[mask] - T_ref[mask]) ** 2))
            + w_rh * float(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2)))


def loss_s2(ach_m3_arr, s1_params, lo_m3, T_out, RH_out, lux,
            T_ref, RH_ref, mask, seg_starts, seg_vent, w_rh=0.5):
    ach_m3 = float(ach_m3_arr[0])
    if ach_m3 < lo_m3:
        return 1e9
    T_sim, RH_sim = simulate_c6(s1_params, ach_m3, T_out, RH_out, lux, seg_starts, seg_vent)
    if not np.all(np.isfinite(T_sim)):
        return 1e9
    return (float(np.mean((T_sim[mask] - T_ref[mask]) ** 2))
            + w_rh * float(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--fast", action="store_true", help="Reduced DE budget")
    args = ap.parse_args()

    # ── load + grid ───────────────────────────────────────────────────────────
    print(f"Loading {CAL_IN.name} ...")
    ts, T_in, RH_in, T_out, RH_out, lux, bitmask_raw, valid = \
        load_calibration_input(CAL_IN)

    print("Resampling to 30 s grid ...")
    (grid, gT_in, gRH_in, gT_out, gRH_out, glux, gbm_f), data_mask = \
        resample_to_grid(ts, T_in, RH_in, T_out, RH_out, lux,
                         bitmask_raw.astype(float))
    gbm_raw   = gbm_f.astype(np.int32)
    vent_mask = vent_mask_from_bitmask(gbm_raw)
    seg_starts, seg_vent = build_segments(vent_mask)

    valid_grid = ffill_bool(ts, valid, grid)
    full_mask  = valid_grid & data_mask
    train_mask = full_mask & (grid < VAL_SPLIT_UTC)
    val_mask   = full_mask & (grid >= VAL_SPLIT_UTC)
    lux_max    = float(glux.max())

    # M3-closed (Stage 1): vent_mask bits[4] = 0
    m3_closed = (vent_mask & 0b100) == 0
    s1_mask   = train_mask & m3_closed
    # M3-open (Stage 2): vent_mask bits[4] = 1
    m3_open   = (vent_mask & 0b100) != 0
    s2_mask   = train_mask & m3_open

    print(f"\n  Priors: ach_m2 = ach_m1  (M1=M2, identical window geometry)")
    print(f"          ach_m3 >= ach_m1  (M3 dominates, 171-step panel vs 21-step M1)")
    print(f"\n  Stage 1 mask (M3-closed): {s1_mask.sum()} training rows")
    for vm, label in [(0,"0b000 all-closed"), (1,"0b001 M1-only"),
                       (2,"0b010 M2-only"),   (3,"0b011 M1+M2")]:
        n = int((train_mask & (vent_mask == vm)).sum())
        print(f"    {label:20s}  {n:5d} rows")
    print(f"\n  Stage 2 mask (M3-open):  {s2_mask.sum()} training rows")
    for vm, label in [(4,"0b100 M3-only"),   (5,"0b101 M1+M3"),
                       (6,"0b110 M2+M3"),     (7,"0b111 M1+M2+M3")]:
        n = int((train_mask & (vent_mask == vm)).sum())
        print(f"    {label:20s}  {n:5d} rows")

    # ── Stage 1 ───────────────────────────────────────────────────────────────
    print(f"\nStage 1: k_solar / c_eff / transp / ach_inf / ach_m1 ...")
    fn1 = lambda p: loss_s1(p, gT_out, gRH_out, glux, gT_in, gRH_in,
                              s1_mask, seg_starts, seg_vent)
    res1_de = differential_evolution(
        fn1, bounds=S1_BOUNDS,
        maxiter=(80 if args.fast else 300),
        popsize=(8 if args.fast else 20),
        seed=42, polish=False, tol=1e-4,
        mutation=(0.5, 1.0), recombination=0.7, init="sobol", workers=1)
    print(f"  DE done: loss={res1_de.fun:.4f}  iters={res1_de.nit}")
    res1_nm = minimize(fn1, x0=res1_de.x, method="Nelder-Mead",
                       options={"xatol":1e-6,"fatol":1e-5,"maxiter":10000})
    s1_p = np.clip(res1_nm.x,
                   [lo for lo,_ in S1_BOUNDS],
                   [hi for _,hi in S1_BOUNDS])
    print(f"  NM done: loss={res1_nm.fun:.4f}")

    print(f"\nStage 1 parameters:")
    for n, v in zip(S1_NAMES, s1_p):
        print(f"  {n:<26} {v:.5f}")
    ach_m1_s1 = float(s1_p[4])
    ach_inf_s1 = float(s1_p[3])
    print(f"  ach_m2 = ach_m1          {ach_m1_s1:.5f}  [prior: M1=M2]")
    print(f"  ach(0b000) = {ach_inf_s1:.3f} /h  (all closed)")
    print(f"  ach(0b001) = {ach_inf_s1+ach_m1_s1:.3f} /h  (M1 only)")
    print(f"  ach(0b011) = {ach_inf_s1+2*ach_m1_s1:.3f} /h  (M1+M2, 2x ach_m1)")

    r_s1_tr = rmse_rh_stats(*simulate_c6(s1_p, 0.0, gT_out, gRH_out, glux, seg_starts, seg_vent),
                              gT_in, gRH_in, s1_mask)
    print(f"  Stage 1 residuals (M3-closed rows only):")
    print(f"    train: T RMSE {r_s1_tr[0]:.2f} C  bias {r_s1_tr[3]:+.2f} C")

    # ── Stage 2 ───────────────────────────────────────────────────────────────
    # Lower bound: M3 must contribute at least as much as M1 (physical prior)
    lo_m3 = max(0.05, ach_m1_s1)
    s2_bounds = [(lo_m3, 60.0)]
    print(f"\nStage 2: ach_m3 from {s2_mask.sum()} M3-open rows ...")
    print(f"  Lower bound: ach_m3 >= {lo_m3:.3f} /h (= ach_m1, M3 area >= M1 area)")
    print(f"  Primary signal: {int((train_mask & (vent_mask==7)).sum())} rows of 0b111 (M1+M2+M3)")

    fn2 = lambda p: loss_s2(p, s1_p, lo_m3, gT_out, gRH_out, glux,
                              gT_in, gRH_in, s2_mask, seg_starts, seg_vent)
    # Grid scan to visualise the loss landscape
    xs  = np.linspace(lo_m3, 15.0, 300)
    ys  = np.array([fn2([x]) for x in xs])
    best_x = xs[np.argmin(ys)]
    print(f"  Grid scan best: ach_m3={best_x:.3f} /h  loss={ys.min():.4f}")

    res2_de = differential_evolution(
        fn2, bounds=s2_bounds,
        maxiter=(40 if args.fast else 150),
        popsize=30, seed=42, polish=True, tol=1e-6, init="sobol", workers=1)
    ach_m3 = float(np.clip(res2_de.x[0], lo_m3, 60.0))
    print(f"  DE done: ach_m3={ach_m3:.5f} /h  loss={res2_de.fun:.4f}")

    ratio = ach_m3 / ach_m1_s1 if ach_m1_s1 > 0 else float("inf")
    physical_ratio = 171.0 / 21.0
    print(f"\n  ach_m3 / ach_m1 = {ratio:.1f}x  (physical area ratio M3/M1 = {physical_ratio:.1f}x)")
    print(f"  M1 contribution in M1+M3 state: {100*ach_m1_s1/(ach_m1_s1+ach_m3):.1f}%  "
          f"-> 'M3+M1 approx= M3' {'SUPPORTED' if ratio > 3 else 'WEAK'}")

    # ── Full model evaluation ─────────────────────────────────────────────────
    T_con, RH_con = simulate_c6(s1_p, ach_m3, gT_out, gRH_out, glux, seg_starts, seg_vent)

    print(f"\nFinal constrained model — ACH by bitmask:")
    k_solar, c_eff, transp, ach_inf, ach_m1 = s1_p
    ach_m2 = ach_m1
    for vm in range(8):
        m1, m2, m3 = (vm>>0)&1, (vm>>1)&1, (vm>>2)&1
        ach = ach_inf + ach_m1*(m1+m2) + ach_m3*m3
        n   = int((train_mask & (vent_mask==vm)).sum())
        print(f"  0b{vm:03b}  M1={m1} M2={m2} M3={m3}  ACH={ach:.3f} /h  ({n} train rows)")

    # Load baselines
    with open(OLD_PLANT) as f:
        op = json.load(f)
    any_open = (vent_mask > 0)
    bin_starts, bin_is_open = build_seg_binary(any_open)
    T_spr, RH_spr = sim_binary(
        np.array([op["solar_peak_w"]/lux_max, op["c_eff_mj_per_c"],
                  op["transpiration_kg_s"], 0.5, 0.5+op["ach_roof"]+op["ach_wall"]]),
        gT_out, gRH_out, glux, bin_starts, bin_is_open)
    with open(BINARY_PLANT) as f:
        bp = json.load(f)
    T_bin, RH_bin = sim_binary(
        np.array([bp["k_solar_w_per_lux"], bp["c_eff_mj_per_c"],
                  bp["transpiration_kg_s"], bp["ach_closed_per_hr"], bp["ach_open_per_hr"]]),
        gT_out, gRH_out, glux, bin_starts, bin_is_open)

    print(f"\n{'':24} {'train (Jun 4-18)':>26} {'val (Jun 19-25)':>26}")
    print("=" * 80)
    def row(label, T_sim, RH_sim):
        def fmt(r):
            return f"T={r[0]:.2f}C RH={r[1]:.1f}% +-1C={r[4]:.0f}%"
        tr = rmse_rh_stats(T_sim, RH_sim, gT_in, gRH_in, train_mask)
        va = rmse_rh_stats(T_sim, RH_sim, gT_in, gRH_in, val_mask)
        print(f"  {label:<22}  {fmt(tr):>28}  {fmt(va):>28}")
    row("spring-2026",       T_spr, RH_spr)
    row("binary summer-2026", T_bin, RH_bin)
    row("constrained 6-param", T_con, RH_con)
    print("=" * 80)

    val_r = rmse_rh_stats(T_con, RH_con, gT_in, gRH_in, val_mask)
    ac9  = val_r[2] <= 1.0 and val_r[4] >= 95.0
    ac10 = val_r[1] <= 5.0
    print(f"\n  AC-9  (val 95th-pct |err| <= 1.0 C, >=95% within +-1C): "
          f"95th={val_r[2]:.2f} C  within+-1C={val_r[4]:.1f}%  -> "
          f"{'PASS' if ac9 else 'FAIL'}")
    print(f"  AC-10 (val RH RMSE <= 5%): "
          f"RH RMSE={val_r[1]:.2f}%  -> {'PASS' if ac10 else 'FAIL'}")

    # ── Write JSON ────────────────────────────────────────────────────────────
    plant = {
        "_comment": (
            f"Constrained 6-param calibration by calibrate_plant_constrained.py "
            f"(summer-2026, Jun 4-18 training). "
            f"Priors: ach_m2=ach_m1 (M1=M2 identical geometry); "
            f"ach_m3 >= ach_m1 (M3 dominates, 171-step panel). "
            f"Stage 1 (M3-closed, {s1_mask.sum()} rows): "
            f"k_solar={k_solar:.4f} W/lux, c_eff={c_eff:.2f} MJ/degC, "
            f"ach_inf={ach_inf:.3f} /h, ach_m1={ach_m1:.3f} /h. "
            f"Stage 2 (M3-open, {s2_mask.sum()} rows, primary=0b111): "
            f"ach_m3={ach_m3:.3f} /h ({ratio:.1f}x ach_m1; area ratio {physical_ratio:.1f}x). "
            f"Validation T RMSE={val_r[0]:.2f} C, 95th-pct={val_r[2]:.2f} C."
        ),
        "volume_m3":          V,
        "k_solar_w_per_lux":  round(k_solar, 5),
        "c_eff_mj_per_c":     round(c_eff, 3),
        "transpiration_kg_s": round(transp, 6),
        "ach_inf":            round(ach_inf, 4),
        "ach_m1":             round(ach_m1, 4),
        "ach_m2":             round(ach_m2, 4),
        "ach_m3":             round(ach_m3, 4),
        "_priors": {
            "ach_m2_equals_ach_m1": True,
            "ach_m3_lower_bound":   round(lo_m3, 4),
            "m3_area_ratio_physical": round(physical_ratio, 1),
            "m3_m1_ratio_fitted":   round(ratio, 2),
            "m1_fraction_in_m1m3":  round(100*ach_m1/(ach_m1+ach_m3), 1),
        },
        "solar_peak_w": round(k_solar * lux_max, 0),
    }
    with open(OUT_PLANT, "w") as f:
        json.dump(plant, f, indent=2)
    print(f"\nWrote {OUT_PLANT.relative_to(Path(__file__).parent)}")

    # ── Plot ──────────────────────────────────────────────────────────────────
    if not args.plot:
        print("(pass --plot to generate calibration_constrained_summer2026.png)")
        return

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates

        invalid_grid = ~valid_grid & data_mask
        dts = [datetime.utcfromtimestamp(t) for t in grid]
        val_start_dt = datetime.utcfromtimestamp(VAL_SPLIT_UTC)

        fig, axes = plt.subplots(4, 1, figsize=(17, 14), sharex=True)
        fig.suptitle(
            "Constrained 6-param calibration  |  Priors: ach_m2=ach_m1 (M1=M2);  M3 dominates when open\n"
            f"ach_inf={ach_inf:.3f}  ach_m1=ach_m2={ach_m1:.3f}  "
            f"ach_m3={ach_m3:.3f} /h  ({ratio:.1f}x ach_m1; area ratio {physical_ratio:.1f}x)\n"
            f"Training: Jun 4-18  |  Validation: Jun 19-25  "
            f"(AC-9 target: +-1 degC >=95%)",
            fontsize=9, fontweight="bold"
        )

        m3_on = m3_open

        def shade(ax):
            ax.fill_between(dts, 0, 1, where=invalid_grid,
                            transform=ax.get_xaxis_transform(),
                            color="#e8a0a0", alpha=0.25, label="_nolegend_")
            ax.fill_between(dts, 0, 1, where=m3_on,
                            transform=ax.get_xaxis_transform(),
                            color="#e67e22", alpha=0.12, label="_nolegend_")
            ax.axvline(val_start_dt, color="purple", lw=1.2, ls="--", alpha=0.5)

        # Panel 0 — temperature
        ax = axes[0]
        shade(ax)
        ax.plot(dts, gT_out, color="0.65", lw=0.6, label="T_out")
        ax.plot(dts, gT_in,  "b-", lw=0.9, alpha=0.75, label="T_in measured")
        r_spr = rmse_rh_stats(T_spr, RH_spr, gT_in, gRH_in, val_mask)
        r_bin = rmse_rh_stats(T_bin, RH_bin, gT_in, gRH_in, val_mask)
        ax.plot(dts, T_spr, color="salmon", lw=0.8, ls="--",
                label=f"spring-2026 (val RMSE {r_spr[0]:.2f} C)")
        ax.plot(dts, T_bin, color="orange", lw=0.9, ls="-.",
                label=f"binary summer (val RMSE {r_bin[0]:.2f} C)")
        ax.plot(dts, T_con, "g-", lw=1.1,
                label=f"constrained 6p (val RMSE {val_r[0]:.2f} C)")
        ax.set_ylabel("Temperature [C]")
        ax.legend(fontsize=7, loc="upper right", ncol=2)
        ax.grid(alpha=0.2)

        # Panel 1 — residuals
        ax = axes[1]
        shade(ax)
        ax.axhline(0, color="k", lw=0.7)
        ax.axhline( 1, color="g", lw=0.5, ls=":")
        ax.axhline(-1, color="g", lw=0.5, ls=":")
        ax.plot(dts, T_spr-gT_in, color="salmon", lw=0.6, alpha=0.6, label="spring err")
        ax.plot(dts, T_bin-gT_in, color="orange", lw=0.6, alpha=0.7, label="binary err")
        ax.plot(dts, T_con-gT_in, "g-", lw=0.7, alpha=0.85, label="constrained err")
        ax.set_ylabel("T error [C]\n(sim - meas)")
        ax.set_ylim(-15, 15)
        ax.legend(fontsize=7, loc="upper right")
        ax.grid(alpha=0.2)

        # Panel 2 — RH
        ax = axes[2]
        shade(ax)
        ax.plot(dts, gRH_in, "b-", lw=0.9, alpha=0.75, label="RH measured")
        ax.plot(dts, RH_spr, color="salmon", lw=0.8, ls="--", label="spring-2026")
        ax.plot(dts, RH_bin, color="orange", lw=0.9, ls="-.", label="binary summer")
        ax.plot(dts, RH_con, "g-", lw=1.1, label="constrained 6p")
        ax.set_ylabel("RH [%]")
        ax.set_ylim(0, 105)
        ax.legend(fontsize=7, loc="upper right", ncol=2)
        ax.grid(alpha=0.2)

        # Panel 3 — channel states + lux
        ax  = axes[3]
        ax2 = ax.twinx()
        shade(ax)
        colors_ch = {"M1": "#2980b9", "M2": "#27ae60", "M3": "#e67e22"}
        for i, ch in enumerate(["M1", "M2", "M3"]):
            ch_open = channel_open(gbm_raw, i).astype(float)
            ax.step(dts, ch_open*(1-0.06*i), where="post",
                    color=colors_ch[ch], lw=1.2, label=ch, alpha=0.85)
        ax.set_ylabel("Channel open (0/1)")
        ax.set_ylim(-0.1, 1.3)
        ax2.fill_between(dts, 0, glux/1000, alpha=0.18, color="#e6a817", step="post")
        ax2.set_ylabel("Outdoor lux [k]", color="#e6a817")
        ax2.tick_params(axis="y", labelcolor="#e6a817")
        ax.legend(fontsize=7, loc="upper left", ncol=3)
        ax.set_xlabel("Date (UTC)  |  orange shading = M3-open states (Stage 2 signal)")
        ax.grid(alpha=0.2, axis="x")

        import matplotlib.patches as mpatches
        leg_extra = [
            mpatches.Patch(color="#e67e22", alpha=0.35, label="M3-open (Stage 2 data)"),
            mpatches.Patch(color="#e8a0a0", alpha=0.4,  label="door open / stale outdoor"),
        ]
        fig.legend(handles=leg_extra, loc="lower right", fontsize=7,
                   bbox_to_anchor=(0.99, 0.01), framealpha=0.8)

        loc = mdates.AutoDateLocator()
        axes[-1].xaxis.set_major_locator(loc)
        axes[-1].xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc))
        fig.autofmt_xdate(rotation=0, ha="center")
        plt.tight_layout(rect=[0, 0, 1, 0.93])
        fig.savefig(OUT_PNG, dpi=130, bbox_inches="tight")
        plt.close(fig)
        print(f"Saved {OUT_PNG.relative_to(Path(__file__).parent)}")

    except ImportError:
        print("matplotlib not available -- skipping plot")


if __name__ == "__main__":
    main()

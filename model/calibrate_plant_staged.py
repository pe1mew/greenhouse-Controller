"""
calibrate_plant_staged.py

Two-stage calibration that breaks the M2/M3 degeneracy in the joint fit:

Stage 1 — fit on all-closed (0b000) + M1-only (0b001) rows
    Parameters: k_solar, c_eff_mj_per_c, transpiration, ach_inf, ach_m1
    Loss mask:  only 0b000 and 0b001 grid points
    The integrator runs over the FULL time series (ach_m2=ach_m3=0 assumed during
    M2/M3-open periods so those T_sim values are wrong, but those points are
    excluded from the loss and do not receive gradient signal).
    M2 and M3 are set to ach_m2=ach_m3=0 during Stage 1.

Stage 2 — fit on M1+M3 (0b101) residuals, Stage 1 params locked
    Parameters: ach_m3 only  (ach_m2 fixed = ach_m1, see below)
    Loss mask:  0b101 rows only
    M2 contribution: set ach_m2 = ach_m1 (same physical size as M1, 21 steps).
    This gives ach_m3 the full residual signal from the 362 rapid-cool-down rows
    where M3 is in its dwell while M1 is still open.

Why ach_m2 = ach_m1:
    M1 and M2 are both 21-step roof windows with identical travel and geometry.
    The assumption ach_m2 = ach_m1 is physically well-motivated. Only 15 rows
    of M2-only data exist, so a free ach_m2 parameter is not identifiable; locking
    it to ach_m1 is the simplest defensible choice.

Why the 0b101 rows are cleaner for M3 identification than 0b111:
    The 0b101 state (362 training rows) arises exclusively when temperature drops
    rapidly — the controller closes M2 (dwell_open_m2=300 s expires) but M3 is
    held open by its 1500 s dwell. With Stage 1 params fixed, ach(0b101) depends
    only on ach_m3. The temperature response is driven by both the cool-down event
    AND M3's ventilation; the stage-1 model provides the cool-down prediction
    and ach_m3 explains the residual.

Usage:
    python model/calibrate_plant_staged.py [--plot] [--fast]
"""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from scipy.optimize import differential_evolution, minimize
from scipy.signal import lfilter

# reuse loaders and helpers from existing calibrators
import sys
sys.path.insert(0, str(Path(__file__).parent))
from calibrate_plant_dynamic import (
    load_calibration_input, resample_to_grid, ffill_bool,
    vent_mask_from_bitmask, build_segments, seg_ach_from_params,
    simulate, piecewise_lag, ah_from_rh, rh_from_ah,
    rmse_rh_stats, channel_open,
    CAL_IN, BINARY_PLANT, OLD_PLANT,
    VAL_SPLIT_UTC, V, RHO_AIR, CP_AIR, DT_S, C_EFF_AIR_FLOOR,
)

OUT_PLANT = Path(__file__).parent / "campaign-summer-2026" / "plant_calibrated_staged_summer2026.json"
OUT_PNG   = Path(__file__).parent / "campaign-summer-2026" / "calibration_staged_summer2026.png"

# ── Stage 1 bounds: k_solar, c_eff, transp, ach_inf, ach_m1 ─────────────────
S1_BOUNDS = [
    (1e-4, 2.0),              # k_solar
    (C_EFF_AIR_FLOOR, 200.0), # c_eff  (enforced in loss)
    (0.0,  0.10),             # transpiration
    (0.02, 10.0),             # ach_inf
    (0.0,  30.0),             # ach_m1
]
S1_NAMES = ("k_solar", "c_eff_mj_per_c", "transpiration_kg_s", "ach_inf", "ach_m1")

# ── Stage 2 bounds: ach_m3 only ──────────────────────────────────────────────
S2_BOUNDS = [(0.01, 60.0)]   # ach_m3  (positive — M3 must contribute something)
S2_NAMES  = ("ach_m3",)


def enforce_bounds(params, bounds):
    """Return 1e9 if any param violates its bound (hard enforcement for NM)."""
    for p, (lo, hi) in zip(params, bounds):
        if p < lo or p > hi:
            return True
    return False


def simulate_s1(s1_params, T_out, RH_out, lux, seg_starts, seg_vent):
    """Simulate with ach_m2=ach_m3=0 (Stage 1 assumption: only M1 matters)."""
    k_solar, c_eff_mj, transp, ach_inf, ach_m1 = s1_params
    # Treat M2 and M3 as zero for purposes of Stage 1
    full_params = np.array([k_solar, c_eff_mj, transp, ach_inf, ach_m1, 0.0, 0.0])
    seg_ach = seg_ach_from_params(full_params, seg_vent)
    return simulate(full_params, T_out, RH_out, lux, seg_starts, seg_ach)


def loss_s1(s1_params, T_out, RH_out, lux, T_ref, RH_ref, mask,
            seg_starts, seg_vent, w_rh=0.5):
    if enforce_bounds(s1_params, S1_BOUNDS):
        return 1e9
    if s1_params[3] < 0.01:
        return 1e9
    T_sim, RH_sim = simulate_s1(s1_params, T_out, RH_out, lux, seg_starts, seg_vent)
    if not np.all(np.isfinite(T_sim)):
        return 1e9
    if not mask.any():
        return 1e9
    return (float(np.mean((T_sim[mask] - T_ref[mask]) ** 2))
            + w_rh * float(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2)))


def simulate_s2(s1_params, ach_m3, ach_m2_eq, T_out, RH_out, lux,
                seg_starts, seg_vent):
    """Simulate with Stage 1 params + fixed ach_m2=ach_m2_eq + free ach_m3."""
    k_solar, c_eff_mj, transp, ach_inf, ach_m1 = s1_params
    full_params = np.array([k_solar, c_eff_mj, transp, ach_inf, ach_m1,
                             ach_m2_eq, ach_m3])
    seg_ach = seg_ach_from_params(full_params, seg_vent)
    return simulate(full_params, T_out, RH_out, lux, seg_starts, seg_ach)


def loss_s2(ach_m3_arr, s1_params, ach_m2_eq, T_out, RH_out, lux,
            T_ref, RH_ref, mask, seg_starts, seg_vent, w_rh=0.5):
    ach_m3 = float(ach_m3_arr[0])
    if ach_m3 < S2_BOUNDS[0][0]:
        return 1e9
    T_sim, RH_sim = simulate_s2(s1_params, ach_m3, ach_m2_eq,
                                 T_out, RH_out, lux, seg_starts, seg_vent)
    if not np.all(np.isfinite(T_sim)):
        return 1e9
    if not mask.any():
        return 1e9
    return (float(np.mean((T_sim[mask] - T_ref[mask]) ** 2))
            + w_rh * float(np.mean((RH_sim[mask] - RH_ref[mask]) ** 2)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--fast", action="store_true",
                    help="Reduced DE budget")
    args = ap.parse_args()

    # ── load + grid ───────────────────────────────────────────────────────────
    print(f"Loading {CAL_IN.name} ...")
    ts, T_in, RH_in, T_out, RH_out, lux, bitmask_raw, valid = \
        load_calibration_input(CAL_IN)

    print("Resampling to 30 s grid ...")
    (grid, gT_in, gRH_in, gT_out, gRH_out, glux, gbm_f), data_mask = \
        resample_to_grid(ts, T_in, RH_in, T_out, RH_out, lux,
                         bitmask_raw.astype(float))
    gbm_raw  = gbm_f.astype(np.int32)
    vent_mask = vent_mask_from_bitmask(gbm_raw)
    seg_starts, seg_vent = build_segments(vent_mask)

    valid_grid = ffill_bool(ts, valid, grid)
    full_mask  = valid_grid & data_mask
    train_mask = full_mask & (grid < VAL_SPLIT_UTC)
    val_mask   = full_mask & (grid >= VAL_SPLIT_UTC)

    lux_max = float(glux.max())

    # ── Stage 1 masks ─────────────────────────────────────────────────────────
    # Loss is restricted to 0b000 and 0b001 rows during Stage 1
    s1_mask = train_mask & (vent_mask <= 1)   # only all-closed and M1-only
    n_s1 = s1_mask.sum()
    print(f"  Stage 1 fit mask: {n_s1} rows "
          f"({int(((vent_mask==0)&train_mask).sum())} all-closed + "
          f"{int(((vent_mask==1)&train_mask).sum())} M1-only)")

    # Stage 2 mask: 0b101 rows in training set
    s2_mask = train_mask & (vent_mask == 0b101)
    n_s2 = s2_mask.sum()
    print(f"  Stage 2 fit mask: {n_s2} rows (M1+M3 only, rapid cool-down events)")

    # ── Stage 1 fit ───────────────────────────────────────────────────────────
    print("\nStage 1: fitting k_solar, c_eff, transp, ach_inf, ach_m1 "
          "on 0b000+0b001 rows ...")

    fn1 = lambda p: loss_s1(p, gT_out, gRH_out, glux, gT_in, gRH_in,
                              s1_mask, seg_starts, seg_vent)
    popsize1 = 8 if args.fast else 20
    maxiter1 = 80 if args.fast else 300

    res1_de = differential_evolution(
        fn1, bounds=S1_BOUNDS,
        maxiter=maxiter1, popsize=popsize1, seed=42,
        polish=False, tol=1e-4,
        mutation=(0.5, 1.0), recombination=0.7, init="sobol", workers=1)
    print(f"  DE done: loss={res1_de.fun:.4f}  iters={res1_de.nit}")

    res1_nm = minimize(fn1, x0=res1_de.x, method="Nelder-Mead",
                       options={"xatol": 1e-6, "fatol": 1e-5, "maxiter": 10000})
    # Enforce bounds violated by NM
    s1_p = np.clip(res1_nm.x,
                   [lo for lo, _ in S1_BOUNDS],
                   [hi for _, hi in S1_BOUNDS])
    print(f"  NM done: loss={res1_nm.fun:.4f}  (params clipped to bounds)")

    print(f"\nStage 1 parameters:")
    for n, v in zip(S1_NAMES, s1_p):
        print(f"  {n:<25} {v:.5f}")
    print(f"  ach(0b000) = {s1_p[3]:.3f} /h  (all closed)")
    print(f"  ach(0b001) = {s1_p[3]+s1_p[4]:.3f} /h  (M1 only)")

    # Residuals on Stage 1 mask only
    T_s1, RH_s1 = simulate_s1(s1_p, gT_out, gRH_out, glux, seg_starts, seg_vent)
    r_s1_tr = rmse_rh_stats(T_s1, RH_s1, gT_in, gRH_in, s1_mask)
    r_s1_va = rmse_rh_stats(T_s1, RH_s1, gT_in, gRH_in, val_mask & (vent_mask <= 1))
    print(f"  Stage 1 residuals (0b000+0b001 only):")
    print(f"    train: T RMSE {r_s1_tr[0]:.2f} C  bias {r_s1_tr[3]:+.2f} C")
    if (val_mask & (vent_mask <= 1)).any():
        print(f"    val  : T RMSE {r_s1_va[0]:.2f} C  bias {r_s1_va[3]:+.2f} C")

    # ── Stage 2: fit ach_m3 ───────────────────────────────────────────────────
    # ach_m2 = ach_m1 (same physical size window)
    ach_m2_eq = float(s1_p[4])
    print(f"\nStage 2: fitting ach_m3 on {n_s2} M1+M3 rows "
          f"(ach_m2 = ach_m1 = {ach_m2_eq:.4f} /h) ...")

    if n_s2 == 0:
        print("  No 0b101 rows in training set — using physical estimate.")
        ach_m3 = ach_m2_eq * (171.0 / 21.0)   # area ratio M3/M1
    else:
        fn2 = lambda p: loss_s2(p, s1_p, ach_m2_eq,
                                  gT_out, gRH_out, glux, gT_in, gRH_in,
                                  s2_mask, seg_starts, seg_vent)
        # 1-D: simple grid search + Brent to warm-start DE
        xs = np.linspace(0.01, 15.0, 200)
        ys = np.array([fn2([x]) for x in xs])
        best_x = xs[np.argmin(ys)]
        print(f"  Grid search best: ach_m3={best_x:.3f}  loss={np.min(ys):.4f}")

        res2_de = differential_evolution(
            fn2, bounds=S2_BOUNDS,
            maxiter=(30 if args.fast else 100), popsize=20, seed=42,
            polish=True, tol=1e-5, init="sobol", workers=1)
        ach_m3 = float(np.clip(res2_de.x[0], *S2_BOUNDS[0]))
        print(f"  DE + polish done: ach_m3={ach_m3:.5f}  loss={res2_de.fun:.4f}")

    print(f"\n  ach_m3 = {ach_m3:.4f} /h  (M3 incremental, 171-step panel)")
    k_m3_over_m1 = ach_m3 / s1_p[4] if s1_p[4] > 0 else float("inf")
    print(f"  ach_m3 / ach_m1 = {k_m3_over_m1:.1f}×  "
          f"(physical area ratio M3/M1 = {171/21:.1f}×)")

    # ── Evaluate staged model on full grid ────────────────────────────────────
    T_stg, RH_stg = simulate_s2(s1_p, ach_m3, ach_m2_eq,
                                  gT_out, gRH_out, glux, seg_starts, seg_vent)

    print(f"\nFinal staged model — implied ACH by vent-mask:")
    k_solar, c_eff, transp, ach_inf, ach_m1 = s1_p
    for vm in range(8):
        m1, m2, m3 = (vm>>0)&1, (vm>>1)&1, (vm>>2)&1
        ach = ach_inf + ach_m1*m1 + ach_m2_eq*m2 + ach_m3*m3
        print(f"  0b{vm:03b}  M1={m1} M2={m2} M3={m3}  ACH={ach:.3f} /h")

    print(f"\n{'':25} {'train (Jun 4-18)':>22} {'val (Jun 19-25)':>22}")
    print("=" * 75)

    from calibrate_plant_campaign import simulate as sim_binary, build_segments as bld_bin
    any_open = (vent_mask > 0)
    bin_starts, bin_is_open = bld_bin(any_open)

    def load_binary_model():
        with open(BINARY_PLANT) as f:
            bp = json.load(f)
        return np.array([bp["k_solar_w_per_lux"], bp["c_eff_mj_per_c"],
                         bp["transpiration_kg_s"],
                         bp["ach_closed_per_hr"], bp["ach_open_per_hr"]])

    models = [("spring-2026",    lambda: sim_binary(
        np.array([OLD_PLANT and json.load(open(OLD_PLANT))["solar_peak_w"]/lux_max,
                  2.89, 0.00477, 0.5, 0.5+0.19+0.96]),
        gT_out, gRH_out, glux, bin_starts, bin_is_open)),
    ]
    # Load spring params properly
    with open(OLD_PLANT) as f:
        op = json.load(f)
    T_spr, RH_spr = sim_binary(
        np.array([op["solar_peak_w"]/lux_max, op["c_eff_mj_per_c"],
                  op["transpiration_kg_s"], 0.5, 0.5+op["ach_roof"]+op["ach_wall"]]),
        gT_out, gRH_out, glux, bin_starts, bin_is_open)
    T_bin, RH_bin = sim_binary(load_binary_model(),
                                gT_out, gRH_out, glux, bin_starts, bin_is_open)

    def row(label, T_sim, RH_sim):
        def fmt(r):
            return f"T={r[0]:.2f}C RH={r[1]:.1f}% +-1C={r[4]:.0f}%"
        tr = rmse_rh_stats(T_sim, RH_sim, gT_in, gRH_in, train_mask)
        va = rmse_rh_stats(T_sim, RH_sim, gT_in, gRH_in, val_mask)
        print(f"  {label:<23}  {fmt(tr):>24}  {fmt(va):>24}")

    row("spring-2026",      T_spr,  RH_spr)
    row("binary summer-26", T_bin,  RH_bin)
    row("staged dynamic",   T_stg,  RH_stg)
    print("=" * 75)

    val_r = rmse_rh_stats(T_stg, RH_stg, gT_in, gRH_in, val_mask)
    ac9  = val_r[2] <= 1.0 and val_r[4] >= 95.0
    ac10 = val_r[1] <= 5.0
    print(f"\n  AC-9  (val 95th-pct |err| <= 1.0 C, >=95% within+-1C): "
          f"95th={val_r[2]:.2f} C  within+-1C={val_r[4]:.1f}%  -> "
          f"{'PASS' if ac9 else 'FAIL'}")
    print(f"  AC-10 (val RH RMSE <= 5%): "
          f"RH RMSE={val_r[1]:.2f}%  -> {'PASS' if ac10 else 'FAIL'}")

    # ── write JSON ────────────────────────────────────────────────────────────
    plant = {
        "_comment": (
            f"Two-stage calibration by calibrate_plant_staged.py "
            f"(summer-2026, Option A, Jun 4-18 training). "
            f"Stage 1 fit on 0b000+0b001 rows: "
            f"k_solar={k_solar:.4f} W/lux, c_eff={c_eff:.2f} MJ/degC, "
            f"ach_inf={ach_inf:.3f} /h, ach_m1={ach_m1:.3f} /h. "
            f"Stage 2 fit on 0b101 rows: ach_m3={ach_m3:.3f} /h "
            f"({k_m3_over_m1:.1f}x M1; physical area ratio {171/21:.1f}x). "
            f"ach_m2 = ach_m1 (identical window geometry). "
            f"Validation T RMSE={val_r[0]:.2f} C, 95th-pct={val_r[2]:.2f} C."
        ),
        "volume_m3":    V,
        "k_solar_w_per_lux": round(k_solar, 5),
        "c_eff_mj_per_c":    round(c_eff, 3),
        "transpiration_kg_s": round(transp, 6),
        "ach_inf":       round(ach_inf, 4),
        "ach_m1":        round(ach_m1, 4),
        "ach_m2":        round(ach_m2_eq, 4),
        "ach_m3":        round(ach_m3, 4),
        "ach_roof":      round(ach_m1, 4),
        "ach_wall":      round(ach_m3, 4),
        "solar_peak_w":  round(k_solar * lux_max, 0),
    }
    with open(OUT_PLANT, "w") as f:
        json.dump(plant, f, indent=2)
    print(f"\nWrote {OUT_PLANT.relative_to(Path(__file__).parent)}")

    # ── plot ──────────────────────────────────────────────────────────────────
    if not args.plot:
        print("(pass --plot to generate calibration_staged_summer2026.png)")
        return

    try:
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates

        invalid_grid = ~valid_grid & data_mask
        dts = [datetime.utcfromtimestamp(t) for t in grid]
        val_start_dt = datetime.utcfromtimestamp(VAL_SPLIT_UTC)
        m1_01 = (vent_mask == 0b001).astype(float)
        m1m3_mask = (vent_mask == 0b101)

        fig, axes = plt.subplots(4, 1, figsize=(17, 14), sharex=True)
        fig.suptitle(
            "Two-stage calibration — M1 fit from 0b000+0b001 data, "
            "M3 fit from 0b101 rapid-cool-down data\n"
            f"ach_m1={s1_p[4]:.3f} /h, ach_m2=ach_m1, "
            f"ach_m3={ach_m3:.3f} /h ({k_m3_over_m1:.1f}×M1  |  "
            f"area ratio={171/21:.1f}×)\n"
            f"Training: Jun 4-18  |  Validation: Jun 19-25  "
            f"(AC-9 target: +-1 degC >=95%)",
            fontsize=10, fontweight="bold"
        )

        def shade(ax):
            ax.fill_between(dts, 0, 1, where=invalid_grid,
                            transform=ax.get_xaxis_transform(),
                            color="#e8a0a0", alpha=0.25)
            # Shade M1+M3 periods (Stage 2 data)
            ax.fill_between(dts, 0, 1, where=m1m3_mask,
                            transform=ax.get_xaxis_transform(),
                            color="#9b59b6", alpha=0.20)
            ax.axvline(val_start_dt, color="purple", lw=1.2, ls="--", alpha=0.5)

        ax = axes[0]
        shade(ax)
        ax.plot(dts, gT_out, color="0.65", lw=0.6, label="T_out")
        ax.plot(dts, gT_in,  "b-",  lw=0.9, alpha=0.75, label="T_in measured")
        ax.plot(dts, T_spr,  color="salmon", lw=0.8, ls="--",
                label=f"spring-2026 (val RMSE {rmse_rh_stats(T_spr,RH_spr,gT_in,gRH_in,val_mask)[0]:.2f} C)")
        ax.plot(dts, T_bin,  color="orange", lw=0.9, ls="-.",
                label=f"binary summer (val RMSE {rmse_rh_stats(T_bin,RH_bin,gT_in,gRH_in,val_mask)[0]:.2f} C)")
        ax.plot(dts, T_stg,  "g-",  lw=1.1,
                label=f"staged dynamic (val RMSE {val_r[0]:.2f} C)")
        ax.set_ylabel("Temperature [C]")
        ax.legend(fontsize=7, loc="upper right", ncol=2)
        ax.grid(alpha=0.2)

        ax = axes[1]
        shade(ax)
        ax.axhline(0, color="k", lw=0.7)
        ax.axhline( 1, color="g", lw=0.5, ls=":")
        ax.axhline(-1, color="g", lw=0.5, ls=":")
        ax.plot(dts, T_spr - gT_in, color="salmon", lw=0.6, alpha=0.6, label="spring err")
        ax.plot(dts, T_bin - gT_in, color="orange", lw=0.6, alpha=0.7, label="binary err")
        ax.plot(dts, T_stg - gT_in, "g-", lw=0.7, alpha=0.85, label="staged err")
        ax.set_ylabel("T error [C]\n(sim − meas)")
        ax.set_ylim(-15, 15)
        ax.legend(fontsize=7, loc="upper right")
        ax.grid(alpha=0.2)

        ax = axes[2]
        shade(ax)
        ax.plot(dts, gRH_in, "b-", lw=0.9, alpha=0.75, label="RH measured")
        ax.plot(dts, RH_spr, color="salmon", lw=0.8, ls="--", label="spring-2026")
        ax.plot(dts, RH_bin, color="orange", lw=0.9, ls="-.", label="binary summer")
        ax.plot(dts, RH_stg, "g-", lw=1.1, label="staged dynamic")
        ax.set_ylabel("RH [%]")
        ax.set_ylim(0, 105)
        ax.legend(fontsize=7, loc="upper right", ncol=2)
        ax.grid(alpha=0.2)

        ax  = axes[3]
        ax2 = ax.twinx()
        shade(ax)
        colors_ch = {"M1": "#2980b9", "M2": "#27ae60", "M3": "#e67e22"}
        for i, ch in enumerate(["M1", "M2", "M3"]):
            ch_open = channel_open(gbm_raw, i).astype(float)
            ax.step(dts, ch_open * (1 - 0.06*i), where="post",
                    color=colors_ch[ch], lw=1.2, label=ch, alpha=0.85)
        ax.set_ylabel("Channel open (0/1)")
        ax.set_ylim(-0.1, 1.3)
        ax2.fill_between(dts, 0, glux/1000, alpha=0.18,
                         color="#e6a817", step="post")
        ax2.set_ylabel("Outdoor lux [k]", color="#e6a817")
        ax2.tick_params(axis="y", labelcolor="#e6a817")
        ax.legend(fontsize=7, loc="upper left", ncol=3)
        ax.set_xlabel("Date (UTC)  — purple shading = 0b101 M1+M3 rapid-cool-down events")
        ax.grid(alpha=0.2, axis="x")

        import matplotlib.patches as mpatches
        legend_extra = [
            mpatches.Patch(color="#9b59b6", alpha=0.4, label="M1+M3 (0b101) — Stage 2 data"),
            mpatches.Patch(color="#e8a0a0", alpha=0.4, label="door open / stale outdoor"),
        ]
        fig.legend(handles=legend_extra, loc="lower right", fontsize=7,
                   bbox_to_anchor=(0.99, 0.01), framealpha=0.8)

        loc = mdates.AutoDateLocator()
        axes[-1].xaxis.set_major_locator(loc)
        axes[-1].xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc))
        fig.autofmt_xdate(rotation=0, ha="center")
        plt.tight_layout(rect=[0, 0, 1, 0.94])
        fig.savefig(OUT_PNG, dpi=130, bbox_inches="tight")
        plt.close(fig)
        print(f"Saved {OUT_PNG.relative_to(Path(__file__).parent)}")
    except ImportError:
        print("matplotlib not available — skipping plot")


if __name__ == "__main__":
    main()

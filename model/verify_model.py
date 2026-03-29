"""
Climate Control Model — Verification Script
============================================
Drives the climate_model control algorithm with a steady-state greenhouse
plant model and real outside conditions (May–Sep 2025) to verify that the
model meets the performance and anti-resonance criteria defined in
model_design.md §9.

Data source
-----------
    Archive/Iteration1/Environment/airTemperature_2025-05-01_to_2025-09-01.csv
    (outside temperature and relative humidity, 30-min intervals)

Plant model
-----------
Steady-state algebraic model from Archive/Iteration1/Simulation (§2.3–2.4).
Indoor temperature and absolute humidity are computed as equilibrium values
at each time step given outside conditions and current window state.

Usage
-----
    python verify_model.py [scenario]

    Scenarios:
        V1   Full summer season (May–Sep 2025, 123 days)  [default]
        V2   Hot day  (Day 0 = May 1, 24 h)
        V3   High humidity (Day 30 = June 1, 24 h)
        V4   Cold night (Day 0 starting at 20:00, 12 h)
        V5   Summer seasonal override vs default (Day 60 = July 1, 24 h)
        ALL  Run all scenarios

Outputs per scenario
--------------------
    results_<scenario>.csv   — time-series (T_in, RH_in, T_out, M1/M2/M3)
    results_<scenario>.png   — four-panel plot (requires matplotlib)

Assessment
----------
Pass/fail against model_design.md §9 is printed to stdout for each scenario.
"""

from __future__ import annotations

import csv
import math
import sys
import argparse
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Locate repo root and import climate_model + outside_conditions
# ---------------------------------------------------------------------------

_MODEL_DIR = Path(__file__).resolve().parent          # .../model/
_REPO_ROOT  = _MODEL_DIR.parent                       # .../greenhouse-Controller/
_ENV_DIR    = _REPO_ROOT / "Archive" / "Iteration1" / "Environment"
_DATA_CSV   = _ENV_DIR / "airTemperature_2025-05-01_to_2025-09-01.csv"

sys.path.insert(0, str(_MODEL_DIR))
sys.path.insert(0, str(_REPO_ROOT / "Archive" / "Iteration1"))

from climate_model import (                           # noqa: E402
    ControlConfig, ControllerState, ScheduleSetpoints,
    apply_control, update_window_states,
    window_states_tuple, active_ach,
)
from Environment.outside_conditions import OutsideConditions  # noqa: E402


# ---------------------------------------------------------------------------
# Physical constants (identical to Archive/Iteration1/Simulation)
# ---------------------------------------------------------------------------

RHO_AIR  = 1.2        # kg/m³
CP_AIR   = 1005.0     # J/kg·°C
P_ATM    = 101_325.0  # Pa
ACH_INF  = 0.5        # h⁻¹ — background infiltration


# ---------------------------------------------------------------------------
# Plant parameters
# ---------------------------------------------------------------------------

@dataclass
class PlantParams:
    V:             float = 2400.0     # m³
    ACH_roof:      float = 8.0        # h⁻¹ per roof vent (M1 or M2)
    ACH_wall:      float = 40.0       # h⁻¹ side wall vent (M3)
    m_transp:      float = 0.010      # kg/s
    Q_solar_peak:  float = 20_000.0   # W


# ---------------------------------------------------------------------------
# Thermodynamic helpers
# ---------------------------------------------------------------------------

def _p_sat(T: float) -> float:
    """Saturation vapour pressure [Pa] — Magnus formula."""
    return 610.78 * math.exp(17.27 * T / (T + 237.3))


def _AH_sat(T: float) -> float:
    ps = _p_sat(T)
    return 0.622 * ps / (P_ATM - ps) * RHO_AIR


def RH_from_AH(AH: float, T: float) -> float:
    sat = _AH_sat(T)
    return min(100.0, AH / sat * 100.0) if sat > 0 else 0.0


def AH_from_RH(RH: float, T: float) -> float:
    return RH / 100.0 * _AH_sat(T)


def _Q_solar(t_s: float, Q_peak: float) -> float:
    """Clear-sky sine approximation; peak at 13:00, zero outside 06:00–20:00."""
    hour = (t_s / 3600.0) % 24.0
    if 6.0 <= hour <= 20.0:
        return Q_peak * math.sin(math.pi * (hour - 6.0) / 14.0) ** 2
    return 0.0


# ---------------------------------------------------------------------------
# Steady-state plant step
# ---------------------------------------------------------------------------

def plant_step(
    T_prev: float,
    AH_prev: float,
    T_out: float,
    RH_out: float,
    ach_total_per_s: float,
    plant: PlantParams,
    t_s: float,
) -> Tuple[float, float]:
    """
    Return new (T_in, AH_in) using the steady-state equilibrium model.

    T_in  = T_out + Q_solar / (ACH * V * rho * cp)
    AH_in = AH_out + m_transp / (ACH * V)

    ACH is always ≥ ACH_INF so denominator is never zero.
    """
    AH_out = AH_from_RH(RH_out, T_out)
    Qs     = _Q_solar(t_s, plant.Q_solar_peak)
    denom  = ach_total_per_s * plant.V

    T_in  = T_out  + Qs / (denom * RHO_AIR * CP_AIR)
    AH_in = AH_out + plant.m_transp / denom
    return T_in, max(0.0, AH_in)


def _ach_total_per_s(ctrl_state: ControllerState, plant: PlantParams) -> float:
    """Total ventilation rate [s⁻¹] including infiltration."""
    w = ctrl_state.windows
    total = ACH_INF
    if w[0].is_open(): total += plant.ACH_roof
    if w[1].is_open(): total += plant.ACH_roof
    if w[2].is_open(): total += plant.ACH_wall
    return total / 3600.0


# ---------------------------------------------------------------------------
# Simulation result container
# ---------------------------------------------------------------------------

@dataclass
class SimResult:
    t_hist:       List[float]              = field(default_factory=list)
    T_in_hist:    List[float]              = field(default_factory=list)
    RH_in_hist:   List[float]              = field(default_factory=list)
    T_out_hist:   List[float]              = field(default_factory=list)
    RH_out_hist:  List[float]              = field(default_factory=list)
    windows_hist: List[Tuple[bool,bool,bool]] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Core simulation runner
# ---------------------------------------------------------------------------

def run_simulation(
    label:       str,
    cfg:         ControlConfig,
    conditions:  OutsideConditions,
    T0:          float,
    RH0:         float,
    duration_s:  float,
    day_offset:  int   = 0,
    dt:          float = 10.0,
    record_every_s: float = 60.0,
) -> Tuple[SimResult, ControllerState]:
    """
    Simulate the closed-loop greenhouse for `duration_s` seconds.

    Parameters
    ----------
    label         : scenario label (for console output)
    cfg           : controller configuration
    conditions    : outside conditions dataset
    T0, RH0       : initial indoor temperature [°C] and humidity [%]
    duration_s    : simulation duration [s]
    day_offset    : start day in dataset (0 = May 1, 2025)
    dt            : integration step [s]
    record_every_s: data recording interval [s]
    """
    print(f"\n{'='*60}")
    print(f"  {label}")
    print(f"  Duration {duration_s/3600:.1f} h  |  dt {dt} s  |  Start day +{day_offset}")
    print(f"  T0={T0}°C  RH0={RH0}%")
    print(f"{'='*60}")

    ctrl  = ControllerState.initial()
    plant = PlantParams()
    result = SimResult()

    T_in  = T0
    AH_in = AH_from_RH(RH0, T0)

    n_steps      = int(duration_s / dt)
    record_every = max(1, int(record_every_s / dt))
    offset_s     = day_offset * 86400.0

    for step in range(n_steps):
        t = step * dt

        # Outside conditions from historical dataset
        T_out, RH_out = conditions.get_conditions_at_elapsed_time(t + offset_s)

        # Hour and month for schedule resolution
        abs_dt = conditions.start_time + timedelta(seconds=t + offset_s)
        hour   = abs_dt.hour + abs_dt.minute / 60.0
        month  = abs_dt.month

        # Advance window states (check if MOVING windows have reached target)
        update_window_states(ctrl, t)

        # Plant model — steady-state equilibrium
        ach = _ach_total_per_s(ctrl, plant)
        T_in, AH_in = plant_step(T_in, AH_in, T_out, RH_out, ach, plant, t)
        RH_in = RH_from_AH(AH_in, T_in)

        # Climate control decision
        apply_control(T_in, RH_in, t, hour, month, cfg, ctrl)

        # Record
        if step % record_every == 0:
            result.t_hist.append(t)
            result.T_in_hist.append(T_in)
            result.RH_in_hist.append(RH_in)
            result.T_out_hist.append(T_out)
            result.RH_out_hist.append(RH_out)
            result.windows_hist.append(window_states_tuple(ctrl))

    return result, ctrl


# ---------------------------------------------------------------------------
# Performance metrics  (model_design §9)
# ---------------------------------------------------------------------------

@dataclass
class Metrics:
    # Climate performance — split into controllable vs uncontrollable deviations
    # (model_design.md §9)
    # Temperature (§9.1.1)
    pct_controllable_T_overtemp:   float = 0.0  # T > T_max+3*dT_step AND T_out<=T_max+3*dT_step
    pct_deadband_T_overtemp:       float = 0.0  # T_max < T <= T_max+3*dT_step (graduated design)
    pct_uncontrollable_T_overtemp: float = 0.0  # T > T_max AND T_out > T_max   (reported only)
    pct_controllable_T_undertemp:  float = 0.0  # T < T_min AND T_out >= T_min  (control defect)
    pct_uncontrollable_T_undertemp:float = 0.0  # T < T_min AND T_out < T_min   (no heating)
    # Humidity (§9.1.2)
    pct_controllable_RH_above_max:   float = 0.0  # RH > RH_max+dRH_step AND T>=T_min AND achievable
    pct_deadband_RH_above_max:       float = 0.0  # RH_max < RH <= RH_max+dRH_step (by design)
    pct_unctrl_RH_Tmin_guard:        float = 0.0  # T < T_min                      (T_min guard)
    pct_unctrl_RH_outdoor_sat:       float = 0.0  # T>=T_min AND not achievable     (hw limit)
    pct_RH_below_min:                float = 0.0  # RH < RH_min                     (informational)

    # Anti-resonance
    total_actuations:    int   = 0    # total window state changes
    max_act_per_hour:    float = 0.0  # max actuations/h for any window
    max_act_per_10min:   int   = 0    # max actuations in any 10-min window
    commands_blocked:    int   = 0    # commands suppressed by dwell guard

    # Observed range
    T_min_obs:  float = 0.0
    T_max_obs:  float = 0.0
    RH_min_obs: float = 0.0
    RH_max_obs: float = 0.0

    def passed(self) -> bool:
        """
        Pass criteria (model_design.md §9 — primary criteria only):
          - Controllable overtemp  <= 5%  (ventilation could have reduced T)
          - Controllable undertemp <= 5%  (window closing could have retained heat)
          - RH above RH_max        <= 10% (ventilation could have reduced RH)
          - Max actuations/10 min  <= 1   (anti-resonance)
        Uncontrollable deviations are excluded from pass/fail.
        """
        return (
            self.pct_controllable_T_overtemp    <= 5.0  and
            self.pct_controllable_T_undertemp   <= 5.0  and
            self.pct_controllable_RH_above_max  <= 10.0 and
            self.max_act_per_10min              <= 1
        )


def compute_metrics(
    result: SimResult,
    ctrl_state: ControllerState,
    cfg: ControlConfig,
    conditions: OutsideConditions,
    day_offset: int,
) -> Metrics:
    """Compute all performance metrics from a simulation result."""
    m = Metrics()
    n = len(result.t_hist)
    if n == 0:
        return m

    # ---- Resolve active setpoints and solar gain per recorded sample ----
    T_min_hist, T_max_hist, RH_min_hist, RH_max_hist = [], [], [], []
    Q_solar_hist: List[float] = []
    for t in result.t_hist:
        abs_dt = conditions.start_time + timedelta(seconds=t + day_offset * 86400.0)
        from climate_model import resolve_setpoints
        sp = resolve_setpoints(
            abs_dt.hour + abs_dt.minute / 60.0,
            abs_dt.month,
            cfg,
        )
        T_min_hist.append(sp.T_min)
        T_max_hist.append(sp.T_max)
        RH_min_hist.append(sp.RH_min)
        RH_max_hist.append(sp.RH_max)
        Q_solar_hist.append(_Q_solar(t + day_offset * 86400.0, PlantParams().Q_solar_peak))

    # ---- Climate performance — split controllable vs uncontrollable ----
    ctrl_overtemp        = 0  # T > T_trigger AND T_in_min_achv <= T_trigger
    deadband_overtemp    = 0  # T_max < T <= T_trigger             (graduated design)
    unctrl_overtemp      = 0  # T > T_max AND (T_out>T_max OR T_in_min > T_trigger)
    ctrl_undertemp       = 0  # T < T_min AND T_out >= T_min      (windows should close)
    unctrl_undertemp     = 0  # T < T_min AND T_out <  T_min      (no heater)
    rh_ctrl              = 0  # RH > RH_max+dRH_step AND T>=T_min AND RH_min_achievable<RH_max
    rh_deadband          = 0  # RH_max < RH <= RH_max+dRH_step   (designed deadband)
    rh_unctrl_tmin       = 0  # RH > RH_max AND T < T_min        (T_min guard)
    rh_unctrl_outdoor    = 0  # RH > RH_max AND T>=T_min AND hw limit
    rh_below_min         = 0  # RH < RH_min

    # Precompute plant constants for achievability check
    _plant_c = PlantParams()
    _ach_max_s = (ACH_INF + _plant_c.ACH_roof * 2 + _plant_c.ACH_wall) / 3600.0

    for T, RH, T_out, RH_out, Q_s, lo, hi, rh_lo, rh_hi in zip(
        result.T_in_hist, result.RH_in_hist,
        result.T_out_hist, result.RH_out_hist,
        Q_solar_hist,
        T_min_hist, T_max_hist, RH_min_hist, RH_max_hist,
    ):
        T_trigger  = hi + 3.0 * cfg.dT_step  # demand=3 trigger (all windows should be open)
        RH_trigger = rh_hi + cfg.dRH_step     # first-action humidity threshold

        # Minimum achievable T_in with all 3 windows fully open
        T_in_min = T_out + Q_s / (_ach_max_s * _plant_c.V * RHO_AIR * CP_AIR)

        # ---- Temperature ----
        if T > T_trigger:
            if T_in_min > T_trigger:
                unctrl_overtemp += 1     # Even max vent can't reach T_trigger; hw/solar limit
            elif T_out > hi:
                unctrl_overtemp += 1     # T_out already above T_max; not a control defect
            else:
                ctrl_overtemp   += 1     # Max vent would help; controller under-ventilates
        elif T > hi:
            deadband_overtemp   += 1     # Graduated demand deadband [T_max, T_trigger]

        if T < lo:
            if T_out >= lo:
                ctrl_undertemp  += 1
            else:
                unctrl_undertemp+= 1

        # ---- Humidity ----
        if RH > rh_hi:
            if T < lo:
                rh_unctrl_tmin += 1      # T_min guard prevents ventilation
            elif RH <= RH_trigger:
                rh_deadband += 1         # Designed deadband [RH_max, RH_max+dRH_step]
            else:
                # Above trigger: check if max ventilation could achieve RH_max
                AH_out_val   = AH_from_RH(RH_out, T_out)
                AH_in_ss_min = AH_out_val + _plant_c.m_transp / (_ach_max_s * _plant_c.V)
                RH_in_ss_min = RH_from_AH(AH_in_ss_min, T_out)  # conserve: T_in → T_out
                if RH_in_ss_min < rh_hi:
                    rh_ctrl += 1         # Achievable; sustained exceedance is a defect
                else:
                    rh_unctrl_outdoor += 1  # Hardware limit; not a control defect

        if RH < rh_lo:
            rh_below_min += 1

    m.pct_controllable_T_overtemp    = ctrl_overtemp      / n * 100.0
    m.pct_deadband_T_overtemp        = deadband_overtemp  / n * 100.0
    m.pct_uncontrollable_T_overtemp  = unctrl_overtemp    / n * 100.0
    m.pct_controllable_T_undertemp   = ctrl_undertemp     / n * 100.0
    m.pct_uncontrollable_T_undertemp = unctrl_undertemp   / n * 100.0
    m.pct_controllable_RH_above_max  = rh_ctrl            / n * 100.0
    m.pct_deadband_RH_above_max      = rh_deadband        / n * 100.0
    m.pct_unctrl_RH_Tmin_guard       = rh_unctrl_tmin     / n * 100.0
    m.pct_unctrl_RH_outdoor_sat      = rh_unctrl_outdoor  / n * 100.0
    m.pct_RH_below_min               = rh_below_min       / n * 100.0

    # ---- Observed range ----
    m.T_min_obs  = min(result.T_in_hist)
    m.T_max_obs  = max(result.T_in_hist)
    m.RH_min_obs = min(result.RH_in_hist)
    m.RH_max_obs = max(result.RH_in_hist)

    # ---- Actuations per window ----
    n_windows = 3
    act_counts = [0] * n_windows
    prev = result.windows_hist[0]
    for ws in result.windows_hist[1:]:
        for i in range(n_windows):
            if ws[i] != prev[i]:
                act_counts[i] += 1
        prev = ws
    m.total_actuations = sum(act_counts)

    # ---- Max actuations per hour ----
    # Compute for each window independently using a rolling 1-h window
    record_dt = result.t_hist[1] - result.t_hist[0] if n > 1 else 60.0
    window_h  = int(3600.0 / record_dt)  # samples per hour

    for wi in range(n_windows):
        w_opens = [1 if result.windows_hist[i][wi] != result.windows_hist[i-1][wi]
                   else 0 for i in range(1, n)]
        for start in range(len(w_opens) - window_h + 1):
            count = sum(w_opens[start:start + window_h])
            m.max_act_per_hour = max(m.max_act_per_hour, count)

    # ---- Max actuations per 10-minute window ----
    window_10m = max(1, int(600.0 / record_dt))
    for wi in range(n_windows):
        w_opens = [1 if result.windows_hist[i][wi] != result.windows_hist[i-1][wi]
                   else 0 for i in range(1, n)]
        for start in range(len(w_opens) - window_10m + 1):
            count = sum(w_opens[start:start + window_10m])
            m.max_act_per_10min = max(m.max_act_per_10min, count)

    m.commands_blocked = ctrl_state.commands_blocked
    return m


def print_metrics(label: str, m: Metrics, ctrl_state: ControllerState) -> None:
    verdict = "PASS" if m.passed() else "FAIL"
    bar = "=" * 65
    print(f"\n{bar}")
    print(f"  Assessment: {label}  ->  {verdict}")
    print(bar)
    print(f"  T observed range   : {m.T_min_obs:.1f} - {m.T_max_obs:.1f} degC")
    print(f"  RH observed range  : {m.RH_min_obs:.1f} - {m.RH_max_obs:.1f} %")
    print()

    # ---- Temperature overtemp split ----
    print("  -- Temperature: overtemperature --")
    print(f"  Controllable overtemp(T>Tmax+3*dT, Tout<=same): "
          f"{m.pct_controllable_T_overtemp:.1f}%   [threshold <=5%]  *")
    print(f"  Graduated deadband (Tmax<T<=Tmax+3*dT_step)  : "
          f"{m.pct_deadband_T_overtemp:.1f}%   [by design; reported only]")
    print(f"  Uncontrollable overtemp (T>Tmax, Tout>Tmax)  : "
          f"{m.pct_uncontrollable_T_overtemp:.1f}%   [reported only]")

    # ---- Temperature undertemp split ----
    print("  -- Temperature: undertemperature --")
    print(f"  Controllable undertemp  (T<Tmin, Tout>=Tmin) : "
          f"{m.pct_controllable_T_undertemp:.1f}%   [threshold <=5%]  *")
    print(f"  Uncontrollable undertemp(T<Tmin, Tout< Tmin) : "
          f"{m.pct_uncontrollable_T_undertemp:.1f}%   [no heating; reported only]")

    # ---- Humidity ----
    print("  -- Humidity --")
    print(f"  Controllable RH overmax (RH>RHmax+dRH, achvble): "
          f"{m.pct_controllable_RH_above_max:.1f}%   [threshold <=10%] *")
    print(f"  Deadband RH (RHmax<RH<=RHmax+dRHstep)          : "
          f"{m.pct_deadband_RH_above_max:.1f}%   [by design; reported only]")
    print(f"  Unctrl RH - T_min guard (T< Tmin)              : "
          f"{m.pct_unctrl_RH_Tmin_guard:.1f}%   [T_min guard; reported only]")
    print(f"  Unctrl RH - HW limit    (max vent insufficient) : "
          f"{m.pct_unctrl_RH_outdoor_sat:.1f}%   [hw limit; reported only]")
    print(f"  RH below RH_min                                 : "
          f"{m.pct_RH_below_min:.1f}%   [informational]")
    print()

    # ---- Anti-resonance ----
    print("  -- Anti-resonance --")
    print(f"  Total window actuations          : {m.total_actuations}")
    print(f"  Max actuations / hour  (any win) : {m.max_act_per_hour:.1f}  [informational]")
    print(f"  Max actuations / 10 min          : {m.max_act_per_10min}   [threshold <=1]  *")
    print(f"  Commands suppressed by dwell     : {m.commands_blocked}")
    print(f"  Conflicts logged                 : {ctrl_state.conflicts_logged}")
    print()
    print("  (* = contributes to pass/fail)")
    print()

    # Per-criterion verdict
    checks = [
        ("Controllable overtemp  <=5%",  m.pct_controllable_T_overtemp  <= 5.0),
        ("Controllable undertemp <=5%",  m.pct_controllable_T_undertemp <= 5.0),
        ("Controllable RH overmax <=10%", m.pct_controllable_RH_above_max <= 10.0),
        ("Actuations/10min       <=1",   m.max_act_per_10min            <= 1),
    ]
    for name, ok in checks:
        print(f"  {'[PASS]' if ok else '[FAIL]'}  {name}")
    print(f"\n  Overall: {verdict}")
    print(bar)


# ---------------------------------------------------------------------------
# CSV and plot output
# ---------------------------------------------------------------------------

def save_csv(result: SimResult, path: Path) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "T_in_C", "RH_in_pct", "T_out_C", "RH_out_pct",
                    "M1_open", "M2_open", "M3_open"])
        for i, t in enumerate(result.t_hist):
            ws = result.windows_hist[i]
            w.writerow([
                f"{t:.0f}",
                f"{result.T_in_hist[i]:.2f}",
                f"{result.RH_in_hist[i]:.1f}",
                f"{result.T_out_hist[i]:.2f}",
                f"{result.RH_out_hist[i]:.1f}",
                int(ws[0]), int(ws[1]), int(ws[2]),
            ])
    print(f"  Saved {path.name}")


def plot_results(
    result:     SimResult,
    cfg:        ControlConfig,
    label:      str,
    path:       Path,
    conditions: OutsideConditions,
    day_offset: int,
) -> None:
    try:
        import matplotlib.pyplot as plt
        import matplotlib.gridspec as gridspec
    except ImportError:
        print("  matplotlib not available — skipping plot")
        return

    from climate_model import resolve_setpoints

    t_h = [t / 3600.0 for t in result.t_hist]

    # Resolve active setpoints at each sample for plot bands
    T_max_line, T_min_line, RH_max_line, RH_min_line = [], [], [], []
    for t in result.t_hist:
        abs_dt = conditions.start_time + timedelta(seconds=t + day_offset * 86400.0)
        sp = resolve_setpoints(
            abs_dt.hour + abs_dt.minute / 60.0, abs_dt.month, cfg
        )
        T_max_line.append(sp.T_max)
        T_min_line.append(sp.T_min)
        RH_max_line.append(sp.RH_max)
        RH_min_line.append(sp.RH_min)

    fig = plt.figure(figsize=(15, 11))
    fig.suptitle(f"Greenhouse Climate Model — {label}", fontsize=13, fontweight="bold")
    gs = gridspec.GridSpec(4, 1, figure=fig, hspace=0.40)

    # ---- Temperature ----
    ax1 = fig.add_subplot(gs[0])
    ax1.plot(t_h, result.T_in_hist,  "r-",  lw=2,   label="T indoor")
    ax1.plot(t_h, result.T_out_hist, "r--", lw=1.2, alpha=0.5, label="T outdoor")
    ax1.plot(t_h, T_max_line, "k:",  lw=1,  label="T_max (active)")
    ax1.plot(t_h, T_min_line, "b:",  lw=1,  label="T_min (active)")
    ax1.fill_between(t_h, T_min_line, T_max_line, alpha=0.08, color="green", label="Target band")
    ax1.set_ylabel("Temperature [°C]")
    ax1.legend(fontsize=8, loc="upper right", ncol=3)
    ax1.grid(True, alpha=0.25)

    # ---- Relative humidity ----
    ax2 = fig.add_subplot(gs[1], sharex=ax1)
    ax2.plot(t_h, result.RH_in_hist,  "b-",  lw=2,   label="RH indoor")
    ax2.plot(t_h, result.RH_out_hist, "b--", lw=1.2, alpha=0.5, label="RH outdoor")
    ax2.plot(t_h, RH_max_line, "k:", lw=1, label="RH_max (active)")
    ax2.plot(t_h, RH_min_line, "c:", lw=1, label="RH_min (active)")
    ax2.axhline(cfg.RH_critical, color="purple", ls="--", lw=0.9,
                alpha=0.7, label=f"RH_critical = {cfg.RH_critical}%")
    ax2.fill_between(t_h, RH_min_line, RH_max_line, alpha=0.08, color="blue", label="Target band")
    ax2.set_ylim(0, 105)
    ax2.set_ylabel("Relative Humidity [%]")
    ax2.legend(fontsize=8, loc="upper right", ncol=3)
    ax2.grid(True, alpha=0.25)

    # ---- Window states ----
    ax3 = fig.add_subplot(gs[2], sharex=ax1)
    names  = ["M1 South roof", "M2 North roof", "M3 Side wall"]
    colors = ["#e67e22", "#27ae60", "#2980b9"]
    ws_T = list(zip(*result.windows_hist)) if result.windows_hist else [[], [], []]
    for i, (name, color) in enumerate(zip(names, colors)):
        vals = [1.0 if o else 0.0 for o in ws_T[i]]
        ax3.step(t_h, [v + i * 1.3 for v in vals],
                 color=color, lw=2, where="post", label=name)
    ax3.set_ylabel("Windows\n(1=open)")
    ax3.set_yticks([0, 1, 1.3, 2.3, 2.6, 3.6])
    ax3.set_yticklabels(["C", "O", "C", "O", "C", "O"], fontsize=7)
    ax3.legend(fontsize=8, loc="upper right")
    ax3.grid(True, alpha=0.25)

    # ---- Actuation events ----
    ax4 = fig.add_subplot(gs[3], sharex=ax1)
    n = len(result.windows_hist)
    act_t, act_y = [], []
    for i in range(1, n):
        for wi, color in enumerate(colors):
            if result.windows_hist[i][wi] != result.windows_hist[i-1][wi]:
                act_t.append(t_h[i])
                act_y.append(wi + 1)
    if act_t:
        ax4.scatter(act_t, act_y, s=12, c=[colors[int(y)-1] for y in act_y], zorder=3)
    ax4.set_yticks([1, 2, 3])
    ax4.set_yticklabels(["M1", "M2", "M3"], fontsize=8)
    ax4.set_ylabel("Actuations")
    ax4.set_xlabel("Time [h]")
    ax4.grid(True, alpha=0.25)

    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {path.name}")


# ---------------------------------------------------------------------------
# Scenario definitions
# ---------------------------------------------------------------------------

def _default_cfg() -> ControlConfig:
    return ControlConfig()


def _summer_cfg() -> ControlConfig:
    """ControlConfig with summer seasonal override enabled."""
    cfg = ControlConfig()
    cfg.summer_enabled = True
    cfg.summer_day   = ScheduleSetpoints(T_min=18.0, T_max=28.0, RH_min=50.0, RH_max=80.0)
    cfg.summer_night = ScheduleSetpoints(T_min=15.0, T_max=22.0, RH_min=55.0, RH_max=85.0)
    return cfg


def _run(
    label:      str,
    cfg:        ControlConfig,
    conditions: OutsideConditions,
    T0:         float,
    RH0:        float,
    duration_s: float,
    day_offset: int,
    out_dir:    Path,
    dt:         float = 10.0,
) -> None:
    result, ctrl = run_simulation(
        label, cfg, conditions, T0, RH0, duration_s, day_offset, dt
    )
    m = compute_metrics(result, ctrl, cfg, conditions, day_offset)
    print_metrics(label, m, ctrl)

    tag = label.replace(" ", "_").replace("/", "-").replace(":", "")
    save_csv(result, out_dir / f"results_{tag}.csv")
    plot_results(result, cfg, label, out_dir / f"results_{tag}.png",
                 conditions, day_offset)


def V1(conditions: OutsideConditions, out_dir: Path) -> None:
    """Full summer season — May 1 to Sep 1, 2025 (123 days)."""
    _run("V1_Full_Summer_Season", _default_cfg(), conditions,
         T0=20.0, RH0=72.0,
         duration_s=123 * 86400.0, day_offset=0, out_dir=out_dir)


def V2(conditions: OutsideConditions, out_dir: Path) -> None:
    """Hot day — May 1, T_out peaks at 33.8°C."""
    _run("V2_Hot_Day", _default_cfg(), conditions,
         T0=20.0, RH0=70.0,
         duration_s=86400.0, day_offset=0, out_dir=out_dir)


def V3(conditions: OutsideConditions, out_dir: Path) -> None:
    """High humidity day — June 1, high RH outside."""
    _run("V3_High_Humidity", _default_cfg(), conditions,
         T0=22.0, RH0=90.0,
         duration_s=86400.0, day_offset=30, out_dir=out_dir)


def V4(conditions: OutsideConditions, out_dir: Path) -> None:
    """Cold night — starts at 20:00 day 0; verifies T_min guard."""
    # Start simulation offset so 20:00 is the first time step
    _run("V4_Cold_Night", _default_cfg(), conditions,
         T0=18.0, RH0=80.0,
         duration_s=43200.0,   # 12 h
         day_offset=0, out_dir=out_dir)


def V5(conditions: OutsideConditions, out_dir: Path) -> None:
    """Summer seasonal override vs default — July 1 (Day 60), 24 h."""
    _run("V5_Default_July",  _default_cfg(), conditions,
         T0=22.0, RH0=75.0, duration_s=86400.0, day_offset=60, out_dir=out_dir)
    _run("V5_Summer_July",   _summer_cfg(),  conditions,
         T0=22.0, RH0=75.0, duration_s=86400.0, day_offset=60, out_dir=out_dir)


SCENARIOS = {"V1": V1, "V2": V2, "V3": V3, "V4": V4, "V5": V5}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Greenhouse Climate Model Verification",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "scenario",
        nargs="?",
        default="V2",
        choices=[*SCENARIOS, "ALL"],
        help="Scenario to run (default: V2)",
    )
    args = parser.parse_args()

    if not _DATA_CSV.exists():
        print(f"ERROR: Dataset not found:\n  {_DATA_CSV}", file=sys.stderr)
        sys.exit(1)

    conditions = OutsideConditions(_DATA_CSV)
    out_dir    = _MODEL_DIR  # results saved alongside this script

    to_run = list(SCENARIOS.values()) if args.scenario == "ALL" else [SCENARIOS[args.scenario]]
    for fn in to_run:
        fn(conditions, out_dir)


if __name__ == "__main__":
    main()

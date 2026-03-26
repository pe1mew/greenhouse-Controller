"""
Greenhouse Controller Simulation
=================================
Implements the plant model (design.md §2.3–2.4), rule-based hysteresis controller
(§3, §3.8 anti-oscillation), and simulation scenarios S1–S5 (§4.1).

Outside temperature and humidity come from historical weather data (May–Sep 2025)
via Environment/outside_conditions.py.

Usage
-----
    python greenhouse_simulation.py [S1|S2|S3|S4|S5|ALL]

Outputs per scenario:
    results_<scenario>.csv   — time-series of T, RH, window states
    results_<scenario>.png   — plot (requires matplotlib)

Plant model
-----------
Steady-state (algebraic) model — no ODE integration. C and UA are eliminated.
T and AH are computed as equilibrium values at each time step given the current
window configuration and outside conditions. Background infiltration (ACH_INF)
prevents division by zero when all windows are closed.
"""

import math
import sys
import csv
import argparse
from pathlib import Path
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import List, Optional, Tuple


# ---------------------------------------------------------------------------
# Locate project root so we can import Environment.outside_conditions
# regardless of where this file is executed from.
# ---------------------------------------------------------------------------
def _find_project_root() -> Path:
    for parent in Path(__file__).resolve().parents:
        if (parent / "Environment" / "outside_conditions.py").exists():
            return parent
    raise RuntimeError(
        "Cannot find project root: 'Environment/outside_conditions.py' not found "
        "in any parent directory of this script."
    )


_PROJECT_ROOT = _find_project_root()
sys.path.insert(0, str(_PROJECT_ROOT))

from Environment.outside_conditions import OutsideConditions  # noqa: E402


# ---------------------------------------------------------------------------
# Physical constants
# ---------------------------------------------------------------------------
RHO_AIR = 1.2      # kg/m³   — air density
CP_AIR  = 1005.0   # J/kg/°C — specific heat of air
P_ATM   = 101325.0 # Pa      — atmospheric pressure
ACH_INF = 0.5      # h⁻¹    — background infiltration (greenhouse leakage, always present)


# ---------------------------------------------------------------------------
# Plant parameters  (design.md §2.6 — all TBD, using placeholder estimates)
# ---------------------------------------------------------------------------
@dataclass
class PlantParameters:
    # Greenhouse air volume (design.md §1.4, §2.6) — fixed from geometry, not a free parameter
    V: float = 2400.0   # m³ — 40×16×3 (base) + ½×16×1.5×40 (roof); gutter 3 m, ridge 4.5 m

    # Ventilation — air changes per hour per window type when fully open (design.md §2.6)
    # M1 and M2 are symmetric roof vents → single ACH_roof parameter
    # ACH_inf background infiltration is a fixed constant (ACH_INF), not listed here
    ACH_roof: float =  8.0   # h⁻¹ — each roof vent (M1 or M2); A=8 m², ≈0.67 m/s effective
    ACH_wall: float = 40.0   # h⁻¹ — north wall vent (M3);      A=80 m², ≈0.33 m/s effective

    # Transpiration — continuous moisture source from plants (design.md §2.6, §3.7)
    m_transp: float = 0.010  # kg/s — general default (≈1.35 mm/m²/day over 640 m²); farmer-configurable

    # Solar heat gain peak (clear-sky noon, through glazing)
    Q_solar_peak: float = 20_000.0  # W — tunable; co-calibrate with ACH values


# ---------------------------------------------------------------------------
# Control parameters  (design.md §3.3, §3.6 — all TBD, using initial values)
# ---------------------------------------------------------------------------
@dataclass
class ControlParameters:
    # Temperature setpoints [°C] — farmer-configurable (design.md §3.7)
    T_sp_day:   float = 24.0   # daytime setpoint   (06:00–20:00)
    T_sp_night: float = 18.0   # night-time setpoint (20:00–06:00)

    # Temperature hysteresis thresholds (relative to active T_sp)
    # Evenly spaced: 1 window at T_sp+dT_step, 2 at +2×step, 3 at +3×step
    dT_step: float =  2.0   # °C — spacing between window-opening levels
    dT_hyst: float =  1.0   # °C — close threshold below T_sp

    # Relative humidity setpoints [%] — farmer-configurable
    RH_sp_day:   float = 65.0   # daytime setpoint
    RH_sp_night: float = 75.0   # night-time setpoint

    # RH hysteresis thresholds (relative to active RH_sp)
    # Evenly spaced: 1 window at RH_sp+dRH_step, 2 at +2×step, 3 at +3×step
    dRH_step: float =  5.0  # % — spacing between window-opening levels
    dRH_hyst: float =  5.0  # % — close threshold below RH_sp

    # Day/night boundary — aligned with Q_solar model (06:00–20:00)
    day_start: float =  6.0   # hour (local)
    day_end:   float = 20.0   # hour (local)

    # Critical RH: force minimum ventilation even when T < T_sp
    RH_critical: float = 88.0  # %

    # Control update interval
    ctrl_interval: float = 60.0  # s — re-evaluate demand every 60 s

    # Anti-oscillation: minimum time a window must stay in OPEN or CLOSED before
    # it can be commanded again.  Prevents rapid open-close cycling when the
    # measured value hovers near a threshold.  (design.md §3.8)
    t_min_dwell: float = 600.0  # s — 10 minutes


# ---------------------------------------------------------------------------
# Window state
# ---------------------------------------------------------------------------
class WindowState(Enum):
    CLOSED       = auto()
    MOVING_OPEN  = auto()
    MOVING_CLOSE = auto()
    OPEN         = auto()


@dataclass
class Window:
    name:    str
    ach:     float           # h⁻¹ — ventilation rate when fully open
    t_motor: float           # s   — time to reach end position (M1/M2: 21 s, M3: 171 s)

    # Estimated state (what the controller believes)
    state:          WindowState = WindowState.CLOSED
    target_state:   WindowState = WindowState.CLOSED
    move_start_time: float = 0.0

    # Physical state (what the plant model uses — may differ in stall scenario S5)
    physically_open: bool = False

    # Anti-oscillation: simulation time [s] when this window last settled into
    # OPEN or CLOSED.  A new command is blocked until t_min_dwell has elapsed.
    last_settled_time: float = 0.0

    def is_open(self) -> bool:
        return self.state == WindowState.OPEN

    def is_moving(self) -> bool:
        return self.state in (WindowState.MOVING_OPEN, WindowState.MOVING_CLOSE)


# ---------------------------------------------------------------------------
# Simulation state
# ---------------------------------------------------------------------------
@dataclass
class SimulationState:
    T:   float          # °C     — indoor air temperature
    AH:  float          # kg/m³  — indoor absolute humidity
    t:   float = 0.0    # s      — elapsed simulation time
    windows: List[Window] = field(default_factory=list)

    # Demand memory for hysteresis deadband
    T_demand_prev:  int = 0
    RH_demand_prev: int = 0

    # Anti-oscillation: count of commands suppressed by t_min_dwell guard
    commands_blocked: int = 0

    # Recorded history (sampled every ~60 s)
    t_hist:       List[float]               = field(default_factory=list)
    T_hist:       List[float]               = field(default_factory=list)
    RH_hist:      List[float]               = field(default_factory=list)
    T_out_hist:   List[float]               = field(default_factory=list)
    RH_out_hist:  List[float]               = field(default_factory=list)
    windows_hist: List[Tuple[bool, ...]]    = field(default_factory=list)  # open flags


# ---------------------------------------------------------------------------
# Thermodynamic helpers
# ---------------------------------------------------------------------------
def p_sat(T: float) -> float:
    """Saturation vapour pressure [Pa] — Magnus formula (design.md §2.4)."""
    return 610.78 * math.exp(17.27 * T / (T + 237.3))


def AH_sat(T: float) -> float:
    """Saturation absolute humidity [kg/m³]."""
    ps = p_sat(T)
    return 0.622 * ps / (P_ATM - ps) * RHO_AIR


def RH_from_AH(AH: float, T: float) -> float:
    """Absolute humidity → relative humidity [%], clamped to [0, 100]."""
    sat = AH_sat(T)
    if sat <= 0:
        return 0.0
    return min(100.0, AH / sat * 100.0)


def AH_from_RH(RH: float, T: float) -> float:
    """Relative humidity [%] + temperature [°C] → absolute humidity [kg/m³]."""
    return RH / 100.0 * AH_sat(T)


# ---------------------------------------------------------------------------
# Solar heat gain model (design.md §2.3 disturbance Q_solar)
# ---------------------------------------------------------------------------
def Q_solar(t_elapsed: float, Q_peak: float) -> float:
    """
    Solar heat gain through glazing [W].

    Clear-sky sine approximation: rises from 0 at 06:00, peaks at 13:00,
    returns to 0 at 20:00 (shifted to match typical greenhouse irradiance peak).
    """
    hour = (t_elapsed / 3600.0) % 24.0
    if 6.0 <= hour <= 20.0:
        # Shift peak to 13:00 (7 h after sunrise at 06:00)
        angle = math.pi * (hour - 6.0) / 14.0
        return Q_peak * math.sin(angle) ** 2
    return 0.0


# ---------------------------------------------------------------------------
# Plant model — ODE right-hand side (design.md §2.3, §2.4)
# ---------------------------------------------------------------------------
def _ach_total(windows: List[Window]) -> float:
    """
    Total ventilation rate [s⁻¹].

    Always includes ACH_INF (background infiltration) so the denominator in
    the steady-state equations is never zero even when all windows are closed.
    """
    total = ACH_INF / 3600.0  # infiltration baseline, h⁻¹ → s⁻¹
    for w in windows:
        if w.physically_open:
            total += w.ach / 3600.0  # h⁻¹ → s⁻¹
    return total


def plant_step(
    state: SimulationState,
    T_out: float,
    RH_out: float,
    plant: PlantParameters,
    dt: float,  # unused in steady-state model; kept for API compatibility
) -> None:
    """
    Update greenhouse state using the steady-state plant model (design.md §2.3, §2.4).

    Setting dT/dt = 0 and d(AH)/dt = 0 gives algebraic equilibrium:

    Temperature (§2.3):
        T = T_out + Q_solar / (ACH_total * V * rho * cp)

    Absolute humidity (§2.4):
        AH = AH_out + m_transp / (ACH_total * V)

    C and UA are eliminated. ACH_total always includes ACH_INF (infiltration)
    so the denominator is never zero.
    """
    AH_out = AH_from_RH(RH_out, T_out)
    ACH    = _ach_total(state.windows)           # s⁻¹  (includes infiltration)
    Qs     = Q_solar(state.t, plant.Q_solar_peak)  # W

    state.T  = T_out + Qs / (ACH * plant.V * RHO_AIR * CP_AIR)
    state.AH = max(0.0, AH_out + plant.m_transp / (ACH * plant.V))


# ---------------------------------------------------------------------------
# Window state manager (design.md §3.5)
# ---------------------------------------------------------------------------
def update_window_states(state: SimulationState) -> None:
    """
    Transition MOVING windows to their target state after w.t_motor seconds.
    Also synchronises the physical state (used by plant model).
    """
    for w in state.windows:
        if w.is_moving():
            if (state.t - w.move_start_time) >= w.t_motor:
                w.state = w.target_state
                # Physical state follows estimated state (nominal case).
                # S5 overrides physically_open before calling this function.
                if not w.is_moving():
                    w.physically_open = (w.state == WindowState.OPEN)
                    # Record when this window settled — used by anti-oscillation guard.
                    w.last_settled_time = state.t


def command_window(
    window: Window,
    desired: WindowState,
    t: float,
    plant: PlantParameters,
    ctrl: "ControlParameters",
    state: "SimulationState",
) -> None:
    """
    Issue OPEN or CLOSE command to a window.

    Ignored if:
    - already in desired state (§3.5), or
    - currently moving (§3.5), or
    - minimum dwell time has not elapsed since last state change (§3.8 anti-oscillation).
    """
    if window.state == desired or window.is_moving():
        return

    # Anti-oscillation guard (§3.8): suppress command if window settled too recently.
    if (t - window.last_settled_time) < ctrl.t_min_dwell:
        state.commands_blocked += 1
        return

    window.target_state    = desired
    window.move_start_time = t
    window.state = (
        WindowState.MOVING_OPEN  if desired == WindowState.OPEN
        else WindowState.MOVING_CLOSE
    )


# ---------------------------------------------------------------------------
# Decision logic (design.md §3.3, §3.4)
# ---------------------------------------------------------------------------
def _demand(
    value: float,
    sp: float,
    d_step: float,
    d_hyst: float,
    prev: int,
) -> int:
    """
    Generic 0–3 demand level with evenly-spaced thresholds and hysteresis deadband.

    Thresholds above sp: 1×d_step → 1 window, 2×d_step → 2, 3×d_step → 3 (all open).
    """
    if   value > sp + 3 * d_step: return 3
    elif value > sp + 2 * d_step: return 2
    elif value > sp +     d_step: return 1
    elif value < sp - d_hyst:     return 0
    else:                         return prev  # deadband — no change


def _active_setpoints(t_elapsed: float, ctrl: ControlParameters):
    """Return (T_sp, RH_sp) for the current hour of day."""
    hour = (t_elapsed / 3600.0) % 24.0
    if ctrl.day_start <= hour < ctrl.day_end:
        return ctrl.T_sp_day, ctrl.RH_sp_day
    return ctrl.T_sp_night, ctrl.RH_sp_night


def apply_control(
    state: SimulationState,
    ctrl: ControlParameters,
    plant: PlantParameters,
) -> None:
    """
    Evaluate demands, resolve T/RH conflict, command windows.

    Window priority order (§3.4): M2 (north roof), M1 (south roof), M3 (side wall).
    The windows list is always [M1, M2, M3]; priority is [M2, M1, M3].
    """
    T  = state.T
    RH = RH_from_AH(state.AH, state.T)
    T_sp, RH_sp = _active_setpoints(state.t, ctrl)

    T_demand = _demand(
        T, T_sp,
        ctrl.dT_step, ctrl.dT_hyst,
        state.T_demand_prev,
    )
    RH_demand = _demand(
        RH, RH_sp,
        ctrl.dRH_step, ctrl.dRH_hyst,
        state.RH_demand_prev,
    )

    # Conflict resolution (§3.3):
    # T takes priority. Exception: T < T_sp but RH > RH_critical → force ≥1 window.
    if T < T_sp and RH > ctrl.RH_critical:
        V_demand = max(1, RH_demand)
    else:
        V_demand = max(T_demand, RH_demand)

    state.T_demand_prev  = T_demand
    state.RH_demand_prev = RH_demand

    # Priority order: M2 (idx 1), M1 (idx 0), M3 (idx 2)
    priority = [state.windows[1], state.windows[0], state.windows[2]]

    for rank, w in enumerate(priority):
        desired = WindowState.OPEN if rank < V_demand else WindowState.CLOSED
        command_window(w, desired, state.t, plant, ctrl, state)


# ---------------------------------------------------------------------------
# Main simulation loop
# ---------------------------------------------------------------------------
def run_simulation(
    scenario_name: str,
    plant: PlantParameters,
    ctrl: ControlParameters,
    conditions: OutsideConditions,
    T0: float,
    RH0: float,
    duration: float,
    dt: float = 10.0,
    day_offset: int = 0,
    stall_window_idx: Optional[int] = None,
    stall_at: Optional[float] = None,
) -> SimulationState:
    """
    Run the simulation.

    Parameters
    ----------
    T0, RH0          : Initial indoor conditions.
    duration          : Simulation duration [s].
    dt                : Integration time step [s] (default 10 s).
    day_offset        : Start day in the historical dataset (0 = May 1, 2025).
    stall_window_idx  : Index in [M1, M2, M3] of window to stall (S5 only).
    stall_at          : Time [s] at which the window physically jams open (S5 only).
    """
    print(f"\n{'=' * 60}")
    print(f"  {scenario_name}")
    print(f"  Duration {duration/3600:.1f} h  |  dt {dt} s  |  Start day +{day_offset}")
    print(f"  Initial: T={T0}°C  RH={RH0}%")
    print(f"{'=' * 60}")

    # Initialise state
    windows = [
        Window("M1_Dakbeluchting_Zuid",  plant.ACH_roof, t_motor=21.0),
        Window("M2_Dakbeluchting_Noord", plant.ACH_roof, t_motor=21.0),
        Window("M3_Zijwandbeluchting",   plant.ACH_wall, t_motor=171.0),
    ]
    state = SimulationState(
        T=T0,
        AH=AH_from_RH(RH0, T0),
        windows=windows,
    )

    offset_s = day_offset * 86400.0
    n_steps  = int(duration / dt)
    # Record approximately every 60 s to keep history compact
    record_every = max(1, int(60.0 / dt))
    # Re-evaluate control at ctrl_interval, rounded to nearest time step
    ctrl_every = max(1, int(ctrl.ctrl_interval / dt))

    stalled = False  # S5 flag

    for step in range(n_steps):
        state.t = step * dt

        # --- Outside conditions from historical data ---
        T_out, RH_out = conditions.get_conditions_at_elapsed_time(
            state.t + offset_s
        )

        # --- S5: physically jam the window (controller still thinks it opens) ---
        if (
            stall_window_idx is not None
            and stall_at is not None
            and not stalled
            and state.t >= stall_at
        ):
            w = state.windows[stall_window_idx]
            # Prevent physical state from ever becoming True for this window
            w.physically_open = False
            # Push move_start_time far into the future so timeout never fires
            # (controller will think it's still MOVING, then give up on next command)
            # Actually: let the timeout fire so the controller believes it's OPEN,
            # but the physically_open flag stays False — this tests the discrepancy.
            stalled = True
            print(f"  [S5] Window {w.name} stalled at t={state.t:.0f} s")

        # --- Window state manager ---
        # For S5: if this window just timed out, force physically_open=False after update
        update_window_states(state)
        if stalled and stall_window_idx is not None:
            state.windows[stall_window_idx].physically_open = False

        # --- Plant model ---
        plant_step(state, T_out, RH_out, plant, dt)

        # --- Controller (periodic) ---
        if step % ctrl_every == 0:
            apply_control(state, ctrl, plant)

        # --- Record history ---
        if step % record_every == 0:
            state.t_hist.append(state.t)
            state.T_hist.append(state.T)
            state.RH_hist.append(RH_from_AH(state.AH, state.T))
            state.T_out_hist.append(T_out)
            state.RH_out_hist.append(RH_out)
            state.windows_hist.append(
                tuple(w.physically_open for w in state.windows)
            )

    # --- Performance metrics (design.md §4.2) ---
    _print_metrics(state, ctrl)
    return state


def _print_metrics(state: SimulationState, ctrl: ControlParameters) -> None:
    if not state.T_hist:
        return

    T_tol  = 2.0   # ±°C tolerance (TBD in design)
    RH_tol = 5.0   # ±%  tolerance (TBD in design)
    n      = len(state.T_hist)

    # Compute active setpoint at each recorded time step
    T_sp_hist  = [_active_setpoints(t, ctrl)[0] for t in state.t_hist]
    RH_sp_hist = [_active_setpoints(t, ctrl)[1] for t in state.t_hist]

    T_in_band  = sum(1 for T,  sp in zip(state.T_hist,  T_sp_hist)  if abs(T  - sp) <= T_tol)  / n * 100
    RH_in_band = sum(1 for RH, sp in zip(state.RH_hist, RH_sp_hist) if abs(RH - sp) <= RH_tol) / n * 100

    worst_T = sum(1 for T, sp in zip(state.T_hist, T_sp_hist) if T > sp + 3 * ctrl.dT_step) / n * 100

    # Count window state changes (proxy for actuations)
    actuations = 0
    prev = state.windows_hist[0]
    for ws in state.windows_hist[1:]:
        actuations += sum(1 for a, b in zip(ws, prev) if a != b)
        prev = ws

    RH_f = RH_from_AH(state.AH, state.T)
    print(f"  Final: T={state.T:.1f}°C  RH={RH_f:.1f}%")
    print(f"  T range : {min(state.T_hist):.1f}–{max(state.T_hist):.1f}°C")
    print(f"  RH range: {min(state.RH_hist):.1f}–{max(state.RH_hist):.1f}%")
    print(f"  Time T within ±{T_tol}°C of active T_sp  : {T_in_band:.1f}%")
    print(f"  Time RH within ±{RH_tol}% of active RH_sp : {RH_in_band:.1f}%")
    print(f"  Time T > active T_sp + dT_high          : {worst_T:.1f}%")
    print(f"  Window actuations                  : {actuations}")
    print(f"  Commands blocked (dwell guard)     : {state.commands_blocked}")


# ---------------------------------------------------------------------------
# CSV export
# ---------------------------------------------------------------------------
def save_csv(state: SimulationState, path: Path) -> None:
    """Write time-series history to a CSV file."""
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "t_s", "T_in_C", "RH_in_pct",
            "T_out_C", "RH_out_pct",
            "M1_open", "M2_open", "M3_open",
        ])
        for i, t in enumerate(state.t_hist):
            ws = state.windows_hist[i]
            writer.writerow([
                f"{t:.0f}",
                f"{state.T_hist[i]:.2f}",
                f"{state.RH_hist[i]:.1f}",
                f"{state.T_out_hist[i]:.2f}",
                f"{state.RH_out_hist[i]:.1f}",
                int(ws[0]), int(ws[1]), int(ws[2]),
            ])
    print(f"  Saved {path.name}")


# ---------------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------------
def plot_results(
    state: SimulationState,
    ctrl: ControlParameters,
    scenario_name: str,
    path: Path,
) -> None:
    try:
        import matplotlib.pyplot as plt
        import matplotlib.gridspec as gridspec
    except ImportError:
        print("  matplotlib not available — skipping plot")
        return

    t_h = [t / 3600.0 for t in state.t_hist]

    fig = plt.figure(figsize=(14, 10))
    fig.suptitle(f"Greenhouse Simulation — {scenario_name}", fontsize=13, fontweight="bold")
    gs  = gridspec.GridSpec(3, 1, figure=fig, hspace=0.35)

    # Temperature
    ax1 = fig.add_subplot(gs[0])
    ax1.plot(t_h, state.T_hist,     "r-",  lw=2,   label="Indoor T")
    ax1.plot(t_h, state.T_out_hist, "r--", lw=1.2, alpha=0.55, label="Outside T")
    ax1.axhline(ctrl.T_sp_day,                    color="k",    ls=":",  lw=1,   label=f"T_sp day = {ctrl.T_sp_day}°C")
    ax1.axhline(ctrl.T_sp_night,                  color="grey", ls=":",  lw=1,   label=f"T_sp night = {ctrl.T_sp_night}°C")
    ax1.axhline(ctrl.T_sp_day   + 3 * ctrl.dT_step, color="red",  ls=":",  lw=0.8, alpha=0.5, label=f"T_sp_day + 3×dT_step")
    ax1.axhline(ctrl.T_sp_night - ctrl.dT_hyst,     color="blue", ls=":",  lw=0.8, alpha=0.5, label=f"T_sp_night − dT_hyst")
    ax1.set_ylabel("Temperature [°C]")
    ax1.legend(fontsize=8, loc="upper right", ncol=3)
    ax1.grid(True, alpha=0.25)

    # Humidity
    ax2 = fig.add_subplot(gs[1], sharex=ax1)
    ax2.plot(t_h, state.RH_hist,     "b-",  lw=2,   label="Indoor RH")
    ax2.plot(t_h, state.RH_out_hist, "b--", lw=1.2, alpha=0.55, label="Outside RH")
    ax2.axhline(ctrl.RH_sp_day,                   color="k",    ls=":",  lw=1,   label=f"RH_sp day = {ctrl.RH_sp_day}%")
    ax2.axhline(ctrl.RH_sp_night,                 color="grey", ls=":",  lw=1,   label=f"RH_sp night = {ctrl.RH_sp_night}%")
    ax2.axhline(ctrl.RH_sp_day  + 3 * ctrl.dRH_step, color="blue", ls=":",  lw=0.8, alpha=0.5)
    ax2.axhline(ctrl.RH_critical,             color="purple", ls="--", lw=0.8, alpha=0.5, label=f"RH_critical")
    ax2.set_ylabel("Relative Humidity [%]")
    ax2.set_ylim(0, 105)
    ax2.legend(fontsize=8, loc="upper right", ncol=3)
    ax2.grid(True, alpha=0.25)

    # Window states
    ax3 = fig.add_subplot(gs[2], sharex=ax1)
    window_names = ["M1 Zuid (south roof)", "M2 Noord (north roof)", "M3 Zijwand (side wall)"]
    colors       = ["#e67e22", "#27ae60", "#2980b9"]
    ws_transposed = list(zip(*state.windows_hist)) if state.windows_hist else [[], [], []]
    for i, (name, color) in enumerate(zip(window_names, colors)):
        open_vals = [1.0 if o else 0.0 for o in ws_transposed[i]]
        ax3.step(t_h, [v + i * 1.3 for v in open_vals],
                 color=color, lw=2, where="post", label=name)
    ax3.set_ylabel("Window (1=open)")
    ax3.set_xlabel("Time [h]")
    ax3.set_yticks([0, 1, 1.3, 2.3, 2.6, 3.6])
    ax3.set_yticklabels(["C", "O", "C", "O", "C", "O"], fontsize=7)
    ax3.legend(fontsize=8, loc="upper right")
    ax3.grid(True, alpha=0.25)

    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {path.name}")


# ---------------------------------------------------------------------------
# Scenario helpers
# ---------------------------------------------------------------------------
def _run(
    name: str,
    plant: PlantParameters,
    ctrl: ControlParameters,
    conditions: OutsideConditions,
    T0: float,
    RH0: float,
    duration: float,
    day_offset: int,
    dt: float = 10.0,
    stall_window_idx: Optional[int] = None,
    stall_at: Optional[float] = None,
    out_dir: Optional[Path] = None,
) -> SimulationState:
    state = run_simulation(
        scenario_name    = name,
        plant            = plant,
        ctrl             = ctrl,
        conditions       = conditions,
        T0               = T0,
        RH0              = RH0,
        duration         = duration,
        dt               = dt,
        day_offset       = day_offset,
        stall_window_idx = stall_window_idx,
        stall_at         = stall_at,
    )
    if out_dir is None:
        out_dir = Path(__file__).parent
    tag = name.replace(" ", "_").replace("/", "-").replace(":", "")
    save_csv(state,  out_dir / f"results_{tag}.csv")
    plot_results(state, ctrl, name, out_dir / f"results_{tag}.png")
    return state


# ---------------------------------------------------------------------------
# Scenarios (design.md §4.1)
# ---------------------------------------------------------------------------
def S1(plant, ctrl, conditions, **kw):
    """S1 — Daytime solar gain, windows start closed.
    Purpose: validate T control; verify window open sequence.
    Day 0 = May 1, 2025 (historically hot: T_out up to 33.8°C, RH drops to 33%).
    """
    return _run(
        "S1 Daytime Solar Gain",
        plant, ctrl, conditions,
        T0=20.0, RH0=70.0,
        duration=86400.0,   # 24 h
        day_offset=0,       # May 1 — hottest day in dataset
        **kw,
    )


def S2(plant, ctrl, conditions, **kw):
    """S2 — High indoor humidity, mild outside conditions.
    Purpose: validate RH control; check conflict handling.
    Day 30 = June 1, 2025 (moderate temperatures).
    Start with high RH (plant load after watering).
    """
    return _run(
        "S2 High Humidity Mild Day",
        plant, ctrl, conditions,
        T0=22.0, RH0=90.0,
        duration=86400.0,
        day_offset=30,      # ~June 1
        **kw,
    )


def S3(plant, ctrl, conditions, **kw):
    """S3 — Full 24-hour day/night cycle.
    Purpose: validate disturbance rejection over a complete diurnal cycle.
    Day 60 = July 1, 2025 (peak summer).
    """
    return _run(
        "S3 Full 24h Day-Night Cycle",
        plant, ctrl, conditions,
        T0=22.0, RH0=75.0,
        duration=86400.0,
        day_offset=60,      # ~July 1
        **kw,
    )


def S4(plant, ctrl, conditions, **kw):
    """S4 — T below setpoint, RH above critical threshold.
    Purpose: validate conflict resolution logic (§3.3 exception).
    Windows should open despite T < T_sp because RH > RH_critical.
    Use a cool early-morning slice: start at 02:00 on May 1 (T_out ≈ 6°C).
    """
    # Lower RH_critical slightly so the conflict triggers clearly
    ctrl_s4 = ControlParameters(**ctrl.__dict__)
    ctrl_s4.RH_critical = 85.0

    return _run(
        "S4 T Below Setpoint RH Critical",
        plant, ctrl_s4, conditions,
        T0=20.0, RH0=92.0,  # cool indoor, very humid
        duration=14400.0,   # 4 h
        day_offset=0,
        **kw,
    )


def S5(plant, ctrl, conditions, **kw):
    """S5 — Motor stall (window jams, cannot open physically).
    Purpose: validate state manager robustness.
    Controller issues OPEN to M2; after 2 h the window physically jams.
    The estimated state eventually reads OPEN, but the plant model uses
    physically_open=False — demonstrating degraded ventilation performance.
    """
    return _run(
        "S5 Motor Stall M2",
        plant, ctrl, conditions,
        T0=20.0, RH0=72.0,
        duration=28800.0,   # 8 h
        day_offset=0,
        stall_window_idx=1, # M2 (index 1 in [M1, M2, M3])
        stall_at=7200.0,    # stall after 2 h
        **kw,
    )


SCENARIOS = {"S1": S1, "S2": S2, "S3": S3, "S4": S4, "S5": S5}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> None:
    parser = argparse.ArgumentParser(
        description="Greenhouse Controller Simulation",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "scenario",
        nargs="?",
        default="S1",
        choices=[*SCENARIOS, "ALL"],
        help="Scenario to run (default: S1)",
    )
    args = parser.parse_args()

    # Load historical outside conditions once
    csv_path = _PROJECT_ROOT / "Environment" / "airTemperature_2025-05-01_to_2025-09-01.csv"
    conditions = OutsideConditions(csv_path)

    plant = PlantParameters()
    ctrl  = ControlParameters()

    to_run = list(SCENARIOS.values()) if args.scenario == "ALL" else [SCENARIOS[args.scenario]]
    for fn in to_run:
        fn(plant, ctrl, conditions)


if __name__ == "__main__":
    main()

"""
Greenhouse Climate Control Model
=================================
Graduated hysteresis control algorithm for the greenhouse ventilation
controller (T6 — Climate Control task, TSDS §5.2).

Control objective: maintain indoor temperature and relative humidity within
farmer-configured thresholds by modulating three window ventilators
(M1, M2, M3).

Design reference: model/model_design.md
TSDS reference:   technicalSoftwareDesignSpecification.md §5.2, §5.10
FRS reference:    functionalRequirementsSpecification.md FR-C01–FR-C04,
                  FR-CR01–FR-CR04, FR-M08–FR-M11

This module contains ONLY the control algorithm — no plant physics.
It is intended to be:
  1. Verified against the plant model in verify_model.py.
  2. Translated to C for the ESP32-S3 firmware (T6 task).

All parameters map directly to the NVS 'climate' and 'motor' namespaces
(TSDS §5.10).  No dynamic allocation is used; all state fits in fixed structs.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from enum import IntEnum
from typing import List, Optional, Tuple


# ---------------------------------------------------------------------------
# Schedule setpoints — one slot (day / night, optionally seasonal)
# ---------------------------------------------------------------------------

@dataclass
class ScheduleSetpoints:
    """
    Temperature and humidity thresholds for one schedule slot.

    FRS: FR-C01 (T_min), FR-C02 (T_max), FR-C03 (RH_min), FR-C04 (RH_max).
    NVS namespace: 'climate'.
    """
    T_min: float = 15.0   # °C — lower temperature limit; close windows below this
    T_max: float = 25.0   # °C — upper temperature limit; open windows above this
    RH_min: float = 50.0  # %  — lower humidity limit (passive — model cannot add moisture)
    RH_max: float = 80.0  # %  — upper humidity limit; open windows above this


# ---------------------------------------------------------------------------
# Full controller configuration — all configurable parameters
# ---------------------------------------------------------------------------

@dataclass
class ControlConfig:
    """
    All configurable parameters of the climate control model.

    Parameters are farmer-level (setpoints) or administrator-level (tuning).
    They are stored in NVS and loaded by T4 (Data Manager) on boot.
    Default values represent a reasonable starting point for a Dutch greenhouse.

    TSDS §5.10 — NVS 'climate' namespace.
    """

    # ---- Day and night base setpoints (farmer-level, FRS FR-C01–FR-C04) ----
    day: ScheduleSetpoints = field(
        default_factory=lambda: ScheduleSetpoints(15.0, 25.0, 50.0, 80.0)
    )
    night: ScheduleSetpoints = field(
        default_factory=lambda: ScheduleSetpoints(12.0, 20.0, 55.0, 85.0)
    )

    # ---- Day / night boundary (admin-level) ----
    day_start: float = 6.0   # hour (local time, 0–24)
    day_end: float   = 20.0  # hour (local time, 0–24)

    # ---- Summer schedule override (optional, farmer-level) ----
    summer_enabled: bool = False
    summer_day: ScheduleSetpoints = field(
        default_factory=lambda: ScheduleSetpoints(18.0, 28.0, 50.0, 80.0)
    )
    summer_night: ScheduleSetpoints = field(
        default_factory=lambda: ScheduleSetpoints(15.0, 22.0, 55.0, 85.0)
    )
    summer_start_month: int = 5  # May  (inclusive)
    summer_end_month:   int = 9  # Sep  (exclusive → Aug is last summer month)

    # ---- Winter schedule override (optional, farmer-level) ----
    winter_enabled: bool = False
    winter_day: ScheduleSetpoints = field(
        default_factory=lambda: ScheduleSetpoints(12.0, 20.0, 50.0, 80.0)
    )
    winter_night: ScheduleSetpoints = field(
        default_factory=lambda: ScheduleSetpoints(10.0, 18.0, 55.0, 85.0)
    )
    winter_start_month: int = 11  # Nov (inclusive)
    winter_end_month:   int = 3   # Mar (exclusive → Feb is last winter month)

    # ---- Graduated hysteresis parameters (admin-level) ----
    dT_step: float  = 2.0   # °C — spacing between graduated T opening levels
    dT_hyst: float  = 1.0   # °C — deadband width below T_max for closing
    dRH_step: float = 5.0   # %  — spacing between graduated RH opening levels
    dRH_hyst: float = 5.0   # %  — deadband width below RH_max for closing

    # ---- Critical RH override (admin-level, TSDS §5.2 conflict resolution) ----
    RH_critical: float = 88.0  # % — force ≥1 window open even when T < T_min

    # ---- Anti-resonance dwell timers (admin-level, model_design §6.2) ----
    dwell_open:  float = 600.0  # s — minimum time in OPEN before close permitted
    dwell_close: float = 600.0  # s — minimum time in CLOSED before open permitted

    # ---- Control evaluation interval (admin-level) ----
    ctrl_interval: float = 60.0  # s


# ---------------------------------------------------------------------------
# Window state machine
# ---------------------------------------------------------------------------

class WindowState(IntEnum):
    """
    Window controller state.  Maps to the relay state machine in T2 (TSDS §4.3).
    IntEnum allows direct comparison and use as array index.
    """
    CLOSED       = 0
    MOVING_OPEN  = 1
    OPEN         = 2
    MOVING_CLOSE = 3


@dataclass
class WindowCtrl:
    """
    Per-window controller state (estimated position + dwell tracking).

    Corresponds to the window struct tracked by T2 in firmware.
    NVS namespace: 'motor' (dwell times, t_motor).
    """
    name:            str         = "M?"
    t_motor:         float       = 21.0       # s — travel time to end position
    state:           WindowState = WindowState.CLOSED
    target:          WindowState = WindowState.CLOSED
    t_move_start:    float       = 0.0        # s — simulation time when move began
    last_settled_t:  float       = -1_000_000.0  # s — ensures first command not blocked


    def is_open(self) -> bool:
        return self.state == WindowState.OPEN

    def is_moving(self) -> bool:
        return self.state in (WindowState.MOVING_OPEN, WindowState.MOVING_CLOSE)

    def is_settled(self) -> bool:
        return not self.is_moving()


# ---------------------------------------------------------------------------
# Controller runtime state — all mutable state between evaluation cycles
# ---------------------------------------------------------------------------

@dataclass
class ControllerState:
    """
    All mutable state owned by the climate control algorithm (T6).

    In firmware this lives in static variables inside the T6 task function.
    """
    windows: List[WindowCtrl]
    T_demand_prev:  int   = 0          # previous T demand level (hysteresis memory)
    RH_demand_prev: int   = 0          # previous RH demand level (hysteresis memory)
    t_last_ctrl:    float = -1_000_000.0  # s — time of last evaluation; ensures first runs immediately

    # Diagnostics
    commands_blocked: int = 0          # count of commands suppressed by dwell guard
    conflicts_logged: int = 0          # count of T/RH conflict resolutions

    @staticmethod
    def initial() -> "ControllerState":
        """Create a controller state with all windows CLOSED and ready to accept commands."""
        return ControllerState(windows=[
            WindowCtrl(name="M1_Zuid_roof",  t_motor=21.0),
            WindowCtrl(name="M2_Noord_roof", t_motor=21.0),
            WindowCtrl(name="M3_Side_wall",  t_motor=171.0),
        ])


# ---------------------------------------------------------------------------
# Schedule resolution  (model_design §3)
# ---------------------------------------------------------------------------

def _is_summer(month: int, cfg: ControlConfig) -> bool:
    """Return True if month falls within the summer season (no year wrap)."""
    if not cfg.summer_enabled:
        return False
    return cfg.summer_start_month <= month < cfg.summer_end_month


def _is_winter(month: int, cfg: ControlConfig) -> bool:
    """Return True if month falls within the winter season (handles year wrap)."""
    if not cfg.winter_enabled:
        return False
    s, e = cfg.winter_start_month, cfg.winter_end_month
    if s > e:          # wraps over year boundary (e.g. Nov=11 → Mar=3)
        return month >= s or month < e
    return s <= month < e


def resolve_setpoints(
    hour: float,
    month: int,
    cfg: ControlConfig,
) -> ScheduleSetpoints:
    """
    Return the active setpoints for the given hour and month.

    Priority:  seasonal override  >  base day/night schedule.
    model_design §3.
    """
    is_day = cfg.day_start <= hour < cfg.day_end

    if _is_summer(month, cfg):
        return cfg.summer_day if is_day else cfg.summer_night
    if _is_winter(month, cfg):
        return cfg.winter_day if is_day else cfg.winter_night
    return cfg.day if is_day else cfg.night


# ---------------------------------------------------------------------------
# Graduated demand with hysteresis  (model_design §4)
# ---------------------------------------------------------------------------

def _demand(
    value: float,
    threshold: float,
    d_step: float,
    d_hyst: float,
    prev: int,
) -> int:
    """
    Compute a graduated demand level (0–3) with hysteresis deadband.

    Opening thresholds (above `threshold`):
        > threshold + 1×d_step  →  1
        > threshold + 2×d_step  →  2
        > threshold + 3×d_step  →  3

    Closing threshold (hysteresis below `threshold`):
        < threshold − d_hyst    →  0

    Deadband: if neither condition is met, demand stays at `prev`.

    Parameters
    ----------
    value     : current measured value
    threshold : upper setpoint (T_max or RH_max)
    d_step    : spacing between opening levels
    d_hyst    : width of the closing deadband below threshold
    prev      : demand level from the previous evaluation cycle
    """
    if   value > threshold + 3.0 * d_step:  return 3
    elif value > threshold + 2.0 * d_step:  return 2
    elif value > threshold + 1.0 * d_step:  return 1
    elif value < threshold - d_hyst:         return 0
    else:                                    return prev  # deadband — no change


# ---------------------------------------------------------------------------
# Conflict resolution  (model_design §5)
# ---------------------------------------------------------------------------

def resolve_demand(
    T: float,
    RH: float,
    sp: ScheduleSetpoints,
    cfg: ControlConfig,
    demand_T: int,
    demand_RH: int,
    state: ControllerState,
) -> int:
    """
    Combine temperature and humidity demands into a single ventilation demand.

    Rules (in priority order):
    1. T < T_min AND RH ≤ RH_critical  →  close all (temperature protection).
    2. T < T_min AND RH > RH_critical  →  force ≥1 window (disease risk override).
    3. No conflict                     →  max(demand_T, demand_RH).

    Returns V_demand ∈ {0, 1, 2, 3}.
    model_design §5.
    """
    # Temperature floor guard
    if T < sp.T_min:
        if RH > cfg.RH_critical:
            # Critical humidity overrides temperature floor (TSDS §5.2)
            v = max(1, demand_RH)
            state.conflicts_logged += 1
            return v
        return 0  # close all: protect temperature

    # Opposing demands (ventilation wanted for one, resisted for the other)
    if demand_T > 0 and demand_RH == 0:
        # T high, RH acceptable → open for temperature
        return demand_T
    if demand_RH > 0 and demand_T == 0:
        # RH high, T acceptable → open for humidity
        return demand_RH

    # Both non-zero (same direction) or both zero — no conflict
    v = max(demand_T, demand_RH)
    if demand_T > 0 and demand_RH == 0 or demand_T == 0 and demand_RH > 0:
        state.conflicts_logged += 1
    return v


# ---------------------------------------------------------------------------
# Window commanding with dwell guard  (model_design §6.2)
# ---------------------------------------------------------------------------

def _command_window(
    window: WindowCtrl,
    desired: WindowState,
    t: float,
    cfg: ControlConfig,
    state: ControllerState,
) -> None:
    """
    Issue an OPEN or CLOSE command to a window, subject to the dwell guard.

    A command is suppressed if:
    - The window is already in (or moving toward) the desired state.
    - The window is currently moving in the OPPOSITE direction (must settle first).
    - The appropriate dwell timer has not yet expired.

    The dwell guard uses the EXPECTED settle time when the window is moving,
    so that a command issued while the window is still travelling does not
    bypass the dwell by using the stale last_settled_t from the previous state.

    model_design §6.2.
    """
    # Already at or moving toward desired state — nothing to do
    if window.target == desired:
        return

    # Window is currently moving in the OPPOSITE direction.
    # Wait for it to physically settle before accepting a reversal.
    # The expected settle time is t_move_start + t_motor; the dwell starts
    # only after that, so we compute the projected reference time now.
    if window.is_moving():
        projected_settled = window.t_move_start + window.t_motor
        elapsed = t - projected_settled
    else:
        elapsed = t - window.last_settled_t

    # Dwell guard — measured from when the window (will have) settled
    if desired == WindowState.OPEN and elapsed < cfg.dwell_close:
        state.commands_blocked += 1
        return
    if desired == WindowState.CLOSED and elapsed < cfg.dwell_open:
        state.commands_blocked += 1
        return

    # Issue command
    window.target       = desired
    window.t_move_start = t
    window.state = (
        WindowState.MOVING_OPEN  if desired == WindowState.OPEN
        else WindowState.MOVING_CLOSE
    )


# ---------------------------------------------------------------------------
# Window state update — call every simulation step
# ---------------------------------------------------------------------------

def update_window_states(state: ControllerState, t: float) -> None:
    """
    Advance MOVING windows to their target state when the motor travel
    time has elapsed.  Call once per simulation step (before apply_control).
    """
    for w in state.windows:
        if w.is_moving():
            if (t - w.t_move_start) >= w.t_motor:
                w.state         = w.target
                w.last_settled_t = t


# ---------------------------------------------------------------------------
# Main control entry point
# ---------------------------------------------------------------------------

def apply_control(
    T_in: float,
    RH_in: float,
    t: float,
    hour: float,
    month: int,
    cfg: ControlConfig,
    state: ControllerState,
) -> None:
    """
    Evaluate climate conditions and issue window commands if needed.

    Call this function periodically (default every `cfg.ctrl_interval` seconds).
    The function is a no-op if called more frequently than the configured interval.

    Parameters
    ----------
    T_in  : indoor air temperature [°C]  (from T4 / T5 sensor poll)
    RH_in : indoor relative humidity [%] (from T4 / T5 sensor poll)
    t     : current elapsed simulation time [s]  (or Unix epoch in firmware)
    hour  : hour of day (0.0–24.0, local time)   (from T4 / RTC)
    month : calendar month (1–12)                (from T4 / RTC)
    cfg   : controller configuration (from T4 NVS)
    state : mutable controller state (owned by T6 task)

    model_design §4, §5, §7.
    """
    # Throttle to ctrl_interval
    if (t - state.t_last_ctrl) < cfg.ctrl_interval:
        return
    state.t_last_ctrl = t

    # ---- Resolve active setpoints for current schedule slot ----
    sp = resolve_setpoints(hour, month, cfg)

    # ---- Graduated demand for T and RH ----
    demand_T = _demand(
        T_in, sp.T_max, cfg.dT_step, cfg.dT_hyst, state.T_demand_prev
    )
    demand_RH = _demand(
        RH_in, sp.RH_max, cfg.dRH_step, cfg.dRH_hyst, state.RH_demand_prev
    )

    state.T_demand_prev  = demand_T
    state.RH_demand_prev = demand_RH

    # ---- Combined demand with conflict resolution ----
    V_demand = resolve_demand(T_in, RH_in, sp, cfg, demand_T, demand_RH, state)

    # ---- Window commands in priority order (M2, M1, M3)  model_design §7 ----
    # Priority order: index 1 (M2 north roof) → 0 (M1 south roof) → 2 (M3 side wall)
    priority: List[WindowCtrl] = [
        state.windows[1],  # M2 — opened first
        state.windows[0],  # M1 — opened second
        state.windows[2],  # M3 — opened last
    ]

    for rank, w in enumerate(priority):
        desired = (
            WindowState.OPEN if rank < V_demand else WindowState.CLOSED
        )
        _command_window(w, desired, t, cfg, state)


# ---------------------------------------------------------------------------
# Convenience helpers
# ---------------------------------------------------------------------------

def window_open_count(state: ControllerState) -> int:
    """Return the number of windows currently in OPEN state."""
    return sum(1 for w in state.windows if w.is_open())


def window_states_tuple(state: ControllerState) -> Tuple[bool, bool, bool]:
    """Return (M1_open, M2_open, M3_open) as booleans."""
    return tuple(w.is_open() for w in state.windows)  # type: ignore[return-value]


def active_ach(state: ControllerState, ach_roof: float = 8.0, ach_wall: float = 40.0) -> float:
    """
    Estimate total ventilation rate [h⁻¹] based on current window states.
    Includes background infiltration of 0.5 h⁻¹.
    """
    ACH_INF = 0.5
    total = ACH_INF
    w = state.windows
    if w[0].is_open(): total += ach_roof   # M1
    if w[1].is_open(): total += ach_roof   # M2
    if w[2].is_open(): total += ach_wall   # M3
    return total

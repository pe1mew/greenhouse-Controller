#!/usr/bin/env python3
"""
Greenhouse Controller Simulation — firmware v1.16.19
=====================================================
Faithful software model of the implemented firmware climate control logic.

Implements (in firmware task order):
  T5  sensor_poll      — sliding-window averaging of indoor T and RH
  T6  climate_control  — graduated ventilation step algorithm with hysteresis guard
  T3  safety_monitor   — wind override (requires windSpeed/windDirection CSV columns)
  T2  relay_controller — motor travel timing and post-open/close dwell enforcement
  T4  data_manager     — day/night setpoint selection via NOAA sunrise/sunset

Plant model:
  Steady-state algebraic greenhouse model.
  Indoor T and RH are computed as equilibrium values at each dt step given
  the current window configuration, outdoor conditions, solar heat gain,
  and crop transpiration.  Background infiltration (ACH_INF = 0.5 h⁻¹)
  prevents division by zero when all windows are closed.

Usage:
    python simulation.py <weather_csv> [settings_json]

Arguments:
    weather_csv   CSV with columns: dateTime, airTemperature, airHumidity
                  Optional additional columns: windSpeed (m/s), windDirection (°)
                  dateTime format: YYYY-MM-DD HH:MM:SS
    settings_json JSON settings file (default: settings.json next to this script).
                  If the file does not exist the firmware factory defaults are used.

Outputs (written to the same directory as weather_csv):
    results_<name>.csv   time-series of indoor/outdoor conditions and window states
    results_<name>.png   four-panel plot (requires matplotlib)
"""

import csv
import json
import math
import sys
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# ── Physical constants ──────────────────────────────────────────────────────
RHO_AIR = 1.2       # kg/m³   air density
CP_AIR  = 1005.0    # J/kg/°C specific heat of air
P_ATM   = 101325.0  # Pa      atmospheric pressure
ACH_INF = 0.5       # h⁻¹    background infiltration (greenhouse leakage, always-on)

# ── Firmware constants ──────────────────────────────────────────────────────
NUM_VENT_STEPS        = 3   # Step 1=M1, Step 2=M1+M2, Step 3=M1+M2+M3
VENT_STEP_NEUTRAL     = -1  # Sentinel: RH has no vote (in-range)
MOTOR_TRAVEL_MARGIN_S = 5   # Safety margin added to every relay pulse (app_types.h)
SP_AVG_DEPTH_MAX      = 360 # Maximum sliding-window depth (sensor_poll.cpp)

# ── Channel bitmasks (climate_control.h) ────────────────────────────────────
VENT_CH_M1 = 0b001
VENT_CH_M2 = 0b010
VENT_CH_M3 = 0b100
# Step → channel-mask table (climate_control.cpp VENT_STEP_TABLE)
VENT_STEP_TABLE = [
    0,                              # step 0 — all closed
    VENT_CH_M1,                     # step 1 — M1 only
    VENT_CH_M1 | VENT_CH_M2,       # step 2 — M1 + M2
    VENT_CH_M1 | VENT_CH_M2 | VENT_CH_M3,  # step 3 — all open
]

# ── Factory defaults (firmware v1.16.19, data_manager.cpp) ──────────────────
DEFAULT_SETTINGS: Dict[str, Any] = {
    "climate": {
        "t_max_day":  28, "t_max_ngt":  20,
        "rh_min_day": 50, "rh_max_day": 75,
        "rh_min_ngt": 55, "rh_max_ngt": 80,
        "hyst_t":      3, "hyst_rh":     5,
        "rh_ctrl_en":  1, "cr_priority": 0,
        "avg_win_t":   3, "avg_win_rh":  5,
    },
    "wind": {
        "wind_prot_en": 1, "v_max": 6,
        "dir_excl_low": 0, "dir_excl_high": 0,
    },
    "motor": {
        "travel_m1":  21,  "travel_m2":  21,  "travel_m3": 171,
        "dwell_open_m1": 120, "dwell_open_m2": 120, "dwell_open_m3": 120,
        "dwell_close_m1":  0, "dwell_close_m2":  0, "dwell_close_m3":  0,
    },
    "system": {
        "poll_interval": 60,
        "lat_deg": 52, "lat_frac": 0,
        "lon_deg":  5, "lon_frac": 0,
    },
    # The plant model lives in a separate JSON referenced via "plant_file"
    # (defaulting to plant_empty_greenhouse.json next to this script). This
    # keeps the controller config (climate / wind / motor / system) cleanly
    # separated from the physical greenhouse description.
    "plant_file": "plant_empty_greenhouse.json",
}


# ══════════════════════════════════════════════════════════════════════════════
# Settings helpers
# ══════════════════════════════════════════════════════════════════════════════

def _deep_merge(base: dict, override: dict) -> dict:
    """Recursively merge override into base; override wins on conflicts."""
    result = dict(base)
    for k, v in override.items():
        if k in result and isinstance(result[k], dict) and isinstance(v, dict):
            result[k] = _deep_merge(result[k], v)
        else:
            result[k] = v
    return result


def _load_plant(plant_path: Path) -> Dict[str, Any]:
    """Load a plant-model JSON file."""
    with open(plant_path) as f:
        plant = json.load(f)
    plant.pop("_comment", None)
    print(f"[settings] loaded plant model: {plant_path.name}")
    return plant


def load_settings(json_path: Optional[Path]) -> Dict[str, Any]:
    """
    Load JSON settings and deep-merge with firmware factory defaults.

    The plant section is stored in a separate file referenced by the
    `plant_file` field (path is relative to the settings JSON's directory,
    or falls back to the simulation.py directory). Inline `plant` sections
    in the settings JSON are still honoured for backward compatibility and
    take precedence over `plant_file` when both are present.
    """
    base_dir = (json_path.parent if (json_path is not None and json_path.exists())
                else Path(__file__).parent)
    if json_path is None or not json_path.exists():
        if json_path is not None:
            print(f"[settings] {json_path.name} not found - using firmware defaults")
        merged = _deep_merge(DEFAULT_SETTINGS, {})
    else:
        with open(json_path) as f:
            user = json.load(f)
        merged = _deep_merge(DEFAULT_SETTINGS, user)
        print(f"[settings] loaded {json_path.name}")

    # Resolve plant_file unless an inline plant section is already present.
    if "plant" not in merged:
        plant_file = merged.get("plant_file")
        if plant_file:
            plant_path = (base_dir / plant_file).resolve()
            if not plant_path.exists():
                # Fall back to the simulation.py directory if the settings
                # file lives elsewhere and the plant_file isn't co-located.
                fallback = (Path(__file__).parent / plant_file).resolve()
                if fallback.exists():
                    plant_path = fallback
            if not plant_path.exists():
                print(f"[settings] plant_file {plant_file!r} not found "
                      f"(looked in {base_dir} and {Path(__file__).parent}) "
                      f"- aborting")
                sys.exit(1)
            merged["plant"] = _load_plant(plant_path)
        else:
            print("[settings] no plant or plant_file specified - aborting")
            sys.exit(1)
    return merged


# ══════════════════════════════════════════════════════════════════════════════
# Weather data
# ══════════════════════════════════════════════════════════════════════════════

class WeatherRow:
    __slots__ = ("ts", "t_out", "rh_out", "wind_speed", "wind_dir")

    def __init__(self, ts: float, t_out: float, rh_out: float,
                 wind_speed: float = 0.0, wind_dir: float = 0.0):
        self.ts         = ts          # Unix timestamp (s)
        self.t_out      = t_out       # °C
        self.rh_out     = rh_out      # %
        self.wind_speed = wind_speed  # m/s
        self.wind_dir   = wind_dir    # °


def load_weather(csv_path: Path) -> List[WeatherRow]:
    """
    Parse weather CSV.  Two formats are accepted:

    Format A — absolute timestamps (production weather data):
      dateTime         — YYYY-MM-DD HH:MM:SS (UTC assumed)
      airTemperature   — °C
      airHumidity      — %
      windSpeed        — m/s  (optional)
      windDirection    — °    (optional)

    Format B — relative-time scenario files (input_*.csv):
      t_s              — seconds from start of scenario
      T_in_C           — °C   (outdoor temperature)
      RH_in_pct        — %    (outdoor relative humidity)
      windSpeed        — m/s  (optional)
      windDirection    — °    (optional)
      The series is anchored to SCENARIO_ANCHOR_DATE so the NOAA
      sunrise/sunset algorithm produces meaningful day/night periods.
    """
    # Anchor date for relative-time scenario files (midsummer, Netherlands)
    SCENARIO_ANCHOR = datetime(2025, 6, 15, 0, 0, 0, tzinfo=timezone.utc)

    rows: List[WeatherRow] = []
    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames or []
        is_relative = "t_s" in fieldnames

        for r in reader:
            if is_relative:
                ts = SCENARIO_ANCHOR.timestamp() + float(r["t_s"])
                t  = float(r["T_in_C"])
                rh = float(r["RH_in_pct"])
            else:
                dt = datetime.strptime(r["dateTime"].strip(), "%Y-%m-%d %H:%M:%S")
                ts = dt.replace(tzinfo=timezone.utc).timestamp()
                t  = float(r["airTemperature"])
                rh = float(r["airHumidity"])
            ws = float(r.get("windSpeed",    0) or 0)
            wd = float(r.get("windDirection", 0) or 0)
            rows.append(WeatherRow(ts, t, rh, ws, wd))

    rows.sort(key=lambda x: x.ts)
    fmt = "B (relative t_s)" if is_relative else "A (absolute dateTime)"
    print(f"[weather] loaded {len(rows)} rows from {csv_path.name}  "
          f"({datetime.fromtimestamp(rows[0].ts, timezone.utc).strftime('%Y-%m-%d %H:%M')} to "
          f"{datetime.fromtimestamp(rows[-1].ts, timezone.utc).strftime('%Y-%m-%d %H:%M')})  "
          f"[format {fmt}]")
    return rows


def interpolate_weather(rows: List[WeatherRow], ts: float) -> WeatherRow:
    """Linearly interpolate outdoor conditions at timestamp ts."""
    if ts <= rows[0].ts:
        return rows[0]
    if ts >= rows[-1].ts:
        return rows[-1]
    # Binary search for bracket
    lo, hi = 0, len(rows) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if rows[mid].ts <= ts:
            lo = mid
        else:
            hi = mid
    a, b = rows[lo], rows[hi]
    frac = (ts - a.ts) / (b.ts - a.ts)
    return WeatherRow(
        ts         = ts,
        t_out      = a.t_out      + frac * (b.t_out      - a.t_out),
        rh_out     = a.rh_out     + frac * (b.rh_out     - a.rh_out),
        wind_speed = a.wind_speed + frac * (b.wind_speed - a.wind_speed),
        wind_dir   = a.wind_dir   + frac * (b.wind_dir   - a.wind_dir),
    )


# ══════════════════════════════════════════════════════════════════════════════
# Thermodynamic helpers
# ══════════════════════════════════════════════════════════════════════════════

def p_sat(T: float) -> float:
    """Saturation vapour pressure [Pa] — Magnus formula."""
    return 610.78 * math.exp(17.27 * T / (T + 237.3))


def ah_sat(T: float) -> float:
    """Saturation absolute humidity [kg/m³]."""
    ps = p_sat(T)
    return 0.622 * ps / (P_ATM - ps) * RHO_AIR


def rh_from_ah(ah: float, T: float) -> float:
    """Absolute humidity → relative humidity [%], clamped [0, 100]."""
    sat = ah_sat(T)
    return 0.0 if sat <= 0 else min(100.0, ah / sat * 100.0)


def ah_from_rh(rh: float, T: float) -> float:
    """Relative humidity [%] + temperature → absolute humidity [kg/m³]."""
    return rh / 100.0 * ah_sat(T)


# ══════════════════════════════════════════════════════════════════════════════
# NOAA sunrise/sunset — direct port of firmware sunrise.cpp
# ══════════════════════════════════════════════════════════════════════════════

def _julian_day(unix_ts: float) -> float:
    """Julian Day Number for the UTC date of unix_ts (noon convention)."""
    unix_days = int(unix_ts) // 86400
    return unix_days + 2440588.0


def sunrise_calc(unix_ts: float, lat_deg: float, lon_deg: float
                 ) -> Tuple[int, int]:
    """
    NOAA solar position algorithm (port of firmware sunrise.cpp).
    Returns (rise_mins_utc, set_mins_utc) — minutes from UTC midnight.
    """
    JD = _julian_day(unix_ts)
    T  = (JD - 2451545.0) / 36525.0

    def wrap360(x: float) -> float:
        x = math.fmod(x, 360.0)
        return x + 360.0 if x < 0 else x

    L0    = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032))
    M_deg = 357.52911 + T * (35999.05029 - T * 0.0001537)
    M     = math.radians(M_deg)
    C     = ((1.914602 - T * (0.004817 + T * 0.000014)) * math.sin(M)
             + (0.019993 - T * 0.000101) * math.sin(2 * M)
             + 0.000289 * math.sin(3 * M))
    theta  = L0 + C
    omega  = 125.04 - 1934.136 * T
    lam    = theta - 0.00569 - 0.00478 * math.sin(math.radians(omega))
    eps0   = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0
    eps    = eps0 + 0.00256 * math.cos(math.radians(omega))
    decl   = math.degrees(math.asin(math.sin(math.radians(eps)) * math.sin(math.radians(lam))))
    e      = 0.016708634 - T * (0.000042037 + T * 0.0000001267)
    y      = math.tan(math.radians(eps / 2.0)) ** 2
    L0r    = math.radians(L0)
    E      = 4.0 * math.degrees(
        y       * math.sin(2 * L0r)
        - 2*e   * math.sin(M)
        + 4*e*y * math.sin(M) * math.cos(2 * L0r)
        - 0.5*y*y * math.sin(4 * L0r)
        - 1.25*e*e * math.sin(2 * M)
    )
    cos_ha = (math.cos(math.radians(90.833))
              / (math.cos(math.radians(lat_deg)) * math.cos(math.radians(decl)))
              - math.tan(math.radians(lat_deg)) * math.tan(math.radians(decl)))
    if cos_ha < -1.0:
        return 0, 1439          # polar day
    if cos_ha > 1.0:
        return 0, 0             # polar night
    ha_deg     = math.degrees(math.acos(cos_ha))
    solar_noon = 720.0 - 4.0 * lon_deg - E
    rise = int(solar_noon - 4.0 * ha_deg + 0.5)
    sset = int(solar_noon + 4.0 * ha_deg + 0.5)
    rise = max(0, min(1439, rise))
    sset = max(0, min(1439, sset))
    return rise, sset


def is_daytime(unix_ts: float, lat_deg: float, lon_deg: float) -> bool:
    """True if unix_ts falls between sunrise and sunset (UTC minutes)."""
    mins_utc = (int(unix_ts) % 86400) // 60
    rise, sset = sunrise_calc(unix_ts, lat_deg, lon_deg)
    return rise <= mins_utc <= sset


# ══════════════════════════════════════════════════════════════════════════════
# Solar heat gain model
# ══════════════════════════════════════════════════════════════════════════════

def q_solar(unix_ts: float, solar_peak_w: float,
            lat_deg: float, lon_deg: float) -> float:
    """
    Solar heat gain through glazing [W].
    Sine approximation between sunrise and sunset; peak at solar noon.
    """
    rise, sset = sunrise_calc(unix_ts, lat_deg, lon_deg)
    if rise >= sset:
        return 0.0
    mins_utc = (int(unix_ts) % 86400) / 60.0
    if not (rise <= mins_utc <= sset):
        return 0.0
    day_len = sset - rise
    angle = math.pi * (mins_utc - rise) / day_len
    return solar_peak_w * math.sin(angle) ** 2


# ══════════════════════════════════════════════════════════════════════════════
# Plant model — steady-state algebraic (from Iteration1 design)
# ══════════════════════════════════════════════════════════════════════════════

def ach_total(window_open: List[bool], ach_roof: float, ach_wall: float) -> float:
    """
    Total ventilation rate [s⁻¹].
    window_open = [M1_open, M2_open, M3_open]
    M1, M2 are roof vents (ach_roof each); M3 is wall vent (ach_wall).
    ACH_INF background infiltration is always included.
    """
    total = ACH_INF / 3600.0
    if window_open[0]: total += ach_roof / 3600.0   # M1
    if window_open[1]: total += ach_roof / 3600.0   # M2
    if window_open[2]: total += ach_wall / 3600.0   # M3
    return total


def plant_step(window_open: List[bool], t_out: float, rh_out: float,
               unix_ts: float, settings: Dict,
               T_in_prev: Optional[float] = None,
               AH_in_prev: Optional[float] = None,
               dt_s: Optional[float] = None) -> Tuple[float, float]:
    """
    Advance indoor T [°C] and AH [kg/m³] by dt_s using a first-order thermal
    and moisture lag toward the current ventilation equilibrium.

    Steady-state targets (dT/dt = 0, d(AH)/dt = 0):
      T_eq  = T_out + Q_solar / (ACH_per_s * V * rho * cp)
      AH_eq = AH_out + m_transp / (ACH_per_s * V)

    Time constants:
      tau_T  = c_eff_J_per_C / (ACH_per_s * V * rho * cp)
      tau_AH = 1 / ACH_per_s

    c_eff_mj_per_c is the air-coupled effective heat capacity of the
    greenhouse (air + soil surface + crop biomass + glazing), set in the
    plant section of the settings file. It controls how slowly indoor T
    follows ventilation changes — small for an empty greenhouse, larger
    when crops and active soil are present.

    When c_eff_mj_per_c <= 0 or no previous state is supplied (initial
    seed call), the function returns the instantaneous equilibrium —
    matching the original steady-state model.
    """
    p  = settings["plant"]
    s  = settings["system"]
    V  = p["volume_m3"]
    ACH = ach_total(window_open, p["ach_roof"], p["ach_wall"])  # s⁻¹
    Qs  = q_solar(unix_ts, p["solar_peak_w"], s["lat_deg"], s["lon_deg"])

    air_throughput = ACH * V * RHO_AIR * CP_AIR  # W/°C
    T_eq           = t_out + Qs / air_throughput
    AH_out         = ah_from_rh(rh_out, t_out)
    AH_eq          = max(0.0, AH_out + p["transpiration_kg_s"] / (ACH * V))

    c_eff_mj = p.get("c_eff_mj_per_c", 0.0)
    if (c_eff_mj <= 0.0 or T_in_prev is None or AH_in_prev is None
            or dt_s is None or dt_s <= 0.0):
        return T_eq, AH_eq

    # First-order Euler integration toward the equilibrium.
    c_eff_J = c_eff_mj * 1e6
    tau_T   = c_eff_J / air_throughput
    tau_AH  = 1.0 / ACH
    T_in    = T_in_prev  + (T_eq  - T_in_prev)  * (dt_s / tau_T)
    AH_in   = AH_in_prev + (AH_eq - AH_in_prev) * (dt_s / tau_AH)
    return T_in, max(0.0, AH_in)


# ══════════════════════════════════════════════════════════════════════════════
# T5 — Sliding-window averaging (sensor_poll.cpp)
# ══════════════════════════════════════════════════════════════════════════════

class SlidingAverage:
    """
    Arithmetic running-sum circular buffer for one channel.
    Window size = max(1, min(SP_AVG_DEPTH_MAX, win_min * 60 / poll_s)).
    """
    def __init__(self) -> None:
        self._buf: deque = deque()
        self._win: int   = 1
        self._sum: float = 0.0

    def set_window(self, win_min: int, poll_s: int) -> None:
        new_win = max(1, min(SP_AVG_DEPTH_MAX, win_min * 60 // max(1, poll_s)))
        if new_win != self._win:
            self._win = new_win
            self._buf.clear()
            self._sum = 0.0

    def push(self, value: float) -> float:
        """Add sample; return current average."""
        self._buf.append(value)
        self._sum += value
        while len(self._buf) > self._win:
            self._sum -= self._buf.popleft()
        return self._sum / len(self._buf)

    @property
    def value(self) -> float:
        return self._sum / len(self._buf) if self._buf else 0.0


# ══════════════════════════════════════════════════════════════════════════════
# T6 — Climate Control step functions (exact port of climate_control.cpp)
# ══════════════════════════════════════════════════════════════════════════════

def _step_from_deviation(deviation: int, hyst: int, current_step: int) -> int:
    """
    Core graduation algorithm — direct port of climate_control.cpp
    step_from_deviation().

    step_width = max(1, hyst // NUM_VENT_STEPS)   (integer floor)
    raw_step   = ceil(deviation / step_width)       (only for deviation > 0)
    clamped    = clamp(raw_step, 0, NUM_VENT_STEPS)

    Close-hysteresis guard: once step > 0, do not step down to 0 until
    deviation <= -hyst.  Hold at step 1 while in the deadband.
    """
    step_width = max(1, hyst // NUM_VENT_STEPS)

    if deviation <= 0:
        raw_step = 0
    else:
        # Integer ceiling division: (a + b - 1) // b
        raw_step = (deviation + step_width - 1) // step_width

    raw_step = max(0, min(NUM_VENT_STEPS, raw_step))

    # Close-hysteresis guard
    if current_step > 0 and raw_step == 0:
        if deviation > -hyst:
            return 1   # hold at minimum open; do not close yet

    return raw_step


def vent_step_required_t(t_avg: int, t_max: int, hyst_t: int,
                         current_step: int) -> int:
    """T6 temperature step — port of vent_step_required_t()."""
    return _step_from_deviation(t_avg - t_max, hyst_t, current_step)


def vent_step_required_rh(rh_avg: int, rh_max: int, rh_min: int,
                          hyst_rh: int, rh_ctrl_en: bool,
                          current_step: int) -> int:
    """
    T6 RH step — port of vent_step_required_rh().

    Returns VENT_STEP_NEUTRAL (-1) when RH is in range.
    Returns 0 (full close) when RH < rh_min.
    Returns 1-3 (graduated open) when RH > rh_max.
    """
    if not rh_ctrl_en:
        return VENT_STEP_NEUTRAL
    if rh_avg > rh_max:
        return _step_from_deviation(rh_avg - rh_max, hyst_rh, current_step)
    if rh_avg < rh_min:
        return 0   # full close — no graduation (Gap G design decision)
    return VENT_STEP_NEUTRAL


def vent_resolve_conflict(step_t: int, step_rh: int, cr_priority: int) -> int:
    """T6 conflict resolution — port of vent_resolve_conflict()."""
    if step_rh == VENT_STEP_NEUTRAL:
        return step_t
    if step_t > 0 and step_rh > 0:
        return max(step_t, step_rh)
    if step_t == step_rh:
        return step_t
    # Genuine conflict (one open, one close)
    if cr_priority == 1:   # CR_RH_FIRST
        return step_rh
    if cr_priority == 2:   # CR_DEVIATION — more ventilation wins
        return max(step_t, step_rh)
    return step_t           # CR_TEMP_FIRST (default, cr_priority == 0)


# ══════════════════════════════════════════════════════════════════════════════
# T3 — Wind override (safety_monitor.cpp)
# ══════════════════════════════════════════════════════════════════════════════

def _dir_in_exclusion_zone(dir_deg: float, excl_low: int,
                            excl_high: int) -> bool:
    """Port of safety_monitor.cpp dir_in_exclusion_zone()."""
    if excl_low < 0 or excl_high < 0:
        return False
    if excl_low == excl_high:   # zero-width = disabled
        return False
    lo, hi = excl_low, excl_high
    d = int(dir_deg) % 360
    if lo < hi:
        return lo <= d <= hi
    return d >= lo or d <= hi   # zone wraps through 0°


def eval_wind_override(wind_speed: float, wind_dir: float,
                       wind_override_active: bool,
                       settings: Dict) -> Tuple[bool, str]:
    """
    T3 wind override state machine.
    Returns (new_active, reason_str).
    reason_str is non-empty only on transitions.
    """
    w = settings["wind"]
    if not w["wind_prot_en"]:
        if wind_override_active:
            return False, "wind_prot disabled"
        return False, ""

    speed_unsafe = (w["v_max"] > 0 and
                    wind_speed >= w["v_max"])
    dir_unsafe   = _dir_in_exclusion_zone(wind_dir,
                                          w["dir_excl_low"],
                                          w["dir_excl_high"])
    is_unsafe = speed_unsafe or dir_unsafe

    if is_unsafe and not wind_override_active:
        reason = f"speed={wind_speed:.1f} m/s" if speed_unsafe else f"dir={wind_dir:.0f}°"
        return True, f"WIND_OVERRIDE SET ({reason})"
    if not is_unsafe and wind_override_active:
        return False, f"WIND_OVERRIDE CLEARED (speed={wind_speed:.1f} m/s dir={wind_dir:.0f}°)"
    return wind_override_active, ""


# ══════════════════════════════════════════════════════════════════════════════
# T2 — Motor state machine (relay_controller.cpp)
# ══════════════════════════════════════════════════════════════════════════════

class MotorState:
    """Per-channel motor state — port of relay_controller.cpp channel_t."""

    CLOSED       = "CLOSED"
    MOVING_OPEN  = "MOVING_OPEN"
    OPEN         = "OPEN"
    MOVING_CLOSE = "MOVING_CLOSE"
    # Internal-only transient states matching firmware ch_state_t. These model
    # the 2-second safety gap inserted between energising opposing relays
    # (relay_controller.cpp RELAY_GAP_MS). They are mapped to MOVING_OPEN /
    # MOVING_CLOSE in the externally-reported window_state, just as the
    # firmware's t2_get_window_states() does.
    GAP_TO_OPEN  = "GAP_TO_OPEN"
    GAP_TO_CLOSE = "GAP_TO_CLOSE"

    RELAY_GAP_MS = 2000   # firmware relay_controller.cpp RELAY_GAP_MS

    def __init__(self, name: str, travel_s: int, dwell_open_s: int,
                 dwell_close_s: int) -> None:
        self.name          = name
        self.travel_ms     = (travel_s + MOTOR_TRAVEL_MARGIN_S) * 1000
        self.dwell_open_ms = dwell_open_s  * 1000
        self.dwell_close_ms= dwell_close_s * 1000
        self.state         = self.CLOSED
        self.relay_deadline_ms: int = 0   # time when MOVING → OPEN/CLOSED
        self.dwell_deadline_ms: int = 0   # time before next opposite command
        self.gap_deadline_ms:   int = 0   # time when GAP_TO_* → MOVING_*

    @property
    def is_open(self) -> bool:
        # Physical end-switch semantic, matching firmware T2: only a window
        # whose travel timer has expired is "open" for ACH purposes. Travelling
        # and gap states contribute no ACH (binary approximation; the 4-state
        # window_state below preserves the full state machine for reporting).
        return self.state == self.OPEN

    @property
    def is_moving(self) -> bool:
        # Includes the safety-gap states so external callers don't need to
        # know about them (the gap is internal to the relay-controller FSM).
        return self.state in (self.MOVING_OPEN, self.MOVING_CLOSE,
                              self.GAP_TO_OPEN, self.GAP_TO_CLOSE)

    @property
    def window_state(self) -> str:
        # Mirrors firmware t2_get_window_states(): GAP_TO_* collapse onto
        # MOVING_* so the reported set is exactly {CLOSED, MOVING_OPEN, OPEN,
        # MOVING_CLOSE}.
        if self.state in (self.GAP_TO_OPEN,):
            return self.MOVING_OPEN
        if self.state in (self.GAP_TO_CLOSE,):
            return self.MOVING_CLOSE
        return self.state

    def tick(self, now_ms: int) -> None:
        """Advance the channel FSM — port of relay_controller.cpp ch_update()."""
        if self.state == self.MOVING_OPEN and now_ms >= self.relay_deadline_ms:
            self.state = self.OPEN
            self.dwell_deadline_ms = now_ms + self.dwell_open_ms
        elif self.state == self.MOVING_CLOSE and now_ms >= self.relay_deadline_ms:
            self.state = self.CLOSED
            self.dwell_deadline_ms = now_ms + self.dwell_close_ms
        elif self.state == self.GAP_TO_OPEN and now_ms >= self.gap_deadline_ms:
            self.state = self.MOVING_OPEN
            self.relay_deadline_ms = now_ms + self.travel_ms
        elif self.state == self.GAP_TO_CLOSE and now_ms >= self.gap_deadline_ms:
            self.state = self.MOVING_CLOSE
            self.relay_deadline_ms = now_ms + self.travel_ms

    def cmd_open(self, now_ms: int, bypass_dwell: bool = False) -> None:
        """Issue OPEN command — port of relay_controller.cpp ch_start_open().

        bypass_dwell mirrors firmware SRC_T3 (safety) commands: they skip the
        post-close dwell timer but still observe the relay safety gap.
        """
        s = self.state
        if s in (self.OPEN, self.MOVING_OPEN, self.GAP_TO_OPEN):
            return  # at target or moving there
        if s == self.MOVING_CLOSE:
            self.state = self.GAP_TO_OPEN
            self.gap_deadline_ms = now_ms + self.RELAY_GAP_MS
            return
        if s == self.GAP_TO_CLOSE:
            self.state = self.GAP_TO_OPEN  # pivot during gap; reuse deadline
            return
        if s == self.CLOSED and not bypass_dwell and now_ms < self.dwell_deadline_ms:
            return  # post-close dwell not yet expired
        self.state             = self.MOVING_OPEN
        self.relay_deadline_ms = now_ms + self.travel_ms

    def cmd_close(self, now_ms: int, bypass_dwell: bool = False) -> None:
        """Issue CLOSE command — port of relay_controller.cpp ch_start_close()."""
        s = self.state
        if s in (self.CLOSED, self.MOVING_CLOSE, self.GAP_TO_CLOSE):
            return
        if s == self.MOVING_OPEN:
            self.state = self.GAP_TO_CLOSE
            self.gap_deadline_ms = now_ms + self.RELAY_GAP_MS
            return
        if s == self.GAP_TO_OPEN:
            self.state = self.GAP_TO_CLOSE  # pivot during gap; reuse deadline
            return
        if s == self.OPEN and not bypass_dwell and now_ms < self.dwell_deadline_ms:
            return  # post-open dwell not yet expired
        self.state             = self.MOVING_CLOSE
        self.relay_deadline_ms = now_ms + self.travel_ms

    def cmd_close_all(self, now_ms: int) -> None:
        """Emergency/wind close — firmware SRC_T3: bypasses dwell, keeps the
        2 s relay safety gap on reversal (handled by cmd_close)."""
        self.cmd_close(now_ms, bypass_dwell=True)


def make_motors(settings: Dict) -> List[MotorState]:
    """Construct the three motor objects from settings."""
    m = settings["motor"]
    return [
        MotorState("M1", m["travel_m1"], m["dwell_open_m1"], m["dwell_close_m1"]),
        MotorState("M2", m["travel_m2"], m["dwell_open_m2"], m["dwell_close_m2"]),
        MotorState("M3", m["travel_m3"], m["dwell_open_m3"], m["dwell_close_m3"]),
    ]


def reconcile_to_step(step: int, motors: List[MotorState], now_ms: int) -> None:
    """
    Drive each motor toward the channel mask for `step` — port of T6's
    reconcile_to_step() in climate_control.cpp. Level-triggered: called every
    poll cycle so commands deferred by T2's post-open/close dwell are retried
    until they take effect.

    Idempotency is provided by MotorState.cmd_open() / cmd_close(): commands
    targeting the current direction are no-ops, opposite-direction commands
    during travel trigger the 2 s reversal gap.
    """
    desired_mask = VENT_STEP_TABLE[step]
    # CLOSE first (narrowing before widening is safer)
    for i, m in enumerate(motors):
        want_open = bool((desired_mask >> i) & 1)
        s = m.window_state
        currently_open_or_opening = s in (MotorState.OPEN, MotorState.MOVING_OPEN)
        if not want_open and currently_open_or_opening:
            m.cmd_close(now_ms)
    for i, m in enumerate(motors):
        want_open = bool((desired_mask >> i) & 1)
        s = m.window_state
        currently_closed_or_closing = s in (MotorState.CLOSED, MotorState.MOVING_CLOSE)
        if want_open and currently_closed_or_closing:
            m.cmd_open(now_ms)


def force_close_all_motors(motors: List[MotorState], now_ms: int) -> None:
    """Safety path (wind override / motor alarm) — bypass dwell on all motors."""
    for m in motors:
        m.cmd_close_all(now_ms)


# ══════════════════════════════════════════════════════════════════════════════
# Main simulation loop
# ══════════════════════════════════════════════════════════════════════════════

class SimRecord:
    """One recorded time-step for CSV / plot output."""
    __slots__ = (
        "unix_ts", "elapsed_s",
        "T_in", "RH_in", "T_out", "RH_out",
        "t_avg", "rh_avg",
        "m1_state", "m2_state", "m3_state",
        "step_t", "step_rh", "step_resolved",
        "is_day", "wind_override",
    )

    def __init__(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)


def run_simulation(weather: List[WeatherRow], settings: Dict,
                   dt: float = 10.0) -> List[SimRecord]:
    """
    Run the full simulation over the weather dataset.

    Parameters
    ----------
    weather   : Loaded and sorted weather rows.
    settings  : Merged settings dict.
    dt        : Integration step [s] (default 10 s).

    Returns
    -------
    List of SimRecord sampled every ~60 s.
    """
    s      = settings["system"]
    c      = settings["climate"]
    poll_s = max(1, s["poll_interval"])
    lat    = s["lat_deg"] + s["lat_frac"] / 1000.0
    lon    = s["lon_deg"] + s["lon_frac"] / 1000.0

    t_start = weather[0].ts
    t_end   = weather[-1].ts
    duration = t_end - t_start

    print(f"\n[sim] duration={duration/3600:.1f} h  dt={dt:.0f} s  poll={poll_s} s")

    # ── Initialise state ────────────────────────────────────────────────────
    motors = make_motors(settings)

    avg_t  = SlidingAverage()
    avg_rh = SlidingAverage()

    # Seed indoor conditions with the first outdoor point
    w0       = weather[0]
    T_in, AH_in = plant_step([False, False, False], w0.t_out, w0.rh_out,
                              w0.ts, settings)

    current_step_t  = 0
    current_step_rh = 0   # VENT_STEP_NEUTRAL treated as 0 for prev-resolved calc
    wind_override   = False

    # For accurate RH step hysteresis we track step_rh separately
    # (can be VENT_STEP_NEUTRAL)
    last_step_rh    = VENT_STEP_NEUTRAL

    records: List[SimRecord] = []
    record_every = max(1, int(60.0 / dt))

    n_steps = int(duration / dt)
    poll_countdown = 0   # count-down until next poll

    print(f"[sim] {n_steps:,} steps...", flush=True)

    for step in range(n_steps):
        now_ts = t_start + step * dt
        now_ms = int(step * dt * 1000)

        # ── Advance motor state machines (T2) ──────────────────────────────
        for m in motors:
            m.tick(now_ms)

        # ── Current window physical open state ─────────────────────────────
        window_open = [m.is_open for m in motors]

        # ── Plant model (first-order thermal/moisture lag) ──────────────────
        wx       = interpolate_weather(weather, now_ts)
        T_in, AH_in = plant_step(window_open, wx.t_out, wx.rh_out,
                                  now_ts, settings,
                                  T_in_prev=T_in, AH_in_prev=AH_in,
                                  dt_s=dt)
        RH_in   = rh_from_ah(AH_in, T_in)

        # ── T5 poll cycle ───────────────────────────────────────────────────
        if poll_countdown <= 0:
            poll_countdown = int(poll_s / dt)

            # Update averaging window sizes
            avg_t.set_window(c["avg_win_t"],  poll_s)
            avg_rh.set_window(c["avg_win_rh"], poll_s)

            # Push integer sensor readings (firmware uses int16 / uint8)
            t_avg_int  = int(round(avg_t.push(T_in)))
            rh_avg_int = max(0, min(100, int(round(avg_rh.push(RH_in)))))

            # ── T3 wind override ────────────────────────────────────────────
            wind_override, reason = eval_wind_override(
                wx.wind_speed, wx.wind_dir, wind_override, settings)
            if reason:
                print(f"  [T3] t={step*dt/3600:.2f}h  {reason}")
                if wind_override:
                    force_close_all_motors(motors, now_ms)
                    current_step_t  = 0
                    current_step_rh = 0
                    last_step_rh    = VENT_STEP_NEUTRAL

            # ── T6 climate control (only when not inhibited) ────────────────
            if not wind_override:
                day = is_daytime(now_ts, lat, lon)
                t_max  = c["t_max_day"]  if day else c["t_max_ngt"]
                rh_max = c["rh_max_day"] if day else c["rh_max_ngt"]
                rh_min = c["rh_min_day"] if day else c["rh_min_ngt"]

                step_t  = vent_step_required_t(
                    t_avg_int, t_max, max(1, c["hyst_t"]), current_step_t)
                step_rh = vent_step_required_rh(
                    rh_avg_int, rh_max, rh_min,
                    max(1, c["hyst_rh"]), bool(c["rh_ctrl_en"]),
                    last_step_rh if last_step_rh != VENT_STEP_NEUTRAL
                    else 0)

                resolved = vent_resolve_conflict(step_t, step_rh, c["cr_priority"])

                # Level-triggered: reconcile every poll cycle so dwell-deferred
                # commands are retried until they land. Mirrors the firmware
                # T6 behaviour after the same fix.
                reconcile_to_step(resolved, motors, now_ms)

                current_step_t  = step_t
                last_step_rh    = step_rh
                # For prev-resolved tracking, treat NEUTRAL as 0
                current_step_rh = step_rh if step_rh != VENT_STEP_NEUTRAL else 0

        else:
            poll_countdown -= 1
            # Reuse last computed averages for recording
            t_avg_int  = int(round(avg_t.value))
            rh_avg_int = max(0, min(100, int(round(avg_rh.value))))
            step_t     = current_step_t
            step_rh    = last_step_rh
            resolved   = vent_resolve_conflict(
                current_step_t,
                last_step_rh if last_step_rh != VENT_STEP_NEUTRAL else 0,
                c["cr_priority"])
            day = is_daytime(now_ts, lat, lon)

        # ── Record (every ~60 s) ────────────────────────────────────────────
        if step % record_every == 0:
            records.append(SimRecord(
                unix_ts       = now_ts,
                elapsed_s     = step * dt,
                T_in          = T_in,
                RH_in         = RH_in,
                T_out         = wx.t_out,
                RH_out        = wx.rh_out,
                t_avg         = t_avg_int,
                rh_avg        = rh_avg_int,
                m1_state      = motors[0].window_state,
                m2_state      = motors[1].window_state,
                m3_state      = motors[2].window_state,
                step_t        = step_t,
                step_rh       = step_rh,
                step_resolved = resolved,
                is_day        = int(day),
                wind_override = int(wind_override),
            ))

    print(f"[sim] done - {len(records)} records")
    _print_metrics(records, settings)
    return records


def _print_metrics(records: List[SimRecord], settings: Dict) -> None:
    if not records:
        return
    c    = settings["climate"]
    T_vals  = [r.T_in  for r in records]
    RH_vals = [r.RH_in for r in records]
    n        = len(records)

    # Time in temperature band (within ±2°C of active t_max setpoint)
    T_sp_vals = [
        c["t_max_day"] if r.is_day else c["t_max_ngt"]
        for r in records
    ]
    T_in_band = sum(1 for T, sp in zip(T_vals, T_sp_vals)
                    if T < sp + 2.0) / n * 100

    # Actuations (transitions in window state — count any state change)
    acts = 0
    prev = (records[0].m1_state, records[0].m2_state, records[0].m3_state)
    for r in records[1:]:
        cur = (r.m1_state, r.m2_state, r.m3_state)
        acts += sum(1 for a, b in zip(cur, prev) if a != b)
        prev = cur

    print(f"\n[metrics]")
    print(f"  Indoor T  : {min(T_vals):.1f} - {max(T_vals):.1f} C")
    print(f"  Indoor RH : {min(RH_vals):.1f} - {max(RH_vals):.1f} %")
    print(f"  Time T < t_max+2 C  : {T_in_band:.1f}%")
    print(f"  Window actuations   : {acts}")
    wind_pct = sum(r.wind_override for r in records) / n * 100
    if wind_pct > 0:
        print(f"  Time wind override  : {wind_pct:.1f}%")


# ══════════════════════════════════════════════════════════════════════════════
# CSV export
# ══════════════════════════════════════════════════════════════════════════════

def save_csv(records: List[SimRecord], path: Path) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "datetime", "elapsed_s",
            "T_in_C", "RH_in_pct", "T_out_C", "RH_out_pct",
            "t_avg_C", "rh_avg_pct",
            "M1_state", "M2_state", "M3_state",
            "step_t", "step_rh", "step_resolved",
            "is_daytime", "wind_override",
        ])
        for r in records:
            dt_str = datetime.fromtimestamp(r.unix_ts, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
            w.writerow([
                dt_str, f"{r.elapsed_s:.0f}",
                f"{r.T_in:.2f}", f"{r.RH_in:.1f}",
                f"{r.T_out:.2f}", f"{r.RH_out:.1f}",
                r.t_avg, r.rh_avg,
                r.m1_state, r.m2_state, r.m3_state,
                r.step_t, r.step_rh, r.step_resolved,
                r.is_day, r.wind_override,
            ])
    print(f"[output] saved {path}")


# ══════════════════════════════════════════════════════════════════════════════
# Plot
# ══════════════════════════════════════════════════════════════════════════════

def save_plot(records: List[SimRecord], settings: Dict, path: Path) -> None:
    try:
        import matplotlib.pyplot as plt
        import matplotlib.gridspec as gridspec
        import matplotlib.dates as mdates
    except ImportError:
        print("[output] matplotlib not available - skipping plot")
        return

    c  = settings["climate"]
    dts = [datetime.fromtimestamp(r.unix_ts, timezone.utc) for r in records]

    T_in   = [r.T_in   for r in records]
    T_out  = [r.T_out  for r in records]
    RH_in  = [r.RH_in  for r in records]
    RH_out = [r.RH_out for r in records]

    # Day/night band background (shading for nighttime)
    is_day = [r.is_day for r in records]

    fig = plt.figure(figsize=(16, 12))
    fig.suptitle(f"Greenhouse Simulation — {path.stem}", fontsize=13, fontweight="bold")
    gs  = gridspec.GridSpec(4, 1, figure=fig, hspace=0.38)

    def _shade_night(ax):
        """Shade nighttime periods."""
        in_night = False
        start = None
        for i, (dt, d) in enumerate(zip(dts, is_day)):
            if not d and not in_night:
                start = dt
                in_night = True
            elif d and in_night:
                ax.axvspan(start, dt, color="#e8e8f0", alpha=0.5, zorder=0)
                in_night = False
        if in_night and start:
            ax.axvspan(start, dts[-1], color="#e8e8f0", alpha=0.5, zorder=0)

    fmt = mdates.AutoDateFormatter(mdates.AutoDateLocator())

    # ── Panel 1: Temperature ────────────────────────────────────────────────
    ax1 = fig.add_subplot(gs[0])
    _shade_night(ax1)
    ax1.plot(dts, T_in,  "r-",  lw=1.8, label="Indoor T (plant model)")
    ax1.plot(dts, T_out, "r--", lw=1.0, alpha=0.5, label="Outdoor T")
    ax1.axhline(c["t_max_day"], color="#c0392b", ls=":", lw=1.2,
                label=f"t_max_day = {c['t_max_day']} °C")
    ax1.axhline(c["t_max_ngt"], color="#e67e22", ls=":", lw=1.2,
                label=f"t_max_ngt = {c['t_max_ngt']} °C")
    ax1.set_ylabel("Temperature [°C]")
    ax1.legend(fontsize=7, loc="upper right", ncol=2)
    ax1.grid(True, alpha=0.25)
    ax1.xaxis.set_major_formatter(fmt)

    # ── Panel 2: Humidity ───────────────────────────────────────────────────
    ax2 = fig.add_subplot(gs[1], sharex=ax1)
    _shade_night(ax2)
    ax2.plot(dts, RH_in,  "b-",  lw=1.8, label="Indoor RH")
    ax2.plot(dts, RH_out, "b--", lw=1.0, alpha=0.5, label="Outdoor RH")
    ax2.axhline(c["rh_max_day"], color="#2980b9", ls=":", lw=1.2,
                label=f"rh_max_day = {c['rh_max_day']} %")
    ax2.axhline(c["rh_min_day"], color="#27ae60", ls=":", lw=1.2,
                label=f"rh_min_day = {c['rh_min_day']} %")
    ax2.axhline(c["rh_max_ngt"], color="#8e44ad", ls=":", lw=1.0,
                label=f"rh_max_ngt = {c['rh_max_ngt']} %")
    ax2.set_ylabel("Relative Humidity [%]")
    ax2.set_ylim(0, 105)
    ax2.legend(fontsize=7, loc="upper right", ncol=3)
    ax2.grid(True, alpha=0.25)

    # ── Panel 3: Window states (4-state, mirrors firmware window_state_t) ───
    # Per-window y-band: CLOSED=0, MOVING_OPEN=0.33, OPEN=1, MOVING_CLOSE=0.67.
    # Reads as a "physical openness" trace: closed→opening→open→closing→closed.
    ax3 = fig.add_subplot(gs[2], sharex=ax1)
    _shade_night(ax3)
    names  = ["M1 (roof vent)",  "M2 (roof vent)",  "M3 (wall vent)"]
    colors = ["#e67e22", "#27ae60", "#2980b9"]
    state_y = {
        "CLOSED":       0.00,
        "MOVING_OPEN":  0.33,
        "MOVING_CLOSE": 0.67,
        "OPEN":         1.00,
    }
    track_h  = 1.3   # vertical span per motor track (1.0 for states + 0.3 gap)
    state_cols = [(r.m1_state, r.m2_state, r.m3_state) for r in records]
    for i, (name, color) in enumerate(zip(names, colors)):
        vals = [state_y.get(c[i], 0.0) + i * track_h for c in state_cols]
        ax3.step(dts, vals, color=color, lw=1.8, where="post", label=name)
    ax3.set_ylabel("Window state\nC ↑ ↓ O")
    yticks, ylabels = [], []
    for i in range(3):
        for s, y in (("C", 0.00), ("↑", 0.33), ("↓", 0.67), ("O", 1.00)):
            yticks.append(y + i * track_h)
            ylabels.append(s)
    ax3.set_yticks(yticks)
    ax3.set_yticklabels(ylabels, fontsize=7)
    ax3.set_ylim(-0.1, 2 * track_h + 1.1)
    ax3.legend(fontsize=7, loc="upper right")
    ax3.grid(True, alpha=0.25, axis="x")

    # ── Panel 4: Steps ──────────────────────────────────────────────────────
    ax4 = fig.add_subplot(gs[3], sharex=ax1)
    _shade_night(ax4)
    step_t    = [r.step_t        for r in records]
    step_res  = [r.step_resolved for r in records]
    wind_ov   = [r.wind_override for r in records]
    ax4.step(dts, step_t,   "#c0392b", lw=1.2, where="post", alpha=0.7,
             label="step_t (temp)")
    ax4.step(dts, step_res, "#2c3e50", lw=1.8, where="post",
             label="step_resolved")
    if any(wind_ov):
        ax4.fill_between(dts, 0, [3 * v for v in wind_ov],
                         color="orange", alpha=0.25, step="post",
                         label="wind override")
    ax4.set_ylabel("Vent step (0–3)")
    ax4.set_ylim(-0.2, 3.3)
    ax4.set_yticks([0, 1, 2, 3])
    ax4.set_xlabel("Time (UTC)")
    ax4.legend(fontsize=7, loc="upper right")
    ax4.grid(True, alpha=0.25)
    ax4.xaxis.set_major_formatter(fmt)

    plt.xticks(rotation=20, ha="right")
    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"[output] saved {path}")


# ══════════════════════════════════════════════════════════════════════════════
# Entry point
# ══════════════════════════════════════════════════════════════════════════════

def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    weather_path  = Path(sys.argv[1])
    settings_path = Path(sys.argv[2]) if len(sys.argv) >= 3 else \
                    Path(__file__).parent / "settings.json"

    if not weather_path.exists():
        print(f"Error: weather file not found: {weather_path}")
        sys.exit(1)

    settings = load_settings(settings_path)
    weather  = load_weather(weather_path)
    records  = run_simulation(weather, settings)

    out_dir  = weather_path.parent
    stem     = weather_path.stem
    save_csv(records,  out_dir / f"results_{stem}.csv")
    save_plot(records, settings, out_dir / f"results_{stem}.png")


if __name__ == "__main__":
    main()

"""
firmware.py -- the firmware around the control law, as the simulator needs it.

Each piece mirrors one firmware function, closely enough that the simulated
controller sees what the real one saw and does what it did:

  SlidingMean -- T5's avg_push()/avg_get() (sensor_poll.cpp): float32, a
      running sum that is never recomputed, and lroundf() on the way out.
      With six 0.1 degC samples the mean lands exactly on x.5 about once
      every half hour, and there the firmware's decision is whatever its
      float32 history since boot says -- so the arithmetic is emulated, not
      approximated. (vent_step_replay.py uses float64 and Python's
      half-to-even round(); fine for its purpose, not for this one.)
  SensorLayer -- T5's T, RH and wind averages over avg_win_t, avg_win_rh and
      avg_win_wind, each window in samples (minutes * 60 / poll_interval);
      the wind direction as a mean of unit vectors (dir_avg_*()). A context
      whose window changes starts empty, as T5 does.
  SafetyMonitor -- T3's wind safety (safety_monitor.cpp): set at the average
      speed >= v_max, clear below v_max - wind_hyst (2.3.0+), the direction
      exclusion arc, and a safe-fail on a wind sensor fault. It closes every
      window (SRC_T3, past the dwell timers) and suspends T6.
  is_daytime() -- T4's day and night: sunrise.cpp itself, compiled from
      firmware/src (firmware_ffi.cpp), at the site's lat/lon, in UTC.
  Actuator -- T2's per-channel state machine (relay_controller.cpp):
      full-travel strokes of travel + 5 s, the 2 s reversal gap, dwell timers
      that defer SRC_T6 only, and -- from 2.3.1 (gh#48) -- SRC_T6 reversals
      deferred while a stroke is in progress. Also tracks the physical
      position, which the binary law never reads but a linear law will.
  Controller -- T6's cycle (climate_control.cpp): an inhibit resets the
      law, setpoints are resolved for day or night, every narrowing command
      goes out before any widening one, and a MODE row is written whenever
      the resolved step changes.

Not modelled here: motor alarms and Q1 overflow. Sensor faults are taken
from their logged ALARM rows: a T/RH fault inhibits T6, a wind fault makes
T3 safe-fail and stops T5's wind averages.

Settings and firmware versions
------------------------------
Settings carries every controller setting; settings.py maps the controller's
keys onto it, says where each one acts, and clamps them as T4 does. What 5C88
ran is settings.schedule_5c88(): its base with the evidence for each value,
then its own SETPT audit rows. profile_5c88() holds the firmware behaviour
that changed between the versions it ran.
"""

from __future__ import annotations

import calendar
import ctypes
import math
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

import numpy as np

import settings as settings_mod
from ventmodel import (BUILD_DIR, VENT_ACT_CLOSE, VENT_ACT_OPEN, VENT_ACT_TARGET,
                       VENT_CAP_DIGITAL, VENT_RES_NONE, VENT_WIN_CLOSED,
                       VENT_WIN_MOVING_CLOSE, VENT_WIN_MOVING_OPEN, VENT_WIN_OPEN,
                       VENT_WIN_UNKNOWN, VentIn, build_dll)

HERE = Path(__file__).resolve().parent
FW_SRC = HERE.parent.parent / "firmware" / "src"
F32 = np.float32

# --------------------------------------------------------------------------
# Settings and firmware behaviour, per date
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Settings:
    """Every controller setting, in the simulator's units. Whole degC / whole %.

    The defaults are cfg_defaults.h's; `closed_loop.py settings` checks that
    they still are. Build one from the controller's keys with
    settings.to_sim(), which also says where each field acts.
    """
    # T6's caller hands these to the law, resolved for day or night
    t_max_day:   int = 28
    t_max_ngt:   int = 20
    rh_max_day:  int = 75
    rh_min_day:  int = 50
    rh_max_ngt:  int = 80
    rh_min_ngt:  int = 55
    hyst_t:      int = 5
    hyst_rh:     int = 12
    rh_ctrl_en:  bool = True
    cr_priority: int = 0
    # T5
    avg_win_t:   int = 6
    avg_win_rh:  int = 10
    poll_s:      int = 30
    # T2
    travel_s:      tuple = (21, 21, 171)
    dwell_open_s:  tuple = (300, 300, 1500)
    dwell_close_s: tuple = (0, 0, 600)
    # T5's wind window and T3
    avg_win_wind:  int = 6
    v_max:         int = 6
    wind_hyst:     int = 1
    dir_excl_low:  int = 0
    dir_excl_high: int = 0
    wind_prot_en:  bool = True
    # T4: the site, and the clock the log's stamps are in
    lat_deg:  int = 52
    lat_frac: int = 0
    lon_deg:  int = 5
    lon_frac: int = 0
    tz_str:   str = "CET-1CEST,M3.5.0,M10.5.0/3"
    # no effect on control (settings.EFFECT says why)
    t_min_day:      int = 16
    t_min_ngt:      int = 14
    deadzone_m3_mm: int = 20
    wpos_fitted_m3: bool = False


def settings_5c88(ts):
    """5C88's settings in force at local time ts: settings.schedule_5c88()."""
    return settings_mod.schedule_5c88().at(ts)


@dataclass(frozen=True)
class Profile:
    """Firmware behaviour that changed between the versions 5C88 ran."""
    defer_in_travel: bool          # gh#48, 2.3.1: T6 reversals wait for the stroke
    calibrating_inhibits_t6: bool  # gh#79, 2.9.2: T6 paused during the CLOSE_ALL sweep
    wind_hyst: bool = True         # gh#46, 2.3.0: T3 clears below v_max - wind_hyst
    own_wind_window: bool = True   # gh#35, 2.1.0: wind averages over avg_win_wind,
                                   # not avg_win_t (sensor_poll.cpp, win_w = win_t before)


# 5C88's firmware changes, each at the BOOT row that started it:
#   2.1.x 2026-06-29 19:03:21 -- a push OTA (SYSTEM 14-16 rows just before).
#         2.1.0 or later: at 19:07:03 the web GUI set avg_win_wind 6 -> 3, a key
#         only 2.1.0 has, and 6 is its default. Before, wind used avg_win_t.
#   2.1.3 2026-07-11 09:39:04 -- the OTA that ended the DS1307 clock (gh#37)
#   2.3.0 2026-07-24 01:04:25 -- ROTA apply (SYSTEM 24 row at 01:04:21)
#   2.3.1 2026-07-29 01:05:29 -- ROTA apply (SD log 2026-07-29_183942.log);
#         it has run 2.3.1 since. Nothing before it had the gh#48 guard.
OWN_WIND_WINDOW_ON_5C88 = datetime(2026, 6, 29, 19, 3, 21)
WIND_HYST_ON_5C88 = datetime(2026, 7, 24, 1, 4, 25)
GH48_ON_5C88 = datetime(2026, 7, 29, 1, 5, 29)
PROFILE_CURRENT = Profile(defer_in_travel=True, calibrating_inhibits_t6=True)


def profile_5c88(ts):
    return Profile(defer_in_travel=ts >= GH48_ON_5C88, calibrating_inhibits_t6=False,
                   wind_hyst=ts >= WIND_HYST_ON_5C88,
                   own_wind_window=ts >= OWN_WIND_WINDOW_ON_5C88)


# --------------------------------------------------------------------------
# T5 -- sliding averages
# --------------------------------------------------------------------------


def lroundf(x):
    """C lroundf(): nearest integer, halves away from zero."""
    x = float(x)
    return int(math.floor(x + 0.5)) if x >= 0.0 else -int(math.floor(-x + 0.5))


def clamp_u8(x):
    r = lroundf(x)
    return 0 if r < 0 else (255 if r > 255 else r)


def calc_win(win_min, poll_s):
    """sensor_poll.cpp calc_win(): samples in the window, clamped 1..360."""
    w = (max(win_min, 1) * 60) // max(poll_s, 1)
    return max(1, min(w, 360))


class SlidingMean:
    """avg_push()/avg_get() in float32, eviction before insertion as in T5."""

    def __init__(self, win):
        self.win = int(win)
        self.buf = deque()
        self.sum = F32(0.0)

    def push(self, val):
        val = F32(val)
        if len(self.buf) >= self.win:
            self.sum = F32(self.sum - self.buf.popleft())
        self.buf.append(val)
        self.sum = F32(self.sum + val)

    def mean(self):
        if not self.buf:
            return F32(0.0)
        return F32(self.sum / F32(len(self.buf)))


_DEG2RAD = F32(F32(math.pi) / F32(180.0))      # (float)M_PI / 180.0f
_RAD2DEG = F32(F32(180.0) / F32(math.pi))      # 180.0f / (float)M_PI


def _atan2_deg(s, c):
    """atan2f(s, c) * (180 / pi), folded into [0, 360) as T5 folds it."""
    d = F32(F32(math.atan2(float(s), float(c))) * _RAD2DEG)
    return F32(d + F32(360.0)) if d < F32(0.0) else d


class DirMean:
    """dir_avg_push()/dir_avg_get()/dir_avg_variation(): unit vectors in float32."""

    def __init__(self, win):
        self.win = int(win)
        self.sin, self.cos = deque(), deque()
        self.ss = self.sc = F32(0.0)

    def push(self, deg):
        rad = F32(F32(deg) * _DEG2RAD)
        s, c = F32(math.sin(float(rad))), F32(math.cos(float(rad)))
        if len(self.sin) >= self.win:
            self.ss = F32(self.ss - self.sin.popleft())
            self.sc = F32(self.sc - self.cos.popleft())
        self.sin.append(s)
        self.cos.append(c)
        self.ss = F32(self.ss + s)
        self.sc = F32(self.sc + c)

    def mean(self):
        return _atan2_deg(self.ss, self.sc) if self.sin else F32(0.0)

    def variation(self):
        """The narrowest arc holding every sample: 360 - the largest gap."""
        if len(self.sin) < 2:
            return F32(0.0)
        a = sorted(_atan2_deg(s, c) for s, c in zip(self.sin, self.cos))
        gap = F32(0.0)
        for x, y in zip(a, a[1:]):
            gap = max(gap, F32(y - x))
        gap = max(gap, F32(F32(F32(360.0) - a[-1]) + a[0]))
        v = F32(F32(360.0) - gap)
        return F32(0.0) if v < 0 else (F32(359.0) if v >= 360 else v)


class SensorLayer:
    """T5: raw samples in, sensor_reading_t's fields out.

    configure() is T5's step 2, on every poll: the windows follow the settings
    in force, and a context whose window changed starts empty. Before 2.1.0
    the wind window was the temperature window (profile.own_wind_window).
    """

    def __init__(self, settings, profile=None):
        self._wins = None
        self.configure(settings, profile or PROFILE_CURRENT)

    def configure(self, s, profile=None):
        if profile is not None:
            self.profile = profile
        win_t = calc_win(s.avg_win_t, s.poll_s)
        win_w = calc_win(s.avg_win_wind, s.poll_s) if self.profile.own_wind_window else win_t
        wins = (win_t, calc_win(s.avg_win_rh, s.poll_s), win_w)
        old = self._wins or (None, None, None)
        if wins[0] != old[0]:
            self.t = SlidingMean(wins[0])
        if wins[1] != old[1]:
            self.rh = SlidingMean(wins[1])
        if wins[2] != old[2]:
            self.ws = SlidingMean(wins[2])
            self.wd = DirMean(wins[2])
        self._wins = wins

    def push_wind(self, ws10, wdir, ok=True):
        """One S200 reading as the log carries it (0.1 m/s, whole degrees).

        ok False: a wind sensor fault. The averages do not advance and the raw
        fields carry the last average, as T5 writes them.
        """
        if ok:
            self.ws.push(F32(F32(ws10) / F32(10.0)))
            self.wd.push(F32(wdir))
            raw = (int(ws10), int(wdir))
        else:
            raw = (lroundf(F32(self.ws.mean() * F32(10.0))), lroundf(self.wd.mean()))
        return {
            "wind_ms10":        raw[0],
            "wind_dir_deg":     raw[1],
            "wind_avg_ms10":    lroundf(F32(self.ws.mean() * F32(10.0))),
            "wind_dir_avg_deg": lroundf(self.wd.mean()),
            "wind_dir_var_deg": lroundf(self.wd.variation()),
            "wind_valid":       bool(ok),
        }

    def push_register(self, t_c10, rh_c10):
        """One reading as the FG6485A registers carry it (0.1 degC, 0.1 %)."""
        t = F32(F32(t_c10) / F32(10.0))       # (float)(int16_t)regs[1] / 10.0f
        rh = F32(F32(rh_c10) / F32(10.0))
        self.t.push(t)
        self.rh.push(rh)
        t_avg = self.t.mean()
        return {
            "t_c10":      int(t_c10),
            "rh_pct":     clamp_u8(rh),
            "t_avg_c":    lroundf(t_avg),
            "t_avg_c10":  lroundf(F32(t_avg * F32(10.0))),
            "rh_avg_pct": clamp_u8(self.rh.mean()),
        }


# --------------------------------------------------------------------------
# T3 -- wind safety
# --------------------------------------------------------------------------


def dir_in_exclusion_zone(dir_deg, lo, hi):
    """safety_monitor.cpp: [lo, hi] on the circle, wrapping when lo > hi;
    a zero-width or negative arc is disabled."""
    if lo < 0 or hi < 0 or lo == hi:
        return False
    return lo <= dir_deg <= hi if lo < hi else (dir_deg >= lo or dir_deg <= hi)


class SafetyMonitor:
    """task_safety_monitor()'s state machine, one evaluation per T5 sample.

    T3 runs at priority 6 against T6's 5, and T4 wakes it first, so it
    decides before T6 does on the same sample; the caller evaluates it first.
    (On the dual core the two can overlap -- the firmware leaves that race.)
    """

    def __init__(self):
        self.active = False

    def reset(self):
        """A boot: EG1 starts clear."""
        self.active = False

    def evaluate(self, meas, wind_fault, s, profile):
        """-> "set" (CLOSE_ALL, T6 suspended), "clear" (RESUME) or None."""
        if not s.wind_prot_en:
            if self.active:
                self.active = False
                return "clear"
            return None
        speed_unsafe = dir_unsafe = False
        if wind_fault:
            speed_unsafe = True                  # FR-W04 safe-fail
        else:
            if s.v_max > 0:
                eff = s.wind_hyst if (self.active and profile.wind_hyst) else 0
                if eff >= s.v_max:
                    eff = s.v_max - 1
                speed_unsafe = meas["wind_avg_ms10"] >= (s.v_max - eff) * 10
            dir_unsafe = dir_in_exclusion_zone(meas["wind_dir_avg_deg"],
                                               s.dir_excl_low, s.dir_excl_high)
        unsafe = speed_unsafe or dir_unsafe
        if unsafe and not self.active:
            self.active = True
            return "set"
        if not unsafe and self.active:
            self.active = False
            return "clear"
        return None


# --------------------------------------------------------------------------
# T4 -- day and night (sunrise.cpp itself)
# --------------------------------------------------------------------------

_FW_LIB = None


def _fw():
    global _FW_LIB
    if _FW_LIB is None:
        src = FW_SRC / "data_manager"
        path = build_dll(BUILD_DIR / "fwhost.dll",
                         [src / "sunrise.cpp", HERE / "firmware_ffi.cpp"],
                         deps=[src / "sunrise.h"], include_dirs=[src],
                         cxx_std="gnu++17")             # MinGW hides M_PI under c++17
        lib = ctypes.CDLL(str(path))
        lib.fw_sunrise_calc.argtypes = [ctypes.c_int32, ctypes.c_float, ctypes.c_float,
                                        ctypes.POINTER(ctypes.c_int32),
                                        ctypes.POINTER(ctypes.c_int32)]
        lib.fw_sunrise_calc.restype = ctypes.c_int
        lib.fw_sunrise_is_daytime.argtypes = [ctypes.c_int32, ctypes.c_float, ctypes.c_float]
        lib.fw_sunrise_is_daytime.restype = ctypes.c_int
        _FW_LIB = lib
    return _FW_LIB


def site(s):
    """The coordinates as update_sun_times() adds them, in float32."""
    return (F32(F32(s.lat_deg) + F32(s.lat_frac) / F32(1000.0)),
            F32(F32(s.lon_deg) + F32(s.lon_frac) / F32(1000.0)))


def unix_of(ts_local, s):
    """A local stamp from the log -> Unix time, by the unit's tz_str."""
    return calendar.timegm(settings_mod.tz(s.tz_str).local_to_utc(ts_local).timetuple())


def is_daytime(ts_local, s):
    """T4's s_cfg.is_daytime at that moment. T4 recomputes it on each RTC read,
    about once a minute, so the firmware's switch can lag this by up to a minute."""
    lat, lon = site(s)
    return bool(_fw().fw_sunrise_is_daytime(unix_of(ts_local, s), float(lat), float(lon)))


def sun_times_local(ts_local, s):
    """(sunrise, sunset) in local minutes after midnight, as T4 logs them (LOG_SUN)."""
    lat, lon = site(s)
    rise, sset = ctypes.c_int32(), ctypes.c_int32()
    _fw().fw_sunrise_calc(unix_of(ts_local, s), float(lat), float(lon),
                          ctypes.byref(rise), ctypes.byref(sset))
    off = settings_mod.tz(s.tz_str).utcoffset(ts_local) // 60
    return (rise.value + off + 14400) % 1440, (sset.value + off + 14400) % 1440


# --------------------------------------------------------------------------
# T2 -- the actuator
# --------------------------------------------------------------------------

# T2-internal states (relay_controller.cpp ch_state_t)
CH_UNKNOWN, CH_CLOSED, CH_MOVING_OPEN, CH_OPEN, CH_MOVING_CLOSE, CH_GAP_TO_OPEN, \
    CH_GAP_TO_CLOSE = range(7)

# t2_get_window_states(): the gap states fold into the moving ones
PUBLIC_STATE = {
    CH_UNKNOWN: VENT_WIN_UNKNOWN, CH_CLOSED: VENT_WIN_CLOSED,
    CH_MOVING_OPEN: VENT_WIN_MOVING_OPEN, CH_GAP_TO_OPEN: VENT_WIN_MOVING_OPEN,
    CH_OPEN: VENT_WIN_OPEN,
    CH_MOVING_CLOSE: VENT_WIN_MOVING_CLOSE, CH_GAP_TO_CLOSE: VENT_WIN_MOVING_CLOSE,
}

# SENSOR_HR ch2 packing: 2 bits per channel (thermalProfileCampaign.md s.5.1)
BITMASK_CODE = {VENT_WIN_CLOSED: 0, VENT_WIN_MOVING_OPEN: 1, VENT_WIN_OPEN: 2,
                VENT_WIN_MOVING_CLOSE: 3, VENT_WIN_UNKNOWN: 0}

# RELAY row value_a (log_relay_event() carries ch_state_t; plot_daily.py
# RELAY_STATE_NAME is the authoritative decoder)
RELAY_TO_CH = {0: CH_UNKNOWN, 1: CH_CLOSED, 2: CH_MOVING_OPEN, 3: CH_OPEN,
               4: CH_MOVING_CLOSE, 5: CH_GAP_TO_OPEN, 6: CH_GAP_TO_CLOSE}

SRC_T6, SRC_T3, SRC_MANUAL = "T6", "T3", "MANUAL"
MOTOR_TRAVEL_MARGIN_MS = 5000   # cfg_defaults.h MOTOR_TRAVEL_MARGIN_S_DEFAULT
RELAY_GAP_MS = 2000             # relay_controller.cpp RELAY_GAP_MS


@dataclass
class ChannelCounters:
    starts:       int = 0   # relay energisations (every drive, either way)
    open_starts:  int = 0   # drives towards OPEN
    reversals:    int = 0   # a moving channel turned round
    dwell_defers: int = 0   # SRC_T6 commands refused on dwell (one per episode)
    travel_defers: int = 0  # SRC_T6 reversals refused mid-stroke (gh#48)


class Channel:
    """One motor channel. Times in ms on the simulator's clock."""

    def __init__(self, travel_s, dwell_open_s, dwell_close_s, profile, traverse_s=None):
        self.travel_ms = int(travel_s) * 1000 + MOTOR_TRAVEL_MARGIN_MS
        # The physical stroke. Production's M3 flap takes 176 s = travel 171 + 5
        # (CLAUDE.md), so by default the leaf arrives as the relay drops.
        self.traverse_ms = int(traverse_s * 1000) if traverse_s else self.travel_ms
        self.dwell_open_ms = int(dwell_open_s) * 1000
        self.dwell_close_ms = int(dwell_close_s) * 1000
        self.profile = profile
        self.state = CH_CLOSED
        self.pos = 0.0                 # 0 = closed .. 1 = open
        self.relay_deadline = 0
        self.gap_deadline = 0
        self.dwell_deadline = 0
        self._t = None                 # time up to which pos is integrated
        self._area = 0.0               # integral of pos since _area_t0 (pos x ms)
        self._area_t0 = None
        self._defer_latched = False
        self.n = ChannelCounters()

    # ---- time ----------------------------------------------------------
    def _move_pos(self, t):
        """Move the leaf to time t, integrating its position on the way
        (the same arithmetic as dataset._openness(), so a replayed and a
        simulated window present the plant with the same openness)."""
        if self._t is None:
            self._t = t
            return
        dt = t - self._t
        if dt <= 0:
            return
        p0 = self.pos
        rate = 1.0 / self.traverse_ms
        if self.state == CH_MOVING_OPEN:
            t_full = (1.0 - p0) / rate
            if dt <= t_full:
                self.pos = p0 + rate * dt
                self._area += (p0 + self.pos) / 2.0 * dt
            else:
                self.pos = 1.0
                self._area += (p0 + 1.0) / 2.0 * t_full + (dt - t_full)
        elif self.state == CH_MOVING_CLOSE:
            t_zero = p0 / rate
            if dt <= t_zero:
                self.pos = p0 - rate * dt
                self._area += (p0 + self.pos) / 2.0 * dt
            else:
                self.pos = 0.0
                self._area += p0 / 2.0 * t_zero
        else:
            self._area += p0 * dt
        self._t = t

    def take_mean(self, now):
        """Mean openness since the previous call: the plant's step into now."""
        self.advance(now)
        span = (now - self._area_t0) if self._area_t0 is not None else 0
        mean = self._area / span if span > 0 else self.pos
        self._area, self._area_t0 = 0.0, now
        return mean

    def advance(self, now):
        """Run every timer that expires up to now, in order (ch_update())."""
        while True:
            if self.state in (CH_MOVING_OPEN, CH_MOVING_CLOSE) and self.relay_deadline <= now:
                t = self.relay_deadline
                self._move_pos(t)
                if self.state == CH_MOVING_OPEN:
                    self.state, self.pos = CH_OPEN, 1.0
                    self.dwell_deadline = t + self.dwell_open_ms
                else:
                    self.state, self.pos = CH_CLOSED, 0.0
                    self.dwell_deadline = t + self.dwell_close_ms
                self._defer_latched = False
            elif self.state in (CH_GAP_TO_OPEN, CH_GAP_TO_CLOSE) and self.gap_deadline <= now:
                t = self.gap_deadline
                self._move_pos(t)
                self._start(CH_MOVING_OPEN if self.state == CH_GAP_TO_OPEN
                            else CH_MOVING_CLOSE, t)
            else:
                break
        self._move_pos(now)

    def _start(self, state, now):
        self.state = state
        self.relay_deadline = now + self.travel_ms
        self.n.starts += 1
        if state == CH_MOVING_OPEN:
            self.n.open_starts += 1

    # ---- commands (ch_start_open() / ch_start_close()) -------------------
    def command(self, want_open, now, source):
        self.advance(now)
        s = self.state
        same = (CH_OPEN, CH_MOVING_OPEN, CH_GAP_TO_OPEN) if want_open else \
               (CH_CLOSED, CH_MOVING_CLOSE, CH_GAP_TO_CLOSE)
        if s in same:
            return "noop"
        moving_away = CH_MOVING_CLOSE if want_open else CH_MOVING_OPEN
        gap_away = CH_GAP_TO_CLOSE if want_open else CH_GAP_TO_OPEN
        settled = CH_CLOSED if want_open else CH_OPEN
        if s == moving_away:
            if source == SRC_T6 and self.profile.defer_in_travel:
                self.n.travel_defers += 1
                return "deferred-travel"
            self.state = CH_GAP_TO_OPEN if want_open else CH_GAP_TO_CLOSE
            self.gap_deadline = now + RELAY_GAP_MS
            self.n.reversals += 1
            return "reversal"
        if s == gap_away:
            self.state = CH_GAP_TO_OPEN if want_open else CH_GAP_TO_CLOSE
            return "pivot"
        if s == settled and source == SRC_T6 and now < self.dwell_deadline:
            if not self._defer_latched:
                self._defer_latched = True
                self.n.dwell_defers += 1
            return "deferred-dwell"
        self._start(CH_MOVING_OPEN if want_open else CH_MOVING_CLOSE, now)
        return "start"

    def sync(self, ch_state, now):
        """Force the state a RELAY row reports, as T2 set it at that moment."""
        self.advance(now)
        if ch_state == CH_MOVING_OPEN and self.state != CH_MOVING_OPEN:
            self._start(CH_MOVING_OPEN, now)
        elif ch_state == CH_MOVING_CLOSE and self.state != CH_MOVING_CLOSE:
            self._start(CH_MOVING_CLOSE, now)
        elif ch_state == CH_OPEN:
            self.state, self.pos = CH_OPEN, 1.0
            self.dwell_deadline = now + self.dwell_open_ms
        elif ch_state == CH_CLOSED:
            self.state, self.pos = CH_CLOSED, 0.0
            self.dwell_deadline = now + self.dwell_close_ms
        elif ch_state in (CH_GAP_TO_OPEN, CH_GAP_TO_CLOSE) and self.state != ch_state:
            self.state = ch_state
            self.gap_deadline = now + RELAY_GAP_MS
        elif ch_state == CH_UNKNOWN:
            self.state = CH_UNKNOWN

    @property
    def public(self):
        return PUBLIC_STATE[self.state]


class Actuator:
    """T2: three channels, M1..M3 at index 0..2."""

    def __init__(self, settings, profile, traverse_s=(None, None, None)):
        self.ch = [Channel(settings.travel_s[i], settings.dwell_open_s[i],
                           settings.dwell_close_s[i], profile, traverse_s[i])
                   for i in range(3)]

    def set_profile(self, profile):
        for c in self.ch:
            c.profile = profile

    def advance(self, now):
        for c in self.ch:
            c.advance(now)

    def command(self, i, want_open, now, source=SRC_T6):
        return self.ch[i].command(want_open, now, source)

    def close_all(self, now, source=SRC_T3):
        for i in range(3):
            self.ch[i].command(False, now, source)

    def public_states(self):
        return [c.public for c in self.ch]

    def bitmask(self):
        s = self.public_states()
        return BITMASK_CODE[s[0]] | (BITMASK_CODE[s[1]] << 2) | (BITMASK_CODE[s[2]] << 4)

    def openness(self, mode="state", now=None):
        """Per-window openness for the plant.

        "state"    -- the calibrator's convention: 1 whenever the window is
                      not CLOSED, both moving states included. Use it with
                      the single-node artifact, which was fitted that way.
        "position" -- the leaf's travelled fraction right now.
        "mean"     -- the leaf's mean position since the previous "mean"
                      call (needs now). What the two-node plant was fitted
                      with, and what a linear law needs.
        """
        if mode == "state":
            return tuple(0.0 if c.public == VENT_WIN_CLOSED else 1.0 for c in self.ch)
        if mode == "mean":
            return tuple(c.take_mean(now) for c in self.ch)
        return tuple(c.pos for c in self.ch)


# --------------------------------------------------------------------------
# T6 -- the caller around the law
# --------------------------------------------------------------------------


@dataclass
class Decision:
    step: int
    step_t: int
    step_rh: int
    reason: int
    closes: list = field(default_factory=list)
    opens: list = field(default_factory=list)
    model_errors: int = 0
    logged: bool = False     # the resolved step changed: T6 writes a MODE row


class Controller:
    """task_climate_control()'s loop body, with the law behind vent_model.h."""

    def __init__(self, law, settings):
        self.law = law
        self.s = settings
        self.vin = VentIn()
        self.prev_inhibited = False
        self.last_step = 0
        self.model_errors = 0

    def reset(self):
        """Boot, or an inhibit's onset: current_step_t/_rh back to 0."""
        self.law.reset()
        self.last_step = 0

    def cycle(self, now_ms, unix_time, daytime, meas, inhibited, actuator=None):
        """One T6 wake. Returns a Decision, or None while inhibited.

        meas: the SensorLayer dict. actuator: None to decide without acting
        (the control gate replays decisions only).
        """
        if inhibited:
            if not self.prev_inhibited:
                self.reset()
            self.prev_inhibited = True
            return None
        self.prev_inhibited = False

        s, v = self.s, self.vin
        v.now_ms = now_ms & 0xFFFFFFFF
        v.unix_time = unix_time & 0xFFFFFFFF
        v.daytime = bool(daytime)
        v.t_c10 = meas["t_c10"]
        v.t_avg_c10 = meas["t_avg_c10"]
        v.t_avg_c = meas["t_avg_c"]
        v.rh_pct = meas["rh_pct"]
        v.rh_avg_pct = meas["rh_avg_pct"]
        v.t_valid = v.rh_valid = True
        # The measured wind, when the caller has T5's wind fields (contract
        # §3a); the stepped law does not read it.
        v.wind_ms10 = meas.get("wind_ms10", 0)
        v.wind_avg_ms10 = meas.get("wind_avg_ms10", 0)
        v.wind_dir_deg = meas.get("wind_dir_deg", 0)
        v.wind_dir_avg_deg = meas.get("wind_dir_avg_deg", 0)
        v.wind_dir_var_deg = meas.get("wind_dir_var_deg", 0)
        v.wind_valid = bool(meas.get("wind_valid", False))
        v.t_max_c10 = (s.t_max_day if daytime else s.t_max_ngt) * 10
        v.rh_max_pct = s.rh_max_day if daytime else s.rh_max_ngt
        v.rh_min_pct = s.rh_min_day if daytime else s.rh_min_ngt
        v.hyst_t_c = s.hyst_t if s.hyst_t > 0 else 1
        v.hyst_rh_pct = s.hyst_rh if s.hyst_rh > 0 else 1
        v.cr_priority = s.cr_priority
        v.rh_ctrl_en = s.rh_ctrl_en
        v.m3_deadzone_x10 = 0          # deadzone_m3 reaches the law with mode 2 (settings.EFFECT)
        v.m3_min_move_ms = 0           # the key does not exist yet (contract §3a)
        states = actuator.public_states() if actuator else [VENT_WIN_CLOSED] * 3
        for i in range(3):
            w = v.win[i]
            w.state = states[i]
            w.cap = VENT_CAP_DIGITAL
            w.pos_x10 = -1
            w.pos_age_ms = 0
            w.last_target_x10 = -1
            w.last_result = VENT_RES_NONE
            w.ms_since_move = 0

        out = self.law.step(v)
        d = Decision(int(out.step), int(out.step_t), int(out.step_rh), int(out.reason))
        for i in range(3):
            act = out.win[i].action
            if act == VENT_ACT_CLOSE:
                d.closes.append(i)
            elif act == VENT_ACT_OPEN:
                d.opens.append(i)
            elif act == VENT_ACT_TARGET:
                # every window is DIGITAL here: a model error, treated as HOLD
                d.model_errors += 1
                self.model_errors += 1

        if actuator is not None:
            # reconcile_to_step(): narrowing before widening
            for i in d.closes:
                actuator.command(i, False, now_ms, SRC_T6)
            for i in d.opens:
                actuator.command(i, True, now_ms, SRC_T6)

        d.logged = d.step != self.last_step    # post_log_mode() on change
        self.last_step = d.step
        return d

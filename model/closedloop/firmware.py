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
  Actuator -- T2's per-channel state machine (relay_controller.cpp):
      full-travel strokes of travel + 5 s, the 2 s reversal gap, dwell timers
      that defer SRC_T6 only, and -- from 2.3.1 (gh#48) -- SRC_T6 reversals
      deferred while a stroke is in progress. Also tracks the physical
      position, which the binary law never reads but a linear law will.
  Controller -- T6's cycle (climate_control.cpp): an inhibit resets the
      law, setpoints are resolved for day or night, every narrowing command
      goes out before any widening one, and a MODE row is written whenever
      the resolved step changes.

Not modelled here: T3's own wind logic (a reproduction takes the logged
override bit, which is exact because the plant cannot change the wind),
motor alarms, sensor faults other than as logged inhibits, and Q1 overflow.

Settings and firmware versions
------------------------------
Settings5C88 and profile_5c88() hold what 5C88 actually ran in summer 2026,
with the evidence for each value. Everything the SD audit rows show is taken
from them; the rest are the firmware defaults, and the controller gate is
what confirms them (the humidity settings in particular are unverified until
the gate reproduces the logged humidity step as well as the temperature one).
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field, replace
from datetime import datetime

import numpy as np

from ventmodel import (VENT_ACT_CLOSE, VENT_ACT_OPEN, VENT_ACT_TARGET,
                       VENT_CAP_DIGITAL, VENT_RES_NONE, VENT_WIN_CLOSED,
                       VENT_WIN_MOVING_CLOSE, VENT_WIN_MOVING_OPEN, VENT_WIN_OPEN,
                       VENT_WIN_UNKNOWN, VentIn)

F32 = np.float32

# --------------------------------------------------------------------------
# Settings and firmware behaviour, per date
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Settings:
    """The climate settings T4 resolves for T6 and T5. Whole degC / whole %."""
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
    avg_win_t:   int = 6
    avg_win_rh:  int = 10
    poll_s:      int = 30
    travel_s:      tuple = (21, 21, 171)
    dwell_open_s:  tuple = (300, 300, 1500)
    dwell_close_s: tuple = (0, 0, 600)


# What 5C88 ran, summer 2026. Evidence per value:
#   t_max_day/ngt 28/20, hyst_t 5, avg_win_t 3 -- vent_step_replay.py reproduces
#       96.8 % of the logged T-demands with exactly these (campaign F8); avg_win_t
#       3 is also recorded in memory/gotcha-log.md 2026-07-28 (it differs from
#       2344's 6 and from the default 6).
#   rh_min_day 50 -> 60 on 2026-06-11 10:47:02 -- SETPT audit row (param 5);
#       rh_max_day 75 confirmed by the audit row beside it (param 6).
#   rh_ctrl_en 1 -- the logged MODE rows carry humidity votes (step_rh 0..3).
#   everything else -- cfg_defaults.h; no audit row changed it from 2026-06-04
#       to 2026-09-17. UNVERIFIED until the control gate reproduces step_rh.
SETTINGS_5C88 = Settings(avg_win_t=3, rh_min_day=60)
RH_MIN_DAY_CHANGE_5C88 = datetime(2026, 6, 11, 10, 47, 2)


def settings_5c88(ts):
    """5C88's settings in force at local time ts."""
    if ts < RH_MIN_DAY_CHANGE_5C88:
        return replace(SETTINGS_5C88, rh_min_day=50)
    return SETTINGS_5C88


@dataclass(frozen=True)
class Profile:
    """Firmware behaviour that changed between the versions 5C88 ran."""
    defer_in_travel: bool          # gh#48, 2.3.1: T6 reversals wait for the stroke
    calibrating_inhibits_t6: bool  # gh#79, 2.9.2: T6 paused during the CLOSE_ALL sweep


# 5C88 committed 2.3.1 at this BOOT (ROTA apply, SD log 2026-07-29_183942.log)
# and has run it since. Nothing before it had the gh#48 guard.
GH48_ON_5C88 = datetime(2026, 7, 29, 1, 5, 29)
PROFILE_CURRENT = Profile(defer_in_travel=True, calibrating_inhibits_t6=True)


def profile_5c88(ts):
    return Profile(defer_in_travel=ts >= GH48_ON_5C88, calibrating_inhibits_t6=False)


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


class SensorLayer:
    """T5 for the T/RH sensor: raw sample in, sensor_reading_t fields out."""

    def __init__(self, settings):
        self.t = SlidingMean(calc_win(settings.avg_win_t, settings.poll_s))
        self.rh = SlidingMean(calc_win(settings.avg_win_rh, settings.poll_s))

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
        self._defer_latched = False
        self.n = ChannelCounters()

    # ---- time ----------------------------------------------------------
    def _move_pos(self, t):
        if self._t is None:
            self._t = t
            return
        dt = t - self._t
        if dt > 0:
            if self.state == CH_MOVING_OPEN:
                self.pos = min(1.0, self.pos + dt / self.traverse_ms)
            elif self.state == CH_MOVING_CLOSE:
                self.pos = max(0.0, self.pos - dt / self.traverse_ms)
        self._t = t

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

    def openness(self, mode="state"):
        """Per-window openness for the plant.

        "state"    -- the calibrator's convention: 1 whenever the window is
                      not CLOSED, both moving states included. Use it with
                      the calibrated parameters, which were fitted that way.
        "position" -- the leaf's travelled fraction. What a linear law needs.
        """
        if mode == "state":
            return tuple(0.0 if c.public == VENT_WIN_CLOSED else 1.0 for c in self.ch)
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
        v.wind_valid = False           # the stepped law does not read wind
        v.t_max_c10 = (s.t_max_day if daytime else s.t_max_ngt) * 10
        v.rh_max_pct = s.rh_max_day if daytime else s.rh_max_ngt
        v.rh_min_pct = s.rh_min_day if daytime else s.rh_min_ngt
        v.hyst_t_c = s.hyst_t if s.hyst_t > 0 else 1
        v.hyst_rh_pct = s.hyst_rh if s.hyst_rh > 0 else 1
        v.cr_priority = s.cr_priority
        v.rh_ctrl_en = s.rh_ctrl_en
        v.m3_deadzone_x10 = 0
        v.m3_min_move_ms = 0
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

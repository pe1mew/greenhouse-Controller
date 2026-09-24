"""
graded_walkthrough.py -- what `graded` does to M1, M2 and M3 as the temperature
moves, as markdown tables.

    python model/closedloop/graded_walkthrough.py

It writes the two tables in gradedCandidate.md's "How it behaves over time":

  1. the ladder: the step, M1 and M2's end state, and M3's aperture demand,
     swept over the average temperature;
  2. a morning: a synthetic ramp driven through the law and the emulated
     firmware, one row per state change.

Both come from the real library (`drivers/ventModel`, through ventmodel.py) and
the real caller (firmware.py's Controller and Actuator, 2.12.0 as built), so a
change to either shows up here. Nothing is read from a log: the ramp is
synthetic and the reading is handed to T6 ready-averaged, which keeps the law's
own behaviour visible. A greenhouse adds T5's window and the sensor's lag on
top -- see NS-10 -- and `closed_loop.py reproduce` is where real weather goes.

ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
for p in (HERE, HERE.parent):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

from firmware import (  # noqa: E402
    CH_CLOSED, CH_OPEN, CH_STOPPED, PROFILE_CURRENT, Actuator, Controller, LinearM3, Settings,
)
from ventmodel import (  # noqa: E402
    VENT_ACT_HOLD, VENT_ACT_CLOSE, VENT_ACT_OPEN, VENT_ACT_TARGET, VENT_CAP_DIGITAL,
    VENT_CAP_LINEAR, VENT_WIN_CLOSED, VentIn, VentModel,
)

ACT = {VENT_ACT_HOLD: "hold", VENT_ACT_CLOSE: "shut", VENT_ACT_OPEN: "open",
       VENT_ACT_TARGET: "target"}
STATE = {CH_CLOSED: "shut", CH_OPEN: "open", CH_STOPPED: "part-open"}

# The ramp: 1 degC per RAMP_MIN minutes from T_LOW to T_HIGH, a hold, and back.
T_LOW, T_HIGH, RAMP_MIN, FLAT_MIN, LEAD_MIN = 27.0, 32.0, 20.0, 80.0, 20.0
POLL_S = 30                      # T6 decides this often (poll_interval)


def t5_whole(t_c):
    """T5 rounds its average to whole degrees the way lroundf() does, and the
    step ladder compares those: a reading of 30.5 is 31 to the ladder."""
    return int(math.floor(t_c + 0.5))


def ladder(s, law):
    """The step and each window's demand at a given average, from shut."""
    v = VentIn()
    v.t_max_c10, v.hyst_t_c, v.hyst_rh_pct = s.t_max_day * 10, s.hyst_t, s.hyst_rh
    v.rh_max_pct, v.rh_min_pct, v.rh_ctrl_en = s.rh_max_day, s.rh_min_day, s.rh_ctrl_en
    v.rh_pct = v.rh_avg_pct = (s.rh_max_day + s.rh_min_day) // 2      # humidity abstains
    v.t_valid = v.rh_valid = v.daytime = True
    v.cr_priority = s.cr_priority
    v.m3_deadzone_x10 = LinearM3().deadzone_x10(s.deadzone_m3_mm)
    rows = []
    t10 = s.t_max_day * 10
    while t10 <= s.t_max_day * 10 + 45:
        law.reset()                      # each row on its own: no rate limit, no hold
        for i in range(3):
            v.win[i].state, v.win[i].cap = VENT_WIN_CLOSED, VENT_CAP_DIGITAL
            v.win[i].pos_x10, v.win[i].last_target_x10 = -1, -1
            v.win[i].ms_since_move = 0xFFFFFFFF
        v.win[2].cap, v.win[2].pos_x10, v.win[2].pos_age_ms = VENT_CAP_LINEAR, 0, 0
        v.t_c10 = v.t_avg_c10 = t10
        v.t_avg_c = t5_whole(t10 / 10.0)
        out = law.step(v)
        rows.append((t10 / 10.0, v.t_avg_c, out.step, ACT[out.win[0].action],
                     ACT[out.win[1].action], out.demand_t_x10 / 10.0))
        t10 += 5
    return rows


def ramp_at(minute):
    """The synthetic morning, in degC."""
    if minute < LEAD_MIN:
        return T_LOW
    up_min = (T_HIGH - T_LOW) * RAMP_MIN
    if minute < LEAD_MIN + up_min:
        return T_LOW + (minute - LEAD_MIN) / RAMP_MIN
    if minute < LEAD_MIN + up_min + FLAT_MIN:
        return T_HIGH
    return max(T_LOW, T_HIGH - (minute - LEAD_MIN - up_min - FLAT_MIN) / RAMP_MIN)


def morning(s, law):
    """The ramp through T6 and T2: one row per change of state or target."""
    act = Actuator(s, PROFILE_CURRENT, m3_linear=LinearM3(min_interval_s=s.min_intv_m3))
    ctl = Controller(law, s)
    # A fresh boot. Without this the law would still hold the step the ladder
    # sweep left it at, and its close guard would open M1 at the first wake.
    law.reset()
    t0 = 10_000_000
    act.advance(t0)
    rows, prev = [], None
    end = 2 * LEAD_MIN + (T_HIGH - T_LOW) * RAMP_MIN * 2 + FLAT_MIN
    for tick in range(int(end * 60 / POLL_S)):
        minute = tick * POLL_S / 60.0
        t_c = ramp_at(minute)
        meas = {"t_c10": int(round(t_c * 10)), "t_avg_c10": int(round(t_c * 10)),
                "t_avg_c": t5_whole(t_c), "rh_pct": (s.rh_max_day + s.rh_min_day) // 2,
                "rh_avg_pct": (s.rh_max_day + s.rh_min_day) // 2}
        now = t0 + int(minute * 60_000)
        act.advance(now)
        d = ctl.cycle(now, 0, True, meas, False, actuator=act)
        m3 = act.ch[2]
        now_state = (act.ch[0].state, act.ch[1].state, m3.state, ctl.m3_last_target)
        if now_state == prev:
            continue
        what = []
        if prev is None or now_state[0] != prev[0]:
            what.append("M1 %s" % STATE.get(now_state[0], "moving"))
        if prev is None or now_state[1] != prev[1]:
            what.append("M2 %s" % STATE.get(now_state[1], "moving"))
        if prev is not None and now_state[3] != prev[3]:
            what.append("T6 asks M3 for %.0f %%" % (now_state[3] / 10.0))
        if prev is None or now_state[2] != prev[2]:
            what.append("M3 %s%s" % (STATE.get(now_state[2], "moving"),
                                     " at %.0f %%" % (m3.reading / 10.0)
                                     if now_state[2] == CH_STOPPED else ""))
        rows.append((minute, t_c, d.step if d else None, "; ".join(what)))
        prev = now_state
    return rows, act


def mode1(s):
    """The same ramp under mode 1, for the count to compare against."""
    law = VentModel("stepped")
    law.reset()
    act = Actuator(s, PROFILE_CURRENT)          # no wire sensor: M3 is binary
    ctl = Controller(law, s)
    t0 = 10_000_000
    act.advance(t0)
    end = 2 * LEAD_MIN + (T_HIGH - T_LOW) * RAMP_MIN * 2 + FLAT_MIN
    rows, prev = [], None
    for tick in range(int(end * 60 / POLL_S)):
        minute = tick * POLL_S / 60.0
        t_c = ramp_at(minute)
        meas = {"t_c10": int(round(t_c * 10)), "t_avg_c10": int(round(t_c * 10)),
                "t_avg_c": t5_whole(t_c), "rh_pct": (s.rh_max_day + s.rh_min_day) // 2,
                "rh_avg_pct": (s.rh_max_day + s.rh_min_day) // 2}
        act.advance(t0 + int(minute * 60_000))
        ctl.cycle(t0 + int(minute * 60_000), 0, True, meas, False, actuator=act)
        st = tuple(c.state for c in act.ch)
        if prev is not None and st[2] != prev[2] and st[2] in (CH_CLOSED, CH_OPEN):
            rows.append("M3 %s at %d:%02d (%.1f degC)"
                        % ("open" if st[2] == CH_OPEN else "shut", int(minute),
                           round(minute % 1 * 60), t_c))
        prev = st
    return rows, act


def main():
    s = Settings()
    law = VentModel("graded")
    dz = LinearM3().deadzone_x10(s.deadzone_m3_mm)
    print("`graded` v%d, settings: t_max_day %d degC, hyst_t %d, travel %s s, "
          "min_intv_m3 %d s, deadzone %d mm = %.1f %%\n"
          % (law.version, s.t_max_day, s.hyst_t, s.travel_s, s.min_intv_m3,
             s.deadzone_m3_mm, dz / 10.0))

    print("| T_avg, degC | as the ladder reads it | Step | M1 | M2 | M3's demand |")
    print("|---|---|---|---|---|---|")
    for t_c, whole, step, m1, m2, demand in ladder(s, law):
        print("| %.1f | %d | %d | %s | %s | %.0f %% |" % (t_c, whole, step, m1, m2, demand))

    print("\n| Time, min:s | T_avg, degC | Step | What happens |")
    print("|---|---|---|---|")
    rows, act = morning(s, law)
    for minute, t_c, step, what in rows:
        print("| %d:%02d | %.1f | %s | %s |" % (int(minute), round(minute % 1 * 60), t_c,
                                                step if step is not None else "-", what))
    print("\nDrives: M1 %d, M2 %d, M3 %d (%d of them to a target)."
          % (act.ch[0].n.starts, act.ch[1].n.starts, act.ch[2].n.starts,
             act.ch[2].n.targets))
    m1_rows, m1_act = mode1(s)
    print("Mode 1 on the same ramp: %s. Drives: M1 %d, M2 %d, M3 %d."
          % ("; ".join(m1_rows), m1_act.ch[0].n.starts, m1_act.ch[1].n.starts,
             m1_act.ch[2].n.starts))
    return 0


if __name__ == "__main__":
    sys.exit(main())

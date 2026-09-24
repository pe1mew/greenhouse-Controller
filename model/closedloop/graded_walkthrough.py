"""
graded_walkthrough.py -- what `graded` does to M1, M2 and M3 as the temperature
and the humidity move, as markdown tables.

    python model/closedloop/graded_walkthrough.py

It writes the tables in gradedCandidate.md's "How it behaves over time" and
"What humidity does":

  1. the temperature ladder: the step, M1 and M2's end state and M3's aperture
     demand, swept over the average temperature;
  2. a morning: a synthetic temperature ramp driven through the law and the
     emulated firmware, one row per state change, and the same ramp under
     mode 1 for the count to compare against;
  3. the humidity ladder, at a cool and at a warm house, and what each
     `cr_priority` makes of the same votes;
  4. a muggy afternoon: humidity rising while the temperature sits one step
     above its setpoint.

Everything comes from the real library (`drivers/ventModel`, through
ventmodel.py) and the real caller (firmware.py's Controller and Actuator,
2.12.0 as built), so a change to either shows up here. Nothing is read from a
log: the ramps are synthetic and the readings are handed to T6 ready-averaged,
which keeps the law's own behaviour visible. A greenhouse adds T5's window and
the sensor's lag on top -- see NS-10 -- and `closed_loop.py reproduce` is where
real weather goes.

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

# The temperature ramp: 1 degC per RAMP_MIN minutes from T_LOW to T_HIGH, a hold, and back.
T_LOW, T_HIGH, RAMP_MIN, FLAT_MIN, LEAD_MIN = 27.0, 32.0, 20.0, 80.0, 20.0
# The humidity ramp, at a house one step above its setpoint: 1 % per RH_RAMP_MIN minutes.
RH_LOW, RH_HIGH, RH_RAMP_MIN, RH_FLAT_MIN, RH_WARM_C = 70, 88, 5.0, 40.0, 29.0
POLL_S = 30                      # T6 decides this often (poll_interval)


def t5_whole(t_c):
    """T5 rounds its average to whole degrees the way lroundf() does, and the
    step ladder compares those: a reading of 30.5 is 31 to the ladder."""
    return int(math.floor(t_c + 0.5))


def fresh_in(s, prio=None):
    """A vent_in_t with every window shut, M3 linear and its position known."""
    v = VentIn()
    v.t_max_c10, v.hyst_t_c, v.hyst_rh_pct = s.t_max_day * 10, s.hyst_t, s.hyst_rh
    v.rh_max_pct, v.rh_min_pct = s.rh_max_day, s.rh_min_day
    v.rh_ctrl_en = s.rh_ctrl_en
    v.cr_priority = s.cr_priority if prio is None else prio
    v.t_valid = v.rh_valid = v.daytime = True
    v.m3_deadzone_x10 = LinearM3().deadzone_x10(s.deadzone_m3_mm)
    for i in range(3):
        v.win[i].state, v.win[i].cap = VENT_WIN_CLOSED, VENT_CAP_DIGITAL
        v.win[i].pos_x10, v.win[i].last_target_x10 = -1, -1
        v.win[i].ms_since_move = 0xFFFFFFFF
    v.win[2].cap, v.win[2].pos_x10, v.win[2].pos_age_ms = VENT_CAP_LINEAR, 0, 0
    return v


def one_call(s, law, t_c, rh, prio=None):
    """One decision from shut, on a law with no memory: no rate limit, no hold."""
    law.reset()
    v = fresh_in(s, prio)
    v.t_c10 = v.t_avg_c10 = int(round(t_c * 10))
    v.t_avg_c = t5_whole(t_c)
    v.rh_pct = v.rh_avg_pct = rh
    return law.step(v)


def temperature_ladder(s, law):
    neutral_rh = (s.rh_max_day + s.rh_min_day) // 2          # humidity abstains
    rows = []
    t10 = s.t_max_day * 10
    while t10 <= s.t_max_day * 10 + 45:
        out = one_call(s, law, t10 / 10.0, neutral_rh)
        rows.append((t10 / 10.0, t5_whole(t10 / 10.0), out.step, ACT[out.win[0].action],
                     ACT[out.win[1].action], out.demand_t_x10 / 10.0))
        t10 += 5
    return rows


def humidity_ladder(s, law):
    """The humidity vote, and what it resolves to at a cool and a warm house.

    step() hands back the model's own buffer, so each result is read before the
    next call: holding both would compare one object with itself.
    """
    rows = []
    for rh in (72, 76, 80, 84, 88, 45):
        out = one_call(s, law, 24.0, rh)                     # temperature votes 0
        vote, cool = out.step_rh, out.step
        out = one_call(s, law, RH_WARM_C, rh)                # temperature votes 1
        warm, m3_rh = out.step, out.demand_rh_x10
        rows.append((rh, vote, cool, warm, m3_rh / 10.0 if m3_rh >= 0 else None))
    return rows


def priority_matrix(s, law):
    """The same votes under each cr_priority, at a cool house."""
    rows = []
    for rh in (76, 84, 45):
        cells = [one_call(s, law, 24.0, rh, prio).step for prio in (0, 1, 2)]
        rows.append((rh, one_call(s, law, 24.0, rh).step_rh, cells))
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


def rh_ramp_at(minute):
    """The muggy afternoon, in whole % RH."""
    up_min = (RH_HIGH - RH_LOW) * RH_RAMP_MIN
    if minute < up_min:
        rh = RH_LOW + minute / RH_RAMP_MIN
    elif minute < up_min + RH_FLAT_MIN:
        rh = RH_HIGH
    else:
        rh = max(RH_LOW, RH_HIGH - (minute - up_min - RH_FLAT_MIN) / RH_RAMP_MIN)
    return int(rh)


def run(s, law, reading, end_min, m3_linear=True):
    """A scenario through T6 and T2: one row per change of state or target.

    reading(minute) -> (degC, %RH), both as T5 would hand them over.
    """
    m3 = LinearM3(min_interval_s=s.min_intv_m3) if m3_linear else None
    act = Actuator(s, PROFILE_CURRENT, m3_linear=m3)
    ctl = Controller(law, s)
    # A fresh boot. Without this the law would still hold the step a previous
    # scenario left it at, and its close guard would open M1 at the first wake.
    law.reset()
    t0 = 10_000_000
    act.advance(t0)
    rows, prev = [], None
    for tick in range(int(end_min * 60 / POLL_S)):
        minute = tick * POLL_S / 60.0
        t_c, rh = reading(minute)
        meas = {"t_c10": int(round(t_c * 10)), "t_avg_c10": int(round(t_c * 10)),
                "t_avg_c": t5_whole(t_c), "rh_pct": rh, "rh_avg_pct": rh}
        now = t0 + int(minute * 60_000)
        act.advance(now)
        d = ctl.cycle(now, 0, True, meas, False, actuator=act)
        state = (act.ch[0].state, act.ch[1].state, act.ch[2].state, ctl.m3_last_target)
        if state == prev:
            continue
        what = []
        if prev is None or state[0] != prev[0]:
            what.append("M1 %s" % STATE.get(state[0], "moving"))
        if prev is None or state[1] != prev[1]:
            what.append("M2 %s" % STATE.get(state[1], "moving"))
        if prev is not None and state[3] != prev[3]:
            what.append("T6 asks M3 for %.0f %%" % (state[3] / 10.0))
        if prev is None or state[2] != prev[2]:
            what.append("M3 %s%s" % (STATE.get(state[2], "moving"),
                                     " at %.0f %%" % (act.ch[2].reading / 10.0)
                                     if state[2] == CH_STOPPED else ""))
        rows.append((minute, t_c, rh, d.step if d else None, "; ".join(what)))
        prev = state
    return rows, act


def timeline(rows, column):
    """Print a scenario as a markdown table; `column` picks T or RH."""
    head = "T_avg, degC" if column == "t" else "RH_avg, %"
    print("\n| Time, min:s | %s | Step | What happens |" % head)
    print("|---|---|---|---|")
    for minute, t_c, rh, step, what in rows:
        value = "%.1f" % t_c if column == "t" else "%d" % rh
        print("| %d:%02d | %s | %s | %s |" % (int(minute), round(minute % 1 * 60), value,
                                              step if step is not None else "-", what))


def main():
    s = Settings()
    law = VentModel("graded")
    dz = LinearM3().deadzone_x10(s.deadzone_m3_mm)
    print("`graded` v%d, settings: t_max_day %d degC, hyst_t %d, rh_max_day %d %%, "
          "rh_min_day %d %%, hyst_rh %d, cr_priority %d, rh_ctrl_en %d, travel %s s, "
          "min_intv_m3 %d s, deadzone %d mm = %.1f %%\n"
          % (law.version, s.t_max_day, s.hyst_t, s.rh_max_day, s.rh_min_day, s.hyst_rh,
             s.cr_priority, s.rh_ctrl_en, s.travel_s, s.min_intv_m3, s.deadzone_m3_mm,
             dz / 10.0))

    print("### the temperature ladder\n")
    print("| T_avg, degC | as the ladder reads it | Step | M1 | M2 | M3's demand |")
    print("|---|---|---|---|---|---|")
    for t_c, whole, step, m1, m2, demand in temperature_ladder(s, law):
        print("| %.1f | %d | %d | %s | %s | %.0f %% |" % (t_c, whole, step, m1, m2, demand))

    print("\n### a morning")
    rows, act = run(s, law, lambda m: (ramp_at(m), (s.rh_max_day + s.rh_min_day) // 2),
                    2 * LEAD_MIN + (T_HIGH - T_LOW) * RAMP_MIN * 2 + FLAT_MIN)
    timeline(rows, "t")
    print("\nDrives: M1 %d, M2 %d, M3 %d (%d of them to a target)."
          % (act.ch[0].n.starts, act.ch[1].n.starts, act.ch[2].n.starts, act.ch[2].n.targets))
    m1_law = VentModel("stepped")
    rows1, act1 = run(s, m1_law, lambda m: (ramp_at(m), (s.rh_max_day + s.rh_min_day) // 2),
                      2 * LEAD_MIN + (T_HIGH - T_LOW) * RAMP_MIN * 2 + FLAT_MIN,
                      m3_linear=False)
    # the drives, not the opening row that says where everything started
    m3_rows = [r for r in rows1[1:] if "M3" in r[4] and "moving" not in r[4]]
    print("Mode 1 on the same ramp: %s. Drives: M1 %d, M2 %d, M3 %d."
          % ("; ".join("M3 %s at %d:%02d" % ("open" if "open" in r[4] else "shut",
                                             int(r[0]), round(r[0] % 1 * 60))
                       for r in m3_rows),
             act1.ch[0].n.starts, act1.ch[1].n.starts, act1.ch[2].n.starts))

    print("\n### the humidity ladder\n")
    print("| RH_avg, %% | humidity's vote | resolved at 24 degC | resolved at %.0f degC "
          "| M3's humidity demand |" % RH_WARM_C)
    print("|---|---|---|---|---|")
    for rh, vote, cool, warm, m3_rh in humidity_ladder(s, law):
        print("| %d | %s | %d | %d | %s |"
              % (rh, "abstains" if vote < 0 else "step %d" % vote, cool, warm,
                 "-" if m3_rh is None else "%.0f %%" % m3_rh))

    print("\n### the same votes under each cr_priority, at 24 degC\n")
    print("| RH_avg, % | humidity's vote | 0 temperature first | 1 humidity first "
          "| 2 the higher |")
    print("|---|---|---|---|---|")
    for rh, vote, cells in priority_matrix(s, law):
        print("| %d | %s | %d | %d | %d |"
              % (rh, "abstains" if vote < 0 else "step %d" % vote, *cells))

    print("\n### a muggy afternoon")
    muggy_end = (RH_HIGH - RH_LOW) * RH_RAMP_MIN * 2 + RH_FLAT_MIN + 20
    rows, act = run(s, law, lambda m: (RH_WARM_C, rh_ramp_at(m)), muggy_end)
    timeline(rows, "rh")
    print("\nDrives: M1 %d, M2 %d, M3 %d (%d of them to a target)."
          % (act.ch[0].n.starts, act.ch[1].n.starts, act.ch[2].n.starts, act.ch[2].n.targets))
    rows1, act1 = run(s, VentModel("stepped"), lambda m: (RH_WARM_C, rh_ramp_at(m)),
                      muggy_end, m3_linear=False)
    m3_rows = [r for r in rows1[1:] if "M3" in r[4] and "moving" not in r[4]]
    print("Mode 1 on the same afternoon: %s. Drives: M1 %d, M2 %d, M3 %d."
          % ("; ".join("M3 %s at %d:%02d" % ("open" if "open" in r[4] else "shut",
                                             int(r[0]), round(r[0] % 1 * 60))
                       for r in m3_rows),
             act1.ch[0].n.starts, act1.ch[1].n.starts, act1.ch[2].n.starts))
    return 0


if __name__ == "__main__":
    sys.exit(main())

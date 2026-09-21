"""
test_linear_m3.py -- the linear M3 (firmware.LinearChannel) against the firmware.

    python model/closedloop/test_linear_m3.py [--quick]

The emulation follows 2.12.0 as built (045a39c): T2's ch_start_target(),
ch_target_tick() and the overrun lead in relay_controller.cpp, T17's settle read
in window_pos_task.cpp, and T6's plan_target(), apply_model_output() and
judge_m3_target() in climate_control.cpp. There is no mode 2 log to compare with
yet, so these check the emulation against those rules, in three layers:

  1. the channel alone: the stop rule, the run-on and the settle read, the
     lead and its learning (on the rig's 13 s stroke, against a control with no
     lead), a new stop point, a reversal refused mid-stroke (gh#48) and the
     target it loses, T3 taking the window, targets at the ends and inside the
     band, the mode 2 dwell, T17's readings and their age, and what the plant
     is handed;
  2. T6's side, with a test-double law: a target for a digital window, mode 1
     with a fitted sensor, the deadband, the minimum interval, the last target
     and its result as T6 infers it, and every narrowing move first;
  3. the closed loop (skipped by --quick): with stepped a fitted sensor
     changes nothing (mode 1); with a test-double mode 2 law M3 rests
     part-open and the plant is handed its opening.

The test-double laws are Python on purpose. They exercise the caller and are
not laws; laws come from drivers/ventModel.

ASCII-only output (Windows console is cp1252 -- see memory/gotcha-log.md).
"""

from __future__ import annotations

import sys
from argparse import Namespace
from pathlib import Path

HERE = Path(__file__).resolve().parent
for p in (HERE, HERE.parent):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

from firmware import (  # noqa: E402
    CH_CLOSED, CH_MOVING_OPEN, CH_OPEN, CH_STOPPED, LEAD_DEFAULT_MS, PROFILE_CURRENT,
    SRC_T3, SRC_T6, T17_IDLE_READ_MS, T17_IDLE_TICK_MS, T17_SETTLE_BASE_MS, Actuator,
    Channel, Controller, LinearChannel, LinearM3, Settings, t17_poll_ms, t17_window_ms,
)
from ventmodel import (  # noqa: E402
    VENT_ACT_CLOSE, VENT_ACT_HOLD, VENT_ACT_OPEN, VENT_ACT_TARGET, VENT_CAP_DIGITAL,
    VENT_CAP_LINEAR, VENT_RES_DONE, VENT_RES_FAIL_TIMEOUT, VENT_RES_NONE, VENT_WIN_PART_OPEN,
    VentOut,
)

T0 = 10_000_000                    # ms on the simulator's clock
TRAVEL, DWELL_OPEN, DWELL_CLOSE = 171, 1500, 600     # production M3 (Settings defaults)
TRAVERSE_MS = TRAVEL * 1000 + 5000                   # the leaf: travel + the 5 s margin
POLL, WINDOW = t17_poll_ms(TRAVEL), t17_window_ms(TRAVEL)
SETTLE = POLL + T17_SETTLE_BASE_MS + WINDOW          # a cut to its settle read
DZ = LinearM3().deadzone_x10(20)                     # deadzone_m3 20 mm over 1500 mm
LEAD0 = (LEAD_DEFAULT_MS * 10000 // (TRAVEL * 1000) + 5) // 10    # 0.1 %, unlearned
COAST = LEAD_DEFAULT_MS * 1000.0 / TRAVERSE_MS       # the run-on, 0.1 % of the stroke

FAILS = []


def check(name, ok, detail=""):
    print("  %-4s %s%s" % ("ok" if ok else "FAIL", name, ("  (%s)" % detail) if detail else ""))
    if not ok:
        FAILS.append(name)


def m3(cfg=None, travel=TRAVEL, traverse_s=None):
    """M3 in mode 2 with no minimum interval, so the stop rule is seen alone."""
    ch = LinearChannel(travel, DWELL_OPEN, DWELL_CLOSE, PROFILE_CURRENT, traverse_s,
                       cfg or LinearM3(min_interval_s=0))
    ch.advance(T0)
    return ch


def run_until(ch, t, done, limit_ms=600_000, step=50):
    end = t + limit_ms
    while not done(ch) and t < end:
        t += step
        ch.advance(t)
    return t


def settle(ch):
    """Run on past the settle read of the drive that just ended."""
    ch.advance(ch.drive_end + SETTLE)
    return ch.drive_end + SETTLE


def expected_cut(start_ms, from_x10, aim, opening=True):
    """The first reading at or past the aim: T17 reads 0.25 s in, then every POLL."""
    k = 0
    while True:
        t = T17_IDLE_TICK_MS // 2 + k * POLL
        frac = from_x10 / 1000.0 + (1 if opening else -1) * t / TRAVERSE_MS
        r = int(frac * 1000.0 + 0.5)
        if (opening and r >= aim) or (not opening and r <= aim):
            return start_ms + t, r, frac * 1000.0
        k += 1


# ==========================================================================
# 1. the channel
# ==========================================================================

def layer_channel():
    print("1. the channel: production M3, %d s traverse, a reading every %d ms, deadband %d,"
          " lead %d until learned (0.1 %%)" % (TRAVERSE_MS // 1000, POLL, DZ, LEAD0))

    ch = m3()
    r = ch.command_target(500, T0, SRC_T6, DZ)
    t_exp, r_exp, at_cut = expected_cut(T0, 0, 500 - LEAD0)
    run_until(ch, T0, lambda c: c.state == CH_STOPPED)
    check("a part-open target starts a drive", r == "start")
    check("it is cut at the first reading at or past the aim, one lead short of the target",
          ch.drive_end == t_exp and ch.reading == r_exp, "cut %d ms in at %d, expected %d at %d"
          % (ch.drive_end - T0, ch.reading, t_exp - T0, r_exp))
    stop = ch.drive_end
    ch.advance(stop + SETTLE - 1)
    before = (ch.reading, ch.read_t)
    ch.advance(stop + SETTLE)
    rest = at_cut + COAST
    check("the leaf runs on %d ms, and T17 reads where it rests one poll, 1 s and a window"
          " after the cut" % LEAD_DEFAULT_MS,
          before == (r_exp, t_exp) and ch.read_t == stop + SETTLE
          and ch.reading == int(rest + 0.5) and abs(ch.pos * 1000 - rest) < 1e-6,
          "read %d at +%d ms, leaf at %.2f" % (ch.reading, ch.read_t - stop, ch.pos * 1000))
    check("it rests within the band, past the target by the run-on less the lead",
          abs(ch.reading - 500) <= DZ and ch.landings == [ch.reading - 500],
          "rests at %d" % ch.reading)
    over = (ch.reading - (500 - LEAD0)) * 10
    lead0 = LEAD_DEFAULT_MS * 10000 // (TRAVEL * 1000)
    check("the settle read teaches the opening lead, at half weight",
          ch.learned == [1, 0] and ch.lead_x100[0] == lead0 + (over - lead0) // 2,
          "lead %d from %d, overrun %d (0.01 %%)" % (ch.lead_x100[0], lead0, over))
    check("at rest part-open: the law sees PART_OPEN", ch.public == VENT_WIN_PART_OPEN)
    now = stop + SETTLE + 20_000
    pos, age = ch.inputs(now)
    check("20 s after the settle read: the position, its age, and the time since the drive",
          pos == ch.reading and age == 20_000 and ch.ms_since_move(now) == SETTLE + 20_000,
          "age %d since %d" % (age, ch.ms_since_move(now)))
    _, age = ch.inputs(stop + SETTLE + T17_IDLE_READ_MS + 5_000)
    check("at rest T17 reads every 30 s", age == 5_000, "age %d" % age)

    # the plant is handed the leaf's mean position; at rest, the position itself
    ch.take_mean(stop + 40_000)
    held = ch.take_mean(stop + 340_000)
    check("the plant gets the part-open leaf", abs(held - ch.pos) < 1e-12 and
          0.49 < held < 0.51, "%.4f" % held)

    # a new stop point the way the leaf already goes: no new start
    t = stop + 400_000
    ch.command_target(800, t, SRC_T6, DZ)
    starts = ch.n.starts
    ages = []
    for _ in range(20):
        t += 1000
        ch.advance(t)
        ages.append(ch.inputs(t)[1])
    r = ch.command_target(700, t, SRC_T6, DZ)
    run_until(ch, t, lambda c: c.state == CH_STOPPED)
    settle(ch)
    check("moving, T17's reading is at most one poll old", max(ages) <= POLL,
          "max %d ms" % max(ages))
    check("a nearer target the same way moves the stop point", r == "retarget" and
          ch.n.starts == starts and ch.n.retargets == 1 and abs(ch.reading - 700) <= DZ,
          "rests at %d" % ch.reading)
    r = ch.command_target(ch.reading + DZ, ch.drive_end + 60_000, SRC_T6, DZ)
    check("a target within the band of the leaf is already there", r == "noop" and
          ch.n.starts == starts)

    # the other way mid-stroke: refused for T6 from 2.3.1 (gh#48) -- and, as built,
    # ch_start_close() disarms the target before it defers, so the drive runs on
    t = ch.drive_end + 60_000
    ch.command_target(950, t, SRC_T6, DZ)
    t += 10_000
    r = ch.command_target(300, t, SRC_T6, DZ)
    run_until(ch, t, lambda c: c.state not in (CH_MOVING_OPEN,))
    check("a reversal mid-stroke is deferred for T6 (gh#48), and the target under way is"
          " lost: the leaf runs on to OPEN (045a39c as built)",
          r == "deferred-travel" and ch.n.travel_defers == 1 and ch.n.lost == 1 and
          ch.state == CH_OPEN and ch.pos == 1.0, "%s, state %d" % (r, ch.state))

    # T3 takes a T6 drive: its full-travel close disarms the target and closes
    t = ch.drive_end + 60_000
    ch.command_target(300, t, SRC_T6, DZ)
    t += 20_000
    r = ch.command(False, t, SRC_T3)
    run_until(ch, t, lambda c: c.state == CH_CLOSED)
    check("T3's close takes a T6 drive to its target: closed on the timer", r == "noop" and
          ch.n.aborts == 1 and ch.target is None and ch.pos == 0.0, "%s" % r)

    # the ends: a target within the band of an end is that end, on to the timer
    t = ch.drive_end + 1000
    r = ch.command_target(1000 - DZ, t, SRC_T6, DZ)
    t_start = t
    run_until(ch, t, lambda c: c.state == CH_OPEN)
    check("a target within the band of OPEN drives on to the timer", r == "start" and
          ch.drive_end == t_start + TRAVERSE_MS and ch.pos == 1.0 and ch.n.timeouts == 0)
    t = ch.drive_end + 1000
    r = ch.command_target(DZ, t, SRC_T6, DZ)
    run_until(ch, t, lambda c: c.state == CH_CLOSED)
    check("... and one within the band of CLOSED closes", r == "start" and ch.pos == 0.0)

    # mode 2: min_intv_m3 is BOTH of M3's dwells, armed at every end of a drive
    ch = m3(LinearM3(min_interval_s=300))
    check("mode 2: both dwells are min_intv_m3", ch.dwell_open_ms == ch.dwell_close_ms ==
          300_000, "%d / %d" % (ch.dwell_open_ms, ch.dwell_close_ms))
    ch.command_target(0, T0, SRC_T6, DZ)
    ch.command(True, T0, SRC_T3)                        # full open, T3 past any dwell
    run_until(ch, T0, lambda c: c.state == CH_OPEN)
    r1 = ch.command_target(400, ch.drive_end + 299_000, SRC_T6, DZ)
    r2 = ch.command_target(400, ch.drive_end + 300_000, SRC_T6, DZ)
    run_until(ch, ch.drive_end + 300_000, lambda c: c.state == CH_STOPPED)
    stop = ch.drive_end
    r3 = ch.command_target(700, stop + 299_000, SRC_T6, DZ)
    r4 = ch.command_target(100, stop + 299_500, SRC_T6, DZ)
    r5 = ch.command_target(100, stop + 300_000, SRC_T6, DZ)
    run_until(ch, stop + 300_000, lambda c: c.state == CH_STOPPED)
    check("after OPEN the dwell is 300 s, not the open dwell of 1500", r1 == "deferred-dwell"
          and r2 == "start", "%s, %s" % (r1, r2))
    check("part-open, the dwell holds either way, then lets T6 go", r3 == r4 ==
          "deferred-dwell" and r5 == "start" and ch.n.dwell_defers == 2, "%s %s %s"
          % (r3, r4, r5))

    # mode 1: a fitted sensor changes nothing that acts
    a = Channel(TRAVEL, DWELL_OPEN, DWELL_CLOSE, PROFILE_CURRENT)
    b = LinearChannel(TRAVEL, DWELL_OPEN, DWELL_CLOSE, PROFILE_CURRENT, None,
                      LinearM3(mode2=False))
    script = [(0, True, SRC_T6), (60_000, False, SRC_T6), (240_000, False, SRC_T6),
              (250_000, True, SRC_T6), (300_000, True, SRC_T3), (330_000, False, SRC_T3),
              (700_000, True, SRC_T6), (2_500_000, True, SRC_T6), (2_600_000, False, SRC_T6)]
    same, todo = True, list(script)
    for t in range(T0, T0 + 3_200_000, 7_000):
        while todo and T0 + todo[0][0] <= t:
            dt, want, src = todo.pop(0)
            same = same and a.command(want, T0 + dt, src) == b.command(want, T0 + dt, src)
        a.advance(t)
        b.advance(t)
        same = same and (a.state, a.pos, a.dwell_deadline) == (b.state, b.pos, b.dwell_deadline)
        same = same and a.take_mean(t) == b.take_mean(t)
    check("mode 1: OPEN and CLOSE move it exactly as a binary M3, with its own dwells",
          same and a.n.starts == b.n.starts and a.n.reversals == b.n.reversals and
          (b.dwell_open_ms, b.dwell_close_ms) == (DWELL_OPEN * 1000, DWELL_CLOSE * 1000))

    # the airflow exponent: the plant is handed opening ** exp, integrated exactly
    c = Channel(TRAVEL, DWELL_OPEN, DWELL_CLOSE, PROFILE_CURRENT)
    c.flow_exp = 2.0
    c.advance(T0)
    c.take_mean(T0)
    c.command(True, T0, SRC_T6)
    stroke = c.take_mean(T0 + TRAVERSE_MS)          # the whole stroke, 0 -> 1
    ch = m3()
    ch.flow_exp = 2.0
    ch.command_target(500, T0, SRC_T6, DZ)
    run_until(ch, T0, lambda c: c.state == CH_STOPPED)
    ch.take_mean(ch.drive_end + 1000)
    held = ch.take_mean(ch.drive_end + 301_000)
    check("airflow exponent 2: a stroke gives 1/3, a part-open leaf its opening squared",
          abs(stroke - 1 / 3) < 1e-9 and abs(held - ch.pos ** 2) < 1e-12,
          "stroke %.6f, held %.4f at %.4f" % (stroke, held, ch.pos))


def layer_rig():
    """The lead on the rig's 13 s window, where it matters: the run-on is 3.2 %
    of the stroke, against a band of 1.3 %."""
    rig_poll = t17_poll_ms(13)
    print("1b. the lead on the rig: 13 s traverse, travel_m3 13, a reading every %d ms"
          % rig_poll)

    def moves(ch, n=10):
        t, want = T0, 300
        for _ in range(n):
            ch.command_target(want, t, SRC_T6, DZ)
            t = run_until(ch, t, lambda c: c.state == CH_STOPPED, step=10)
            t = ch.drive_end + 5_000
            ch.advance(t)
            want = 600 if want == 300 else 300
        return ch.landings

    led = moves(m3(travel=13, traverse_s=13))
    bare = m3(travel=13, traverse_s=13)
    bare._lead_x100 = lambda opening: 0          # the cut ON the target: no lead
    unled = moves(bare)
    check("with the lead, every stop rests within +-1 % of its target (FR-WP05)",
          len(led) == 10 and all(abs(e) <= 10 for e in led), str(led))
    check("the control, no lead: every stop runs about the run-on past it",
          len(unled) == 10 and all(abs(e) >= 25 for e in unled), str(unled))
    ch = m3(travel=13, traverse_s=13)
    moves(ch)
    check("both directions learn their own lead", ch.learned[0] >= 4 and ch.learned[1] >= 4
          and all(300 <= x <= 420 for x in ch.lead_x100), "%s from %s" % (ch.lead_x100,
                                                                           ch.learned))


# ==========================================================================
# 2. T6's side
# ==========================================================================

class DoubleLaw:
    """A test double: returns what plan(vin) says, and keeps the last vin."""
    name, version = "double", 1

    def __init__(self, plan):
        self.plan = plan
        self.out = VentOut()
        self.seen = None

    def reset(self):
        pass

    def step(self, vin):
        self.seen = (vin.win[2].cap, vin.win[2].pos_x10, vin.win[2].pos_age_ms,
                     vin.win[2].last_target_x10, vin.win[2].last_result,
                     vin.m3_deadzone_x10, vin.m3_min_interval_ms)
        out = self.out
        out.step = out.step_t = out.step_rh = -1
        for i, (act, tgt) in enumerate(self.plan(vin)):
            out.win[i].action = act
            out.win[i].target_x10 = tgt
        return out


MEAS = {"t_c10": 300, "t_avg_c10": 300, "t_avg_c": 30, "rh_pct": 60, "rh_avg_pct": 60}


class Recorder(Actuator):
    """An Actuator that notes the order of the commands it is given."""

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.calls = []

    def command(self, i, want_open, now, source=SRC_T6):
        self.calls.append((i, "open" if want_open else "close"))
        return super().command(i, want_open, now, source)

    def command_target(self, i, target, now, source=SRC_T6, dz_x10=0):
        self.calls.append((i, target))
        return super().command_target(i, target, now, source, dz_x10)


def cycle_until(ctl, act, t, done, every=30_000, limit=600):
    for _ in range(limit):
        if done():
            break
        t += every
        act.advance(t)
        ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    return t


def layer_t6():
    print("2. T6's side, with a test-double law")
    s = Settings()
    hold = (VENT_ACT_HOLD, 0)

    law = DoubleLaw(lambda v: [hold, hold, (VENT_ACT_TARGET, 600)])
    act = Actuator(s, PROFILE_CURRENT)
    act.advance(T0)
    d = Controller(law, s).cycle(T0, 0, True, MEAS, False, actuator=act)
    check("a target for a digital M3 is a model error, and nothing moves",
          d.model_errors == 1 and act.ch[2].state == CH_CLOSED)

    act = Actuator(s, PROFILE_CURRENT, m3_linear=LinearM3(mode2=False))
    act.advance(T0)
    d = Controller(law, s).cycle(T0, 0, True, MEAS, False, actuator=act)
    check("mode 1, sensor fitted: M3 is DIGITAL to the law, with its position, and a target"
          " is a model error", law.seen[0] == VENT_CAP_DIGITAL and law.seen[1] == 0 and
          d.model_errors == 1 and act.ch[2].n.starts == 0, str(law.seen))

    law = DoubleLaw(lambda v: [hold, hold, (VENT_ACT_TARGET, 10)])
    act = Actuator(s, PROFILE_CURRENT, m3_linear=LinearM3(min_interval_s=0))
    act.advance(T0)
    ctl = Controller(law, s)
    d = ctl.cycle(T0, 0, True, MEAS, False, actuator=act)
    check("the law is handed M3's capability, position and the deadband",
          law.seen[0] == VENT_CAP_LINEAR and law.seen[1] == 0 and law.seen[5] == DZ and
          law.seen[6] == 0, str(law.seen))
    check("a target within the deadband of M3 at rest is dropped", d.dropped == 1 and
          act.ch[2].state == CH_CLOSED and act.ch[2].n.starts == 0)

    # the minimum interval: 600 s after a drive ends, then the target goes out
    law = DoubleLaw(lambda v: [hold, hold, (VENT_ACT_TARGET, target[0])])
    target = [500]
    act = Actuator(s, PROFILE_CURRENT, m3_linear=LinearM3(min_interval_s=600))
    act.advance(T0)
    ctl = Controller(law, s)
    ctl.cycle(T0, 0, True, MEAS, False, actuator=act)
    cycle_until(ctl, act, T0, lambda: act.ch[2].state == CH_STOPPED)
    stop = act.ch[2].drive_end
    target[0] = 800
    t = stop + 100_000
    act.advance(t)
    d1 = ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    t = stop + 601_000
    act.advance(t)
    d2 = ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    check("the minimum interval holds a target, then lets it go",
          d1.deferred == 1 and d2.deferred == 0 and act.ch[2].target == 800,
          "deferred %d then %d" % (d1.deferred, d2.deferred))
    check("the law sees its last target, DONE as T6 judges it at rest, and the 600 s",
          law.seen[3] == 500 and law.seen[4] == VENT_RES_DONE and law.seen[6] == 600_000,
          str(law.seen))
    t += 30_000
    act.advance(t)
    ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    check("while M3 moves T6 holds the new target outstanding",
          ctl.m3_last_target == 800 and ctl.m3_last_result == VENT_RES_NONE and
          law.seen[4] == VENT_RES_NONE)

    # T3 takes the window: T6 infers FAIL_TIMEOUT from where it rests, never ABORTED
    first = [True]

    def once(v):
        want = (VENT_ACT_TARGET, 700) if first[0] else hold
        first[0] = False
        return [hold, hold, want]

    law = DoubleLaw(once)
    act = Actuator(s, PROFILE_CURRENT, m3_linear=LinearM3(min_interval_s=0))
    act.advance(T0)
    ctl = Controller(law, s)
    ctl.cycle(T0, 0, True, MEAS, False, actuator=act)
    act.close_all(T0 + 20_000, SRC_T3)
    t = cycle_until(ctl, act, T0 + 20_000, lambda: act.ch[2].state == CH_CLOSED)
    t += 30_000
    act.advance(t)
    ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    check("T3 takes the target's drive: the law is told FAIL_TIMEOUT once M3 rests",
          law.seen[3] == 700 and law.seen[4] == VENT_RES_FAIL_TIMEOUT and
          act.ch[2].n.aborts == 1, str(law.seen))

    # every narrowing move before any widening one, each pass in window order
    for m3_target, want in ((200, [(0, "close"), (2, 200), (1, "open")]),
                            (900, [(0, "close"), (1, "open"), (2, 900)])):
        act = Recorder(s, PROFILE_CURRENT, m3_linear=LinearM3(min_interval_s=0))
        for i in (0, 2):
            act.ch[i].sync(CH_OPEN, T0)
        act.advance(T0 + 5000)                                  # T17 has read it open
        act.ch[2].command_target(500, T0 + 5000, SRC_T3, DZ)     # park M3 part-open
        t = run_until(act.ch[2], T0 + 5000, lambda c: c.state == CH_STOPPED) + 5000
        act.advance(t)
        act.calls = []
        law = DoubleLaw(lambda v: [(VENT_ACT_CLOSE, 0), (VENT_ACT_OPEN, 0),
                                   (VENT_ACT_TARGET, m3_target)])
        Controller(law, s).cycle(t, 0, True, MEAS, False, actuator=act)
        check("M3 to %d: the narrowing pass, then the widening one" % m3_target,
              act.calls == want, str(act.calls))


# ==========================================================================
# 3. the closed loop
# ==========================================================================

def ramp_law(vin):
    """A test double for mode 2: M1 and M2 at two fixed steps, M3 proportional
    over the 3 degC above the threshold. Not a law: it only moves M3 part-way."""
    dev = vin.t_avg_c10 - vin.t_max_c10
    m1 = VENT_ACT_OPEN if dev > 0 else VENT_ACT_CLOSE
    m2 = VENT_ACT_OPEN if dev > 10 else VENT_ACT_CLOSE
    return [(m1, 0), (m2, 0), (VENT_ACT_TARGET, max(0, min(1000, dev * 1000 // 30)))]


def layer_loop():
    import bisect
    from datetime import datetime

    import closed_loop as cl
    import dataset as dsmod
    print("3. the closed loop, 2026-08-01 .. 08-03, the primary plant, today's firmware")
    ds = dsmod.build()
    lo = bisect.bisect_left(ds.t, datetime(2026, 8, 1))
    hi = bisect.bisect_left(ds.t, datetime(2026, 8, 4))
    p2 = (HERE.parent / "campaign-summer-2026" / "plant2" /
          "plant2_summer2026_Ca2.9_tau120_tau90_ev5_dir.json")
    import json
    params = json.loads(p2.read_text())["params"]

    def args(fitted, mode2=False):
        sets = (["wpos_fitted_m3=1"] if fitted else []) + (["ctrl_mode_m3=1"] if mode2 else [])
        return Namespace(model="stepped", warmup_h=6.0, rh_from_log=False,
                         calibrator_hold=False, openness="state", t3="sim", daynight="sim",
                         firmware="current", config=None, plant2=str(p2),
                         set=sets or None, m3_span_mm=1500)

    keys = ("T_sim", "RH_sim", "bm_sim", "pos_m3", "step", "step_t", "step_rh", "override")
    runs = []
    for fitted in (False, True):
        a = args(fitted)
        recs, act, ctl = cl.run_closed_loop(ds, lo, hi, "two", params, a,
                                            cl.schedule_from_args(a))
        runs.append((recs, act))
    same = all(all(r0[k] == r1[k] for k in keys) for r0, r1 in zip(runs[0][0], runs[1][0]))
    check("mode 1 (stepped): a fitted sensor changes nothing in the loop",
          same and len(runs[0][0]) == len(runs[1][0]) and runs[1][1].m3_linear is not None
          and not runs[1][1].m3_linear.mode2, "%d samples" % len(runs[0][0]))

    law = DoubleLaw(ramp_law)
    a = args(True, mode2=True)
    recs, act, ctl = cl.run_closed_loop(ds, lo, hi, "two", params, a, cl.schedule_from_args(a),
                                        law=law)
    ch = act.ch[2]
    pos = [r["pos_m3"] for r in recs]
    part = sum(1 for p in pos if 0.02 < p < 0.98) / len(pos)
    at_rest_part = sum(1 for r in recs if 0.02 < r["pos_m3"] < 0.98 and
                       (r["bm_sim"] >> 4) & 3 == 2)
    dT = max(abs(r0["T_sim"] - r1["T_sim"]) for r0, r1 in zip(runs[0][0], recs))
    check("mode 2 (a test double): M3 rests part-open and the plant follows",
          act.m3_linear.mode2 and ctl.model_errors == ctl.n0[2] and ch.n.targets > 0 and
          at_rest_part > 0 and all(0.0 <= p <= 1.0 for p in pos) and dT > 0.1,
          "part-open %.0f %% of samples, %d targets, %d drives, T differs by up to %.1f degC"
          % (100 * part, ch.n.targets, ch.n.starts, dT))
    check("... and its targeted stops settle within the band",
          ch.landings and all(abs(e) <= DZ for e in ch.landings),
          "%d stops, worst %d" % (len(ch.landings), max(abs(e) for e in ch.landings)
                                  if ch.landings else 0))
    cl.linear_summary(recs, act, ctl)


def main(argv=None):
    quick = "--quick" in (argv if argv is not None else sys.argv[1:])
    layer_channel()
    layer_rig()
    layer_t6()
    if not quick:
        layer_loop()
    print("\n%s" % ("PASS" if not FAILS else "FAIL: " + ", ".join(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

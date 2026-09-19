"""
test_linear_m3.py -- the linear M3 (firmware.LinearChannel) against its design.

    python model/closedloop/test_linear_m3.py [--quick]

There is no firmware to compare with yet: T2's target path is 2.11.0 (plan
§5b). So these check the emulation against the contract (§3, §7) and the
plan's stop rule, not against a log, in three layers:

  1. the channel alone: the stop rule and its bound, a new stop point, a
     reversal refused mid-stroke (gh#48), T3 taking the window, the ends, the
     dwell, T17's readings and their age, and what the plant is handed;
  2. T6's side, with a test-double law: a target for a digital window, the
     deadband, the minimum interval, and every narrowing move first;
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
    CH_CLOSED, CH_OPEN, CH_STOPPED, PROFILE_CURRENT, SRC_T3, SRC_T6, Actuator, Channel,
    Controller, LinearChannel, LinearM3, Settings, t17_poll_ms,
)
from ventmodel import (  # noqa: E402
    VENT_ACT_CLOSE, VENT_ACT_HOLD, VENT_ACT_OPEN, VENT_ACT_TARGET, VENT_CAP_LINEAR,
    VENT_RES_ABORTED, VENT_RES_DONE, VENT_RES_NONE, VENT_WIN_OPEN, VentOut,
)

T0 = 10_000_000                    # ms on the simulator's clock
TRAVEL, DWELL_OPEN, DWELL_CLOSE = 171, 1500, 600     # production M3 (Settings defaults)
TRAVERSE_MS = TRAVEL * 1000 + 5000                   # the leaf: travel + the 5 s margin
POLL = t17_poll_ms(TRAVEL)
DZ = LinearM3().deadzone_x10(20)                     # deadzone_m3 20 mm over 1500 mm

FAILS = []


def check(name, ok, detail=""):
    print("  %-4s %s%s" % ("ok" if ok else "FAIL", name, ("  (%s)" % detail) if detail else ""))
    if not ok:
        FAILS.append(name)


def m3(cfg=None):
    ch = LinearChannel(TRAVEL, DWELL_OPEN, DWELL_CLOSE, PROFILE_CURRENT, None, cfg or LinearM3())
    ch.advance(T0)
    return ch


def run_until(ch, t, done, limit_ms=600_000, step=50):
    end = t + limit_ms
    while not done(ch) and t < end:
        t += step
        ch.advance(t)
    return t


def expected_stop(start_ms, from_x10, target, opening=True):
    """The first reading within the band: readings every POLL from the start."""
    k = 1
    while True:
        frac = from_x10 / 1000.0 + (1 if opening else -1) * k * POLL / TRAVERSE_MS
        r = int(frac * 1000.0 + 0.5)
        if (opening and r >= target - DZ) or (not opening and r <= target + DZ):
            return start_ms + k * POLL, r
        k += 1


# ==========================================================================
# 1. the channel
# ==========================================================================

def layer_channel():
    print("1. the channel: production M3, %d s traverse, a reading every %d ms, deadband %d"
          % (TRAVERSE_MS // 1000, POLL, DZ))
    one_step = -(-POLL * 1000 // TRAVERSE_MS)         # a reading's travel, 0.1 %, rounded up

    ch = m3()
    r = ch.command_target(500, T0, SRC_T6, DZ)
    t_exp, r_exp = expected_stop(T0, 0, 500)
    run_until(ch, T0, lambda c: c.state == CH_STOPPED)
    check("a part-open target starts a drive", r == "start")
    check("it stops at the first reading within the band", ch.drive_end == t_exp and
          ch.reading == r_exp, "stopped %d ms in at %d, expected %d at %d"
          % (ch.drive_end - T0, ch.reading, t_exp - T0, r_exp))
    check("the stop lies within the band plus one reading's travel",
          500 - DZ <= ch.reading <= 500 - DZ + one_step, "%d in %d..%d"
          % (ch.reading, 500 - DZ, 500 - DZ + one_step))
    check("at rest part-open: the law sees OPEN, DONE, the target", ch.public == VENT_WIN_OPEN
          and ch.result == VENT_RES_DONE and ch.last_target == 500)
    stop = ch.drive_end
    cap, pos, age, last, res, since = ch.inputs(stop + 20_000)
    check("20 s after the stop: position and ages", cap == VENT_CAP_LINEAR and
          pos == ch.reading and age == 20_000 and since == 20_000, "age %d since %d" % (age, since))
    _, _, age, _, _, _ = ch.inputs(stop + 35_000)
    check("at rest T17 reads every 30 s", age == 5_000, "age %d" % age)

    # the plant is handed the leaf's mean position; at rest, the position itself
    ch.take_mean(stop + 40_000)
    held = ch.take_mean(stop + 340_000)
    check("the plant gets the part-open leaf", abs(held - ch.pos) < 1e-12 and
          0.48 < held < 0.50, "%.4f" % held)

    # a new stop point the way the leaf already goes: no new start
    t = stop + 400_000
    ch.command_target(800, t, SRC_T6, DZ)
    starts = ch.n.starts
    ages = []
    for _ in range(20):
        t += 1000
        ch.advance(t)
        ages.append(ch.inputs(t)[2])
    r = ch.command_target(700, t, SRC_T6, DZ)
    run_until(ch, t, lambda c: c.state == CH_STOPPED)
    check("moving, T17's reading is at most one poll old", max(ages) <= POLL,
          "max %d ms" % max(ages))
    check("a nearer target the same way moves the stop point", r == "retarget" and
          ch.n.starts == starts and 700 - DZ <= ch.reading <= 700 - DZ + one_step,
          "stopped at %d" % ch.reading)

    # the other way mid-stroke: refused for T6 from 2.3.1 (gh#48), re-issued later
    t = ch.drive_end + 60_000
    ch.command_target(950, t, SRC_T6, DZ)
    t += 10_000
    r = ch.command_target(300, t, SRC_T6, DZ)
    run_until(ch, t, lambda c: c.state == CH_STOPPED)
    check("a reversal mid-stroke is refused for T6 (gh#48)", r == "deferred-travel" and
          ch.n.travel_defers == 1 and ch.reading >= 950 - DZ, "stopped at %d" % ch.reading)

    # T3 takes the window from a T6 drive: ABORTED, and the leaf closes on the timer
    t = ch.drive_end + 60_000
    ch.command_target(300, t, SRC_T6, DZ)
    t += 20_000
    r = ch.command(False, t, SRC_T3)
    run_until(ch, t, lambda c: c.state == CH_CLOSED)
    check("T3's close-all takes a T6 drive: ABORTED", r == "retarget" and
          ch.result == VENT_RES_ABORTED and ch.last_target == 300 and ch.pos == 0.0,
          "%s, result %d" % (r, ch.result))

    # the ends: on to the timer. Mode 2: no open dwell, the close dwell stays
    t = ch.drive_end + DWELL_CLOSE * 1000 + 1000        # T3's close armed the close dwell
    r = ch.command_target(1000, t, SRC_T6, DZ)
    t_start = t
    run_until(ch, t, lambda c: c.state == CH_OPEN)
    check("target 1000 drives on to the timer", r == "start" and
          ch.drive_end == t_start + TRAVERSE_MS and ch.pos == 1.0 and ch.result == VENT_RES_DONE)
    t = ch.drive_end + 1000
    r = ch.command_target(600, t, SRC_T6, DZ)
    check("mode 2: no open dwell, a target right after OPEN moves", r == "start")
    run_until(ch, t, lambda c: c.state == CH_STOPPED)
    t = ch.drive_end + 1000
    ch.command_target(0, t, SRC_T6, DZ)
    run_until(ch, t, lambda c: c.state == CH_CLOSED)
    t = ch.drive_end + 10_000
    r1 = ch.command_target(500, t, SRC_T6, DZ)
    r2 = ch.command_target(500, ch.drive_end + DWELL_CLOSE * 1000 + 1, SRC_T6, DZ)
    check("the close dwell still defers T6, then lets it go", r1 == "deferred-dwell" and
          r2 == "start" and ch.n.dwell_defers == 1, "%s, %s" % (r1, r2))

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
    check("mode 1: OPEN and CLOSE move it exactly as a binary M3", same and
          a.n.starts == b.n.starts and a.n.reversals == b.n.reversals)


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
                     vin.m3_deadzone_x10, vin.m3_min_move_ms)
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

    law = DoubleLaw(lambda v: [hold, hold, (VENT_ACT_TARGET, 10)])
    act = Actuator(s, PROFILE_CURRENT, m3_linear=LinearM3())
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
    act = Actuator(s, PROFILE_CURRENT, m3_linear=LinearM3(min_move_s=600))
    act.advance(T0)
    ctl = Controller(law, s)
    t = T0
    ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    while act.ch[2].state != CH_STOPPED:
        t += 30_000
        act.advance(t)
        ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    stop = act.ch[2].drive_end
    target[0] = 800
    t = stop + 100_000
    act.advance(t)
    d1 = ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    t = stop + 601_000
    act.advance(t)
    d2 = ctl.cycle(t, 0, True, MEAS, False, actuator=act)
    check("the minimum interval defers a target, then lets it go",
          d1.deferred == 1 and d2.deferred == 0 and act.ch[2].target == 800,
          "deferred %d then %d" % (d1.deferred, d2.deferred))
    check("the law sees its last target and how it ended; 600 s caps at the uint16 field",
          law.seen[3] == 500 and law.seen[4] == VENT_RES_DONE and law.seen[6] == 0xFFFF,
          str(law.seen))
    check("while M3 moves the law sees no result yet",
          act.ch[2].inputs(t + 1000)[4] == VENT_RES_NONE)

    # every narrowing move before any widening one (reconcile_to_step())
    for m3_target, want in ((200, [(0, "close"), (2, 200), (1, "open")]),
                            (900, [(0, "close"), (2, 900), (1, "open")])):
        act = Recorder(s, PROFILE_CURRENT, m3_linear=LinearM3())
        for i in (0, 2):
            act.ch[i].sync(CH_OPEN, T0)
        act.advance(T0 + 1000)
        act.ch[2].command_target(500, T0 + 1000, SRC_T3, DZ)     # park M3 part-open
        t = run_until(act.ch[2], T0 + 1000, lambda c: c.state == CH_STOPPED) + 1000
        act.advance(t)
        act.calls = []
        law = DoubleLaw(lambda v: [(VENT_ACT_CLOSE, 0), (VENT_ACT_OPEN, 0),
                                   (VENT_ACT_TARGET, m3_target)])
        Controller(law, s).cycle(t, 0, True, MEAS, False, actuator=act)
        narrowing = [c for c in act.calls if c[1] == "close" or
                     (c[1] != "open" and c[1] < 500)]
        first = act.calls[:len(narrowing)]
        check("M3 to %d: every narrowing move goes out first" % m3_target,
              first == narrowing and act.calls == want, str(act.calls))


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

    def args(fitted):
        return Namespace(model="stepped", warmup_h=6.0, rh_from_log=False,
                         calibrator_hold=False, openness="state", t3="sim", daynight="sim",
                         firmware="current", config=None, plant2=str(p2),
                         set=["wpos_fitted_m3=1"] if fitted else None,
                         m3_span_mm=1500, m3_min_move_s=0)

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
    a = args(True)
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
    cl.linear_summary(recs, act, ctl)


def main(argv=None):
    quick = "--quick" in (argv if argv is not None else sys.argv[1:])
    layer_channel()
    layer_t6()
    if not quick:
        layer_loop()
    print("\n%s" % ("PASS" if not FAILS else "FAIL: " + ", ".join(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

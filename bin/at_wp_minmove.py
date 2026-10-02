#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Floor 2 of M3's minimum-move deadband: the shortest relay pulse that
actually moves the leaf (design/integrateWindowPositionSensor.md §3.6;
decisions 8 and 10).

WHY
---
Under linear control a correction smaller than the deadband is not worth
energising for, and the deadband has two floors. Floor 1 is the sampling
resolution. Floor 2 is the actuator's: "static friction and contactor make/break
time mean a short pulse may move nothing at all". It was never measured, so the
deadband default is still a fixed 20 mm instead of a derived one. The plan's
experiment: command progressively shorter pulses, and find where displacement
stops tracking pulse width.

HOW
---
A bench build since 2026-10-02 has CMD_PULSE:
POST /api/diag/windowpos {"pulse_ms":N,"dir":"open"|"close"} energises M3's
relay for N ms and stops part-way, and GET ?pulses returns each pulse's real
width, timed at the relay GPIO's own edges. This script:
  - holds T6 off (STANDBY, which the hook requires) and puts M3 at 50 %;
  - for each width, longest first, gives REPS pulses OPEN then REPS CLOSE,
    so the leaf stays mid-stroke, and does it PASSES times;
  - starts each block with one uncounted TAKE-UP pulse (200 ms) in the block's
    direction. The rope has ~10 mm of slack, so after a reversal a short pulse
    moves nothing until enough pulses have taken it up, and five 30 ms pulses
    cannot say whether 30 ms moves the leaf (the first run, 2026-10-02);
  - reads the leaf LIVE (the diag route's direct device read) once it has
    settled after every pulse, so each pulse's displacement is after - before;
  - checks before every pulse that nothing else holds or has moved M3, and
    recovers if something has (2026-10-02: the wind safe-fail closed it
    mid-run, and the run went on pulsing it against its end switch);
  - restores: M3 closed, AUTOMATIC (whose recalibration closes it anyway).
It also reads the leaf at rest a few times first: the spread of those readings
is the noise a "move" has to beat.

READING THE RESULT
------------------
Per width and direction: the displacement's mean, sigma and minimum, in mm and
in % of the taught window. Then a straight line through the widths that clearly
moved: displacement = speed x (width - dead time). Two answers come out:
  - **dead time**: where the line crosses zero, the pulse below which nothing
    moves (contactor make time, motor start, static friction);
  - **floor 2**: the shortest commanded width at which EVERY pulse moved more
    than the noise, and how far that moved it.
Directions are kept apart: the flap is paid out to open and lifted to close.
This is the RIG's mechanism; 5C88's motor and rope drum are their own.

Usage
  python bin/at_wp_minmove.py --host 192.168.20.160
  python bin/at_wp_minmove.py --host 192.168.20.160 --widths 200,100,50 --reps 3 --passes 1
"""
import argparse
import csv
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from at_wp02 import Rig, say                                   # noqa: E402

DEFAULT_HOST = "192.168.20.160"
DEFAULT_PIN = "12345678"
DEFAULT_WIDTHS = "300,200,150,120,100,80,60,50,40,30,20"
SETTLE_S = 1.5         # after the cut: the leaf coasts ~0.2 s, the encoder publishes every 100 ms
PULSE_SEEN_S = 5.0     # how long to wait for the pulse to appear in T2's log
NOISE_READS = 6


def mean(xs):
    return sum(xs) / float(len(xs)) if xs else float("nan")


def sd(xs):
    if len(xs) < 2:
        return float("nan")
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


class Pulser(object):
    def __init__(self, rig):
        self.rig = rig
        self.u = rig.u

    def live_mm(self):
        """A FRESH device read of the opening, 0.1 mm -> mm (None if it failed)."""
        d = self.u.diag() or {}
        v = d.get("opening_mm_x10")
        return None if (v is None or not d.get("ok")) else int(v) / 10.0

    def pulse_seq(self):
        sc, out = self.u._req("GET", "/api/diag/windowpos?pulses")
        if sc != 200 or not isinstance(out, dict) or "pulses" not in out:
            sys.exit("GET /api/diag/windowpos?pulses gave no pulse log (HTTP %s): "
                     "a bench build from before 2026-10-02?" % sc)
        return int(out.get("seq") or 0), out.get("pulses") or []

    def pulse(self, ms, opening):
        """One pulse; returns T2's record of it, or None if it never happened."""
        seq0, _ = self.pulse_seq()
        sc, out = self.u._req("POST", "/api/diag/windowpos",
                              {"pulse_ms": ms, "dir": "open" if opening else "close"})
        if sc != 200 or not isinstance(out, dict) or not out.get("ok"):
            sys.exit("the pulse hook refused (HTTP %s): %s" % (sc, out))
        end = time.time() + PULSE_SEEN_S + ms / 1000.0
        while time.time() < end:
            seq, recs = self.pulse_seq()
            if seq > seq0:
                rec = [r for r in recs if r.get("n") == seq0 + 1]
                return rec[0] if rec else None
            time.sleep(0.2)
        return None


HOLDING = ("wind_override", "motor_alarm", "calibrating")
LEFT_STANDBY = "the unit left STANDBY"
RECOVER_WAIT_S = 300
MID_LO_X10, MID_HI_X10 = 100, 900   # a pulse is only a measurement away from both ends


def held(rig):
    """What stops T2 taking a pulse now (and may move M3 by itself), or None."""
    m = (rig.u.status() or {}).get("mode") or {}
    for f in HOLDING:
        if f in (m.get("flags") or []):
            return "the unit shows '%s'" % f
    if str(m.get("current", "")).upper() != "STANDBY":
        return LEFT_STANDBY
    return None


def off_mid(rig):
    """Why M3 is not where a pulse means anything, or None. At an end the leaf
    is pressed into its stop: a pulse moves it nothing, or off the switch only,
    and the encoder then reports NOT FOLLOWING (bit 7) -- seen 2026-10-02."""
    w = (rig.u.status() or {}).get("windows") or {}
    if w.get("M3_at_end_sensor"):
        return "M3 is at an end sensor"
    pct = w.get("M3_percent_x10")
    if pct is not None and not (MID_LO_X10 <= int(pct) <= MID_HI_X10):
        return "M3 is at %.1f %%, too near an end" % (int(pct) / 10.0)
    return None


def window_mm(rig):
    sc, c = rig.u._req("GET", "/api/diag/commission")
    if sc == 200 and isinstance(c, dict) and c.get("window_mm"):
        return float(c["window_mm"])
    return None


def fit(points):
    """Least squares through (width_ms, disp_mm): returns (slope, intercept)."""
    n = len(points)
    if n < 3:
        return None
    mx = mean([p[0] for p in points])
    my = mean([p[1] for p in points])
    sxx = sum((p[0] - mx) ** 2 for p in points)
    if sxx <= 0:
        return None
    sxy = sum((p[0] - mx) * (p[1] - my) for p in points)
    slope = sxy / sxx
    return slope, my - slope * mx


def mark_reversals(rows, first_opening=True):
    """Tag each pulse that reversed the leaf's last direction of travel.

    The smoke run (2026-10-02) showed the first pulse after a reversal moving
    far less than the next: 10.4 mm against 22.6 at 200 ms, 0 against 1.8 at
    50 ms. The rope's slack is taken up first, so those pulses measure the
    slack as well as the actuator, and are reported apart. The move to the
    start position opened the window, hence `first_opening`. A pulse that never
    happened reverses nothing."""
    prev = first_opening
    for r in rows:
        r["reversal"] = (r["opening"] != prev)
        if r["disp_mm"] is not None:
            prev = r["opening"]


def analyse(rows, noise_mm, win_mm):
    pct = (lambda mm: 100.0 * mm / win_mm) if win_mm else (lambda mm: float("nan"))
    thresh = max(3.0 * noise_mm, 1.0)
    print("\nnoise at rest: sigma %.2f mm over %d reads; a pulse 'moved' when it moved "
          "more than %.1f mm" % (noise_mm, NOISE_READS, thresh))
    if "reversal" not in (rows[0] if rows else {}):
        mark_reversals(rows)
    steps = sorted(set(round(abs(r["disp_mm"]), 1) for r in rows
                       if r["disp_mm"] is not None and abs(r["disp_mm"]) > 0.05))
    if steps:
        print("smallest non-zero displacement read: %.1f mm (the encoder's step shows here)"
              % steps[0])
    print("\nthe first pulse after a reversal (rope slack taken up), against the same width "
          "in the same direction:")
    for opening in (True, False):
        for w in sorted(set(r["req_ms"] for r in rows), reverse=True):
            rev = [r["disp_mm"] for r in rows if r["opening"] == opening and r["req_ms"] == w
                   and r["reversal"] and r["disp_mm"] is not None]
            same = [r["disp_mm"] for r in rows if r["opening"] == opening and r["req_ms"] == w
                    and not r["reversal"] and r["disp_mm"] is not None]
            if rev and same:
                print("  %-5s %4d ms: after a reversal %5.2f mm (n %d), same direction %5.2f mm "
                      "(n %d): %+.2f mm" % ("open" if opening else "close", w, mean(rev), len(rev),
                                            mean(same), len(same), mean(rev) - mean(same)))
            elif rev:
                print("  %-5s %4d ms: after a reversal %5.2f mm (n %d); no same-direction pulse "
                      "of this width to set it against"
                      % ("open" if opening else "close", w, mean(rev), len(rev)))
    print("\nbelow, SAME-DIRECTION pulses only, take-up pulses left out:")
    verdict = {}
    for opening in (True, False):
        d = "open" if opening else "close"
        sub = [r for r in rows if r["opening"] == opening and r["disp_mm"] is not None
               and not r.get("takeup")
               and not r["reversal"]]
        if not sub:
            continue
        print("\n%s: req ms   real ms (mean)   n   disp mm mean / sigma / min   %% of window   moved"
              % d.upper())
        widths = sorted(set(r["req_ms"] for r in sub), reverse=True)
        floor2, reliable, ok_so_far = None, [], True
        for w in widths:
            g = [r for r in sub if r["req_ms"] == w]
            disp = [r["disp_mm"] for r in g]
            real = mean([r["width_ms"] for r in g])
            moved = sum(1 for x in disp if x > thresh)
            print("      %5d   %7.1f          %2d   %6.2f / %5.2f / %6.2f      %6.2f      %d/%d"
                  % (w, real, len(g), mean(disp), sd(disp), min(disp), pct(mean(disp)),
                     moved, len(g)))
            # floor 2 counts only an unbroken run from the longest width down:
            # one width that failed ends it, however a shorter one then does
            ok_so_far = ok_so_far and moved == len(g)
            if ok_so_far:
                floor2 = (w, real, mean(disp))
                reliable.append(w)
        # the line through the widths in that run
        pts = [(r["width_ms"], r["disp_mm"]) for r in sub if r["req_ms"] in reliable]
        f = fit(pts)
        if f:
            slope, icpt = f
            dead = -icpt / slope if slope > 0 else float("nan")
            print("   fit over %d pulses: %.1f mm/s (%.2f %%/s), dead time %.0f ms"
                  % (len(pts), slope * 1000.0, pct(slope * 1000.0), dead))
        else:
            dead = float("nan")
            print("   too few moving pulses for a fit")
        if floor2:
            print("   floor 2: %d ms commanded (%.1f ms real) is the shortest width at which "
                  "every pulse moved: %.2f mm, %.2f %% of the window"
                  % (floor2[0], floor2[1], floor2[2], pct(floor2[2])))
        else:
            print("   floor 2: no width moved the leaf on every pulse")
        verdict[d] = (floor2, dead)
    return verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--widths", default=DEFAULT_WIDTHS, help="ms, comma-separated")
    ap.add_argument("--reps", type=int, default=5, help="pulses per width and direction, per pass")
    ap.add_argument("--passes", type=int, default=2)
    ap.add_argument("--start", type=int, default=500, help="where to put M3 first, 0.1 %%")
    ap.add_argument("--out", default="minmove.csv", help="one row per pulse")
    ap.add_argument("--takeup", type=int, default=200,
                    help="ms of an uncounted pulse that takes up the rope's slack before each "
                         "block (0 = none); see mark_reversals()")
    a = ap.parse_args()
    widths = [int(w) for w in a.widths.split(",") if w.strip()]

    rig = Rig(a.host, a.pin)
    print("at_wp_minmove -- unit %s, fw %s" % (rig.unit_id, rig.fw))
    if "bench" not in rig.fw:
        sys.exit("this needs a bench build: the pulse hook is bench-only")
    p = Pulser(rig)
    rows = []
    try:
        rig.standby(True)
        if not rig.wait_gate():
            sys.exit("T17 never published position control: the start position needs it")
        p.pulse_seq()                                  # refuses an old build early
        win = window_mm(rig)
        say("putting M3 at %.1f %% (window %s mm)" % (a.start / 10.0, win))
        rig.go(a.start)
        rest = [x for x in (p.live_mm() for _ in range(NOISE_READS)) if x is not None]
        # the first reading can still catch the leaf settling (1.8 mm on
        # 2026-10-02), which is a move, not noise
        noise = sd(rest[1:]) if len(rest) > 2 else 0.0
        say("at rest: %s mm" % ", ".join("%.1f" % x for x in rest))
        state = {"before": rest[-1] if rest else p.live_mm()}

        def one(ps, req, opening, rep, takeup):
            """One pulse and the reading after it, as a row. A pulse that did
            not complete measured nothing, so its row carries no displacement."""
            rec = p.pulse(req, opening)
            time.sleep(SETTLE_S)
            after = p.live_mm()
            before = state["before"]
            disp = None
            if (rec is not None and rec.get("completed") and before is not None
                    and after is not None):
                disp = (after - before) if opening else (before - after)
            rows.append({"pass": ps + 1, "req_ms": req, "opening": opening, "rep": rep,
                         "takeup": takeup,
                         "width_ms": (rec or {}).get("width_us", 0) / 1000.0,
                         "completed": (rec or {}).get("completed"),
                         "before_mm": before, "after_mm": after, "disp_mm": disp})
            state["before"] = after
            return rec

        def recover():
            """Make the next pulse a measurement again. 2026-10-02: one failed
            read of the emulated wind sensor raised T3's safe-fail, which closed
            every window, and the first run went on pulsing M3 against its
            closed end switch for ten minutes and called it data."""
            end = time.time() + RECOVER_WAIT_S
            why = held(rig)
            while why:
                if why == LEFT_STANDBY:
                    sys.exit("the unit left STANDBY during the run (someone else has it) -- stopping")
                if time.time() > end:
                    sys.exit("still %s after %d s -- stopping" % (why, RECOVER_WAIT_S))
                time.sleep(2.0)
                why = held(rig)
            why = off_mid(rig)
            if why:
                say("%s -- putting it back at %.1f %%" % (why, a.start / 10.0))
                rig.go(a.start)
                state["before"] = p.live_mm()
                return True
            return False

        for ps in range(a.passes):
            for w in widths:
                for opening in (True, False):
                    need_takeup = a.takeup > 0
                    i = 0
                    while i < a.reps:
                        why = held(rig) or off_mid(rig)
                        if why:
                            say("pass %d, %d ms %s: %s" % (ps + 1, w, "open" if opening else "close", why))
                            if recover():
                                need_takeup = a.takeup > 0     # the move back undid the take-up
                        if need_takeup:
                            one(ps, a.takeup, opening, 0, True)
                            need_takeup = False
                            continue                           # re-check before a counted pulse
                        rec = one(ps, w, opening, i + 1, False)
                        if rec is None or not rec.get("completed"):
                            why = held(rig) or off_mid(rig)
                            say("pass %d, %d ms %s #%d: the pulse %s (%s)"
                                % (ps + 1, w, "open" if opening else "close", i + 1,
                                   "never happened" if rec is None else "was ended by something else",
                                   why or "nothing in the way"))
                            if why is None:
                                sys.exit("a pulse failed with nothing in its way -- stopping")
                            continue                           # recover, then redo this one
                        i += 1
                def shown(opening):
                    return ", ".join("%.1f" % r["disp_mm"] for r in rows
                                     if r["pass"] == ps + 1 and r["req_ms"] == w
                                     and r["opening"] == opening and not r["takeup"]
                                     and r["disp_mm"] is not None)
                say("pass %d, %3d ms: open %s / close %s mm" % (ps + 1, w, shown(True), shown(False)))
    finally:
        print("\nrestoring ...")
        try:
            rig.go(0)
        finally:
            rig.standby(False)
            rig.restore()
            rig.u.logout()

    with open(a.out, "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=["pass", "req_ms", "opening", "rep", "takeup", "width_ms",
                                            "completed",
                                            "before_mm", "after_mm", "disp_mm"])
        wr.writeheader()
        wr.writerows(rows)
    print("%d pulses written to %s" % (len(rows), a.out))
    analyse(rows, noise, win)
    return 0


if __name__ == "__main__":
    sys.exit(main())

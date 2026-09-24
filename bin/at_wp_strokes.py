#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Scripted M3 strokes for a soak (bin/at_wp_soak.py): nobody at the rig.

A soak needs strokes, at least 10 JUDGED drives, spread over its window, and a
person altering the emulated temperature by hand for hours is the weak link
(2.9.2's soak: all strokes in two hours, then an idle evening). This makes them
on a fixed schedule instead, through the same levers as bin/at_wp_confirm.py:

  - T6 is steered, never T2 directly: OPEN = the active temperature maximum to
    its minimum and cr_priority 2; CLOSE = the maximum to its top value and
    cr_priority 0. So every drive is an ordinary SRC_T6 command, dwells and all;
  - M3's two dwells are cut to 5 s for the session only (a dwell already
    running keeps its old length, gh#51, so the first move may wait for it);
  - after every session EVERYTHING it changed is restored and it logs out, so
    between sessions the unit runs on its own configuration -- and T6 may well
    move M3 itself, which the soak counts too.

One session = M3 to CLOSED if it is not, then open, then CLOSED: at least two
judged drives. **What "open" means depends on the control mode.** In mode 1 the
law drives M3 to the OPEN end. In mode 2 it commands an APERTURE proportional to
demand -- on this rig the session's maximum demand settles around 49 % -- so the
open stroke is a completed judged DRIVE, wherever the law stops it, and only the
close has to make an end sensor (which since 2.12.1 it must). It judges nothing: at_wp_soak.py --report does, over the whole
window. A session is SKIPPED, not forced, when the unit is not in a state to
stroke (wind override, motor alarm, calibrating, STANDBY, the sensor gate not
ok, travel_m3 not the rig's 13). The run STOPS if a setting it steers differs
from its value at the first session: every session restores to what it read,
so a failed restore or a change made by hand would otherwise be taken for the
original.

USAGE
-----
    python bin/at_wp_strokes.py --host 192.168.20.160                  # 8 x 75 min
    python bin/at_wp_strokes.py --host 192.168.20.160 --sessions 4 --every-min 30

Exit 0 = every session ran or was skipped with a reason, 1 = a session failed
(M3 did not move; two in a row end the run), 2 = refused. Needs a 2.10.0 bench
build (it reuses bin/at_wp_confirm.py). Refuses to run on 5C88. If the process
is killed mid-session, the restore does not run: check travel_m3, the active
t_max, cr_priority and M3's dwells by hand.
"""

import argparse
import atexit
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from at_wp_confirm import (Rig, RIG_TRAVEL_S, flags, m3, say,   # noqa: E402
                           DEFAULT_PIN, MOVE_LIMIT_S, SETTLE_S)
from at_wp_teach_standby import public_status                  # noqa: E402

FIRST_CLOSE_LIMIT_S = 1800   # a 25 min open dwell may still be running
LINEAR_MOVE_LIMIT_S = 1800   # mode 2: SEVERAL drives, ~600 s apart -- see below
BLOCKING = ("wind_override", "motor_alarm", "calibrating", "standby")
SHOWN = ("strokes", "confirmed", "not_reached", "not_judged", "stall_faults",
         "early_stops", "rejected_rate", "err_comm")

# What the sessions steer, as GET /api/config reports it. Recorded at the first
# session and required unchanged at every later one: each session restores to
# what it READ, so a restore that failed, or a setting changed by hand between
# sessions, would otherwise become the "original" the next session restores to.
STEERED = ("t_max_day", "t_max_ngt", "cr_priority", "dwell_open_s", "dwell_close_s",
           "travel_s")
_base = [None]
STOP = "stop"

_current = [None]            # the session's Rig, for the exit-time restore


def _restore_current():
    rig = _current[0]
    if rig is not None:
        rig.rs.run()
        rig.u.logout()
        _current[0] = None


atexit.register(_restore_current)


def m3_pct(st):
    """M3's opening in percent, from the public status."""
    try:
        return float((st.get("windows") or {}).get("M3_percent_x10", -1)) / 10.0
    except (TypeError, ValueError):
        return -1.0


def linear_open(rig, limit_s):
    """Mode 2's open stroke: a COMPLETED JUDGED DRIVE, not the OPEN state.

    `graded` commands an APERTURE proportional to demand, so even this session's
    maximum demand settles part-way -- 49.0 % on the rig -- and M3 has no reason
    to reach the OPEN end at all. Session 1 of the 2026-09-24 soak waited 900 s
    for a state the law was never going to produce and failed with M3 correctly
    part-open. What a soak counts is JUDGED DRIVES, and a targeted drive is
    judged like any other, so in mode 2 the stroke is done when the DRIVE is:
    M3 moved and then stopped, and the drive counter advanced.

    The close is unchanged in both modes: demand falls to nothing, the law asks
    for 0, and since 2.12.1 a target inside the deadzone of an end IS that end,
    so M3 must make the closed end sensor. That is half of what this soak exists
    to show.
    """
    s0 = int(rig.soak().get("strokes") or 0)
    rig.want(True)
    say("waiting for T6 to drive M3 open (mode 2 targets an aperture, not an end)")
    t0 = time.time()
    seen_move = False
    while time.time() - t0 < limit_s:
        st = rig.status()
        state = m3(st)
        if state and state.startswith("MOVING"):
            seen_move = True
        elif seen_move:
            now = int(rig.soak().get("strokes") or 0)
            if now > s0:
                took = time.time() - t0
                say("M3 %s at %.1f %% after %.0f s (drives +%d)"
                    % (state, m3_pct(st), took, now - s0))
                time.sleep(SETTLE_S)
                return took
        time.sleep(2.0)
    sys.exit("M3 made no completed drive within %d s (now %s at %.1f %%)"
             % (limit_s, m3(rig.status()), m3_pct(rig.status())))


def why_not(host):
    """The reason the unit cannot be stroked now, or None."""
    st = public_status(host) or {}
    sysb = st.get("system") or {}
    if "5C88" in str(sysb.get("unit_id", "")).upper():
        return "REFUSED: this is the production unit"
    for bad in BLOCKING:
        if bad in flags(st):
            return "the unit shows '%s'" % bad
    return None


def session(host, pin, n):
    """One stroke session. True = ran, False = failed, None = skipped, STOP =
    the steered settings are not what the run started with."""
    try:
        rig = Rig(host, pin)
    except SystemExit as e:              # the harness exits when it cannot start
        say("session %d FAILED to start: %s" % (n, e))
        return False
    # This script restores per session; the harness's own exit hooks would
    # otherwise log in again at exit for every session that ever ran.
    for fn in (rig.u.logout, rig.rs.run, rig.clear):
        atexit.unregister(fn)
    _current[0] = rig
    try:
        seen = dict((k, rig.cfg0.get(k)) for k in STEERED)
        if _base[0] is None:
            _base[0] = seen
            say("steered settings at the start: %s" % seen)
        elif seen != _base[0]:
            say("session %d: the steered settings changed since the run started "
                "(a failed restore, or a change by hand) -- stopping, nothing touched.\n"
                "      start: %s\n      now:   %s" % (n, _base[0], seen))
            return STOP
        g = rig.gate()
        if str(g.get("reason_str")) != "ok":
            say("session %d SKIPPED: the sensor gate is %s" % (n, g.get("reason_str")))
            return None
        travel = int((rig.cfg0.get("travel_s") or [0, 0, 0])[2])
        if travel != RIG_TRAVEL_S:
            say("session %d SKIPPED: travel_m3 is %d, not the rig's %d"
                % (n, travel, RIG_TRAVEL_S))
            return None
        s0 = rig.soak()
        # Mode 2 strokes are slower by design, and not by one drive's worth.
        # The law holds M3 for 10 min after its last drive (M3_HOLD_MS) and
        # `min_intv_m3` -- 600 s on this rig -- replaces the dwells this session
        # cut, which mode 2 ignores; and `graded` closes in STEPS (measured
        # 2026-09-24: 49 % -> 25 % -> ... -> the end), so a close from half open
        # is three drives about 600 s apart. 420 s fails a stroke that was
        # always going to take longer, and so does 900 s; the limit follows the
        # mode actually in force (2026-09-24).
        linear = (rig.status().get("windows") or {}).get("M3_ctrl_mode") == "LINEAR"
        limit = LINEAR_MOVE_LIMIT_S if linear else MOVE_LIMIT_S
        say("session %d: M3 %s, gate %s, %s control (move limit %d s)"
            % (n, m3(rig.status()), g.get("mode_str"),
               "LINEAR" if linear else "timed", limit))
        rig.dwells_short()
        if m3(rig.status()) != "CLOSED":
            rig.move(False, max(FIRST_CLOSE_LIMIT_S, limit))
        if linear:
            linear_open(rig, limit)
        else:
            rig.move(True, limit)
        rig.move(False, limit)
        s1 = rig.soak()
        say("session %d done: %s" % (n, ", ".join(
            "%s +%d" % (k, int(s1.get(k, 0) or 0) - int(s0.get(k, 0) or 0)) for k in SHOWN)))
        return True
    except SystemExit as e:              # Rig.move() and friends exit on a failure
        say("session %d FAILED: %s" % (n, e))
        return False
    finally:
        _restore_current()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--sessions", type=int, default=8)
    ap.add_argument("--every-min", type=float, default=75.0)
    a = ap.parse_args()

    why = why_not(a.host)
    if why and why.startswith("REFUSED"):
        print(why)
        return 2
    t0 = time.time()
    say("%d sessions, one every %.0f min, from now" % (a.sessions, a.every_min))
    ran = skipped = failed = in_a_row = 0
    for i in range(a.sessions):
        due = t0 + i * a.every_min * 60.0
        if time.time() < due:
            time.sleep(due - time.time())
        why = why_not(a.host)
        if why:
            say("session %d SKIPPED: %s" % (i + 1, why))
            skipped += 1
            continue
        try:
            ok = session(a.host, a.pin, i + 1)
        except Exception as e:           # noqa: BLE001 -- a network error, say
            say("session %d FAILED: %r" % (i + 1, e))
            ok = False
        if ok == STOP:
            failed += 1
            break
        if ok is None:
            skipped += 1
        elif ok:
            ran += 1
            in_a_row = 0
        else:
            failed += 1
            in_a_row += 1
            if in_a_row >= 2:
                say("two sessions in a row failed -- stopping")
                break
    say("finished: %d ran, %d skipped, %d failed" % (ran, skipped, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

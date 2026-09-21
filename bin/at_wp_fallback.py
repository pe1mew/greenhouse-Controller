#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT for 2.12.0: a part-open M3 must reach an end under mode 1 (the fallback).

THE DEFECT THIS EXISTS FOR
--------------------------
After mode 2 leaves M3 part-open, mode 1's law asks for whichever end its step
wants -- `vent_model_stepped.cpp` treats VENT_WIN_PART_OPEN as eligible both
ways. But T6's apply filter only posted a CLOSE for a window that was OPEN or
MOVING_OPEN, and an OPEN for one that was CLOSED or MOVING_CLOSE. PART_OPEN was
in neither, so T6 dropped the command and M3 was **stranded**: neither closable
nor openable by the climate law, until a wind override or a recalibration
happened to move it. Found in the 2026-09-20 soak on 2344: 28 minutes of T6
asking to close a part-open M3, ended only by a wind override at 16:22.

That is the FALLBACK PATH. A sensor fault while M3 is part-open demotes to
mode 1 at once -- and then leaves M3 where it stopped, for good.

No earlier stage could see it: `at_wp_target.py` drives T2 through the bench
hook, and never goes through T6's apply path.

HOW IT PUTS M3 PART-OPEN WITHOUT T6 FIGHTING
-------------------------------------------
T6 is level-triggered and re-posts its step every cycle, so it reverses any
drive AWAY from where it wants M3. Two things make the setup deterministic:

  - a long DWELL holds T6 off while M3 rests at an end. Dwells defer SRC_T6
    only; the bench hook is SRC_OPERATOR_MANUAL and is not deferred;
  - the hook then drives M3 part-way TOWARD the end T6 wants. T6's filter
    ignores a window already moving that way, so nothing reverses it.

What remains is exactly the question: M3 at rest part-open, T6 wanting an end,
the dwell short. The fixed build drives it there; the defective one never does.

STAGES
------
  close     T6 wants CLOSED; M3 left part-open by a closing drive -> must reach CLOSED
  open      T6 wants OPEN;   M3 left part-open by an opening drive -> must reach OPEN
  aborted   (2026-09-21) mode 2 through T6 itself: graded moves M3, the operator
            takes it mid-drive, and T6 must tell the law ABORTED (4) -- not
            FAIL_TIMEOUT (2), which is all it ever said before. ~12 min: graded
            does not move M3 within 10 min of its last drive, a constant
  sincemove (2026-09-21) what the law and the linear dwell are told about M3's
            last drive: "none since boot" is UINT32_MAX, never 0 (a law reads 0
            as "just moved"); a recalibration sweep sets the time like any
            other drive; and in mode 2 the sweep arms min_intv_m3, not the
            close dwell. To see the "never" value, leave every window CLOSED
            before the push (a recalibration does it) so the boot skips its
            sweep, and run this stage first -- otherwise that check is skipped

The two 2026-09-21 stages came from the model session's closed-loop simulator,
which found both while replaying 2.12.0 as built.

FAIL-FIRST
----------
The build this was written against IS the defect, so run it there first and
watch both stages fail. Afterwards `-DWPOS_FAILFIRST_212=16` restores the old
filter on its own, so the demonstration can be repeated. `aborted` fails with
bit 512 (T6 never sends ABORTED), `sincemove` with bit 1024 (0 = never, and the
sweep sets no time and arms the close dwell).

Everything changed is restored on exit, and M3 is returned to an end even when
the stage leaves it stranded.

Usage
  python bin/at_wp_fallback.py --host 192.168.20.160
  python bin/at_wp_fallback.py --host 192.168.20.160 --stage close
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import at_wp_ramp                                               # noqa: E402
from at_wp_confirm import Rig, m3, say                          # noqa: E402

# The rig's link was marginal when this was written (-78 dBm, 15 % ping loss):
# a slow reply is not a failure of the rule under test.
at_wp_ramp.HTTP_TIMEOUT_S = 30

TARGET_X10 = 500          # half open: part-open by any measure, far from both bands
LONG_DWELL_S = 900        # holds T6 off while the hook positions M3
SHORT_DWELL_S = 5         # lets T6 act promptly once M3 is part-open
T6_LIMIT_S = 200          # a T6 wake (30 s poll) + the 18 s drive + dwell, with margin
HOOK_LIMIT_S = 40
MODE_LIMIT_S = 150        # T6 decides the effective mode once per wake (poll <= 120 s)
GRADED_HOLD_S = 600       # graded's own hold after any M3 drive (M3_HOLD_MS)
SWEEP_LIMIT_S = 240       # a recalibration lasts the slowest channel's travel
MIN_INTV_TEST_S = 600     # sincemove: the linear dwell (the default), > any sweep
NEVER = 4294967295        # UINT32_MAX: "no drive since boot"
RESULTS = {0: "NONE", 1: "DONE", 2: "FAIL_TIMEOUT", 3: "FAIL_FAULT", 4: "ABORTED"}


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------


def _post(rig, path, body):
    """POST, tolerating a slow reply.

    The command can land and its REPLY still time out: the unit's web server
    shares the flash with T4, and the steering writes just before a hook go to
    NVS. Seen on 2344, 2026-09-21: the hook's reply exceeded the 10 s client
    timeout while M3 obediently went part-open. Crashing there loses the run
    and, worse, the observation window. So a timeout is reported and the
    caller judges by STATE, which is what the stage asserts anyway."""
    try:
        return rig.u._req("POST", path, body)
    except (TimeoutError, OSError) as e:
        say("reply to %s timed out (%s) -- judging by state" % (path, type(e).__name__))
        return (None, None)


def hook(rig, x10):
    sc, out = _post(rig, "/api/diag/windowpos", {"target_x10": x10})
    if sc == 404:
        sys.exit("no target hook (404): this needs a 2.12.0 bench build")
    return out


def set_mode(rig, standby):
    _post(rig, "/api/mode", {"mode": "standby" if standby else "automatic"})
    time.sleep(3.0)


def state(rig, tries=4):
    """M3's state. An unreadable status is retried, never judged.

    /api/status is read with a 6 s timeout and ANY failure comes back as an
    empty dict, which reads as "?" -- on a marginal link that is a lost reply,
    not a state. Judged as a state it made a stage inconclusive on 2026-09-21:
    "T6 was not held off by the dwell (OPEN)" -- the check had read "?", the
    message read again and got OPEN. So: retry, and let each check judge and
    report the SAME reading."""
    for _ in range(tries):
        s = m3(rig.status())
        if s:
            return str(s)
        time.sleep(1.0)
    return "?"


def wait_for(rig, wanted, limit_s):
    """Wait until M3's state is in `wanted`; return (state, seconds) or (last, None)."""
    t0 = time.time()
    last = state(rig)
    while time.time() - t0 < limit_s:
        last = state(rig)
        if last in wanted:
            return last, time.time() - t0
        time.sleep(0.7)
    return last, None


def m3_dwell(rig, key, field, value):
    cur = list(rig.cfg0.get(field) or [0, 0, 0])
    cur[2] = value
    rig.set_cfg("motor", key, value, field, cur)


def dwells(rig, open_s, close_s):
    m3_dwell(rig, "dwell_open_m3", "dwell_open_s", open_s)
    m3_dwell(rig, "dwell_close_m3", "dwell_close_s", close_s)


def to_an_end(rig):
    """Put M3 at CLOSED whatever state it is in, and leave the unit AUTOMATIC.

    Done in STANDBY with the hook, because a stranded M3 is exactly what the
    climate law cannot move on a defective build. Leaving STANDBY recalibrates
    (a CLOSE_ALL sweep), which is harmless: M3 is already closed by then."""
    if state(rig) in ("CLOSED",):
        return
    say("returning M3 to CLOSED (was %s)" % state(rig))
    set_mode(rig, True)
    hook(rig, 0)
    wait_for(rig, ("CLOSED",), HOOK_LIMIT_S)
    set_mode(rig, False)
    wait_for(rig, ("CLOSED",), 60)


def fresh_start(rig):
    """M3 at CLOSED with a close-dwell armed from the CURRENT setting.

    A dwell that is already running keeps its old length when the setting
    changes (gh#51; at_wp_confirm.py warns of the same), so a stage that has
    just set its dwells cannot assume they are in force -- the deadline armed
    by M3's last arrival still stands, and at the rig's own settings that is
    up to 25 minutes. A recalibration arms `now + dwell_close` with the value
    in force, so leaving STANDBY straight after setting the dwells gives every
    stage the same starting state instead of whatever the last drive left.

    (This docstring first said the trap had held up a run on 2026-09-21. It
    had not: that run died on a network timeout, and the dwell was blamed
    before the traceback was read. The hazard is real; that incident was not.)"""
    set_mode(rig, True)
    if state(rig) != "CLOSED":
        hook(rig, 0)
        wait_for(rig, ("CLOSED",), HOOK_LIMIT_S)
    set_mode(rig, False)                      # the recalibration re-arms the dwell
    st, secs = wait_for(rig, ("CLOSED",), 60)
    time.sleep(3.0)
    say("fresh start: M3 %s, dwells as set" % st)


def check_mode1(rig):
    """The fallback path is mode 1. Refuse to run otherwise."""
    cfg = rig.u.cfg() or {}
    if cfg.get("ctrl_mode_m3", 0) != 0:
        rig.set_cfg("motor", "ctrl_mode_m3", 0)
    w = (rig.status().get("windows") or {})
    if w.get("M3_ctrl_mode") not in (None, "TIMED"):
        time.sleep(35.0)          # one T6 wake re-decides the effective mode
    w = (rig.status().get("windows") or {})
    if w.get("M3_ctrl_mode") not in (None, "TIMED"):
        sys.exit("M3 is not under timed control -- the fallback cannot be tested")


def block(rig, name):
    """One of the bench diag's blocks (t2, t6); {} on an older image."""
    b = rig.diag().get(name)
    return b if isinstance(b, dict) else {}


def need_blocks(rig):
    if "taken_m3" not in block(rig, "t2") or "last_result" not in block(rig, "t6"):
        sys.exit("the diag has no t2.taken_m3 / t6.last_result -- this stage needs a "
                 "2.12.0 bench build from 2026-09-21 on")


def ctrl_mode(rig, linear, min_intv=None):
    """Mode 2 on or off, and wait until the EFFECTIVE mode follows.

    The setting is only the operator's half: T6 decides the mode in force on
    every wake, and promotes only with a trusted position."""
    if min_intv is not None:
        rig.set_cfg("motor", "min_intv_m3", min_intv)
    rig.set_cfg("motor", "ctrl_mode_m3", 1 if linear else 0)
    want = "LINEAR" if linear else "TIMED"
    t0 = time.time()
    while time.time() - t0 < MODE_LIMIT_S:
        if (rig.status().get("windows") or {}).get("M3_ctrl_mode") == want:
            say("M3 under %s control" % want)
            return True
        time.sleep(2.0)
    say("M3 never came under %s control (gate %s)" % (want, rig.gate().get("mode_str")))
    return False


def sweep(rig):
    """Leave STANDBY -- which recalibrates -- and return when the sweep ends."""
    set_mode(rig, False)
    t0 = time.time()
    seen = False
    while time.time() - t0 < SWEEP_LIMIT_S:
        f = ((rig.status().get("mode") or {}).get("flags")) or []
        if "calibrating" in f:
            seen = True
        elif seen:
            return True
        time.sleep(0.5)
    return False


def t6_drives(rig, to_open, label):
    rig.want(to_open)
    want = ("OPEN",) if to_open else ("CLOSED",)
    st, secs = wait_for(rig, want, T6_LIMIT_S)
    if secs is None:
        sys.exit("setup: T6 did not drive M3 %s within %d s (%s)" % (label, T6_LIMIT_S, st))
    say("setup: T6 drove M3 %s" % st)


# --------------------------------------------------------------------------
# stages
# --------------------------------------------------------------------------


def stage_close(rig):
    """T6 wants CLOSED and M3 is part-open: T6 must finish the close."""
    dwells(rig, LONG_DWELL_S, SHORT_DWELL_S)
    fresh_start(rig)
    t6_drives(rig, True, "OPEN")              # arrives OPEN -> arms the 900 s open dwell
    rig.want(False)                           # T6 now wants CLOSED, deferred by that dwell
    time.sleep(3.0)
    st = state(rig)
    if st != "OPEN":
        say("setup: T6 was not held off by the dwell (%s) -- inconclusive" % st)
        return None
    hook(rig, TARGET_X10)                     # closing drive, toward what T6 wants
    st, secs = wait_for(rig, ("PART_OPEN",), HOOK_LIMIT_S)
    if secs is None:
        say("setup: M3 never came to rest part-open (%s) -- inconclusive" % st)
        return None
    say("M3 part-open; T6 wants CLOSED, the dwell is %d s" % SHORT_DWELL_S)
    st, secs = wait_for(rig, ("CLOSED",), T6_LIMIT_S)
    if secs is not None:
        say("PASS  T6 closed the part-open M3 after %.0f s" % secs)
        return True
    say("FAIL  M3 still %s after %d s: T6 asked for CLOSED and never posted it"
        % (st, T6_LIMIT_S))
    return False


def stage_open(rig):
    """T6 wants OPEN and M3 is part-open: T6 must finish the open."""
    dwells(rig, SHORT_DWELL_S, SHORT_DWELL_S)
    fresh_start(rig)
    t6_drives(rig, True, "OPEN")
    dwells(rig, SHORT_DWELL_S, LONG_DWELL_S)
    t6_drives(rig, False, "CLOSED")           # arrives CLOSED -> arms the 900 s close dwell
    rig.want(True)                            # T6 now wants OPEN, deferred by that dwell
    time.sleep(3.0)
    st = state(rig)
    if st != "CLOSED":
        say("setup: T6 was not held off by the dwell (%s) -- inconclusive" % st)
        return None
    hook(rig, TARGET_X10)                     # opening drive, toward what T6 wants
    st, secs = wait_for(rig, ("PART_OPEN",), HOOK_LIMIT_S)
    if secs is None:
        say("setup: M3 never came to rest part-open (%s) -- inconclusive" % st)
        return None
    say("M3 part-open; T6 wants OPEN, the dwell is %d s" % SHORT_DWELL_S)
    st, secs = wait_for(rig, ("OPEN",), T6_LIMIT_S)
    if secs is not None:
        say("PASS  T6 opened the part-open M3 after %.0f s" % secs)
        return True
    say("FAIL  M3 still %s after %d s: T6 asked for OPEN and never posted it"
        % (st, T6_LIMIT_S))
    return False


def stage_aborted(rig):
    try:
        return _aborted(rig)
    finally:
        ctrl_mode(rig, False)                 # every other stage is mode 1


def stage_sincemove(rig):
    try:
        return _sincemove(rig)
    finally:
        ctrl_mode(rig, False)


def _aborted(rig):
    """T6 tells the law ABORTED when a window it commanded is taken (2026-09-21).

    Until then T6 inferred every result from where M3 came to rest, so a window
    taken by T3 or the operator read as FAIL_TIMEOUT -- which vent_model.h
    reserved for "did not arrive". graded treats the two alike, so no decision
    changed; a law that tells them apart would have been misinformed."""
    need_blocks(rig)
    if not ctrl_mode(rig, True, min_intv=0):  # no linear dwell: T6 acts at once
        return None
    rig.want(True)                            # the law wants M3 open
    set_mode(rig, True)
    if state(rig) != "CLOSED":
        hook(rig, 0)
        wait_for(rig, ("CLOSED",), HOOK_LIMIT_S)
    # The whole sweep, not just M3's part of it: M3 reads MOVING_CLOSE while it
    # runs, which the wait below would take for T6's drive.
    if not sweep(rig):
        say("setup: no recalibration sweep seen -- inconclusive")
        return None
    say("waiting for graded to move M3 (its own hold: %d s after the sweep)"
        % GRADED_HOLD_S)
    t0 = time.time()
    st = state(rig)
    while st in ("CLOSED", "?"):
        if time.time() - t0 > GRADED_HOLD_S + T6_LIMIT_S:
            say("setup: T6 never moved M3 (%s) -- inconclusive" % st)
            return None
        time.sleep(0.5)
        st = state(rig)
    hook(rig, 0)                              # the operator takes it, at once
    say("T6 moved M3 (%s) after %.0f s; the operator closed it" % (st, time.time() - t0))
    wait_for(rig, ("CLOSED",), HOOK_LIMIT_S)
    t0 = time.time()
    t6 = {}
    while time.time() - t0 < T6_LIMIT_S:     # T6 judges on its next wake at rest
        t6 = block(rig, "t6")
        if t6.get("last_result") not in (None, 0):
            break
        time.sleep(2.0)
    t2 = block(rig, "t2")
    res = t6.get("last_result")
    say("T6: target %s, result %s (%s); taken %s, at the post %s"
        % (t6.get("last_target_x10"), res, RESULTS.get(res, "?"),
           t2.get("taken_m3"), t6.get("taken_at_post")))
    if res == 1:
        say("T6 judged its target DONE before the operator's close landed -- "
            "inconclusive, run it again")
        return None
    ok = (res == 4)
    say("%s  the law was told %s for a window the operator took"
        % ("PASS" if ok else "FAIL", RESULTS.get(res, res)))
    return ok


def _sincemove(rig):
    """What the law and the linear dwell are told about M3's last drive (2026-09-21)."""
    need_blocks(rig)
    ok = True
    v = block(rig, "t2").get("ms_since_move_m3")
    if v == NEVER:
        say("PASS  no drive since boot reads UINT32_MAX (long ago)")
    elif v == 0:
        say("FAIL  no drive since boot reads 0 -- 'just moved' to a law")
        ok = False
    else:
        say("NOTE  M3 has moved since boot (%s ms ago): the 'never' value is only "
            "seen straight after a boot that skipped its sweep" % v)
    # M3's dwells short, the linear dwell long -- longer than any sweep, which
    # runs for the SLOWEST channel's travel while M3's dwell is already armed
    # from M3's own end. Which one the sweep arms is then plain from whether a
    # T6 command waits.
    dwells(rig, SHORT_DWELL_S, SHORT_DWELL_S)
    set_mode(rig, True)
    if not ctrl_mode(rig, True, min_intv=MIN_INTV_TEST_S):
        return None
    hook(rig, 1000)                           # an operator drive, so "since" is not 0
    wait_for(rig, ("OPEN",), HOOK_LIMIT_S)
    say("M3 OPEN; 40 s before the recalibration")
    time.sleep(40.0)
    set_mode(rig, False)                      # leaving STANDBY recalibrates
    st, secs = wait_for(rig, ("CLOSED",), HOOK_LIMIT_S + 20)   # M3's part of the sweep
    v = block(rig, "t2").get("ms_since_move_m3")
    fresh = v is not None and 0 < v < 15000
    ok &= fresh
    say("%s  as M3's part of the sweep ended, M3 last moved %s ms ago (the sweep, "
        "not the operator's drive ~60 s before)" % ("PASS" if fresh else "FAIL", v))
    t0 = time.time()
    while time.time() - t0 < SWEEP_LIMIT_S:   # T2 reads Q1 only after the sweep
        if "calibrating" not in ((rig.status().get("mode") or {}).get("flags") or []):
            break
        time.sleep(0.5)
    set_mode(rig, True)                       # T6 off M3 for the dwell check
    mode = (rig.status().get("windows") or {}).get("M3_ctrl_mode")
    if mode != "LINEAR":
        say("NOTE  M3 left LINEAR control during the sweep (%s): the dwell check "
            "cannot tell the two dwells apart -- skipped" % mode)
        return ok
    time.sleep(SHORT_DWELL_S + 3.0)           # past the close dwell, well inside the other
    target_t6(rig, TARGET_X10)                # as T6: the dwell applies
    time.sleep(8.0)
    st = state(rig)
    held = (st == "CLOSED")
    ok &= held
    say("%s  in mode 2 the sweep armed min_intv_m3 (%d s), not the %d s close dwell: "
        "a T6 target %s (M3 %s)" % ("PASS" if held else "FAIL", MIN_INTV_TEST_S,
                                    SHORT_DWELL_S, "waited" if held else "went", st))
    return ok


def target_t6(rig, x10):
    """A target sent as T6 sends one, so the dwell applies (the bench hook)."""
    sc, out = _post(rig, "/api/diag/windowpos", {"target_x10": x10, "source": "t6"})
    if sc == 404:
        sys.exit("no target hook (404): this needs a 2.12.0 bench build")
    if isinstance(out, dict) and out.get("source") != "t6":
        sys.exit("the hook ignored \"source\":\"t6\" -- this needs a build from 2026-09-21 on")
    return out


STAGES = {"close": stage_close, "open": stage_open,
          "aborted": stage_aborted, "sincemove": stage_sincemove}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="192.168.20.160")
    ap.add_argument("--pin", default="12345678")
    ap.add_argument("--stage", choices=sorted(STAGES) + ["all"], default="all")
    a = ap.parse_args()

    rig = Rig(a.host, a.pin)
    sysb = (rig.u.status() or {}).get("system") or {}
    ff = ((rig.u.diag() or {}).get("gate") or {}).get("failfirst_212")
    print("at_wp_fallback -- unit %s, fw %s, failfirst_212 %s"
          % (sysb.get("unit_id"), sysb.get("fw_ver"), ff))
    check_mode1(rig)

    # sincemove first: its "never moved" check needs M3 untouched since boot.
    names = (["sincemove", "close", "open", "aborted"] if a.stage == "all"
             else [a.stage])
    results = {}
    try:
        for n in names:
            print("\n== %s ==" % n)
            results[n] = STAGES[n](rig)
    finally:
        print("\nreturning M3 to an end ...")
        to_an_end(rig)

    print("\n== result ==")
    for n in names:
        r = results.get(n)
        print("  %-9s %s" % (n, "PASS" if r else ("INCONCLUSIVE" if r is None else "FAIL")))
    bad = [n for n in names if results.get(n) is False]
    inc = [n for n in names if results.get(n) is None]
    print("\n%s" % ("ALL PASS" if not bad and not inc else
                    ("FAILED: " + ", ".join(bad)) if bad else
                    "INCONCLUSIVE: " + ", ".join(inc)))
    return 1 if bad else (2 if inc else 0)


if __name__ == "__main__":
    sys.exit(main())

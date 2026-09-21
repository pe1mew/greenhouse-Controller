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
  close   T6 wants CLOSED; M3 left part-open by a closing drive -> must reach CLOSED
  open    T6 wants OPEN;   M3 left part-open by an opening drive -> must reach OPEN

FAIL-FIRST
----------
The build this was written against IS the defect, so run it there first and
watch both stages fail. Afterwards `-DWPOS_FAILFIRST_212=16` restores the old
filter on its own, so the demonstration can be repeated.

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


STAGES = {"close": stage_close, "open": stage_open}


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

    names = ["close", "open"] if a.stage == "all" else [a.stage]
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
        print("  %-6s %s" % (n, "PASS" if r else ("INCONCLUSIVE" if r is None else "FAIL")))
    bad = [n for n in names if results.get(n) is False]
    inc = [n for n in names if results.get(n) is None]
    print("\n%s" % ("ALL PASS" if not bad and not inc else
                    ("FAILED: " + ", ".join(bad)) if bad else
                    "INCONCLUSIVE: " + ", ".join(inc)))
    return 1 if bad else (2 if inc else 0)


if __name__ == "__main__":
    sys.exit(main())

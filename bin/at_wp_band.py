#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Plan §5e step 2 on the rig: M3's band in force, where it comes from, and
that T2 obeys it.

WHAT STEP 2 CHANGED
-------------------
Until §5e, M3's deadband was the setting `deadzone_m3` and nothing else.
Step 2 made it "the band in force" (dm_m3_deadzone(), T4):
  - `deadzone_src_m3` = 0: the typed `deadzone_m3`;
  - `deadzone_src_m3` = 1 (the default): the band the characterisation run
    measured, with the typed value in force until a run has measured one;
  - a characterisation run's check band (phase 4b) beats both while it runs.
GET /api/diag/commission reports it as `dz`. A bench build has two hooks to
set what the run (step 3) will set for real: {"meas_band_mm":N} stores a
record with a measured band, {"band_override_mm":N} sets the check band.

STAGES
------
  config  `deadzone_src_m3` is published, reads 1 by default, round-trips 0
          and 1, and has bounds 0-1 in /api/config/limits.
  source  `dz` names the right source and band for every combination:
          typed, unmeasured, measured, override.
  t2      T2 obeys the band in force. A correction sized BETWEEN two bands
          moves M3 under the smaller band, and is "already there" under the
          larger one, so M3 does not move. Checked for a measured band and for
          the override, each against the typed band. M3 moves 2-3 % at a time
          around 50 %, in STANDBY.
  pulse   CMD_PULSE still works from its new home in every build: one 100 ms
          pulse, and its record in GET ?pulses.
  persist (--expect-measured N, after a reboot) the measured band came back
          from NVS.

It restores what it changed: `deadzone_src_m3` as found, the override to 0,
the stored band to 0 (unless --leave-record), and AUTOMATIC. The record itself
stays in NVS with no band: there is no route to erase it, and a record with
band 0 is "no measurement". The next real run replaces it.

Usage
  python bin/at_wp_band.py --host 192.168.20.160
  python bin/at_wp_band.py --host 192.168.20.160 --leave-record 40     # before a reboot
  python bin/at_wp_band.py --host 192.168.20.160 --expect-measured 40  # after it
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from at_wp02 import Rig, say                                    # noqa: E402

DEFAULT_HOST = "192.168.20.160"
DEFAULT_PIN = "12345678"
CFG_SETTLE_S = 2.0       # POST /api/config is applied by T4 a loop later
MOVE_WATCH_S = 4.0       # how long a correction has to show it started
NO_MOVE_X10 = 3          # at most 0.3 % of drift for "did not move"
MOVED_X10 = 5            # at least 0.5 % for "moved"

FAILS = []


def check(ok, what):
    say("  %s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        FAILS.append(what)
    return ok


def comm(rig):
    sc, c = rig.u._req("GET", "/api/diag/commission")
    return c if sc == 200 and isinstance(c, dict) else {}


def dz(rig):
    return comm(rig).get("dz") or {}


def bench(rig, body):
    sc, out = rig.u._req("POST", "/api/diag/windowpos", body)
    if sc == 404:
        sys.exit("POST /api/diag/windowpos answered 404 -- this needs a bench build")
    return out if isinstance(out, dict) else {}


def set_src(rig, v):
    rig.u.post_cfg("motor", "deadzone_src_m3", v)
    end = time.time() + 10.0
    while time.time() < end:
        time.sleep(0.5)
        if (rig.u.cfg() or {}).get("deadzone_src_m3") == v:
            time.sleep(CFG_SETTLE_S)
            return True
    return False


def correction(rig, step_x10):
    """Command a target `step_x10` from where the leaf rests, and report
    whether M3 moved: (moved, before, after)."""
    before = rig.live_x10()
    rig.target(before + step_x10)
    seen = False
    end = time.time() + MOVE_WATCH_S
    while time.time() < end:
        if "MOVING" in rig.state().upper():
            seen = True
            break
        time.sleep(0.2)
    if seen:
        rig.wait_rest()
        time.sleep(2.5)                     # T17's settle read
    after = rig.live_x10()
    moved = seen or abs(after - before) >= MOVED_X10
    still = (not seen) and abs(after - before) <= NO_MOVE_X10
    return moved, still, before, after


def stage_config(rig):
    say("config")
    cfg = rig.u.cfg() or {}
    check("deadzone_src_m3" in cfg, "/api/config publishes deadzone_src_m3 (%r)"
          % cfg.get("deadzone_src_m3"))
    sc, lim = rig.u._req("GET", "/api/config/limits")
    lims = lim if isinstance(lim, dict) else {}
    row = lims.get("deadzone_src_m3") or (lims.get("motor") or {}).get("deadzone_src_m3")
    check(row is not None and list(row)[:2] == [0, 1] if isinstance(row, (list, tuple))
          else row is not None, "/api/config/limits has deadzone_src_m3 0-1 (%r)" % (row,))
    return cfg.get("deadzone_src_m3")


def stage_source(rig, typed_mm):
    say("source")
    bench(rig, {"band_override_mm": 0})
    bench(rig, {"meas_band_mm": 0})
    check(set_src(rig, 0), "deadzone_src_m3 -> 0 applied")
    d = dz(rig)
    check(d.get("source") == "typed" and d.get("mm") == typed_mm,
          "typed: %s %s mm" % (d.get("source"), d.get("mm")))
    check(set_src(rig, 1), "deadzone_src_m3 -> 1 applied")
    d = dz(rig)
    check(d.get("source") == "unmeasured" and d.get("mm") == typed_mm,
          "measured, none stored: %s %s mm (the typed value)" % (d.get("source"), d.get("mm")))
    out = bench(rig, {"meas_band_mm": 40})
    check(bool(out.get("ok")), "a stored measured band of 40 mm")
    d = dz(rig)
    check(d.get("source") == "measured" and d.get("mm") == 40 and d.get("measured_mm") == 40,
          "measured: %s %s mm" % (d.get("source"), d.get("mm")))
    check(set_src(rig, 0), "deadzone_src_m3 -> 0 applied")
    d = dz(rig)
    check(d.get("source") == "typed" and d.get("mm") == typed_mm and d.get("measured_mm") == 40,
          "typed beats the measurement: %s %s mm (measured %s kept)"
          % (d.get("source"), d.get("mm"), d.get("measured_mm")))
    bench(rig, {"band_override_mm": 60})
    d = dz(rig)
    check(d.get("source") == "override" and d.get("mm") == 60,
          "override beats both: %s %s mm" % (d.get("source"), d.get("mm")))
    bench(rig, {"band_override_mm": 0})
    d = dz(rig)
    check(d.get("source") == "typed" and d.get("mm") == typed_mm,
          "override cleared: %s %s mm" % (d.get("source"), d.get("mm")))


def stage_t2(rig, typed_mm, win_mm):
    say("t2  (typed %d mm = %.2f %% of a %d mm window)" % (typed_mm, 100.0 * typed_mm / win_mm,
                                                         win_mm))
    if not rig.wait_gate():
        check(False, "T17 published position control")
        return
    rig.go(500)
    sign = 1

    def between(small_mm, large_mm):
        """A correction larger than small_mm and smaller than large_mm, 0.1 %."""
        mid_mm = (small_mm + large_mm) / 2.0
        return int(round(1000.0 * mid_mm / win_mm))

    # A measured band of 40 mm against the typed band.
    bench(rig, {"meas_band_mm": 40})
    step = between(typed_mm, 40)
    set_src(rig, 1)
    moved, still, b, a = correction(rig, sign * step)
    check(still, "measured 40 mm in force: a %.1f %% correction is 'already there' "
          "(%.1f -> %.1f %%)" % (step / 10.0, b / 10.0, a / 10.0))
    set_src(rig, 0)
    moved, still, b, a = correction(rig, sign * step)
    check(moved, "typed %d mm in force: the same correction moves M3 (%.1f -> %.1f %%)"
          % (typed_mm, b / 10.0, a / 10.0))
    sign = -sign

    # The override, 60 mm, against the typed band.
    step = between(typed_mm, 60)
    bench(rig, {"band_override_mm": 60})
    moved, still, b, a = correction(rig, sign * step)
    check(still, "override 60 mm in force: a %.1f %% correction is 'already there' "
          "(%.1f -> %.1f %%)" % (step / 10.0, b / 10.0, a / 10.0))
    bench(rig, {"band_override_mm": 0})
    moved, still, b, a = correction(rig, sign * step)
    check(moved, "override cleared: the same correction moves M3 (%.1f -> %.1f %%)"
          % (b / 10.0, a / 10.0))


def stage_pulse(rig):
    say("pulse")
    sc, before = rig.u._req("GET", "/api/diag/windowpos?pulses")
    seq0 = (before or {}).get("seq", 0) if isinstance(before, dict) else 0
    out = bench(rig, {"pulse_ms": 100, "dir": "close"})
    check(bool(out.get("ok")), "CMD_PULSE accepted (%r)" % out)
    time.sleep(3.0)
    sc, after = rig.u._req("GET", "/api/diag/windowpos?pulses")
    after = after if isinstance(after, dict) else {}
    recs = after.get("pulses") or []
    last = recs[-1] if recs else {}
    check(after.get("seq", 0) == seq0 + 1 and last.get("completed") in (1, True)
          and 95000 <= int(last.get("width_us", 0)) <= 106000,
          "its record: seq %s -> %s, completed %s, %s us at the relay"
          % (seq0, after.get("seq"), last.get("completed"), last.get("width_us")))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--leave-record", type=int, default=0,
                    help="leave a stored measured band of N mm, for a reboot test")
    ap.add_argument("--expect-measured", type=int, default=0,
                    help="only check that a band of N mm came back after a reboot")
    a = ap.parse_args()

    rig = Rig(a.host, a.pin)
    print("at_wp_band -- unit %s, fw %s" % (rig.unit_id, rig.fw))
    if "bench" not in rig.fw:
        sys.exit("this needs a bench build: the hooks that set the band are bench-only")
    c = comm(rig)
    win_mm = c.get("window_mm") or 0
    if not win_mm or "dz" not in c:
        sys.exit("no taught window, or no `dz` in /api/diag/commission: not a step-2 build?")
    cfg = rig.u.cfg() or {}
    typed_mm = int(cfg.get("deadzone_m3_mm") or 0)
    src0 = cfg.get("deadzone_src_m3")

    if a.expect_measured:
        say("persist")
        d = dz(rig)
        check(d.get("measured_mm") == a.expect_measured,
              "after the reboot the stored band is back: measured_mm %s, in force %s %s mm"
              % (d.get("measured_mm"), d.get("source"), d.get("mm")))
        bench(rig, {"meas_band_mm": 0})
        d = dz(rig)
        check(d.get("measured_mm") == 0, "and it clears: measured_mm %s" % d.get("measured_mm"))
    else:
        try:
            stage_config(rig)
            stage_source(rig, typed_mm)
            rig.standby(True)
            stage_t2(rig, typed_mm, win_mm)
            stage_pulse(rig)
        finally:
            say("restoring ...")
            bench(rig, {"band_override_mm": 0})
            bench(rig, {"meas_band_mm": a.leave_record})
            if src0 is not None:
                set_src(rig, src0)
            rig.standby(False)
            d = dz(rig)
            say("  band in force: %s %s mm, measured %s, deadzone_src_m3 %s"
                % (d.get("source"), d.get("mm"), d.get("measured_mm"),
                   (rig.u.cfg() or {}).get("deadzone_src_m3")))
    rig.u.logout()
    print("\n== result ==")
    print("  %s" % ("ALL PASS" if not FAILS else "%d FAILED:\n    - %s"
                    % (len(FAILS), "\n    - ".join(FAILS))))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

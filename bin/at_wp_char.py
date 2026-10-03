#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Plan §5e on the rig: run M3's characterisation and judge what it measured.

WHAT IT DOES
------------
Starts a characterisation run through the commissioning route (admin-only, in
every build), follows it to the end, and judges the record it leaves against
what the rig is known to be (plan §3.6 and §0 item 4, measured with the bench
harnesses on 2026-10-01/02):

  speed            117 / 122 mm/s opening / closing      within 10 %
  reversal loss    10-12 mm (11.9 / 9.9 at 200 ms)       5-17 mm (plan §5e, restated 2026-10-03)
  floor 2          30 / 35 ms                            at most 60 ms, found both ways
  AT-WP02          0.8-1.0 % spread with step 2          PASS (spread <= 2.0 %)
  band             estimated ~30 mm (the lead)           derived, 10-80 mm

It also checks the hold, the teach's rule: STANDBY held from the start, still
held after the run while this session lives, released (with its
recalibration) once the session ends.

  --abort-in-phase N   abort once phase N starts (1-5; 5 is 4b), and check the
                       run ends 'operator' with the earlier phases kept
  --rest S             the rest between motor starts (default 3 s on the rig)

THE RIG MOVES for the whole run, ~15 min at a 3 s rest. Run it from Shuttle2
for anything longer: a run outlives this script by design, but its judging
does not.

Usage
  python bin/at_wp_char.py --host 192.168.20.160
  python bin/at_wp_char.py --host 192.168.20.160 --abort-in-phase 3
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from at_wp_ramp import Unit                                     # noqa: E402

DEFAULT_HOST = "192.168.20.160"
DEFAULT_PIN = "12345678"
POLL_S = 5.0
RUN_LIMIT_S = 3 * 3600

PHASES = {0: "-", 1: "1 speed", 2: "2 reversal", 3: "3 minimum move", 4: "4a AT-WP02",
          5: "4b band check"}
FAILS = []


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def check(ok, what):
    say("  %s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        FAILS.append(what)
    return ok


def comm(u):
    sc, c = u._req("GET", "/api/diag/commission")
    return c if sc == 200 and isinstance(c, dict) else {}


def standby(u):
    m = (u.status() or {}).get("mode") or {}
    return str(m.get("current", "")).upper() == "STANDBY"


def follow(u, abort_in_phase):
    """Poll until the run is over; return its last status."""
    t0 = time.time()
    last = None
    aborted = False
    while time.time() - t0 < RUN_LIMIT_S:
        ch = comm(u).get("char") or {}
        line = (ch.get("state"), ch.get("phase"), ch.get("round"), ch.get("band_mm"))
        if line != last:
            say("%-8s phase %-16s starts %3s/%s  elapsed %4ss  eta %5ss%s"
                % (ch.get("state"), PHASES.get(ch.get("phase"), ch.get("phase")),
                   ch.get("starts"), ch.get("starts_est"), ch.get("elapsed_s"), ch.get("eta_s"),
                   ("  round %s at %s mm" % (ch.get("round"), ch.get("band_mm"))
                    if ch.get("phase") == 5 else "")))
            last = line
        if ch.get("state") in ("done", "failed"):
            return ch
        if abort_in_phase and not aborted and ch.get("phase") == abort_in_phase:
            sc, out = u._req("POST", "/api/diag/commission", {"action": "abort"})
            say("abort requested in phase %s -> %s %s" % (abort_in_phase, sc, out))
            aborted = True
        time.sleep(POLL_S)
    return {}


def judge(rec, abort_in_phase, meas_before):
    print("\n  record: %s" % rec)
    if abort_in_phase:
        check(rec.get("outcome") == "operator", "the run ended 'operator' (%s)" % rec.get("outcome"))
        want = (1 << (abort_in_phase - 1)) - 1                      # the phases before it
        check(rec.get("phases") == want, "phases completed 0x%02x, expected 0x%02x"
              % (rec.get("phases", 0), want))
        check(rec.get("band_mm") == 0, "it derived no band (%s)" % rec.get("band_mm"))
        check(rec.get("meas_band_mm") == meas_before,
              "the last complete run's band is carried over (%s mm, was %s)"
              % (rec.get("meas_band_mm"), meas_before))
        return
    check(rec.get("outcome") == "none", "the run completed with a band (%s)" % rec.get("outcome"))
    check(rec.get("phases") == 0x1F, "all five phases done (0x%02x)" % rec.get("phases", 0))
    # Phase 1 reads the CRUISE speed: the encoder's own rate in the middle of a
    # long move. Not 3.6's 117 / 122 mm/s, which is the slope through pulses of
    # 20-120 ms that never reach cruise. The step-1/2 stop logs (33 stops at
    # full speed, 2026-10-01) put the encoder's rate at the cut at 122-147.5
    # mm/s by direction (medians), so the check is that band, with margin.
    so, sc = (rec.get("speed_x10") or [0, 0])
    check(1150 <= so <= 1650 and 1150 <= sc <= 1650,
          "cruise speed %.1f / %.1f mm/s (the stop logs' rate at full speed: 122-148)"
          % (so / 10.0, sc / 10.0))
    lo, lc = (rec.get("loss_x100") or [0, 0])
    check(500 <= lo <= 1700 and 500 <= lc <= 1700,       # plan 5e, restated 2026-10-03
          "reversal loss %.1f / %.1f mm (5-17 mm)" % (lo / 100.0, lc / 100.0))
    fo, fc = (rec.get("floor2_ms") or [0, 0])
    check(0 < fo <= 60 and 0 < fc <= 60, "floor 2 %s / %s ms (rig 30 / 35)" % (fo, fc))
    wp = rec.get("wp02") or {}
    check(bool(wp.get("pass")), "AT-WP02 spread %.2f %%, hysteresis %+.2f %%, landing error "
          "%.2f %% (pass at <= 2.0)" % (wp.get("spread_x100", 0) / 100.0,
                                        wp.get("hyst_x100", 0) / 100.0,
                                        wp.get("rms_x100", 0) / 100.0))
    b = rec.get("band_mm", 0)
    check(10 <= b <= 80, "band derived %s mm: candidate %s mm (term %s), %s round(s), worst "
          "landing %.1f mm" % (b, rec.get("b0_mm"), rec.get("b0_term"), rec.get("rounds"),
                               rec.get("worst_x10", 0) / 10.0))
    check(rec.get("meas_band_mm") == b, "the band is now the measured band (%s mm)"
          % rec.get("meas_band_mm"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--rest", type=int, default=3)
    ap.add_argument("--abort-in-phase", type=int, default=0)
    a = ap.parse_args()

    u = Unit(a.host, a.pin)
    sysb = (u.status() or {}).get("system") or {}
    print("at_wp_char -- unit %s, fw %s" % (sysb.get("unit_id"), sysb.get("fw_ver")))
    c = comm(u)
    if "char" not in c:
        sys.exit("no `char` in /api/diag/commission: not a build with the run")
    dz0 = c.get("dz") or {}
    meas_before = dz0.get("measured_mm", 0)
    say("before: band in force %s mm (%s); STANDBY %s" % (dz0.get("mm"), dz0.get("source"),
                                                          standby(u)))
    sc, out = u._req("POST", "/api/diag/commission",
                     {"action": "characterise", "rest_s": a.rest})
    say("start -> %s %s" % (sc, out))
    if not (isinstance(out, dict) and out.get("ok")):
        sys.exit("the run did not start")
    time.sleep(2.0)
    check(standby(u), "STANDBY is held from the start")

    end = follow(u, a.abort_in_phase)
    if not end:
        sys.exit("the run did not end within %d s -- left running on the unit" % RUN_LIMIT_S)
    say("ended: %s, reason %s, %s starts" % (end.get("state"), end.get("reason"), end.get("starts")))
    c = comm(u)
    judge(((c.get("char") or {}).get("rec")) or {}, a.abort_in_phase, meas_before)
    dz = c.get("dz") or {}
    say("band in force now: %s mm (%s), measured %s mm"
        % (dz.get("mm"), dz.get("source"), dz.get("measured_mm")))
    check(standby(u), "STANDBY is still held after the run while this session lives")

    u.logout()
    time.sleep(40.0)               # T17's release check runs within its 30 s idle cadence
    # /api/status is public: read it without opening a session, which would
    # only defer a ROTA apply for no reason.
    import json
    import urllib.request
    with urllib.request.urlopen("http://%s/api/status" % a.host, timeout=8) as resp:
        st = json.loads(resp.read().decode("utf-8", "replace"))
    mode = str(((st.get("mode") or {}).get("current", ""))).upper()
    check(mode != "STANDBY", "after logout STANDBY is released (mode %s)" % mode)

    print("\n== result ==")
    print("  %s" % ("ALL PASS" if not FAILS else "%d FAILED:\n    - %s"
                    % (len(FAILS), "\n    - ".join(FAILS))))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Plan §5e step 5: the characterisation run's acceptance tests, on the rig.

One STAGE per invocation. Each stage starts from rest, restores every setting it
changes (also when it dies half way, through atexit), logs out at the end so the
run's STANDBY hold is released the teach's way, and exits 0 on PASS, 1 on FAIL
and 2 when it could not be run (no wind, wrong build).

  full      a complete run (--rest, default 3 s) judged against the plan's
            criteria; every param-253 SD row against the record
  refusals  not fitted, M3 moving, a teach running; with --bench also a
            calibration that is not valid (the sensor's own fault):
            refused with the reason, no hold taken, M3 never moved by the run
  abort     Abort once in each phase 1-5 (--phases to choose): ended
            'operator', the earlier phases kept, no start after it, the hold
            released at logout
  hold      AUTOMATIC chosen during a run: 'hold_lost', no start after it
  wind      v_max set below the emulated wind (the emulator is left alone):
            'wind', no start after it. Exit 2 when the wind is too calm
  sensor    (bench) the sensor's fault, then its absence: 'sensor' each time
  alarm     (bench) the motor alarm: 'motor_alarm', no start after it; then
            the real clearance (60 s guard, recalibration)
  moved     (bench) M3 driven by the bench target hook during a run: 'm3_busy'
  reboot    a restart during a run, made by pushing --image with
            bin/ota_push.py (there is no reboot route; this is a software
            reset, not a power cut): after the boot no run, no STANDBY and the
            previous record intact
  rollback  2.15.1 (--old-image) pushed back, then this release (--image):
            the old firmware runs on the typed band with the new key and
            record in NVS, and this release finds both as it left them
  source    Typed <-> Measured, from the card's own view (`dz`); with --bench
            also Measured with no record (this REPLACES the stored record with
            a bench one: run `full` afterwards to get a real record back)
  rota      (bench) ROTA's apply quiet gate while a run carries on after its
            session logged out: probed as T16 would judge it (an end-to-end
            apply needs a new manifest seq, i.e. a publish). Fail-first bit 4
            opens it between the run's moves
  latency   (bench) corrections of 1.25 x the band from rest, S R S R, landed
            by T2 and measured LIVE: --band N sets the band to check
            (default: the one in force). The fail-first 2 build derives b0
            without phase 4b: there, short corrections land outside it

The fail-first build (-DWPOS_FAILFIRST_216=<mask>, failfirst_216.h) is read
from GET /api/diag/windowpos and printed, so a result is never read against the
wrong build. With bit 1 the safety stages MUST fail: the run carries on.

THE RIG MOVES. Run long stages (full, abort) from Shuttle2: a run outlives this
script by design, but its judging does not.

Usage
  python bin/at_wp_char_accept.py --host 192.168.20.160 full
  python bin/at_wp_char_accept.py --host 192.168.20.160 refusals
  python bin/at_wp_char_accept.py --host 192.168.20.160 --bench sensor
  python bin/at_wp_char_accept.py --host 192.168.20.160 reboot --image bin/2.16.0/greenhouse-controller-2.16.0.bin
"""
import argparse
import atexit
import csv
import http.client
import io
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from at_wp_ramp import Unit                                     # noqa: E402

DEFAULT_HOST = "192.168.20.160"
DEFAULT_PIN = "12345678"
POLL_S = 1.0
RUN_LIMIT_S = 3 * 3600
AFTER_WATCH_S = 20.0       # "no start after it": several 3 s rests
RELEASE_WAIT_S = 45.0      # T17's release check runs within its 30 s idle cadence
REST_TIMEOUT_S = 240.0     # a recalibration, or the alarm's 60 s guard and one

FAILS = []


def say(msg):
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def check(ok, what):
    say("  %s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        FAILS.append(what)
    return ok


def public_status(host):
    """GET /api/status without a session: a session would only defer ROTA."""
    try:
        with urllib.request.urlopen("http://%s/api/status" % host, timeout=8) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:                                          # noqa: BLE001
        return None


def x100_to_x10(v):
    """As characterise.cpp's x100_to_x10(): half away from zero."""
    return int((v + 5) / 10) if v >= 0 else -int((-v + 5) / 10)


class Rig(object):
    def __init__(self, host, pin, bench):
        self.host = host
        self.pin = pin
        self.u = Unit(host, pin)
        self.restore = []                     # (ns, key, value, field, want)
        atexit.register(self.cleanup)
        st = self.status()
        sysb = st.get("system") or {}
        self.fw = sysb.get("fw_ver")
        print("at_wp_char_accept -- unit %s, fw %s, assets %s"
              % (sysb.get("unit_id"), self.fw, sysb.get("asset_version")))
        c = self.comm()
        if "char" not in c:
            sys.exit("no `char` in /api/diag/commission: not a build with the run")
        self.bench = bench
        self.ff = 0
        if bench:
            sc, d = self.u._req("GET", "/api/diag/windowpos")
            g = (d or {}).get("gate") if isinstance(d, dict) else None
            if sc != 200 or not isinstance(g, dict):
                print("--bench needs a bench build (GET /api/diag/windowpos answered %s)" % sc)
                sys.exit(2)
            self.ff = int(g.get("failfirst_216") or 0)
            say("bench build; failfirst_216 = %d%s" % (self.ff, "  <-- A FAIL-FIRST BUILD"
                                                      if self.ff else ""))

    # --- views --------------------------------------------------------------
    def status(self):
        return public_status(self.host) or {}

    def comm(self):
        sc, c = self.u._req("GET", "/api/diag/commission")
        return c if sc == 200 and isinstance(c, dict) else {}

    def char(self):
        return self.comm().get("char") or {}

    def mode(self):
        return str(((self.status().get("mode") or {}).get("current", ""))).upper()

    def flags(self):
        return list((self.status().get("mode") or {}).get("flags") or [])

    def m3(self):
        w = self.status().get("windows") or {}
        return str(w.get("M3", "")), w.get("M3_mm_x10")

    # --- settings, restored at exit -----------------------------------------
    def set_cfg(self, ns, key, value, field=None, want=None):
        field = field or key
        cfg0 = self.u.cfg() or {}
        orig = cfg0.get(field)
        if not any(r[1] == key for r in self.restore):
            self.restore.append((ns, key, orig[2] if isinstance(orig, list) else orig, field, orig))
        if self.u.post_cfg(ns, key, value) != 200 or not self.u.settle_cfg(
                field, value if want is None else want):
            sys.exit("could not set %s/%s = %s" % (ns, key, value))
        say("%s/%s -> %s" % (ns, key, value))

    def cleanup(self):
        for ns, key, value, field, want in reversed(self.restore):
            try:
                self.u.post_cfg(ns, key, value)
                self.u.settle_cfg(field, want)
                say("restored %s/%s = %s" % (ns, key, value))
            except Exception as e:                             # noqa: BLE001
                say("COULD NOT RESTORE %s/%s = %s (%s)" % (ns, key, value, e))
        self.restore = []
        if self.bench:
            for body in ({"inject": "none"}, {"motor_alarm": "off"}):
                try:
                    if body.get("motor_alarm") and not self.alarm_injected:
                        continue
                    self.u._req("POST", "/api/diag/windowpos", body)
                except Exception:                              # noqa: BLE001
                    pass
        try:
            self.u.logout()
        except Exception:                                      # noqa: BLE001
            pass

    alarm_injected = False

    def bench_post(self, body):
        sc, out = self.u._req("POST", "/api/diag/windowpos", body)
        if sc != 200 or not (isinstance(out, dict) and out.get("ok")):
            sys.exit("bench hook %s refused: HTTP %s %s" % (body, sc, out))
        if "motor_alarm" in body:
            self.alarm_injected = body["motor_alarm"] == "on"
        say("bench -> %s" % body)
        return out

    # --- the rig at rest ------------------------------------------------------
    def wait_rest(self, limit_s=REST_TIMEOUT_S, why=""):
        """M3 not moving, nothing calibrating, no alarm: the state a stage starts
        from and a refusal must leave."""
        t0 = time.time()
        while time.time() - t0 < limit_s:
            s, _ = self.m3()
            f = self.flags()
            if not s.startswith("MOVING") and not ({"calibrating", "motor_alarm"} & set(f)):
                return True
            time.sleep(1.0)
        sys.exit("the rig did not come to rest within %d s%s" % (limit_s, why))

    # --- the run --------------------------------------------------------------
    def start(self, rest):
        sc, out = self.u._req("POST", "/api/diag/commission",
                              {"action": "characterise", "rest_s": rest})
        ok = isinstance(out, dict) and bool(out.get("ok"))
        say("start (rest %d s) -> %s %s" % (rest, sc, out))
        # With no sensor fitted the ROUTE refuses before the run is asked
        # (gh#73), as {"ok":false,"error":"not_fitted"}: the same reason, in
        # another field.
        why = (out.get("char_reason") or out.get("error")) if isinstance(out, dict) else None
        return ok, why

    def abort(self):
        sc, out = self.u._req("POST", "/api/diag/commission", {"action": "abort"})
        say("abort -> %s %s" % (sc, out))

    def follow(self, until_phase=0, limit_s=RUN_LIMIT_S, quiet=False):
        """Poll until the run ends, or until it reaches until_phase (then the
        caller acts). Returns the last `char` seen."""
        t0 = time.time()
        last = None
        ch = {}
        while time.time() - t0 < limit_s:
            ch = self.char()
            line = (ch.get("state"), ch.get("phase"), ch.get("round"))
            if line != last and not quiet:
                say("%-8s phase %s  starts %s/%s  elapsed %ss  eta %ss"
                    % (ch.get("state"), ch.get("phase"), ch.get("starts"), ch.get("starts_est"),
                       ch.get("elapsed_s"), ch.get("eta_s")))
                last = line
            if ch.get("state") != "running":
                return ch
            if until_phase and ch.get("phase") == until_phase:
                return ch
            time.sleep(POLL_S)
        return ch

    def wait_end(self, limit_s):
        """Poll until the run is no longer running; None if it carries on."""
        t0 = time.time()
        while time.time() - t0 < limit_s:
            ch = self.char()
            if ch.get("state") != "running":
                return ch
            time.sleep(POLL_S)
        return None

    def no_start_after(self, end, secs=AFTER_WATCH_S):
        """The run issues nothing after its end: still not running, the start
        count frozen. (M3 may move for OTHER reasons -- a safety close, a
        recalibration, T6 after AUTOMATIC -- which is not the run's doing.)"""
        n0 = end.get("starts")
        time.sleep(secs)
        ch = self.char()
        return check(ch.get("state") != "running" and ch.get("starts") == n0,
                     "no start after it: %s s later state %s, starts %s (at the end %s)"
                     % (int(secs), ch.get("state"), ch.get("starts"), n0))

    def end_reason(self, end, want, what):
        return check(end is not None and end.get("state") == "failed" and end.get("reason") == want,
                     "%s: the run ended '%s' (%s)"
                     % (what, want, "still RUNNING" if end is None
                        else "%s / %s" % (end.get("state"), end.get("reason"))))

    def logout_release(self):
        """Log out and see STANDBY released, as the teach's hold is."""
        self.u.logout()
        time.sleep(RELEASE_WAIT_S)
        mode = self.mode()
        check(mode != "STANDBY", "after logout STANDBY is released (mode %s)" % mode)
        self.wait_rest(why=" after the release's recalibration")
        self.u = Unit(self.host, self.pin)          # a fresh session for what follows

    # --- the SD log -----------------------------------------------------------
    def sd_rows(self, iso_from):
        st = self.status()
        sysb = st.get("system") or {}
        unit = str(sysb.get("unit_id", ""))
        boot = int(time.time()) - int(sysb.get("uptime_s", 0))
        names = ["%s_%s.csv" % (unit, time.strftime("%Y%m%d%H%M%S", time.localtime(boot + dt)))
                 for dt in range(-3, 20)]
        sc, files = self.u._req("GET", "/api/log/files")
        listed = [n for n in ((files or {}).get("sd_files") or []) if n.startswith(unit + "_")]
        names += sorted(listed, key=lambda n: re.sub(r"\D", "", n)[-14:], reverse=True)[:3]
        for name in names:
            c = http.client.HTTPConnection(self.host, 80, timeout=120)
            try:
                c.request("GET", "/api/log/download?file=" + name,
                          headers={"Cookie": self.u.cookie or ""})
                r = c.getresponse()
                body = r.read()
            finally:
                c.close()
            if r.status != 200 or not body:
                continue
            out = []
            for row in csv.reader(io.StringIO(body.decode("utf-8", "replace"))):
                if len(row) < 7 or row[0][:2] != "20" or row[0] < iso_from:
                    continue
                if row[1] == "ALARM" and row[3] == "6" and row[4] == "253":
                    out.append((row[0], int(row[5]), int(row[6])))
            say("SD log %s: %d param-253 rows since %s" % (name, len(out), iso_from))
            return out
        sys.exit("could not find this boot's SD log file")


def iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(time.time() - 1))


# =============================================================================
# stages
# =============================================================================

def stage_full(rig, a):
    rig.wait_rest()
    iso = iso_now()
    ok, why = rig.start(a.rest)
    if not check(ok, "the run starts (%s)" % why):
        return
    end = rig.follow()
    if end.get("state") == "running":
        sys.exit("the run did not end within %d s -- left running on the unit" % RUN_LIMIT_S)
    rec = (rig.comm().get("char") or {}).get("rec") or {}
    print("\n  record: %s\n" % json.dumps(rec))
    check(end.get("state") == "done" and rec.get("outcome") == "none",
          "complete (%s / %s)" % (end.get("state"), rec.get("outcome")))
    check(rec.get("phases") == 0x1F, "all five phases (0x%02x)" % rec.get("phases", 0))
    # The plan's "speed within 5 % of §3.6" compared two different things:
    # phase 1 measures the CRUISE speed, the encoder's rate in the middle of a
    # long move; §3.6's 117 / 122 mm/s is the slope through 20-120 ms pulses,
    # which never reach cruise (found in step 3). The stop logs' full-speed rate
    # (122-148 mm/s) is the reference, with margin, as in bin/at_wp_char.py.
    so, sc_ = rec.get("speed_x10") or [0, 0]
    check(1150 <= so <= 1650 and 1150 <= sc_ <= 1650,
          "cruise speed %.1f / %.1f mm/s (the stop logs' 122-148; §3.6's figure is a different "
          "quantity, see the docstring)" % (so / 10.0, sc_ / 10.0))
    lo, lc = rec.get("loss_x100") or [0, 0]
    # Restated by the operator on 2026-10-03 from 7-15 mm: nine runs read
    # 5.4-10.9 opening and 11.9-16.1 closing (the rope; closing always the
    # larger). It sanity-checks the measurement; the band is not derived from it.
    check(500 <= lo <= 1700 and 500 <= lc <= 1700,
          "reversal loss %.1f / %.1f mm (the plan: 5-17 mm)" % (lo / 100.0, lc / 100.0))
    fo, fc = rec.get("floor2_ms") or [0, 0]
    check(0 < fo <= 50 and 0 < fc <= 50, "floor 2 %s / %s ms (the plan: at most 50)" % (fo, fc))
    wp = rec.get("wp02") or {}
    check(bool(wp.get("pass")) and wp.get("spread_x100", 999) <= 200,
          "AT-WP02 spread %.2f %% (the plan: at most 2.0), hysteresis %+.2f %%, landing %.2f %%"
          % (wp.get("spread_x100", 0) / 100.0, wp.get("hyst_x100", 0) / 100.0,
             wp.get("rms_x100", 0) / 100.0))
    b = rec.get("band_mm", 0)
    check(b > 0 and rec.get("meas_band_mm") == b,
          "a band derived: %s mm (candidate %s, %s round(s), worst landing %.1f mm), now the "
          "measured band" % (b, rec.get("b0_mm"), rec.get("rounds"), rec.get("worst_x10", 0) / 10.0))
    dz = rig.comm().get("dz") or {}

    # Every param-253 row against the record.
    time.sleep(20.0)                                 # T9 writes the rows
    rows = rig.sd_rows(iso)
    got = {}
    for t, item, v in rows:
        got.setdefault(item, []).append(v)
    want = {
        1: a.rest, 10: so, 11: sc_, 12: rec.get("read_ms"),
        20: x100_to_x10(lo), 21: x100_to_x10(lc),
        30: (rec.get("dead_ms") or [0, 0])[0], 31: (rec.get("dead_ms") or [0, 0])[1],
        32: fo, 33: fc,
        34: x100_to_x10((rec.get("floor2_x100") or [0, 0])[0]),
        35: x100_to_x10((rec.get("floor2_x100") or [0, 0])[1]),
        40: wp.get("spread_x100"), 41: wp.get("hyst_x100"), 42: wp.get("rms_x100"),
        50: rec.get("b0_mm"), 51: rec.get("rounds"), 52: rec.get("worst_x10"),
        60: b, 61: dz.get("mm"), 2: 0,
    }
    bad = []
    for item, v in sorted(want.items()):
        vals = got.get(item)
        if not vals or vals[-1] != v:
            bad.append("item %d: row %s, record %s" % (item, vals, v))
    check(not bad, "every param-253 row matches the record (%d items)%s"
          % (len(want), "" if not bad else ": " + "; ".join(bad)))
    rig.logout_release()


def stage_refusals(rig, a):
    rig.wait_rest()
    mode0 = rig.mode()

    def refused(why_want, what, m3_may_move=False):
        s0, mm0 = rig.m3()
        ok, why = rig.start(a.rest)
        check(not ok and why == why_want, "%s: refused '%s' (%s)"
              % (what, why_want, "STARTED" if ok else why))
        if ok:
            rig.abort()
        time.sleep(3.0)
        ch = rig.char()
        check(ch.get("state") != "running" and not ch.get("starts"),
              "%s: the run never started (%s, %s starts)" % (what, ch.get("state"), ch.get("starts")))
        if not m3_may_move:
            s1, mm1 = rig.m3()
            # With no sensor fitted the status publishes no position at all, so
            # M3's state is the evidence there; otherwise the position too.
            still = (mm0 is None and mm1 is None) or (
                mm0 is not None and mm1 is not None and abs(mm1 - mm0) <= 30)
            check(not s1.startswith("MOVING") and s1 == s0 and still,
                  "%s: M3 did not move (%s %s -> %s %s, 0.1 mm)" % (what, s0, mm0, s1, mm1))
        return ok

    # 1. not fitted
    rig.set_cfg("motor", "wpos_fitted_m3", 0)
    refused("not_fitted", "not fitted")
    check(rig.mode() == mode0, "not fitted: no hold taken (mode %s)" % rig.mode())
    for r in list(rig.restore):
        if r[1] == "wpos_fitted_m3":
            rig.u.post_cfg(r[0], r[1], r[2])
            rig.u.settle_cfg(r[3], r[4])
            rig.restore.remove(r)
            say("restored motor/wpos_fitted_m3 = %s" % r[2])
    time.sleep(35.0)                                 # T17 re-opens its gate within 30 s

    # 2. (bench) a calibration that is not valid: the sensor reports its own fault
    if rig.bench:
        rig.bench_post({"inject": "fault"})
        # The injection reaches T17's reads only: characterise_start()'s own
        # fresh DIRECT read still sees the healthy device, and at rest T17 reads
        # once per IDLE_READ_MS (30 s). So wait until T17 has taken a faulted
        # reading and shut its gate; what is tested is then the refusal through
        # the position gate (dm_m3_position()), the path the injection can reach
        # (2026-10-03: tried 4 s after the injection, the run was accepted).
        t0 = time.time()
        reason = None
        while time.time() - t0 < 45.0:
            sc, d = rig.u._req("GET", "/api/diag/windowpos")
            reason = ((d or {}).get("gate") or {}).get("reason_str") if isinstance(d, dict) else None
            if reason == "device_fault":
                break
            time.sleep(1.0)
        say("T17's gate after %.0f s: %s" % (time.time() - t0, reason))
        cs = rig.comm()
        say("calibration now %s (%s)" % (cs.get("verdict"), cs.get("cal_reason")))
        # The verdict is judged on T17's reading, which the injection reaches, so
        # it should read INVALID (wiper open) and the run refuse as not taught.
        # If T17's gate shut first and the verdict still reads valid, the
        # refusal is the sensor's -- still right, but not the case under test.
        if cs.get("verdict") == "valid":
            say("NOTE: the verdict still reads valid, so this exercises the sensor refusal, "
                "not 'calibration not valid'")
            refused("sensor", "a faulted sensor (calibration still valid)")
        else:
            refused("not_taught", "calibration not valid (%s)" % cs.get("cal_reason"))
        rig.bench_post({"inject": "none"})
        time.sleep(35.0)

    # 3. M3 moving: a recalibration (STANDBY, then AUTOMATIC) drives every window closed
    rig.u._req("POST", "/api/mode", {"mode": "standby"})
    time.sleep(1.5)
    rig.u._req("POST", "/api/mode", {"mode": "automatic"})
    t0 = time.time()
    while time.time() - t0 < 10.0 and not rig.m3()[0].startswith("MOVING"):
        time.sleep(0.2)
    say("M3 now %s" % rig.m3()[0])
    refused("m3_busy", "M3 moving", m3_may_move=True)
    rig.wait_rest()

    # 4. a teach running: start one, refuse during it, let it finish
    sc, out = rig.u._req("POST", "/api/diag/commission", {"action": "teach"})
    say("teach -> %s %s" % (sc, out))
    t0 = time.time()
    while time.time() - t0 < 15.0 and rig.comm().get("state") not in ("arming", "traversing"):
        time.sleep(0.3)
    cs = rig.comm()
    say("teach state %s, calibration %s (%s)" % (cs.get("state"), cs.get("verdict"),
                                                cs.get("cal_reason")))
    refused("teach_running", "a teach running", m3_may_move=True)
    t0 = time.time()
    while time.time() - t0 < 180.0 and rig.comm().get("state") in ("arming", "traversing",
                                                                   "committing"):
        time.sleep(1.0)
    cs = rig.comm()
    say("teach ended: %s, calibration %s, taught %s..%s"
        % (cs.get("state"), cs.get("verdict"), cs.get("taught_closed"), cs.get("taught_open")))
    check(cs.get("verdict") == "valid", "the calibration is valid again after the teach (%s)"
          % cs.get("verdict"))
    rig.logout_release()


def stage_abort(rig, a):
    phases = [int(p) for p in a.phases.split(",")]
    for n in phases:
        print("\n  -- Abort in phase %d" % n)
        rig.wait_rest()
        meas0 = (rig.comm().get("dz") or {}).get("measured_mm", 0)
        ok, why = rig.start(a.rest)
        if not check(ok, "the run starts (%s)" % why):
            continue
        ch = rig.follow(until_phase=n)
        if ch.get("state") != "running":
            check(False, "phase %d: the run ended before it (%s, %s)"
                  % (n, ch.get("state"), ch.get("reason")))
            continue
        rig.abort()
        end = rig.wait_end(30.0)
        rig.end_reason(end, "operator", "Abort in phase %d" % n)
        rec = (rig.comm().get("char") or {}).get("rec") or {}
        wantph = (1 << (n - 1)) - 1
        check(rec.get("outcome") == "operator" and rec.get("phases") == wantph,
              "phase %d: the record keeps the phases completed (0x%02x, want 0x%02x)"
              % (n, rec.get("phases", 0), wantph))
        check(rec.get("band_mm") == 0 and rec.get("meas_band_mm") == meas0,
              "phase %d: no band derived, the measured %s mm carried over (%s)"
              % (n, meas0, rec.get("meas_band_mm")))
        if end:
            rig.no_start_after(end)
        check(rig.mode() == "STANDBY", "phase %d: STANDBY still held while the session lives" % n)
        rig.logout_release()


def stage_hold(rig, a):
    rig.wait_rest()
    ok, why = rig.start(a.rest)
    if not check(ok, "the run starts (%s)" % why):
        return
    rig.follow(until_phase=2)
    sc, out = rig.u._req("POST", "/api/mode", {"mode": "automatic"})
    say("AUTOMATIC chosen during the run -> %s %s" % (sc, out))
    end = rig.wait_end(20.0)
    rig.end_reason(end, "hold_lost", "AUTOMATIC during a run")
    if end:
        rig.no_start_after(end)
    rig.logout_release()


def stage_wind(rig, a):
    rig.wait_rest()
    w = rig.status().get("wind") or {}
    avg = float(w.get("speed_avg_ms") or 0.0)
    say("emulated wind now %.1f m/s (avg)" % avg)
    if avg < 1.0:
        print("SKIPPED: the wind averages %.1f m/s, below v_max's minimum of 1 -- nothing can "
              "be set below it, and the emulator is left alone" % avg)
        sys.exit(2)
    ok, why = rig.start(a.rest)
    if not check(ok, "the run starts (%s)" % why):
        return
    rig.follow(until_phase=2)
    rig.set_cfg("wind", "v_max", 1)
    end = rig.wait_end(120.0)                        # T3 acts on its next wind sample
    say("flags: %s" % rig.flags())
    rig.end_reason(end, "wind", "the wind override")
    if end is None:
        rig.abort()
        end = rig.wait_end(30.0) or {}
    if end:
        rig.no_start_after(end)
    rig.cleanup_one("v_max")
    rig.wait_rest(why=" after the wind override")
    rig.logout_release()


def stage_sensor(rig, a):
    if not rig.bench:
        sys.exit("the sensor stage needs --bench (the injections are bench-only)")
    for how in ("fault", "absent"):
        print("\n  -- the sensor: %s" % how)
        rig.wait_rest()
        ok, why = rig.start(a.rest)
        if not check(ok, "the run starts (%s)" % why):
            continue
        rig.follow(until_phase=2)
        rig.bench_post({"inject": how})
        end = rig.wait_end(60.0)
        rig.end_reason(end, "sensor", "the sensor's %s" % how)
        if end is None:
            rig.abort()
            end = rig.wait_end(30.0) or {}
        if end:
            rig.no_start_after(end)
        rig.bench_post({"inject": "none"})
        time.sleep(35.0)                             # the gate re-opens
        rig.logout_release()


def stage_alarm(rig, a):
    if not rig.bench:
        sys.exit("the alarm stage needs --bench (the injection is bench-only)")
    rig.wait_rest()
    ok, why = rig.start(a.rest)
    if not check(ok, "the run starts (%s)" % why):
        return
    rig.follow(until_phase=2)
    rig.bench_post({"motor_alarm": "on"})
    end = rig.wait_end(30.0)
    say("flags: %s" % rig.flags())
    rig.end_reason(end, "motor_alarm", "the motor alarm")
    if end is None:
        rig.abort()
        end = rig.wait_end(30.0) or {}
    if end:
        rig.no_start_after(end)
    rig.bench_post({"motor_alarm": "off"})
    say("clearance: the 60 s guard, then a recalibration")
    # T2 is BLOCKED through the guard and no flag says so: motor_alarm clears as
    # the guard begins, and calibrating rises only when the CLOSE_ALL starts. A
    # stage started inside the guard sees an idle rig whose T2 takes nothing
    # (2026-10-03: the next stage began 50 s into it).
    time.sleep(62.0)
    t0 = time.time()
    while time.time() - t0 < 20.0 and "calibrating" not in rig.flags():
        time.sleep(0.5)
    say("recalibration %s" % ("seen" if "calibrating" in rig.flags() else "NOT seen"))
    rig.wait_rest(why=" after the alarm's clearance")
    rig.logout_release()


def stage_moved(rig, a):
    if not rig.bench:
        sys.exit("the moved stage needs --bench (the target hook is bench-only)")
    rig.wait_rest()
    ok, why = rig.start(a.rest)
    if not check(ok, "the run starts (%s)" % why):
        return
    rig.follow(until_phase=2)
    # Between two of the run's drives: wait for a rest, then drive M3 ourselves.
    t0 = time.time()
    while time.time() - t0 < 30.0 and rig.m3()[0].startswith("MOVING"):
        time.sleep(0.1)
    pct = (rig.status().get("windows") or {}).get("M3_percent_x10") or 500
    tgt = 300 if pct > 500 else 700
    rig.bench_post({"target_x10": tgt})
    end = rig.wait_end(30.0)
    rig.end_reason(end, "m3_busy", "M3 driven by something else")
    if end is None:
        rig.abort()
        end = rig.wait_end(30.0) or {}
    if end:
        rig.no_start_after(end)
    rig.wait_rest()
    rig.logout_release()


def stage_reboot(rig, a):
    if not a.image or not os.path.isfile(a.image):
        sys.exit("the reboot stage needs --image <the image the unit runs, with its "
                 "web-assets zip beside it>")
    rig.wait_rest()
    rec0 = (rig.comm().get("char") or {}).get("rec")
    ok, why = rig.start(a.rest)
    if not check(ok, "the run starts (%s)" % why):
        return
    rig.follow(until_phase=2)
    say("restarting the unit mid-run: bin/ota_push.py %s" % os.path.basename(a.image))
    p = subprocess.run([sys.executable, os.path.join(HERE, "ota_push.py"), a.image,
                        "--host", rig.host, "--pin", rig.pin],
                       capture_output=True, text=True, timeout=900)
    tail = (p.stdout or "").strip().splitlines()[-4:]
    say("ota_push exit %s: %s" % (p.returncode, " | ".join(tail)))
    if p.returncode != 0:
        sys.exit("the push failed; the unit may still be running the run")
    time.sleep(10.0)
    rig.u = Unit(rig.host, rig.pin)                  # the old session died with the boot
    ch = rig.char()
    st = rig.status()
    check(ch.get("state") != "running", "after the boot: no run (%s)" % ch.get("state"))
    check(rig.mode() != "STANDBY", "after the boot: no STANDBY (mode %s)" % rig.mode())
    check(ch.get("rec") == rec0, "after the boot: the previous record intact%s"
          % ("" if ch.get("rec") == rec0 else ": was %s, now %s" % (rec0, ch.get("rec"))))
    say("uptime %s s" % ((st.get("system") or {}).get("uptime_s")))
    rig.wait_rest()


def stage_source(rig, a):
    rig.wait_rest()
    cfg = rig.u.cfg() or {}
    typed = cfg.get("deadzone_m3_mm")
    dz = rig.comm().get("dz") or {}
    meas = dz.get("measured_mm") or 0
    say("typed %s mm, measured %s mm, in force %s mm (%s)" % (typed, meas, dz.get("mm"),
                                                             dz.get("source")))
    rig.set_cfg("motor", "deadzone_src_m3", 0)
    dz = rig.comm().get("dz") or {}
    check(dz.get("source") == "typed" and dz.get("mm") == typed,
          "Typed: the typed %s mm in force (%s mm, %s)" % (typed, dz.get("mm"), dz.get("source")))
    rig.u.post_cfg("motor", "deadzone_src_m3", 1)
    rig.u.settle_cfg("deadzone_src_m3", 1)
    dz = rig.comm().get("dz") or {}
    want = ("measured", meas) if meas else ("unmeasured", typed)
    check(dz.get("source") == want[0] and dz.get("mm") == want[1],
          "Measured: %s mm (%s) in force (%s mm, %s)" % (want[1], want[0], dz.get("mm"),
                                                       dz.get("source")))
    if rig.bench:
        rig.bench_post({"meas_band_mm": 0})
        dz = rig.comm().get("dz") or {}
        check(dz.get("source") == "unmeasured" and dz.get("mm") == typed,
              "Measured with no record: the typed %s mm in force (%s mm, %s)"
              % (typed, dz.get("mm"), dz.get("source")))
        say("the stored record is now a bench one: run `full` to measure again")


def stage_latency(rig, a):
    """Corrections of 1.25 x b from where the leaf rests, S R S R twice,
    landed by T2 and measured LIVE through the bench diag's direct read."""
    if not rig.bench:
        sys.exit("the latency stage needs --bench (the target hook is bench-only)")
    rig.wait_rest()
    # STANDBY for the whole stage: in AUTOMATIC T6 may move M3 between two
    # corrections, and a landing would then be measured against the wrong aim.
    rig.u._req("POST", "/api/mode", {"mode": "standby"})
    say("STANDBY for the corrections")
    dz = rig.comm().get("dz") or {}
    band = a.band or dz.get("mm")
    if a.band:
        rig.bench_post({"band_override_mm": a.band})
    win = rig.comm().get("window_mm") or 1500
    step_x10 = int(round(1.25 * band * 1000.0 / win))
    say("checking a %s mm band: corrections of %.1f %% (1.25 x)" % (band, step_x10 / 10.0))

    def live_x10():
        sc, d = rig.u._req("GET", "/api/diag/windowpos")
        r = (d or {}).get("reading") or d or {}
        return int(r.get("percent_x10")), int(r.get("opening_mm_x10"))

    pattern = (True, False, False, True, True, False, False, True)
    pct0, _ = live_x10()
    open0 = pct0 < 500
    outside = []
    for i, same in enumerate(pattern):
        pct, _ = live_x10()
        up = open0 if same else not open0
        tgt = max(100, min(900, pct + (step_x10 if up else -step_x10)))
        tgt_mm_x10 = tgt * win // 100
        rig.bench_post({"target_x10": tgt})
        t0 = time.time()
        seen = False
        while time.time() - t0 < 15.0:
            s = rig.m3()[0]
            if s.startswith("MOVING"):
                seen = True
            elif seen:
                break
            time.sleep(0.1)
        time.sleep(2.5)                              # T17's settle read
        _, mm = live_x10()
        err = (mm - tgt_mm_x10) / 10.0
        say("correction %d: to %.1f %% (%s) landed %+.1f mm from its target"
            % (i + 1, tgt / 10.0, "open" if up else "close", err))
        if abs(err) > band:
            outside.append(round(err, 1))
        time.sleep(a.rest)
        open0 = up
    if a.band:
        rig.bench_post({"band_override_mm": 0})
    check(not outside, "every correction landed within the %s mm band%s"
          % (band, "" if not outside else " -- outside: %s mm" % outside))
    rig.u._req("POST", "/api/mode", {"mode": "automatic"})   # back, with its recalibration
    say("AUTOMATIC again")
    time.sleep(3.0)
    rig.wait_rest(why=" after the recalibration")


def stage_rota(rig, a):
    """ROTA's apply quiet gate against a run that outlives its session.

    End to end this needs a NEW manifest seq (T16 refuses any seq at or below
    its high-water mark), so on the bench the gate itself is probed:
    GET /api/diag/windowpos?rota_gate judges it as T16 would, with the asking
    session exempt (as gh#41 exempts the session that forced a check). Session
    A starts the run and logs out; session B probes. While the run is active
    and M3 rests between its moves, every other part of the gate is open, so
    the run is the only thing that may hold it shut."""
    if not rig.bench:
        sys.exit("the rota stage needs --bench (the probe is bench-only)")
    rig.wait_rest()
    ok, why = rig.start(a.rest)
    if not check(ok, "the run starts (%s)" % why):
        return
    rig.follow(until_phase=2, quiet=True)
    rig.u.logout()                                   # session A: the run carries on
    say("the run's session logged out; probing as another session")
    b = Unit(rig.host, rig.pin)
    rig.u = b
    rests, open_at_rest, after_open = 0, [], False
    t0 = time.time()
    run_over_at = None
    while time.time() - t0 < a.rota_watch:
        sc, g = b._req("GET", "/api/diag/windowpos?rota_gate")
        if sc != 200 or not isinstance(g, dict) or "rota_gate" not in g:
            sys.exit("no rota_gate probe (HTTP %s): build older than step 5" % sc)
        if g.get("characterising"):
            if not g.get("m3_moving"):
                rests += 1
                if g.get("rota_gate"):
                    open_at_rest.append(time.strftime("%H:%M:%S"))
        else:
            if run_over_at is None:
                run_over_at = time.time()
                say("the run is over; waiting for the gate to open")
            if g.get("rota_gate"):
                after_open = True
                break
            if time.time() - run_over_at > 150.0:
                break
        time.sleep(0.5)
    say("%d probes with the run active and M3 at rest; the gate open in %d of them"
        % (rests, len(open_at_rest)))
    check(rests >= 10, "the run was probed at rest often enough (%d >= 10)" % rests)
    check(not open_at_rest, "the gate stayed SHUT while the run was active%s"
          % ("" if not open_at_rest else ": open at %s" % ", ".join(open_at_rest[:6])))
    check(after_open, "the gate opened after the run ended (an apply could proceed)")
    rig.wait_rest()


def push(rig, image):
    p = subprocess.run([sys.executable, os.path.join(HERE, "ota_push.py"), image,
                        "--host", rig.host, "--pin", rig.pin],
                       capture_output=True, text=True, timeout=900)
    tail = (p.stdout or "").strip().splitlines()[-3:]
    say("push %s: exit %s | %s" % (os.path.basename(image), p.returncode, " | ".join(tail)))
    return p.returncode == 0


def stage_rollback(rig, a):
    """2.15.1 pushed back, then this release again: the old firmware runs with
    the new key and record in NVS (it knows neither, so its band is the typed
    one), and the new one finds both as it left them."""
    if not (a.old_image and a.image and os.path.isfile(a.old_image) and os.path.isfile(a.image)):
        sys.exit("the rollback stage needs --old-image (2.15.1) and --image (this release)")
    rig.wait_rest()
    c0 = rig.comm()
    rec0, dz0 = (c0.get("char") or {}).get("rec"), c0.get("dz")
    typed = (rig.u.cfg() or {}).get("deadzone_m3_mm")
    say("before: in force %s, record outcome %s" % (dz0, (rec0 or {}).get("outcome")))
    rig.u.logout()
    if not push(rig, a.old_image):
        sys.exit("the push of the old image failed")
    time.sleep(60.0)                                 # past the old image's self-test
    st = rig.status()
    sysb = st.get("system") or {}
    rig.u = Unit(rig.host, rig.pin)
    cfg = rig.u.cfg() or {}
    sc, c = rig.u._req("GET", "/api/diag/commission")
    say("old firmware %s / assets %s, uptime %s s" % (sysb.get("fw_ver"), sysb.get("asset_version"),
                                                     sysb.get("uptime_s")))
    check(str(sysb.get("fw_ver", "")).startswith("2.15.1") and int(sysb.get("uptime_s", 0)) >= 50,
          "the old firmware runs, and keeps running, with the new key and record in NVS "
          "(fw %s, uptime %s s)" % (sysb.get("fw_ver"), sysb.get("uptime_s")))
    check("deadzone_src_m3" not in cfg and cfg.get("deadzone_m3_mm") == typed,
          "the old firmware knows no source key: its band is the typed %s mm (config %s)"
          % (typed, cfg.get("deadzone_m3_mm")))
    check(isinstance(c, dict) and "dz" not in c and "char" not in c,
          "the old firmware has neither the band in force nor the run (HTTP %s)" % sc)
    rig.u.logout()
    if not push(rig, a.image):
        sys.exit("the push back to this release failed")
    time.sleep(10.0)
    rig.u = Unit(rig.host, rig.pin)
    c1 = rig.comm()
    sysb = (rig.status().get("system") or {})
    check(sysb.get("fw_ver") == rig.fw, "this release runs again (%s)" % sysb.get("fw_ver"))
    check((c1.get("char") or {}).get("rec") == rec0, "the record survived the rollback")
    check(c1.get("dz") == dz0, "the band in force is as before the rollback (%s)" % c1.get("dz"))
    rig.wait_rest()


def cleanup_one(self, key):
    for r in list(self.restore):
        if r[1] == key:
            self.u.post_cfg(r[0], r[1], r[2])
            self.u.settle_cfg(r[3], r[4])
            self.restore.remove(r)
            say("restored %s/%s = %s" % (r[0], r[1], r[2]))


Rig.cleanup_one = cleanup_one

STAGES = {
    "full": stage_full, "refusals": stage_refusals, "abort": stage_abort, "hold": stage_hold,
    "wind": stage_wind, "sensor": stage_sensor, "alarm": stage_alarm, "moved": stage_moved,
    "reboot": stage_reboot, "source": stage_source, "latency": stage_latency,
    "rota": stage_rota, "rollback": stage_rollback,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stage", choices=sorted(STAGES))
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--rest", type=int, default=3)
    ap.add_argument("--bench", action="store_true", help="the unit runs a bench build")
    ap.add_argument("--phases", default="1,2,3,4,5", help="abort: the phases to abort in")
    ap.add_argument("--image", help="reboot, rollback: this release's image (its zip beside it)")
    ap.add_argument("--old-image", help="rollback: the previous release's image (2.15.1)")
    ap.add_argument("--band", type=int, default=0, help="latency: the band to check, mm")
    ap.add_argument("--rota-watch", type=int, default=1500,
                    help="rota: how long to probe, s (the run at a 3 s rest takes ~9 min)")
    a = ap.parse_args()

    rig = Rig(a.host, a.pin, a.bench)
    print("== stage %s ==" % a.stage)
    STAGES[a.stage](rig, a)

    print("\n== result: stage %s, fw %s%s ==" % (a.stage, rig.fw,
                                                 ", FAIL-FIRST %d" % rig.ff if rig.ff else ""))
    print("  %s" % ("ALL PASS" if not FAILS else "%d FAILED:\n    - %s"
                    % (len(FAILS), "\n    - ".join(FAILS))))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

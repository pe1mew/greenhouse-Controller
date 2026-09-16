#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""gh#65 acceptance: the LCD manual menu's STANDBY is a session hold.

WHY THIS EXISTS
---------------
Opening the LCD manual-motor menu (screen 6, `#`) pauses automatic control
(STANDBY) for the rest of the admin's LCD session. Ending the session resumes
it. Until 2.8.0 the menu wrote that STANDBY to NVS, while the flag that lets
the session end clear it lived only in RAM. A reboot during the session (a
power blip, a ROTA night update) kept the STANDBY and lost the flag, so
ventilation stayed paused indefinitely and no later session could clear it
(gh#65). Since 2.8.0 the menu takes a STANDBY *hold*, the same mechanism a web
teach uses: never persisted, shared between sessions, and released when the
last holder's session ends.

WHAT IT DOES -- you work the LCD keypad, the script does the rest
------------------------------------------------------------------
Run it in a terminal next to the controller and follow the >>> prompts. Each
case starts in AUTOMATIC with the windows at rest.

  L  logout    open the menu -> STANDBY on. Log out -> STANDBY off, and the
               windows recalibrate (close once). That recalibration has been in
               the code since 2026-09-10 (0caff3f). Until this case it had only
               been seen in passing, in soak #1's log (2026-09-16 09:32).
  E  explicit  the script sets STANDBY first, as an operator would. Open the
               menu and log out -> STANDBY is STILL on: it was not the menu's.
               AUTOMATIC is restored afterwards.
  C  co-hold   the script runs a teach (M3 MOVES) and keeps that web session
               open. Open the menu. The script logs the web session out ->
               STANDBY stays on, because the menu still holds it. Log out on
               the LCD -> STANDBY off, and the windows recalibrate.
  R  reboot    open the menu. The script reboots the unit (it uploads the
               unit's own web assets) while the LCD session is open. After the
               boot, STANDBY must be OFF. This is gh#65 itself.

At the end the script prints the LCD SESSION rows, the STANDBY rows and the
boot rows that the run left in the SD log. That record does not depend on the
prompts having been followed.

FAIL-FIRST
----------
A build without the fix (the ropeSensor build of 088e463, for example) must
FAIL case R (STANDBY survives the reboot) and case C (the web logout ends the
pause while the menu is still open). L and E pass on both builds. They guard
the rewritten session end against regressions.

USAGE
-----
    python bin/at_lcd_standby.py --host 192.168.20.169
    python bin/at_lcd_standby.py --host 192.168.20.169 --cases CR --zip <web-assets zip>

Cases E and C, and the clean-up after a failed case, ask you to press Enter,
so run it in an interactive terminal.
Exit 0 = pass, 1 = fail, 2 = could not run or could not judge.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import csv
import http.client
import io
import os
import pathlib
import re
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# logparser formats the SD rows. Appended, not prepended, so nothing in log/
# can shadow a stdlib module.
sys.path.append(os.path.join(HERE, "..", "log"))

# The script's own directory is on sys.path, so the shared helpers import as-is.
from at_wp_ramp import Unit, DEFAULT_HOST, DEFAULT_PIN            # noqa: E402
from at_wp_teach_standby import (public_status, flags, standby_on,  # noqa: E402
                                 wait_for, quiet, set_mode, teach, not_held)
import ota_push                                                    # noqa: E402
import logparser                                                   # noqa: E402

ENTER_MENU = [
    "On the LCD: press D until the screen shows M1 M2 M3 (screen 6),",
    "press #, type the 8-digit ADMIN PIN if it asks, and press #.",
    "You are in the menu when the LCD shows '1=M1 2=M2 3=M3*B'.",
    "Do not pick a window.",
]
LOG_OUT = [
    "On the LCD, log out: press * (this leaves the menu), then A (this opens",
    "the main menu), then 3 (Access), then 3 (Logout).",
]
SETTLE_S = 300          # a recalibration drives every channel its full travel


class Ctx(object):
    lcd_timeout_s = 300
    teach_limit = 90
    zip_path = None


def say(msg):
    print("    %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def banner(lines):
    print("")
    print("  " + "=" * 70)
    for ln in lines:
        print("  >>> " + ln)
    print("  " + "=" * 70)
    sys.stdout.flush()


def ask(lines):
    """Show a prompt and wait for Enter. Raises EOFError without a terminal."""
    banner(list(lines) + ["Then press Enter here."])
    input("  ")


def stays_on(host, secs):
    """(True, None) if STANDBY is on at every public reading for secs."""
    end = time.time() + secs
    while time.time() < end:
        st = public_status(host)
        if st is not None and not standby_on(st):
            return False, st
        time.sleep(1.0)
    return True, None


def recalibrates(host):
    """A recalibration shows as the `calibrating` flag (mode WINDOW_CAL)."""
    ok, _s, _st = wait_for(
        host,
        lambda s: ("calibrating" in flags(s)
                   or (s.get("mode") or {}).get("current") == "WINDOW_CAL"),
        15, "the recalibration")
    return ok


def enter_menu(a):
    """Prompt for the menu and wait for its STANDBY. Returns (status, time) or (None, None)."""
    banner(ENTER_MENU)
    ok, _s, st = wait_for(a.host, standby_on, a.wait_min * 60, "the menu's STANDBY")
    if not ok:
        return None, None
    say("STANDBY is on: the menu has paused automatic control")
    return st, time.time()


class KeepAlive(threading.Thread):
    """Keeps a web session alive while the script waits for a person.

    Uses the raw request, never the retrying one: a re-login would open a NEW
    session, and the hold under test belongs to the old one."""

    def __init__(self, u):
        threading.Thread.__init__(self)
        self.daemon = True
        self.u = u
        self.halt = threading.Event()
        self.lost = False

    def run(self):
        while not self.halt.wait(20.0):
            try:
                sc, _body = self.u._raw("GET", "/api/diag/commission")
            except Exception:                                  # noqa: BLE001
                continue
            if sc == 401:
                self.lost = True
                return


# --------------------------------------------------------------------- cases
def case_l(a, ctx):
    print("\n[L] logging out ends the menu's STANDBY, and the windows recalibrate")
    st, t_on = enter_menu(a)
    if st is None:
        return None, "the menu's STANDBY never appeared"
    banner(LOG_OUT)
    ok, _s, st = wait_for(a.host, lambda s: not standby_on(s), ctx.lcd_timeout_s + 60,
                          "STANDBY to clear")
    if not ok:
        return False, "STANDBY still on %.0f s after the menu opened" % (time.time() - t_on)
    held = time.time() - t_on
    say("STANDBY cleared %.0f s after the menu opened (%s)"
        % (held, "the session TIMED OUT" if held >= ctx.lcd_timeout_s - 5 else "your logout"))
    if not recalibrates(a.host):
        return False, "no recalibration after the session ended"
    say("recalibration running: the windows close once")
    return True, ""


def case_e(a, ctx):
    print("\n[E] an operator's own STANDBY outlives a menu session")
    if not set_mode(a.host, a.pin, "standby"):
        return None, "could not set STANDBY via /api/mode"
    say("STANDBY set by the script, as an operator would (over the web)")
    try:
        ask(["1. Open the menu:"] + ENTER_MENU + ["2. Then log out:"] + LOG_OUT)
        ok, st = stays_on(a.host, 15)
        if not ok:
            return False, ("the menu's session end cleared an operator's STANDBY (flags %s)"
                           % flags(st))
        say("15 s after the logout STANDBY is still on: correct, it was the operator's")
        return True, ""
    finally:
        set_mode(a.host, a.pin, "automatic")
        say("AUTOMATIC restored")


def case_c(a, ctx):
    print("\n[C] a teach and the menu hold the pause together")
    u = Unit(a.host, a.pin)
    try:
        ok, why, at_start = teach(u, ctx.teach_limit, a.host)
        if at_start is False:
            return None, not_held(ok, why)
        if not ok:
            return None, why
        sc, c = u._req("GET", "/api/diag/commission")
        if not (isinstance(c, dict) and c.get("standby_held")):
            return None, "the teach does not hold STANDBY after it finished"
        say("teach done. Its web session holds STANDBY and stays open")
        ka = KeepAlive(u)
        ka.start()
        try:
            ask(ENTER_MENU + ["Stay in the menu after that."])
        finally:
            ka.halt.set()
            ka.join()
        if ka.lost:
            return None, "the teach's web session ended while waiting"
        t_menu = time.time()
        st = public_status(a.host)
        if not standby_on(st):
            return None, "STANDBY is already off (flags %s)" % flags(st)
    finally:
        u.logout()
    say("the teach's web session is logged out: its hold ends, the menu's must not")

    def dropped(st):
        if time.time() - t_menu >= ctx.lcd_timeout_s - 5:
            return None, "the LCD session may have timed out first -- run the case again"
        return False, ("STANDBY ended with the teach's session while the menu was still "
                       "open (flags %s)" % flags(st))

    v = Unit(a.host, a.pin)
    released = False
    try:
        end = time.time() + a.release_s
        while time.time() < end:
            st = public_status(a.host)
            if st is not None and not standby_on(st):
                return dropped(st)
            _sc, c = v._req("GET", "/api/diag/commission")
            if isinstance(c, dict) and c.get("standby_held") is False:
                released = True
                break
            time.sleep(1.0)
    finally:
        v.logout()
    if not released:
        return False, "the teach kept its hold %d s after its session ended" % a.release_s
    ok, st = stays_on(a.host, 5)
    if not ok:
        return dropped(st)
    say("the teach's hold is released and STANDBY is still on: the menu holds it")
    banner(LOG_OUT)
    ok, _s, st = wait_for(a.host, lambda s: not standby_on(s), ctx.lcd_timeout_s + 60,
                          "STANDBY to clear")
    if not ok:
        return False, "STANDBY did not clear when the LCD session ended"
    say("STANDBY cleared: the last hold ended")
    if not recalibrates(a.host):
        return False, "no recalibration after the last hold ended"
    say("recalibration running: the windows close once")
    return True, ""


def case_r(a, ctx):
    print("\n[R] a reboot ends the menu's STANDBY (gh#65)")
    st, _t = enter_menu(a)
    if st is None:
        return None, "the menu's STANDBY never appeared"
    up0 = st["system"]["uptime_s"]
    say("rebooting the unit with the LCD session open (uploading %s)" % ctx.zip_path.name)
    cookie = ota_push.login(a.host, a.pin)
    code, _resp = ota_push.post_bytes(a.host, "/api/ota/assets", ctx.zip_path.read_bytes(),
                                      cookie, "application/zip", timeout=120)
    say("upload answered HTTP %s" % code)
    # The unit extracts for a few seconds before it reboots. If STANDBY is
    # already gone now, the reboot cannot be what ended it.
    st2 = public_status(a.host)
    if st2 is not None and st2["system"]["uptime_s"] >= up0 and not standby_on(st2):
        return None, "STANDBY ended BEFORE the reboot -- did the LCD session end?"
    ok, _s, st = wait_for(a.host, lambda s: s["system"]["uptime_s"] < up0, 120, "the reboot")
    if not ok:
        return None, "the unit did not reboot (upload answered HTTP %s)" % code
    ok, _s, st = wait_for(a.host, lambda s: s["system"]["uptime_s"] >= 30, 90,
                          "30 s of uptime")
    if not ok:
        return None, "the unit did not come back"
    say("back up: uptime %s s, mode %s, flags %s"
        % (st["system"]["uptime_s"], st["mode"]["current"], flags(st)))
    if standby_on(st):
        return False, ("STANDBY survived the reboot, and no session is left to end it "
                       "-- the gh#65 trap")
    say("STANDBY is off after the reboot: the pause ended with the session")
    return True, ""


CASES = {"L": case_l, "E": case_e, "C": case_c, "R": case_r}


# ------------------------------------------------------------------ SD record
def sd_log(a, unit_id):
    """(file name, rows) of this unit's newest SD log, or (None, None)."""
    u = Unit(a.host, a.pin)
    try:
        sc, files = u._req("GET", "/api/log/files")
        names = files.get("sd_files") if isinstance(files, dict) else None
        if sc != 200 or not names:
            return None, None
        # The card moves between modules; names carry the writer's unit id.
        own = [n for n in names if n.upper().startswith(unit_id.upper() + "_")] or names
        newest = max(own, key=lambda n: re.sub(r"\D", "", n)[-14:])
        c = http.client.HTTPConnection(a.host, 80, timeout=60)
        try:
            c.request("GET", "/api/log/download?file=" + newest,
                      headers={"Cookie": u.cookie})
            r = c.getresponse()
            raw = r.read().decode("utf-8", "replace")
            if r.status != 200:
                return None, None
        finally:
            c.close()
    except Exception:                                          # noqa: BLE001
        return None, None
    finally:
        u.logout()
    return newest, list(csv.DictReader(io.StringIO(raw)))


def worth_showing(row):
    kind = (row.get("type") or "").strip().upper()
    who = (row.get("initiator") or "").strip().upper()
    if kind == "SESSION":
        return who in ("ADMIN", "FARMER")                      # the LCD's, not the harness's
    if kind == "MODE":
        return (row.get("param") or "").strip() == "47"        # STANDBY transitions
    if kind == "SYSTEM":
        return (row.get("value_a") or "").strip() == "5"       # boot
    return False


def print_sd_record(before, after):
    name0, rows0 = before
    name1, rows1 = after
    print("\n--- SD log record of this run ---")
    if rows1 is None:
        print("  no SD log readable")
        return
    if name1 == name0 and rows0 is not None:
        new = rows1[len(rows0):]
    else:
        new = rows1
        print("  (a new log file started during the run; earlier rows are in %s)" % name0)
    shown = [r for r in new if worth_showing(r)]
    print("  %s: %d new rows, %d shown (LCD sessions, STANDBY, boots)"
          % (name1, len(new), len(shown)))
    for i, row in enumerate(shown):
        print("  " + logparser._format_row(row, i))


# ---------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--cases", default="LECR")
    ap.add_argument("--zip", help="web-assets zip that case R uploads to reboot the unit "
                                  "(default: bin/<the unit's fw_ver>/)")
    ap.add_argument("--wait-min", type=int, default=10,
                    help="how long to wait for the menu to be opened")
    ap.add_argument("--release-s", type=int, default=45,
                    help="how long the teach's release may take after its logout")
    a = ap.parse_args()
    cases = a.cases.upper()
    if not cases or any(c not in CASES for c in cases):
        print("--cases takes letters from %s" % "".join(CASES))
        return 2

    st = public_status(a.host)
    if st is None:
        print("no answer from %s" % a.host)
        return 2
    sysb = st["system"]
    print("unit %s  fw %s  assets %s  uptime %s s  mode %s %s"
          % (sysb.get("unit_id"), sysb.get("fw_ver"), sysb.get("asset_version"),
             sysb.get("uptime_s"), st["mode"]["current"], flags(st)))

    ctx = Ctx()
    u = Unit(a.host, a.pin)
    try:
        cfg = u.cfg()
        cfg = cfg if isinstance(cfg, dict) else {}
        timeout_min = cfg.get("session_timeout_min")
        travel = (cfg.get("travel_s") or [0, 0, 171])[2]
        has_teach = u._req("GET", "/api/diag/commission")[0] == 200
    finally:
        u.logout()
    if not timeout_min:
        print("cannot read session_timeout_min")
        return 2
    ctx.lcd_timeout_s = int(timeout_min) * 60
    ctx.teach_limit = 3 * (int(travel) + 5 + 2) + 30
    print("LCD session timeout %s min; travel_m3 %s s" % (timeout_min, travel))
    if "C" in cases and not has_teach:
        print("case C needs a commissioning build (GET /api/diag/commission failed)")
        return 2
    if "R" in cases:
        if a.zip:
            ctx.zip_path = pathlib.Path(a.zip)
        else:
            ver = sysb["fw_ver"]
            _b, ctx.zip_path, _v = ota_push.derive_artifacts(
                pathlib.Path(os.path.join(HERE, ver, "greenhouse-controller-%s.bin" % ver)))
        if not ctx.zip_path.exists():
            print("case R needs %s" % ctx.zip_path)
            return 2

    print("\nYou need to be at the controller's keypad. Cases: %s" % cases)
    before = sd_log(a, sysb["unit_id"])
    results = []
    try:
        for name in cases:
            if standby_on(public_status(a.host)):
                say("STANDBY is on before case %s -- switching to AUTOMATIC first" % name)
                set_mode(a.host, a.pin, "automatic")
            ok, _s, _st = wait_for(a.host, lambda s: quiet(s) and not standby_on(s),
                                   SETTLE_S, "a quiet AUTOMATIC unit with M3 at rest")
            if not ok:
                results.append((name, None, "the unit did not settle"))
                break
            time.sleep(3.0)
            res, why = CASES[name](a, ctx)
            label = {True: "PASS", False: "FAIL", None: "NOT JUDGED"}[res]
            print("  case %s: %s%s" % (name, label, "" if res else " -- " + why))
            results.append((name, res, why))
            if res is not True and name != "R":
                ask(["If the LCD still has an admin session, log out now."] + LOG_OUT)
    except EOFError:
        print("\nthis needs an interactive terminal: it asks you to press Enter")
        return 2
    except KeyboardInterrupt:
        print("\ninterrupted")
        results.append(("?", None, "interrupted"))
    finally:
        st = public_status(a.host)
        if st is not None and standby_on(st):
            set_mode(a.host, a.pin, "automatic")
            say("STANDBY was still on at the end -- AUTOMATIC restored")

    time.sleep(10.0)                 # let the last rows reach the card
    print_sd_record(before, sd_log(a, sysb["unit_id"]))

    print("\n--- verdict ---")
    for name, res, why in results:
        print("  %s %s%s" % (name, {True: "PASS", False: "FAIL", None: "NOT JUDGED"}[res],
                             "" if res else " -- " + why))
    if any(res is False for _n, res, _w in results):
        print("RESULT: FAIL")
        return 1
    if len(results) < len(cases) or any(res is None for _n, res, _w in results):
        print("RESULT: INCOMPLETE")
        return 2
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

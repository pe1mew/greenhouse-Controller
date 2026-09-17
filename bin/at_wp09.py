#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT-WP09 -- obstruct M3 mid-travel, confirm the divergence is reported.

The acceptance test that `integrateWindowPositionSensor.md` records as *"not
run, and not yet runnable -- it needs 12.4 rule 1, which IS the divergence
detector"*. Rule 1 landed 2026-09-15, so this is now runnable.

WHAT IT PROVES
--------------
The controller notices that the leaf is not following the motor. That matters
because the device cannot notice it: a shorted wiper reports a perfectly
plausible CONSTANT position, every status bit stays clear, `sensor_fault` stays
false, and bit 6 is inert on this installation for lack of electrical headroom.
Rule 1 is the only thing standing between that and a window the controller
believes it has moved.

WHY IT NEEDS A HUMAN
--------------------
The divergence has to be physical. Three ways to produce it, easiest first:

  1. **Detach the draw-wire** from the leaf and command a stroke. The motor
     runs, the wire does not, position sits still. Closest to the real fault.
  2. **Obstruct the leaf** so the motor drives against a jam.
  3. **Short the wiper** -- what the rule exists for, but do not do this to a
     working sensor just to test it.

So this script is an OBSERVER, not a driver. It reads the unit's own counters
across a stroke you cause, and judges what it sees.

THE PASS IS THE EASY HALF -- READ THE FAIL-FIRST NOTE
-----------------------------------------------------
A detector that fires is worth little until you know it can stay silent. This
is a fault detector on a mechanism that moves several times a day, so a false
positive is not a nuisance: it would eventually be the reason someone stops
believing the row. `--healthy` runs the same measurement across an UNOBSTRUCTED
stroke and asserts the counter does NOT move. **Run both.** The gotcha log's
standing rule is "show the check can fail before trusting a pass"; for a
detector the inverse matters just as much, and the AT-WP05 lesson was exactly
this -- a headline counter that could not fail proved nothing over 7117 reads.

THE SEQUENCE (--sequence): both halves, and the case the exemption could hide
---------------------------------------------------------------------------
Rule 1 got an at-end exemption on 2026-09-16: a stroke toward the end the leaf
already sits at is not judged. The exemption holds only while, on every sample
of the grace window, the end sensor stays made AND the reading stays at the
TARGET end. A leaf that does not follow must still be judged, and the case that
needs care is a reading stuck at the target end while the leaf starts at the
other end with its end sensor made (a shorted wiper reads 0 on an open window):
there, only the end sensor dropping as the leaf leaves breaks the exemption.

Whether a run exercises that "continuity break" depends on the rig, so each
fault stroke REPORTS whether the exemption's start conditions held (end sensor
made, reading at the target end), judged from the last reading before the
stroke. On FDA4 on 2026-09-17 they did not: the detached wire held its last
reading (1500 mm, the open end), and with the wire off the closed end sensor
released at rest (contract 5.3: a sensor zone shorter than the overtravel).
Both fault strokes still reported the stall, but neither tested the break.

`--sequence` walks through it at the LCD manual menu (screen 6, `#`, `3`):

  1. healthy CLOSE                 wire attached (skipped if M3 starts CLOSED)
  2. healthy OPEN                  wire attached, M3 ends OPEN
  3. detach the wire               ONLY with M3 OPEN and at rest
  4. fault CLOSE                   the exemption case: rule 1 must report it
  5. fault OPEN                    rule 1 must report it; M3 ends OPEN
  6. reattach the wire             ONLY with M3 OPEN and at rest; the sensor
                                   must come back
  7. healthy CLOSE                 wire attached
  8. log out on the LCD            the session end closes the windows once

The wire may be detached and reattached only with the rig OPEN (operator,
2026-09-17), so the script waits until `/api/status` shows M3 OPEN before it
asks, and gets M3 back to OPEN first if a stroke did not end there.

A fault stroke can also end INCONCLUSIVE instead of PASS or FAIL. That happens
when the detached wire makes the device report its fault sentinel, which shuts
the sensor gate before rule 1 gets to judge; that is the correct response to a
faulting sensor, but it does not test rule 1. It also happens when the position
moved during the stroke, which means the wire was still attached.

**It adds two deliberate stall faults to the unit's counters**, so a soak
running at the same time will report NOT CLEAN. Judge the soak first.

**Safety.** The LCD session pauses automatic control, but the wind safety can
still close the windows at any time (the rig's emulated wind sensor has done
so). Touch the wire only when the script says M3 is at rest. If the LCD session
times out (5 min without a key), the controller closes the windows: in the M3
screen, `3` does nothing and keeps the session alive.

USAGE
-----
    # arm the observer, then cause an obstructed stroke when it says to
    python bin/at_wp09.py --host 192.168.20.169

    # the false-positive half: a normal, unobstructed stroke must NOT trip it
    python bin/at_wp09.py --host 192.168.20.169 --healthy

    # everything above in one guided run (needs a person at the rig)
    python bin/at_wp09.py --host 192.168.20.169 --sequence

Exit 0 = the criterion held, 1 = it did not, 2 = could not run or could not judge.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import atexit
import http.client
import json
import sys
import time

DEFAULT_HOST = "192.168.20.169"     # FDA4; pass --host for 2344 (.160)
DEFAULT_PIN = "12345678"
POLL_S = 1.0
DIAG = "/api/diag/windowpos"
# Rule 1 decides within min(5 s, travel_m3 / 2) of the stroke start. A stroke is
# judged over at least this long, and until M3 reads OPEN or CLOSED again.
JUDGE_S = 8.0
SETTLED = ("OPEN", "CLOSED")
PASS, FAIL, INCONCLUSIVE = 0, 1, 2
WORD = {PASS: "PASS", FAIL: "FAIL", INCONCLUSIVE: "INCONCLUSIVE"}
# A leaf that moves more than this while the wire is supposed to be detached
# means the wire was still on it (0.1 mm).
MOVED_X10 = 500

# The full opening (40004) in 0.1 mm, learned from any reading well away from
# the closed end; used only to say whether a reading sat at the open end.
_full_x10 = [None]
# Start conditions of the last observed stroke: (end sensor made, reading at the
# target end), or None when they could not be judged.
LAST_START = [None]


def learn_full(d):
    pct = int(d.get("percent_x10", 0) or 0)
    mm = d.get("opening_mm_x10")
    if d.get("ok") and not d.get("sensor_fault") and isinstance(mm, int) and pct >= 500:
        _full_x10[0] = mm * 1000 // pct


def deadzone_x10_of(u):
    sc, cfg = u._req("GET", "/api/config")
    mm = cfg.get("deadzone_m3_mm") if isinstance(cfg, dict) else None
    return int(mm) * 10 if isinstance(mm, int) and mm > 0 else 200


LCD_MENU = [
    "On the LCD: press D until the screen shows M1 M2 M3 (screen 6), press #,",
    "type the 8-digit ADMIN PIN if it asks, press #, then press 3 (M3).",
    "The LCD then shows '[M3] ...' with '1=Open 2=Cls *Bk'. Stay on that screen:",
    "the script tells you which key to press for each stroke.",
]
LOG_OUT = [
    "On the LCD, log out: press * twice (back to the status screens), then A",
    "(main menu), then 3 (Access), then 3 (Logout).",
]


class Unit(object):
    """Admin session against one unit. Same shape as at_cfg_roundtrip.py's."""

    def __init__(self, host, pin):
        self.host = host
        self.pin = pin
        self.cookie = None
        self._login(pin)

    def _raw(self, method, path, body=None):
        c = http.client.HTTPConnection(self.host, 80, timeout=10)
        hdr = {"Content-Type": "application/json"}
        if self.cookie:
            hdr["Cookie"] = self.cookie
        c.request(method, path,
                  json.dumps(body).encode() if body is not None else None, hdr)
        r = c.getresponse()
        raw = r.read()
        sc = r.getcode()
        sk = r.getheader("Set-Cookie")
        if sk:
            self.cookie = sk.split(";")[0]
        c.close()
        try:
            return sc, json.loads(raw.decode("utf-8"))
        except Exception:                                      # noqa: BLE001
            return sc, raw.decode("utf-8", "replace")

    def _req(self, method, path, body=None, _retry=True):
        # The admin session expires after session_timeout minutes (default 5)
        # and an obstructed-stroke run can outlast it while waiting for a human.
        sc, out = self._raw(method, path, body)
        if sc == 401 and _retry and path != "/api/login":
            self.cookie = None
            self._login(self.pin)
            return self._raw(method, path, body)
        return sc, out

    def _login(self, pin):
        sc, _ = self._req("POST", "/api/login", {"role": "admin", "pin": pin})
        if sc != 200 or not self.cookie:
            sys.exit("login failed (HTTP %s) -- wrong PIN, or the unit is not up" % sc)
        atexit.register(self.logout)

    def logout(self):
        """Give the session slot back. The unit holds FOUR, RAM-only, and an
        open admin session defers ROTA (gh#41) and keeps a teach's STANDBY
        hold alive, so a script must not leave one behind. Registered with
        atexit at login, so every exit -- sys.exit() included -- releases it."""
        if self.cookie:
            try:
                self._raw("POST", "/api/logout", {})
            except Exception:                                  # noqa: BLE001
                pass                       # best effort: the timeout still frees it
            self.cookie = None

    def diag(self):
        sc, out = self._req("GET", DIAG)
        if sc != 200 or not isinstance(out, dict):
            sys.exit("%s returned HTTP %s -- is this a ropeSensor build?" % (DIAG, sc))
        return out

    def status(self):
        sc, out = self._req("GET", "/api/status")
        return out if sc == 200 and isinstance(out, dict) else {}


def soak_of(d):
    s = d.get("soak")
    if not isinstance(s, dict):
        sys.exit("no `soak` block in %s -- build too old for this test" % DIAG)
    return s


def gate_of(d):
    g = d.get("gate")
    return g if isinstance(g, dict) else {}


def m3_of(st):
    return (st.get("windows") or {}).get("M3")


def flags_of(st):
    return (st.get("mode") or {}).get("flags") or []


def delta(s, s0, key):
    return int(s.get(key, 0) or 0) - int(s0.get(key, 0) or 0)


def say(msg):
    print("    %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def banner(lines):
    print("")
    print("  " + "=" * 72)
    for ln in lines:
        print("  >>> " + ln)
    print("  " + "=" * 72)
    sys.stdout.flush()


def ask(lines):
    """Show a prompt and wait for Enter. Raises EOFError without a terminal."""
    banner(list(lines) + ["Then press Enter here."])
    input("  ")


# ------------------------------------------------------------ one stroke
def observe(u, healthy, timeout, prompt, deadzone_x10=200):
    """Watch one stroke that the operator commands, and judge it.

    The stroke is found in /api/status (M3 leaves OPEN/CLOSED) as well as in
    T17's `strokes` counter: with the sensor gate shut T17 does not count
    strokes, and a fault stroke may run exactly like that.

    Returns (code, reason).
    """
    d0 = u.diag()
    s0 = soak_of(d0)
    learn_full(d0)
    rest = d0 if (d0.get("ok") and not d0.get("sensor_fault")) else None
    banner(prompt)
    t0 = time.time()
    moved_at = counted_at = fired_at = None
    direction = None
    reasons = set()
    fault_readings = 0
    pos = []
    s = s0
    timed_out = True
    while time.time() - t0 < timeout:
        time.sleep(POLL_S)
        now = time.time() - t0
        d = u.diag()
        s = soak_of(d)
        learn_full(d)
        reasons.add(str(gate_of(d).get("reason_str", "?")))
        if d.get("ok"):
            if d.get("sensor_fault"):
                fault_readings += 1
            elif isinstance(d.get("opening_mm_x10"), int):
                pos.append(d["opening_mm_x10"])
        m3 = m3_of(u.status())
        if moved_at is None and m3 in SETTLED and d.get("ok") and not d.get("sensor_fault"):
            rest = d                                 # the last reading before the stroke
        if moved_at is None and m3 is not None and m3 not in SETTLED:
            moved_at = now
            direction = m3
        if counted_at is None and delta(s, s0, "strokes") > 0:
            counted_at = now
        if fired_at is None and delta(s, s0, "stall_faults") > 0:
            fired_at = now
        start = moved_at if moved_at is not None else counted_at
        if start is not None and now - start >= JUDGE_S and m3 in SETTLED:
            timed_out = False
            break

    start = moved_at if moved_at is not None else counted_at
    k = dict((key, delta(s, s0, key)) for key in (
        "strokes", "reads_ok", "rejected_rate", "err_comm", "stall_faults",
        "early_stops", "at_end_exempt", "gated_polls"))
    span = (max(pos) - min(pos)) if pos else None
    say("stroke seen: %s (T17 counted it: %s)%s"
        % ("t+%.0f s" % start if start is not None else "no",
           "yes" if counted_at is not None else "no",
           "  -- still moving when the wait ran out" if timed_out and start is not None else ""))
    say("stall reported: %s"
        % ("%.0f s after the stroke started" % (fired_at - start)
           if fired_at is not None and start is not None
           else ("yes" if fired_at is not None else "no")))
    say("reads +%d, rejected +%d, comm errors +%d, early stops +%d, "
        "exempt at end +%d, gated polls +%d"
        % (k["reads_ok"], k["rejected_rate"], k["err_comm"], k["early_stops"],
           k["at_end_exempt"], k["gated_polls"]))
    say("gate reasons seen: %s; fault readings: %d; position moved: %s"
        % (", ".join(sorted(reasons)), fault_readings,
           "%.1f mm" % (span / 10.0) if span is not None else "no valid reading"))

    LAST_START[0] = None
    target = {"MOVING_CLOSE": "CLOSED", "MOVING_OPEN": "OPEN"}.get(direction)
    if rest is not None and target is not None:
        mm = rest.get("opening_mm_x10")
        bit3 = bool(rest.get("at_end_sensor"))
        if not isinstance(mm, int):
            at_target = False
        elif target == "CLOSED":
            at_target = mm <= deadzone_x10
        else:
            at_target = _full_x10[0] is not None and mm + deadzone_x10 >= _full_x10[0]
        LAST_START[0] = (bit3, at_target)
        say("before the stroke: end sensor %s, reading %.1f mm (%s the %s end)"
            % ("made" if bit3 else "clear", mm / 10.0 if isinstance(mm, int) else -1,
               "at" if at_target else "not at", target))
    if start is None:
        return INCONCLUSIVE, ("no stroke was observed -- nothing was tested. T2 must "
                              "actually energise M3; a stroke you did not cause is not "
                              "a null result")
    judged = k["reads_ok"] >= 2 and k["gated_polls"] == 0 and reasons == {"ok"}

    if healthy:
        if fired_at is not None:
            return FAIL, ("rule 1 tripped on an UNOBSTRUCTED stroke. This is a false "
                          "positive. Before touching the threshold, check `travel_m3` "
                          "against the wired window -- nominal derives from it, so a rig "
                          "value left at the production 171 makes every real stroke look "
                          "~13x too slow and would trip this every time")
        if k["at_end_exempt"] > 0:
            return INCONCLUSIVE, ("the stroke was exempt (the leaf was already at the end "
                                  "it was driven toward), so rule 1 did not judge it")
        if not judged:
            return INCONCLUSIVE, ("rule 1 did not get to judge this stroke: the sensor "
                                  "gate was not open throughout, or T17 took no readings")
        return PASS, "a healthy stroke did NOT trip 12.4 rule 1"

    if fired_at is not None:
        if LAST_START[0] == (True, True):
            note = ("The exemption's start conditions held, so the end sensor dropping is "
                    "what kept this stroke judged: the continuity break was exercised.")
        elif LAST_START[0] is None:
            note = "Whether the exemption's start conditions held could not be judged."
        else:
            note = ("The exemption's start conditions did not hold (end sensor made: %s, "
                    "reading at the target end: %s), so this stroke did NOT test the "
                    "continuity break." % ("yes" if LAST_START[0][0] else "no",
                                           "yes" if LAST_START[0][1] else "no"))
        return PASS, ("the divergence was reported. " + note + " The ALARM ch6 param 249 "
                      "row on the SD log carries the peak rate and threshold; "
                      "`logparser.py` decodes it")
    if k["at_end_exempt"] > 0:
        return FAIL, ("the at-end exemption SWALLOWED a real stall: the stroke counted as "
                      "exempt although the leaf did not follow the motor")
    if span is not None and span >= MOVED_X10:
        return INCONCLUSIVE, ("the position followed the leaf (%.1f mm), so the wire was "
                              "still attached: not a divergence" % (span / 10.0))
    if not judged:
        return INCONCLUSIVE, ("the sensor dropped out (gate: %s) before rule 1 could judge. "
                              "That is the right response to a faulting sensor, but it does "
                              "not test rule 1; command the stroke sooner after detaching"
                              % ", ".join(sorted(reasons)))
    return FAIL, ("the leaf did not follow and 12.4 rule 1 did NOT report it. Check, in "
                  "this order: did `strokes` move? is `rejected_rate` climbing -- every "
                  "sample implausible also means no accepted evidence of movement")


# ------------------------------------------------------- single stroke
def single(args, u):
    st = u.status()
    unit_id = (st.get("system") or {}).get("unit_id", "?")
    d0 = u.diag()
    s0 = soak_of(d0)
    g0 = gate_of(d0)
    if "stall_faults" not in s0:
        sys.exit("this build has no `stall_faults` counter -- 12.4 rule 1 is not\n"
                 "in it, so AT-WP09 has nothing to observe. Flash a build with it.")

    print("AT-WP09 -- %s on %s" % (
        "FALSE-POSITIVE half (healthy stroke)" if args.healthy
        else "divergence half (obstructed stroke)", unit_id))
    print("  gate now           : mode %s, reason %s"
          % (g0.get("mode_str", "?"), g0.get("reason_str", "?")))
    print("  strokes so far     : %s" % s0.get("strokes"))
    print("  stall_faults so far: %s" % s0.get("stall_faults"))
    # The published MODE may read timed while T17 polls (promotion waits for a
    # stroke boundary); what stops T17 polling is a gate REASON other than ok.
    if str(g0.get("reason_str", "")) != "ok":
        print("\n  NOTE: the sensor gate is shut, so T17 cannot judge this stroke.")

    if args.healthy:
        prompt = ["Command a NORMAL, unobstructed M3 stroke now."]
    else:
        prompt = ["Obstruct M3 (or detach the draw-wire from the leaf), then",
                  "command an M3 stroke now."]
    prompt.append("Waiting up to %d s." % args.timeout)
    code, why = observe(u, args.healthy, args.timeout, prompt, deadzone_x10_of(u))
    print("\n%s: %s" % (WORD[code], why))
    return code


# ------------------------------------------------------------- sequence
def wait_m3(u, want, limit_s):
    end = time.time() + limit_s
    while time.time() < end:
        if m3_of(u.status()) == want:
            return True
        time.sleep(POLL_S)
    return False


def ensure_open(u):
    """The wire may be handled only with M3 OPEN (operator, 2026-09-17)."""
    for _ in range(3):
        if m3_of(u.status()) == "OPEN":
            time.sleep(2.0)                          # and at rest, not just arrived
            return m3_of(u.status()) == "OPEN"
        banner(["M3 is not OPEN, and the wire may only be handled with M3 OPEN.",
                "Press 1 (Open) on the LCD and wait."])
        wait_m3(u, "OPEN", 90)
    return False


def sensor_back(u, limit_s):
    """After reattaching: the gate is open and the reading is near the open end."""
    end = time.time() + limit_s
    d = {}
    while time.time() < end:
        d = u.diag()
        g = gate_of(d)
        if (d.get("ok") and not d.get("sensor_fault")
                and str(g.get("reason_str", "")) == "ok"
                and int(d.get("percent_x10", 0) or 0) >= 900):
            return True, d
        time.sleep(2.0)
    return False, d


def sequence(args, u):
    st = u.status()
    sysb = st.get("system") or {}
    d0 = u.diag()
    s0 = soak_of(d0)
    g0 = gate_of(d0)
    print("AT-WP09 sequence -- %s  fw %s" % (sysb.get("unit_id", "?"), sysb.get("fw_ver", "?")))
    if "stall_faults" not in s0 or "at_end_exempt" not in s0:
        print("this build lacks the rule 1 counters or the at-end exemption -- not run")
        return INCONCLUSIVE
    fl = flags_of(st)
    for bad in ("wind_override", "motor_alarm", "calibrating", "standby"):
        if bad in fl:
            print("the unit shows '%s' -- clear it first (STANDBY: switch to AUTOMATIC; "
                  "the menu sets its own)" % bad)
            return INCONCLUSIVE
    if not d0.get("ok") or str(g0.get("reason_str", "")) != "ok":
        print("the sensor is not answering cleanly (gate reason %s) -- fix that first"
              % g0.get("reason_str", "?"))
        return INCONCLUSIVE
    sc, cfg = u._req("GET", "/api/config")
    travel = ((cfg.get("travel_s") or [0, 0, 0])[2]) if isinstance(cfg, dict) else 0
    print("travel_m3 %s s, gate %s/%s, M3 %s, counters: strokes %s, stall_faults %s, "
          "at_end_exempt %s"
          % (travel, g0.get("mode_str"), g0.get("reason_str"), m3_of(st),
             s0.get("strokes"), s0.get("stall_faults"), s0.get("at_end_exempt")))
    end = time.time() + 60
    while time.time() < end and m3_of(u.status()) not in SETTLED:
        time.sleep(POLL_S)
    if m3_of(u.status()) not in SETTLED:
        print("M3 is not at rest -- not run")
        return INCONCLUSIVE
    start_open = m3_of(u.status()) == "OPEN"

    print("\nThe run: %s healthy OPEN, detach the wire (M3 OPEN), fault CLOSE, "
          "fault OPEN, reattach (M3 OPEN), healthy CLOSE, log out."
          % ("healthy CLOSE," if start_open else ""))
    ask(["This adds two deliberate stall faults to the unit's counters: a soak",
         "running now will report NOT CLEAN afterwards.",
         "The wind safety can still close the windows at any time. Touch the",
         "wire only when this script says M3 is OPEN and at rest.",
         "If 4 minutes pass without a key, press 3 on the LCD: it does nothing",
         "on the M3 screen and keeps the session (and the pause) alive."])

    banner(LCD_MENU)
    t0 = time.time()
    while time.time() - t0 < 600 and "standby" not in flags_of(u.status()):
        time.sleep(POLL_S)
    if "standby" not in flags_of(u.status()):
        print("the menu's STANDBY did not appear within 10 min -- not run")
        return INCONCLUSIVE
    say("the LCD menu holds STANDBY: automatic control is paused")

    results = []
    continuity = []
    dz = deadzone_x10_of(u)

    def stroke(label, healthy, key, end):
        verb = "Open" if key == "1" else "Close"
        print("\n[%s]" % label)
        code, why = observe(u, healthy, args.timeout,
                            ["%s: press %s (%s) on the LCD." % (label, key, verb)], dz)
        print("  %s: %s -- %s" % (label, WORD[code], why))
        results.append((label, code))
        if not healthy:
            continuity.append(LAST_START[0] == (True, True))
        if not wait_m3(u, end, 60):
            say("M3 did not end %s" % end)
        return code

    if start_open:
        stroke("1 healthy CLOSE", True, "2", "CLOSED")
    stroke("2 healthy OPEN", True, "1", "OPEN")

    if not ensure_open(u):
        print("M3 could not be brought to OPEN, so the wire must not be touched -- stopping")
        results.append(("3 detach", INCONCLUSIVE))
        return finish(u, results, s0)
    say("M3 is OPEN and at rest")
    ask(["Detach the draw-wire from the leaf now.",
         "Leave the LCD on the M3 screen."])
    stroke("4 fault CLOSE (wire detached)", False, "2", "CLOSED")
    stroke("5 fault OPEN (wire detached)", False, "1", "OPEN")

    if not ensure_open(u):
        print("M3 could not be brought to OPEN, so the wire must not be touched.")
        print("Reattach it later, with M3 OPEN. Stopping.")
        results.append(("6 reattach", INCONCLUSIVE))
        return finish(u, results, s0)
    say("M3 is OPEN and at rest")
    ask(["Reattach the draw-wire to the leaf now."])
    ok, d = sensor_back(u, 120)
    say("sensor after reattaching: ok %s, fault %s, gate %s, position %.1f %%"
        % (d.get("ok"), d.get("sensor_fault"), gate_of(d).get("reason_str"),
           int(d.get("percent_x10", 0) or 0) / 10.0))
    results.append(("6 sensor back at the open end", PASS if ok else FAIL))
    if not ok:
        print("  the sensor did not come back within 120 s -- check the wire before "
              "the last stroke")

    stroke("7 healthy CLOSE", True, "2", "CLOSED")

    banner(LOG_OUT)
    t0 = time.time()
    while time.time() - t0 < 360 and "standby" in flags_of(u.status()):
        time.sleep(POLL_S)
    say("STANDBY %s" % ("cleared: automatic control resumes"
                        if "standby" not in flags_of(u.status())
                        else "still on after 6 min -- log out on the LCD"))
    if continuity and not any(continuity):
        print("\nNOTE: no fault stroke started with the exemption's conditions met, so the")
        print("continuity break (end sensor dropping as the leaf leaves) was not exercised.")
    return finish(u, results, s0)


def finish(u, results, s0):
    time.sleep(20.0)                  # the logout's recalibration, if any
    s = soak_of(u.diag())
    print("\n--- counters over the whole run ---")
    for key in ("strokes", "stall_faults", "early_stops", "at_end_exempt",
                "rejected_rate", "err_comm", "gated_polls"):
        print("  %-14s +%d" % (key, delta(s, s0, key)))
    print("\n--- verdict ---")
    for label, code in results:
        print("  %-34s %s" % (label, WORD[code]))
    if any(code == FAIL for _l, code in results):
        print("RESULT: FAIL")
        return FAIL
    if any(code != PASS for _l, code in results):
        print("RESULT: INCOMPLETE")
        return INCONCLUSIVE
    print("RESULT: PASS")
    return PASS


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--healthy", action="store_true",
                    help="false-positive half: an UNOBSTRUCTED stroke must not trip the rule")
    ap.add_argument("--sequence", action="store_true",
                    help="both halves in one guided run, including the exemption case")
    ap.add_argument("--timeout", type=int, default=300,
                    help="seconds to wait for each stroke (default 300)")
    args = ap.parse_args()

    u = Unit(args.host, args.pin)
    try:
        return sequence(args, u) if args.sequence else single(args, u)
    except EOFError:
        print("\nthis needs an interactive terminal: it asks you to press Enter")
        return INCONCLUSIVE
    except KeyboardInterrupt:
        print("\ninterrupted")
        return INCONCLUSIVE


if __name__ == "__main__":
    sys.exit(main())

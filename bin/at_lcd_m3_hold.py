#!/usr/bin/env python3
"""at_lcd_m3_hold.py -- gh#93 acceptance: the LCD's tap-or-hold control of M3, on the rig.

gh#93 (2.17.0): on the LCD's manual-motor screen, M3's Open (1) and Close (2)
keys are timed. A tap (released within 1 s) is a full stroke, on the release;
a 1-2 s press does nothing, and the LCD says why; a hold of 2 s or more
moves M3 from the 2 s mark until the key is released, and T2 stops it there
(CMD_STOP, PART_OPEN). M1 and M2 are unchanged: one press, a full stroke.

Nobody needs to be at the keypad. A bench build's POST /api/diag/key holds a
key in T7 for a given time, so the press, the repeats and the release all come
from T7's own code, and GET /api/diag/key reports T8's screen and the M3 hold's
latest action. The script walks the menus itself: status page 6, `#`, the
admin PIN, then `3`.

Cases, run in the order given (default: all, in this order):

  tap    M3 CLOSED first. A 300 ms press of 1: action FULL, then M3 reaches
         OPEN by itself. A 300 ms press of 2: FULL, then CLOSED.
  mid    A 1500 ms press of 1: action MID, the LCD says "Tap <1s: to end",
         and M3 stays CLOSED.
  hold   From CLOSED, a 5000 ms press of 1. T8 decides the move 2000-2150 ms
         into the press, and the LCD shows "Hold 2s to move", then "Release to
         stop". The action is MOVE_STOP with the press's length, and M3 rests
         PART_OPEN. Then, from there, 3000 ms and 5000 ms presses: each ends
         PART_OPEN further open, the 5 s one 2-4.5 times as far as the 3 s one
         (3 s of drive against 1). The first hold is not measured: from CLOSED
         the rig's rope takes up its slack for about the first second of a drive.
  back   A 4000 ms press of 2: PART_OPEN, at least 5 points less open.
  long   A press of 1 longer than T2's full stroke (travel_m3 + 5 s + 3 s):
         M3 reaches OPEN by the timer while the key is still down, and the
         release changes nothing: M3 stays OPEN. Then a tap of 2: CLOSED.
  m1     M1 unchanged: a 300 ms press of 1 starts M1 on the press itself (its
         state changes while the key is down) and the M3 hold records nothing.
         Then 2 closes it.

Fail-first: on the M3H_FAILFIRST_NOSTOP build (GET says nostop: T2 ignores
CMD_STOP), `hold` fails -- M3 runs on past the release.

At the end the script logs out of the LCD session (*, *, A, 3, 3), which ends
the menu's STANDBY hold: the windows recalibrate and T6 resumes (gh#65).

Usage:
  python bin/at_lcd_m3_hold.py [--host H] [--pin P] [CASE ...]

Exit code 0 when every verdict passed. Refuses 5C88. Stdlib only, plus
at_wp_ramp.Unit. Design: design/lcdM3HoldControl.md.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from at_wp_ramp import Unit                                     # noqa: E402

T0 = time.time()
RESULTS = []
ACT = {0: "none", 1: "FULL", 2: "MOVE_START", 3: "MOVE_STOP", 4: "MID"}
CASES = ("tap", "mid", "hold", "back", "long", "m1")


def say(*a):
    print("%7.1f s " % (time.time() - T0), *a, flush=True)


def verdict(case, what, ok, detail=""):
    RESULTS.append((case, what, ok))
    say("%s  %-5s %s%s" % ("PASS" if ok else "FAIL", case, what, ("  [%s]" % detail) if detail else ""))


class Rig:
    def __init__(self, host, pin):
        self.host = host
        self.pin = pin
        self.u = Unit(host, pin)

    # -- the unit -----------------------------------------------------------
    def status(self):
        with urllib.request.urlopen("http://%s/api/status" % self.host, timeout=6) as r:
            return json.loads(r.read().decode())

    def windows(self):
        return self.status()["windows"]

    def lcd(self):
        sc, d = self.u._req("GET", "/api/diag/key")
        if sc != 200 or not isinstance(d, dict):
            raise RuntimeError("GET /api/diag/key: HTTP %s" % sc)
        return d

    # -- the keypad ---------------------------------------------------------
    def press(self, key, ms, wait=True):
        """Hold `key` for `ms` in T7; with `wait`, until T8 takes keys again."""
        sc, r = self.u._req("POST", "/api/diag/key", {"key": key, "ms": ms})
        if not (isinstance(r, dict) and r.get("ok")):
            raise RuntimeError("POST /api/diag/key %s %s: %s" % (key, ms, r))
        if wait:
            self.settle(ms / 1000.0 + 8)

    def settle(self, limit_s=10.0):
        """The key is up, and T8 shows no message and discards no keys."""
        t_end = time.time() + limit_s
        while time.time() < t_end:
            d = self.lcd()
            if not d["busy"] and not d["msg"] and not d["suppress"] and not d["hold"]["active"]:
                time.sleep(0.15)
                return d
            time.sleep(0.1)
        raise RuntimeError("T8 did not settle in %.0f s" % limit_s)

    def tap(self, key):
        self.press(key, 150)

    # -- the menus ----------------------------------------------------------
    def to_m3(self):
        """Walk to the M3 action screen."""
        for _ in range(12):
            d = self.settle()
            if d["screen"] == "motor_action":
                if d["motor"] == 3:
                    return d
                self.tap("*")
            elif d["screen"] == "motor_pick":
                self.tap("3")
            elif d["screen"] == "pin":
                for k in self.pin:
                    self.tap(k)
                self.tap("#")
            elif d["screen"] != "status":
                self.tap("D")                       # D: back to the status pages
            elif d["page"] != 5:
                self.tap("D")                       # D: the next status page
            else:
                self.tap("#")                       # # on the windows page
        raise RuntimeError("could not reach the M3 action screen: %s" % self.lcd())

    def logout(self):
        for _ in range(8):
            d = self.settle()
            if d["session"] == 0:
                return True
            if d["screen"] in ("motor_action", "motor_pick"):
                self.tap("*")
            elif d["screen"] == "status":
                self.tap("A")                       # any key but D/#: the main menu
            elif d["screen"] == "menu_root":
                self.tap("3")                       # Access
            elif d["screen"] == "menu_access":
                self.tap("3")                       # Logout
            else:
                self.tap("D")
        return self.lcd()["session"] == 0

    # -- M3 -----------------------------------------------------------------
    def wait_m3(self, states, limit_s):
        t_end = time.time() + limit_s
        w = self.windows()
        while time.time() < t_end:
            w = self.windows()
            if w.get("M3") in states:
                return w
            time.sleep(0.3)
        return w

    def hold_and_watch(self, key, ms):
        """Press `key` for `ms`, polling M3 meanwhile. Returns (seconds from the
        press to the first MOVING_* reading or None, the last LCD view during the
        hold, the windows after it settled)."""
        moving = "MOVING_OPEN" if key == "1" else "MOVING_CLOSE"
        t_press = time.time()
        self.press(key, ms, wait=False)
        first = None
        views = []
        while time.time() - t_press < ms / 1000.0 + 0.2:
            w = self.windows()
            if first is None and w.get("M3") == moving:
                first = time.time() - t_press
            if len(views) < 40:
                views.append(self.lcd())
            time.sleep(0.12)
        self.settle(10)
        time.sleep(1.5)                             # T17's settle read after a stop
        return first, views, self.windows()


def pct(w):
    v = w.get("M3_percent_x10")
    return None if v is None else v / 10.0


def last(rig):
    return rig.lcd()["last"]


def ensure_closed(rig):
    w = rig.windows()
    if w.get("M3") != "CLOSED":
        rig.press("2", 300)
        w = rig.wait_m3(("CLOSED",), 30)
    return w.get("M3") == "CLOSED"


def case_tap(rig, travel_s):
    c = "tap"
    if not ensure_closed(rig):
        verdict(c, "precondition: M3 CLOSED", False, json.dumps(rig.windows()))
        return
    s0 = last(rig)["seq"]
    rig.press("1", 300)
    lt = last(rig)
    verdict(c, "a 300 ms press of Open is a tap (FULL, open)",
            lt["seq"] == s0 + 1 and lt["action"] == 1 and lt["opening"] and lt["result"] == 0,
            "last %s" % lt)
    w = rig.wait_m3(("OPEN",), travel_s + 8)
    verdict(c, "M3 reaches OPEN", w.get("M3") == "OPEN", "M3 %s at %s %%" % (w.get("M3"), pct(w)))
    rig.press("2", 300)
    lt = last(rig)
    verdict(c, "a 300 ms press of Close is a tap (FULL, close)",
            lt["action"] == 1 and not lt["opening"] and lt["result"] == 0, "last %s" % lt)
    w = rig.wait_m3(("CLOSED",), travel_s + 8)
    verdict(c, "M3 reaches CLOSED", w.get("M3") == "CLOSED", "M3 %s at %s %%" % (w.get("M3"), pct(w)))


def case_mid(rig):
    c = "mid"
    if not ensure_closed(rig):
        verdict(c, "precondition: M3 CLOSED", False, json.dumps(rig.windows()))
        return
    p0 = pct(rig.windows())
    s0 = last(rig)["seq"]
    rig.press("1", 1500, wait=False)
    seen = ""
    t_end = time.time() + 4.0
    while time.time() < t_end:
        d = rig.lcd()
        if d["msg"]:
            seen = "|".join(d["lcd"])
        time.sleep(0.15)
    rig.settle()
    lt = last(rig)
    verdict(c, "a 1500 ms press is MID", lt["seq"] == s0 + 1 and lt["action"] == 4,
            "last %s" % lt)
    verdict(c, "the LCD says why", seen.startswith("Tap <1s: to end"), seen)
    time.sleep(2.0)
    w = rig.windows()
    p1 = pct(w)
    verdict(c, "M3 did not move", w.get("M3") == "CLOSED" and (p0 is None or p1 is None or abs(p1 - p0) < 1.0),
            "M3 %s, %s %% -> %s %%" % (w.get("M3"), p0, p1))


def case_hold(rig, state):
    c = "hold"
    if not ensure_closed(rig):
        verdict(c, "precondition: M3 CLOSED", False, json.dumps(rig.windows()))
        return

    def one(ms, label, start_pct):
        """A hold of `ms`; verdicts on T8's timing and the stop. Returns the % after."""
        first, views, w = rig.hold_and_watch("1", ms)
        lt = last(rig)
        p = pct(w)
        verdict(c, "%s: T8 starts the move at the 2 s mark" % label,
                2000 <= lt.get("move_at_ms", 0) <= 2150,
                "move %s ms into the press; MOVING_OPEN seen %s s after it" % (
                    lt.get("move_at_ms"), None if first is None else round(first, 2)))
        verdict(c, "%s: the release is a MOVE_STOP of the press's length" % label,
                lt["action"] == 3 and lt["opening"] and abs(lt["held_ms"] - ms) <= 250
                and lt["result"] == 0, "last %s" % lt)
        verdict(c, "%s: M3 rests PART_OPEN" % label, w.get("M3") == "PART_OPEN",
                "M3 %s at %s %% (from %s %%)" % (w.get("M3"), p, start_pct))
        return p, views

    p0 = pct(rig.windows())
    pa, views = one(5000, "5 s from CLOSED", p0)
    texts = {v["lcd"][1].strip() for v in views}
    verdict(c, "the LCD shows the hold, then the move",
            "Hold 2s to move" in texts and "Release to stop" in texts, sorted(texts))
    pb, _ = one(3000, "3 s", pa)
    pc, _ = one(5000, "5 s", pb)
    if None in (pa, pb, pc):
        verdict(c, "positions read", False, "%s %s %s" % (pa, pb, pc))
        return
    d3, d5 = pb - pa, pc - pb
    verdict(c, "3 s of hold moves M3 open; 5 s moves it 2-4.5 times as far",
            d3 >= 2.0 and 2.0 <= d5 / d3 <= 4.5,
            "3 s: +%.1f points; 5 s: +%.1f points; ratio %.2f" % (d3, d5, d5 / d3 if d3 else 0.0))
    state["p"] = pc


def case_back(rig, state):
    c = "back"
    w0 = rig.windows()
    if w0.get("M3") != "PART_OPEN":
        verdict(c, "precondition: M3 PART_OPEN (run after hold)", False, json.dumps(w0))
        return
    p0 = pct(w0)
    first, views, w = rig.hold_and_watch("2", 4000)
    lt = last(rig)
    p1 = pct(w)
    verdict(c, "4 s of Close: MOVE_STOP, part-open, less open",
            lt["action"] == 3 and not lt["opening"] and w.get("M3") == "PART_OPEN"
            and p0 is not None and p1 is not None and p1 <= p0 - 5.0,
            "M3 %s at %s %% (from %s %%); first MOVING_CLOSE %s s" % (
                w.get("M3"), p1, p0, None if first is None else round(first, 2)))


def case_long(rig, travel_s):
    c = "long"
    if not ensure_closed(rig):
        verdict(c, "precondition: M3 CLOSED", False, json.dumps(rig.windows()))
        return
    ms = int((travel_s + 5 + 3) * 1000)
    t_press = time.time()
    rig.press("1", ms, wait=False)
    open_at = None
    while time.time() - t_press < ms / 1000.0 - 0.3:
        w = rig.windows()
        if open_at is None and w.get("M3") == "OPEN":
            open_at = time.time() - t_press
        time.sleep(0.3)
    verdict(c, "M3 reaches OPEN by the timer while the key is down",
            open_at is not None, "OPEN %s s into a %.0f s press" % (
                None if open_at is None else round(open_at, 1), ms / 1000.0))
    rig.settle(10)
    time.sleep(1.5)
    w = rig.windows()
    lt = last(rig)
    verdict(c, "the release changes nothing: M3 stays OPEN",
            w.get("M3") == "OPEN" and lt["action"] == 3, "M3 %s; last %s" % (w.get("M3"), lt))
    rig.press("2", 300)
    w = rig.wait_m3(("CLOSED",), travel_s + 8)
    verdict(c, "a tap of Close returns it to CLOSED", w.get("M3") == "CLOSED", "M3 %s" % w.get("M3"))


def case_m1(rig, travel_s):
    c = "m1"
    rig.tap("*")                                    # back to the motor picker
    rig.tap("1")                                    # M1
    d = rig.lcd()
    if d["screen"] != "motor_action" or d["motor"] != 1:
        verdict(c, "precondition: the M1 action screen", False, json.dumps(d))
        return
    w0 = rig.windows()
    if w0.get("M1") != "CLOSED":
        rig.press("2", 300)
        t_end = time.time() + 120
        while time.time() < t_end and rig.windows().get("M1") != "CLOSED":
            time.sleep(1)
    s0 = last(rig)["seq"]
    rig.press("1", 1500, wait=False)                # a long press: M1 must still act at once
    t_press = time.time()
    moved = None
    while time.time() - t_press < 1.4:
        if rig.windows().get("M1") in ("MOVING_OPEN", "OPEN"):
            moved = time.time() - t_press
            break
        time.sleep(0.1)
    rig.settle()
    verdict(c, "M1 starts on the press itself, as before",
            moved is not None and moved < 1.0, "M1 moving %s s after the press" % (
                None if moved is None else round(moved, 2)))
    verdict(c, "the M3 hold records nothing for M1", last(rig)["seq"] == s0, "last %s" % last(rig))
    rig.press("2", 300)
    t_end = time.time() + 120
    while time.time() < t_end and rig.windows().get("M1") != "CLOSED":
        time.sleep(1)
    verdict(c, "M1 closes again", rig.windows().get("M1") == "CLOSED", "M1 %s" % rig.windows().get("M1"))
    rig.tap("*")
    rig.tap("3")                                    # back to M3


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="192.168.20.160")
    ap.add_argument("--pin", default="12345678")
    ap.add_argument("cases", nargs="*", help="any of: %s (default: all)" % ", ".join(CASES))
    a = ap.parse_args()
    cases = a.cases or list(CASES)
    for x in cases:
        if x not in CASES:
            sys.exit("unknown case %r; choose from %s" % (x, ", ".join(CASES)))
    rig = Rig(a.host, a.pin)
    s = rig.status()
    y = s["system"]
    if "5C88" in str(y.get("unit_id", "")).upper():
        sys.exit("REFUSED: 5C88 is production")
    d = rig.lcd()
    sc, cfg = rig.u._req("GET", "/api/config")
    travel_s = int(cfg["travel_s"][2]) if isinstance(cfg, dict) and "travel_s" in cfg else 13
    say("unit %s fw %s assets %s uptime %s; M3 %s %s, gate %s; travel_m3 %s s; nostop %s" % (
        y["unit_id"], y["fw_ver"], y["asset_version"], y["uptime_s"], s["windows"].get("M3"),
        s["windows"].get("M3_ctrl_mode"), s["windows"].get("M3_pos_gate"), travel_s, d.get("nostop")))
    if d.get("nostop"):
        say("fail-first build (M3H_FAILFIRST_NOSTOP): `hold` must FAIL")
    state = {}
    try:
        rig.to_m3()
        say("on the M3 action screen")
        for name in cases:
            say("== case %s" % name)
            if name == "tap":
                case_tap(rig, travel_s)
            elif name == "mid":
                case_mid(rig)
            elif name == "hold":
                case_hold(rig, state)
            elif name == "back":
                case_back(rig, state)
            elif name == "long":
                case_long(rig, travel_s)
            elif name == "m1":
                case_m1(rig, travel_s)
            rig.to_m3()
    finally:
        try:
            out = rig.logout()
            say("LCD session logged out: %s" % out)
        except Exception as e:                              # noqa: BLE001
            say("logout failed: %s" % e)
        try:
            rig.u.logout()
        except Exception:                                   # noqa: BLE001
            pass
    bad = [r for r in RESULTS if not r[2]]
    say("== %d verdicts, %d failed" % (len(RESULTS), len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

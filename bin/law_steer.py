#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Steer T6 on a schedule so a soak sees the climate law decide: release build, no sensor.

WHY
---
bin/at_wp_strokes.py makes a soak's M3 strokes, but it needs a bench build and a
fitted, gated position sensor. A soak of production's configuration -- the
release image, `wpos_fitted_m3` 0, M3 on its timer, the law `stepped` v2 --
has neither, and on a cold day production's climate gives the law nothing to
decide: in late October the greenhouse sits below `t_min`, so T6 holds step 0
for hours and a soak judges nothing (2026-10-25: 11.5 C against t_min 14/16).

So this steers the one lever the law reads that an operator would also turn:
the temperature thresholds, through the ordinary admin `POST /api/config`.
Every decision stays T6's own, every drive an ordinary SRC_T6 command with the
dwells in force, and bin/law_conformance.py judges each one afterwards (it
follows the thresholds through the log's SETPT rows).

ONE SESSION
-----------
  1. Skip it, with the reason, when the unit is in no state to be steered
     (wind override, motor alarm, calibrating, STANDBY), or when even the
     lowest t_max of the period in force cannot make a temperature demand
     (the law steps on T5's average rounded to a whole degree).
  2. Lower both periods' thresholds: t_min to LOW_TMIN first, then t_max to
     its lowest bound (15 by day, 10 at night). T6 sees a temperature demand
     on its next cycle; with humidity above rh_max, both demands open.
  3. Log out, and watch the public status for --hold-min minutes.
  4. Restore exactly what it read, t_max first, then t_min, and read it back.
     The greenhouse is then below t_min, so the law closes everything, the
     close dwells in force deciding when.

The thresholds read at the first session are required unchanged at every later
one: each session restores to what it READ, so a restore that failed, or a
change made by hand, would otherwise become the "original". If killed during a
hold, the exit hook restores; killed harder than that, check t_max_day,
t_min_day, t_max_ngt and t_min_ngt by hand.

USAGE
-----
    python3 law_steer.py --host 192.168.20.160 --sessions 16 --every-min 90 --hold-min 40

Exit 0 = every session ran or was skipped with a reason, 1 = a write or a
restore failed (the run stops), 2 = refused (5C88, or no admin login).
"""

import argparse
import atexit
import json
import math
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from at_wp_ramp import Unit                                    # noqa: E402

DEFAULT_PIN = "12345678"
STEERED = ("t_max_day", "t_min_day", "t_max_ngt", "t_min_ngt")
LOW_TMIN = 5            # both periods' t_min while steered: the day bound's floor
BLOCKING = ("wind_override", "motor_alarm", "calibrating", "standby")
POLL_S = 30
READBACK_S = 4          # POST /api/config is asynchronous: T4 applies it a loop later

_base = [None]          # the thresholds read at the first session
_pending = [None]       # (host, pin) while a session's thresholds are lowered


def say(msg):
    print("%s  %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg), flush=True)


def public_status(host):
    with urllib.request.urlopen("http://%s/api/status" % host, timeout=15) as r:
        return json.load(r)


def lround(x):
    """C lroundf(): half away from zero, the way the average reaches the law."""
    return int(math.copysign(math.floor(abs(x) + 0.5), x))


def flags(st):
    return list((st.get("mode") or {}).get("flags") or [])


def write_all(u, values, order):
    """POST each key in `order`, then read back. Returns the keys that differ."""
    for k in order:
        sc = u._req("POST", "/api/config", {"ns": "climate", "key": k, "value": int(values[k])})[0]
        if sc != 200:
            say("  POST %s=%s answered HTTP %s" % (k, values[k], sc))
    time.sleep(READBACK_S)
    got = u._req("GET", "/api/config")[1]
    return [k for k in order if int(got.get(k, -999)) != int(values[k])]


def restore(host, pin):
    """Put the first session's thresholds back: t_max before t_min, so t_min
    never rises above a still-lowered t_max."""
    base = _base[0]
    if base is None:
        return True
    u = Unit(host, pin)
    try:
        bad = write_all(u, base, ("t_max_day", "t_max_ngt", "t_min_day", "t_min_ngt"))
    finally:
        u.logout()
    if bad:
        say("RESTORE FAILED for %s -- check them by hand (want %s)" % (bad, base))
        return False
    say("restored %s" % base)
    return True


def _exit_restore():
    if _pending[0] is not None:
        host, pin = _pending[0]
        _pending[0] = None
        say("exit during a hold: restoring")
        restore(host, pin)


atexit.register(_exit_restore)


def describe(st):
    c = st.get("climate") or {}
    w = st.get("windows") or {}
    return ("T %s (avg %s) RH %s (avg %s) | t_max in force %s | M1 %s M2 %s M3 %s | %s %s"
            % (c.get("temp_c"), c.get("temp_avg_c"), c.get("rh_pct"), c.get("rh_avg_pct"),
               c.get("temp_max_active"), w.get("M1"), w.get("M2"), w.get("M3"),
               (st.get("mode") or {}).get("current"), ",".join(flags(st)) or "-"))


def session(host, pin, n, hold_min, limits):
    """True = ran, None = skipped, False = a write or restore failed."""
    st = public_status(host)
    sysb = st.get("system") or {}
    if "5C88" in str(sysb.get("unit_id", "")).upper():
        say("REFUSED: this is the production unit")
        sys.exit(2)
    for bad in BLOCKING:
        if bad in flags(st):
            say("session %d SKIPPED: the unit shows '%s'" % (n, bad))
            return None

    u = Unit(host, pin)
    try:
        cfg = u._req("GET", "/api/config")[1]
        seen = dict((k, int(cfg[k])) for k in STEERED)
        if _base[0] is None:
            _base[0] = seen
            say("thresholds at the start: %s" % seen)
        elif seen != _base[0]:
            say("session %d: the thresholds changed since the run started (a failed restore, "
                "or a change by hand) -- stopping, nothing touched.\n      start: %s\n      now:   %s"
                % (n, _base[0], seen))
            return False

        c = st.get("climate") or {}
        in_force = c.get("temp_max_active")
        day = (in_force == cfg["t_max_day"])
        key = "t_max_day" if day else "t_max_ngt"
        floor = int(limits[key][0])
        t = c.get("temp_avg_c")
        if t is None or lround(float(t)) <= floor:
            say("session %d SKIPPED: no heat demand possible -- the average %s C rounds to %s, "
                "not above the lowest %s %d" % (n, t, None if t is None else lround(float(t)), key, floor))
            return None

        low = {"t_min_day": max(LOW_TMIN, int(limits["t_min_day"][0])),
               "t_min_ngt": max(LOW_TMIN, int(limits["t_min_ngt"][0])),
               "t_max_day": int(limits["t_max_day"][0]),
               "t_max_ngt": int(limits["t_max_ngt"][0])}
        say("session %d (%s): %s" % (n, "day" if day else "night", describe(st)))
        _pending[0] = (host, pin)
        bad = write_all(u, low, ("t_min_day", "t_min_ngt", "t_max_day", "t_max_ngt"))
        if bad:
            say("session %d: the lowered thresholds did not take for %s" % (n, bad))
            u.logout()
            restore(host, pin)
            _pending[0] = None
            return False
        say("  lowered to %s; holding %d min" % (low, hold_min))
    finally:
        u.logout()

    last = None
    t_end = time.time() + hold_min * 60.0
    while time.time() < t_end:
        time.sleep(POLL_S)
        try:
            d = describe(public_status(host))
        except Exception as e:                                  # noqa: BLE001
            d = "status unreadable: %s" % e
        if d != last:
            say("  " + d)
            last = d
    ok = restore(host, pin)
    _pending[0] = None
    return True if ok else False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--sessions", type=int, default=8)
    ap.add_argument("--every-min", type=float, default=90.0)
    ap.add_argument("--hold-min", type=float, default=40.0)
    ap.add_argument("--start-delay-min", type=float, default=0.0)
    a = ap.parse_args()
    if a.hold_min >= a.every_min:
        sys.exit("--hold-min must be shorter than --every-min")

    u = Unit(a.host, a.pin)
    try:
        lim = u._req("GET", "/api/config/limits")[1]
    finally:
        u.logout()
    limits = dict((k, lim[k]) for k in STEERED)
    say("bounds %s" % limits)

    t0 = time.time() + a.start_delay_min * 60.0
    ran = skipped = 0
    for i in range(a.sessions):
        due = t0 + i * a.every_min * 60.0
        if due > time.time():
            time.sleep(due - time.time())
        r = session(a.host, a.pin, i + 1, a.hold_min, limits)
        if r is False:
            say("STOP after session %d: ran %d, skipped %d" % (i + 1, ran, skipped))
            return 1
        if r:
            ran += 1
        else:
            skipped += 1
    say("done: %d sessions ran, %d skipped" % (ran, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())

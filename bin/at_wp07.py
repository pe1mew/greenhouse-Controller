#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AT-WP07 -- the wind override still closes M3 with the position sensor gone.

The requirement under test is **FR-WP18: never gate anything safety-related on
position**. Wind override, motor-alarm handling and boot CLOSE_ALL stay
time-based, so losing the encoder must not weaken any of them. This test proves
the wind path specifically, because that is the one that closes the greenhouse.

WHY THIS IS THE TEST THAT MATTERS MOST OF THE THREE
---------------------------------------------------
AT-WP06 and AT-WP09 fail *visibly* -- a fault that is not detected shows up as a
missing row. AT-WP07 fails **silently and in the dangerous direction**: a wind
override that quietly stopped working would look exactly like a calm week, and
the first evidence would be storm damage. Wind is a safety input and position
explicitly is not (FR-WP18); this test is what keeps those two facts separate in
the code rather than only in the requirement.

THE VACUOUS PASS IS THE WHOLE DIFFICULTY
-----------------------------------------
"M3 ended up closed" proves nothing if it was already closed, or if the sensor
was present, or if no override was ever raised. Each of those makes the test
pass while testing nothing, so this script REFUSES rather than reporting a pass:

  * M3 must be OPEN (or opening) when the override is raised.
  * The presence gate must be SHUT -- sensor disconnected. With the sensor
    present this measures the normal path, not the requirement.
  * The override must actually be observed to rise (EG1 bit 0), not assumed.

That is the gotcha log's standing rule applied to a safety test: before reading
a pass, name the input that would make it fail and confirm the check sees it.

TRIGGERING THE OVERRIDE
-----------------------
Two ways, and the script picks whichever the rig allows:

  --force-vmax   Lower `v_max` under the measured wind so T3 raises the
                 override, then restore it. Needs measured wind >= 1 m/s,
                 because CFG_MIN_V_MAX is 1 (0 would assert the override
                 permanently, which is why the bound exists).
  default        You raise the wind at the emulator, or disconnect the wind
                 sensor to exercise the safe-fail path, and this observes.

**This test closes windows.** It refuses to run on the production unit, and it
restores `v_max` in a `finally` -- a script that died holding a lowered `v_max`
would leave the unit permanently overridden.

USAGE
-----
    python bin/at_wp07.py --host 192.168.20.169
    python bin/at_wp07.py --host 192.168.20.169 --force-vmax

Exit 0 = the criterion held, 1 = it did not, 2 = could not run the test.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import http.client
import json
import sys
import time

DEFAULT_HOST = "192.168.20.169"
DEFAULT_PIN = "12345678"
CFG_MIN_V_MAX = 1          # cfg_limits.h -- 0 would assert the override forever
EG1_WIND_OVERRIDE = 1 << 0
EG1_SENSOR_FAULT_W = 1 << 3
EG1_STANDBY        = 1 << 7   # T6 suspended; a setpoint change moves nothing
POLL_S = 1.0
SETTLE_S = 20


class Unit(object):
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

    def status(self):
        """The whole status document, with the identity block proven present.

        /api/status nests identity under "system"; reading it flat yields None,
        which is exactly how a production guard silently fails OPEN. A safety
        check that cannot see what it is guarding must fail closed."""
        j = self._req("GET", "/api/status")[1]
        if not isinstance(j, dict) or "system" not in j:
            sys.exit("GET /api/status has no `system` block -- refusing to run "
                     "without knowing which unit this is")
        return j

    def post_cfg(self, ns, key, value):
        return self._req("POST", "/api/config", {"ns": ns, "key": key, "value": value})[0]

    def get_cfg(self):
        return self._req("GET", "/api/config")[1]

    def settle_cfg(self, field, want, secs=SETTLE_S):
        """POST /api/config is ASYNCHRONOUS (Q4 -> T4 a loop later)."""
        end = time.time() + secs
        while time.time() < end:
            c = self.get_cfg()
            if isinstance(c, dict) and c.get(field) == want:
                return True
            time.sleep(0.5)
        return False


def m3_of(st):
    return (st.get("windows") or {}).get("M3", "?")


def eg1_of(st):
    return int((st.get("system") or {}).get("eg1", 0) or 0)


def wind_of(st):
    w = st.get("wind") or {}
    return float(w.get("speed_ms", 0) or 0), float(w.get("speed_avg_ms", 0) or 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--force-vmax", action="store_true",
                    help="lower v_max under the measured wind to raise the override")
    ap.add_argument("--hold-open", action="store_true",
                    help="lower the active T maximum so T6 opens M3 and HOLDS it open "
                         "for the duration; restored in the finally")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    u = Unit(args.host, args.pin)
    st = u.status()
    sysb = st.get("system") or {}
    uid = str(sysb.get("unit_id", "?"))

    print("AT-WP07 -- wind override with the position sensor absent")
    print("  unit %s  fw %s" % (uid, sysb.get("fw_ver")))

    # ---- this test CLOSES WINDOWS -----------------------------------------
    if "5C88" in uid.upper():
        sys.exit("REFUSING: %s is the production unit. This test forces a wind "
                 "override, which closes the greenhouse." % uid)

    # ---- optionally open M3 first, and KEEP it open ------------------------
    # The test needs M3 open when the override rises, and it needs T6 to be
    # actively WANTING it open -- otherwise "M3 closed" is ambiguous between
    # "the override closed it" and "T6 closed it anyway because demand fell".
    # Holding the demand up for the whole test makes the attribution clean:
    # the vent algorithm wants it open and the override takes it closed.
    restore_setpoint = None
    # Whichever maximum is ACTIVE right now -- lowering the other one does
    # nothing, and the test would then sit waiting for a stroke that cannot come.
    sp_field = "t_max_day" if (st.get("sun") or {}).get("is_daytime", True) else "t_max_ngt"

    # STANDBY suspends T6, so lowering a setpoint cannot open anything. Writing
    # it anyway is worse than useless: the run then PRINTS "holding M3 open"
    # while nothing holds it, and a later reader credits the hold for a window
    # that someone opened by hand. Found on the 2026-09-15 run, where an LCD
    # manual move had left the unit in STANDBY (bit 7) for the whole test.
    if args.hold_open and (eg1_of(st) & EG1_STANDBY):
        print("\n  NOT holding M3 open: the unit is in STANDBY, so T6 is suspended")
        print("  and a setpoint change cannot move anything. Open M3 by hand (LCD),")
        print("  which is dwell-free anyway, and note that T6 is NOT competing for")
        print("  the window during this test.")
        args.hold_open = False

    if args.hold_open:
        c = u.get_cfg()
        cur_sp = c.get(sp_field) if isinstance(c, dict) else None
        if cur_sp is None:
            print("\nCannot read %s from /api/config." % sp_field)
            return 2
        tavg = float((st.get("climate") or {}).get("temp_avg_c", 0) or 0)
        tgt = int(max(1, int(tavg) - 5))
        print("\n  holding M3 open: %s %s -> %d (temp_avg %.1f)"
              % (sp_field, cur_sp, tgt, tavg))
        if u.post_cfg("climate", sp_field, tgt) != 200:
            print("  POST /api/config rejected the write")
            return 2
        restore_setpoint = cur_sp
        u.settle_cfg(sp_field, tgt)

    try:
        return _run(u, args, st, sp_field)
    finally:
        if restore_setpoint is not None:
            print("\n  restoring %s -> %s ..." % (sp_field, restore_setpoint))
            ok = (u.post_cfg("climate", sp_field, restore_setpoint) == 200 and
                  u.settle_cfg(sp_field, restore_setpoint))
            print("  restored and confirmed." if ok else
                  "  *** RESTORE FAILED -- set %s back to %s BY HAND NOW ***"
                  % (sp_field, restore_setpoint))


def _run(u, args, st, sp_field):
    # Wait for M3 to actually open before judging preconditions, otherwise the
    # hold above is raced by the precondition check it exists to satisfy.
    if args.hold_open:
        print("  waiting for M3 to open ...")
        end = time.time() + 240
        while time.time() < end:
            st = u.status()
            if m3_of(st) in ("OPEN", "MOVING_OPEN"):
                break
            time.sleep(2.0)
        print("  M3 now %s" % m3_of(st))

    # ---- preconditions, each of which would otherwise pass vacuously ------
    m3 = m3_of(st)
    spd, avg = wind_of(st)
    eg1 = eg1_of(st)
    gate = "?"
    sc, dg = u._req("GET", "/api/diag/windowpos")
    if sc == 200 and isinstance(dg, dict):
        gate = str((dg.get("gate") or {}).get("mode_str", "?"))

    print("  M3 now     : %s" % m3)
    print("  wind       : %.1f m/s (avg %.1f)" % (spd, avg))
    print("  eg1        : 0x%02X" % eg1)
    print("  gate mode  : %s" % gate)

    problems = []
    if m3 not in ("OPEN", "MOVING_OPEN"):
        problems.append(
            "M3 is %s, not OPEN. A window that is already closed cannot\n"
            "     demonstrate that the override closed it -- this is the vacuous\n"
            "     pass this test exists to avoid. Open M3 first (LCD manual move,\n"
            "     or let T6 open it), then re-run." % m3)
    if gate.lower().startswith("position"):
        problems.append(
            "the presence gate is POSITION, so the sensor is present and\n"
            "     trusted. AT-WP07 is specifically about the override working\n"
            "     WITHOUT it (FR-WP18). Disconnect the encoder at addr 40 and\n"
            "     wait for the gate to demote, then re-run.")
    if eg1 & EG1_WIND_OVERRIDE:
        problems.append(
            "the wind override is ALREADY up (eg1 0x%02X). Let it clear first,\n"
            "     or this measures nothing." % eg1)
    if problems:
        print("\nREFUSING to report a result:")
        for p in problems:
            print("  *  %s" % p)
        return 2

    # ---- raise the override ------------------------------------------------
    restore_vmax = None
    try:
        if args.force_vmax:
            cfg = u.get_cfg()
            cur = cfg.get("v_max") if isinstance(cfg, dict) else None
            if cur is None:
                print("\nCannot read v_max from /api/config -- use the emulator instead.")
                return 2
            target = int(max(CFG_MIN_V_MAX, 1))
            if avg < target:
                print("\nCannot force the override: measured wind is %.1f m/s and the\n"
                      "lowest legal v_max is %d m/s (0 would assert the override\n"
                      "permanently, which is why CFG_MIN_V_MAX exists). Raise the\n"
                      "wind at the emulator and run without --force-vmax."
                      % (avg, CFG_MIN_V_MAX))
                return 2
            print("\n  lowering v_max %s -> %d to raise the override ..." % (cur, target))
            if u.post_cfg("wind", "v_max", target) != 200:
                print("  POST /api/config rejected the v_max write")
                return 2
            restore_vmax = cur
            u.settle_cfg("v_max", target)
        else:
            print("\n  >>> Raise the wind above v_max at the emulator now")
            print("  >>> (or disconnect the wind sensor to exercise the safe-fail path).")

        print("  waiting up to %d s for the override and for M3 to close ...\n" % args.timeout)

        t0 = time.time()
        saw_override = False
        saw_moving = False
        closed_at = None
        while time.time() - t0 < args.timeout:
            time.sleep(POLL_S)
            s = u.status()
            e = eg1_of(s)
            m = m3_of(s)
            if (e & EG1_WIND_OVERRIDE) or (e & EG1_SENSOR_FAULT_W):
                if not saw_override:
                    which = "wind override" if (e & EG1_WIND_OVERRIDE) else "wind sensor-fault safe-fail"
                    print("  %s raised at t+%.1f s (eg1 0x%02X)"
                          % (which, time.time() - t0, e))
                saw_override = True
            if m == "MOVING_CLOSE":
                saw_moving = True
            if saw_override and m == "CLOSED":
                closed_at = time.time() - t0
                print("  M3 CLOSED at t+%.1f s" % closed_at)
                break

        # ---- verdict -------------------------------------------------------
        if not saw_override:
            print("\nINCONCLUSIVE: no override was ever raised, so nothing was tested.")
            print("An override that never rose is not a null result.")
            return 2
        if closed_at is None:
            print("\nFAIL: the override was raised and M3 did NOT reach CLOSED.")
            print("This is the FR-WP18 violation the test exists to catch -- a")
            print("safety path that stopped working when the position sensor went")
            print("away. M3 last read %s, moving-close seen: %s."
                  % (m3_of(u.status()), saw_moving))
            return 1

        print("\nPASS: with the position sensor absent (gate %s), the override" % gate)
        print("raised and M3 closed in %.1f s. FR-WP18 holds on the wind path." % closed_at)
        return 0

    finally:
        if restore_vmax is not None:
            print("\n  restoring v_max -> %s ..." % restore_vmax)
            ok = (u.post_cfg("wind", "v_max", restore_vmax) == 200 and
                  u.settle_cfg("v_max", restore_vmax))
            if ok:
                print("  v_max restored and confirmed.")
            else:
                print("  *** RESTORE FAILED -- the unit is holding a lowered v_max ***")
                print("  *** and will keep the wind override asserted.            ***")
                print("  *** Set it back by hand NOW:                             ***")
                print("  ***   POST /api/config {\"ns\":\"wind\",\"key\":\"v_max\","
                      "\"value\":%s}   ***" % restore_vmax)


if __name__ == "__main__":
    sys.exit(main())

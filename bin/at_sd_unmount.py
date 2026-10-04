#!/usr/bin/env python3
"""at_sd_unmount.py -- gh#90 acceptance: an SD unmount while FAT is busy must not panic the unit.

gh#90: POST /api/sd/unmount ran in the HTTP task and unmounted at once. A T9
write already inside FAT held FAT's per-volume lock, the unmount closed that
lock, and newlib's _lock_close() asserted: a panic and a reboot (2026-10-04,
decoded from the coredump). 2.16.2 runs every SD operation, the unmount
included, under one lock in drivers/sdCard: an unmount waits for the write in
flight, and the next write finds the card gone.

Stages, run in the order given; each prints PASS/FAIL per verdict:

  race    bench builds (GET/POST /api/diag/sd). ROUNDS rounds, each:
          1. start the bench writer: 16 KB appends through the driver, back to
             back, so FAT is busy nearly all the time (precondition: it writes);
          2. POST /api/sd/unmount while it runs;
          3. the unit must NOT reboot (uptime keeps rising for 8 s) and must not
             store a new coredump, and the unmount must answer ok;
          4. the writer must have stopped on the unmount (last_rc no_card);
          5. POST /api/sd/mount must answer ok, and the card read mounted.
          Fail-first: the SD_FAILFIRST_NOLOCK build (GET says nolock) panics in
          round 1, the way 2.16.1 did.

  idle    any build: an unmount and a mount on a settled unit, nothing else.

Usage:
  python bin/at_sd_unmount.py [--host H] [--rounds N] STAGE [STAGE ...]

Exit code 0 when every verdict passed. Refuses 5C88. Stdlib only, plus
at_wp_ramp.Unit.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from at_wp_ramp import Unit                                     # noqa: E402

PIN = "12345678"
T0 = time.time()
RESULTS = []


def say(*a):
    print("%7.1f s " % (time.time() - T0), *a, flush=True)


def verdict(stage, what, ok, detail=""):
    RESULTS.append((stage, what, ok))
    say("%s  %-6s %s%s" % ("PASS" if ok else "FAIL", stage, what, ("  [%s]" % detail) if detail else ""))


class Rig:
    def __init__(self, host):
        self.host = host
        self.unit = None

    def public(self):
        with urllib.request.urlopen("http://%s/api/status" % self.host, timeout=4) as r:
            return json.loads(r.read().decode())["system"]

    def login(self):
        self.unit = Unit(self.host, PIN)

    def logout(self):
        try:
            if self.unit:
                self.unit.logout()
        except Exception:                                       # noqa: BLE001
            pass
        self.unit = None

    def req(self, method, path, body=None):
        return self.unit._req(method, path, body)

    def settle(self, need_s=40):
        """Let the boot-loop guard's 30 s pass before anything that may reboot."""
        while True:
            try:
                if self.public()["uptime_s"] >= need_s:
                    return
            except Exception:                                   # noqa: BLE001
                pass
            time.sleep(3)

    def watch_no_reboot(self, up_before, seconds=8):
        """True if uptime stays above `up_before`, taken BEFORE the unmount, for
        `seconds`; else the reason. A panic can come during the unmount's own
        request (2026-10-04: the request timed out and the unit was back up 10 s
        later), so comparing only within this window would miss it."""
        last = None
        t_end = time.time() + seconds
        while time.time() < t_end:
            try:
                up = self.public()["uptime_s"]
            except Exception as e:                              # noqa: BLE001
                return False, "status unreachable (%s): rebooting" % type(e).__name__
            if up < up_before or (last is not None and up < last):
                return False, "uptime %s s after %s s before the unmount: rebooted" % (up, up_before)
            last = up
            time.sleep(1)
        return True, "uptime rose to %s s" % last


def coredump_state(rig):
    """(present, size): a panic stores a new dump, so the size tells a new one
    from one that was already there."""
    sc, c = rig.req("GET", "/api/coredump/status")
    c = c if isinstance(c, dict) else {}
    return bool(c.get("present")), c.get("size_bytes")


def stress_state(rig):
    sc, d = rig.req("GET", "/api/diag/sd")
    return (d.get("stress", {}) if isinstance(d, dict) else {}), sc


def stage_race(rig, rounds):
    s = "race"
    sc, d = rig.req("GET", "/api/diag/sd")
    if sc != 200 or not isinstance(d, dict):
        verdict(s, "precondition: /api/diag/sd answers (bench build)", False, "HTTP %s" % sc)
        return
    if d.get("nolock"):
        say("fail-first build (nolock): this stage must FAIL")
    rig.settle()
    cd0 = coredump_state(rig)
    for n in range(1, rounds + 1):
        if not rig.public().get("sd_mounted"):
            rig.req("POST", "/api/sd/mount", {})
            time.sleep(2)
        sc, r = rig.req("POST", "/api/diag/sd", {"stress_s": 30, "kb": 16})
        if not (isinstance(r, dict) and r.get("ok")):
            verdict(s, "round %d precondition: the writer starts" % n, False, json.dumps(r))
            return
        t_end = time.time() + 5
        st = {}
        while time.time() < t_end:
            st, _ = stress_state(rig)
            if st.get("writes", 0) >= 3:
                break
            time.sleep(0.3)
        if st.get("writes", 0) < 3:
            verdict(s, "round %d precondition: the writer writes" % n, False, json.dumps(st))
            return
        up_before = rig.public()["uptime_s"]
        t_u = time.time()
        try:
            sc, u = rig.req("POST", "/api/sd/unmount", {})
        except Exception as e:                                  # noqa: BLE001
            sc, u = None, {"error": type(e).__name__}
        dt = time.time() - t_u
        ok_nr, why = rig.watch_no_reboot(up_before, 8)
        if not ok_nr:
            # One panic proves the race; more rounds would only feed the
            # boot-loop guard (four boots inside 30 s windows roll the image back).
            verdict(s, "round %d: unmount during a write, no reboot" % n, False,
                    "%s; unmount %s after %.1f s; writer had %s writes" % (why, u, dt, st.get("writes")))
            rig.unit = None
            rig.settle(20)
            rig.login()
            cd1 = coredump_state(rig)
            verdict(s, "round %d: no new coredump" % n, cd1 == cd0,
                    "coredump before %s, after %s" % (cd0, cd1))
            say("stopping at the first reboot")
            return
        verdict(s, "round %d: unmount during a write, no reboot" % n,
                bool(isinstance(u, dict) and u.get("ok")),
                "unmount %s in %.2f s; %s; writer had %s writes" % (u, dt, why, st.get("writes")))
        st, _ = stress_state(rig)
        verdict(s, "round %d: the writer stopped on the unmount" % n,
                st.get("running") is False and st.get("last_rc") == "no_card", json.dumps(st))
        sc, m = rig.req("POST", "/api/sd/mount", {})
        time.sleep(2)
        verdict(s, "round %d: the card mounts again" % n,
                bool(isinstance(m, dict) and m.get("ok")) and rig.public().get("sd_mounted") is True,
                "mount %s" % m)
        time.sleep(2)
    cd1 = coredump_state(rig)
    verdict(s, "no new coredump across %d rounds" % rounds, cd1 == cd0, "before %s, after %s" % (cd0, cd1))


def stage_idle(rig):
    s = "idle"
    rig.settle(60)
    sc, u = rig.req("POST", "/api/sd/unmount", {})
    time.sleep(2)
    unmounted = rig.public().get("sd_mounted") is False
    sc, m = rig.req("POST", "/api/sd/mount", {})
    time.sleep(2)
    verdict(s, "unmount and mount on a settled unit",
            bool(u.get("ok")) and unmounted and bool(m.get("ok")) and rig.public().get("sd_mounted") is True,
            "unmount %s, mount %s" % (u, m))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="192.168.20.160")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("stages", nargs="+", choices=("race", "idle"))
    a = ap.parse_args()
    rig = Rig(a.host)
    y = rig.public()
    if "5C88" in str(y.get("unit_id", "")).upper():
        sys.exit("REFUSED: 5C88 is production")
    say("unit %s fw %s assets %s uptime %s sd_mounted %s" % (y["unit_id"], y["fw_ver"], y["asset_version"],
                                                            y["uptime_s"], y.get("sd_mounted")))
    rig.login()
    try:
        for name in a.stages:
            say("== stage %s" % name)
            if name == "race":
                stage_race(rig, a.rounds)
            else:
                stage_idle(rig)
    finally:
        rig.logout()
    bad = [r for r in RESULTS if not r[2]]
    say("== %d verdicts, %d failed" % (len(RESULTS), len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

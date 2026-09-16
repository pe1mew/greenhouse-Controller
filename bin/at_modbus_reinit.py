#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""gh#69 acceptance: a Modbus re-init must never crash a bus caller.

WHY THIS EXISTS
---------------
`modbus_init()` deletes and reinstalls the UART driver. T5 calls it a second
time at task entry, and on `ropeSensor` T17 is a second bus caller: on
2026-09-16 FDA4 panicked twice in a row (LoadProhibited inside
`uart_get_buffered_data_len`) because that re-init landed under a T17 read.
The fix makes a re-init take the bus mutex.

The boot race is timing-dependent -- two of three boots crashed -- so a few
clean boots prove little. This test does not wait for the race: it asks the
bench build to call `modbus_init()` over and over while a companion task keeps
a transaction in flight nearly all the time (`POST /api/diag/modbus`,
`{"action":"reinit"}`).

WHAT IT CHECKS
--------------
  * the unit did not reboot: uptime kept counting and no coredump appeared;
  * every requested re-init ran and none was skipped (`reinit_skipped` +0);
  * the companion's reads succeeded while the re-inits went on, and no
    transaction with the encoder (addr 40) failed -- the emulated slaves at
    addr 1 and 44 are reported but not judged, because they time out at a
    known rate on the dev rig whatever this test does (gh#68);
  * free heap came back after hundreds of driver reinstalls -- taken a few
    seconds after the run, as the best of several samples, because the run's
    own two task stacks are only freed later by the idle task.

A run takes ~70 ms per re-init with companion reads on, whatever
`--interval-ms` says: each re-init waits for the read in flight and then
restarts the inter-frame timer. The test waits on progress, not on a clock.

FAIL-FIRST -- DO THIS BEFORE TRUSTING A PASS
--------------------------------------------
Build once with `MODBUS_FAILFIRST_UNLOCKED_REINIT` defined (one line near the
top of `drivers/modBus/src/modbus_rtu.cpp`). That build reports
`reinit_locked: false`, and this test must report CRASHED against it. Then put
the line back, flash the real build (`reinit_locked: true`) and expect a pass.
The test prints which variant it ran against, so a pass can never be read off
the wrong build.

**Before a fail-first run, leave every window CLOSED and the unit in STANDBY.**
The crash reboots the board, and a reboot with any window not recorded as
closed starts T2's boot recalibration -- which re-opens the original boot race
on the unlocked build and can make it panic again, boot after boot.

USAGE
-----
    python bin/at_modbus_reinit.py --host 192.168.20.169
    python bin/at_modbus_reinit.py --host 192.168.20.169 --count 1000 --interval-ms 10

Exit 0 = survived and clean, 1 = crashed or not clean, 2 = could not run.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import sys
import time

# The script's own directory is on sys.path, so the shared client imports as-is.
from at_wp_ramp import Unit, DEFAULT_HOST, DEFAULT_PIN

POLL_S = 0.5
STALL_S = 30               # no re-init for this long = stuck
COME_BACK_S = 120          # how long to wait for a rebooted unit
SETTLE_S = 3               # let the idle task free the run's two task stacks
HEAP_SAMPLES = 5
HEAP_TOLERANCE = 4096      # bytes; other tasks allocate too
ENCODER_ADDR = 40


def safe(fn, *a):
    """Call fn, returning None instead of raising when the unit is unreachable."""
    try:
        return fn(*a)
    except Exception:                                          # noqa: BLE001
        return None


def status(u):
    sc, j = u._req("GET", "/api/status")
    if sc != 200 or not isinstance(j, dict):
        raise RuntimeError("status HTTP %s" % sc)
    return j


def diag(u):
    """The whole windowpos diagnostic: `modbus` totals plus per-slave `s<addr>` rows."""
    sc, d = u._req("GET", "/api/diag/windowpos")
    if sc != 200 or not isinstance(d, dict):
        raise RuntimeError("diag HTTP %s" % sc)
    return d


def coredump(u):
    sc, j = u._req("GET", "/api/coredump/status")
    if sc != 200 or not isinstance(j, dict):
        raise RuntimeError("coredump HTTP %s" % sc)
    return j


def reinit_status(u):
    sc, j = u._req("POST", "/api/diag/modbus", {"action": "reinit_status"})
    if sc != 200 or not isinstance(j, dict) or not j.get("ok"):
        raise RuntimeError("reinit_status HTTP %s %s" % (sc, j))
    return j


def wait_back(u, limit_s):
    """Poll until the unit answers again; return its status or None."""
    end = time.time() + limit_s
    while time.time() < end:
        u.cookie = None                       # sessions do not survive a reboot
        j = safe(status, u)
        if j is not None:
            return j
        time.sleep(2.0)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--count", type=int, default=500, help="re-inits to run (1..5000)")
    ap.add_argument("--interval-ms", type=int, default=20, help="gap between re-inits (5..1000)")
    ap.add_argument("--no-hammer", action="store_true",
                    help="no companion reads: rely on T5/T17 traffic alone")
    ap.add_argument("--allow-coredump", action="store_true",
                    help="run although a coredump is already stored")
    a = ap.parse_args()

    u = Unit(a.host, a.pin)
    try:
        st0 = status(u)
        t_up0 = time.time()                   # when uptime `up0` was true
        sysb = st0.get("system") or {}
        dg0 = diag(u)
        mb0 = dg0.get("modbus") or {}
        cd0 = coredump(u)
        locked = mb0.get("reinit_locked")
        print("unit %s  fw %s  uptime %s s  eg1 0x%02x"
              % (sysb.get("unit_id"), sysb.get("fw_ver"), sysb.get("uptime_s"),
                 int(sysb.get("eg1") or 0)))
        if locked is None:
            print("this build has no re-init counters -- it predates the gh#69 test")
            return 2
        print("re-init under the bus lock: %s%s" % (
            "YES (real build)" if locked else "NO",
            "" if locked else "  <-- FAIL-FIRST BUILD: this run is expected to CRASH"))
        if cd0.get("present") and not a.allow_coredump:
            print("a coredump is already stored -- erase it first (POST /api/coredump/erase)"
                  " so a new one can only mean THIS run, or pass --allow-coredump")
            return 2
        win = st0.get("windows") or {}
        print("windows M1 %s M2 %s M3 %s   mode %s"
              % (win.get("M1"), win.get("M2"), win.get("M3"), st0.get("mode")))

        t0 = time.time()
        up0 = int(sysb.get("uptime_s") or 0)
        started = safe(u._req, "POST", "/api/diag/modbus",
                       {"action": "reinit", "count": a.count,
                        "interval_ms": a.interval_ms, "hammer": 0 if a.no_hammer else 1})
        last, lost = None, False
        if started is None:
            # On the fail-first build the board can go down before the start
            # request is even answered (seen 2026-09-16: within ~3 s).
            lost = True
            print("  t+%5.1fs  the unit stopped answering before confirming the start"
                  % (time.time() - t0))
        else:
            sc, r = started
            if sc != 200 or not (isinstance(r, dict) and r.get("ok")):
                print("the unit refused the run: HTTP %s %s" % (sc, r))
                return 2
            print("started: %d re-inits every %d ms, companion reads %s"
                  % (a.count, a.interval_ms, "off" if a.no_hammer else "on"))

        # Wait on PROGRESS, not on a clock derived from the interval: a re-init
        # waits for any read in flight and then restarts the inter-frame timer,
        # so with companion reads each one takes ~70 ms whatever the interval
        # (measured 2026-09-16). A run that stops advancing is stuck.
        stalled, t_prog = False, time.time()
        while not lost:
            s = safe(reinit_status, u)
            if s is None:
                lost = True
                print("  t+%5.1fs  the unit stopped answering" % (time.time() - t0))
                break
            if s.get("done") != last:
                last, t_prog = s.get("done"), time.time()
                print("  t+%5.1fs  %4s/%s re-inits   companion ok %s fail %s (busy %s)"
                      % (time.time() - t0, s.get("done"), s.get("requested"),
                         s.get("hammer_ok"), s.get("hammer_fail"), s.get("hammer_busy")))
            if not s.get("running"):
                break
            if time.time() - t_prog > STALL_S:
                stalled = True
                print("  t+%5.1fs  no progress for %d s" % (time.time() - t0, STALL_S))
                break
            time.sleep(POLL_S)

        if lost:
            back = wait_back(u, COME_BACK_S)
            if back is None:
                print("\nRESULT: CRASHED -- and the unit did not come back within %d s" % COME_BACK_S)
                return 1
            up1 = int((back.get("system") or {}).get("uptime_s") or 0)
            rebooted = up1 < up0 + (time.time() - t_up0) - 5
            cd1 = safe(coredump, u) or {}
            print("\nback after %.0f s: uptime %s s (was %s s before the run) -> %s"
                  % (time.time() - t0, up1, up0, "REBOOTED" if rebooted else "no reboot"))
            print("coredump stored now: %s (%s bytes)" % (cd1.get("present"), cd1.get("size_bytes")))
            if rebooted:
                print("the reset reason is the SD log's 'Boot: esp_reset_reason' row -- an"
                      " interrupt-watchdog lockup (5) stores no coredump, a panic (4) does")
                print("\nRESULT: CRASHED%s" % (
                    " -- as the fail-first build must" if locked is False
                    else " -- ON THE REAL BUILD: the fix does not hold"))
                return 1
            print("\nRESULT: NOT CLEAN -- the unit stopped answering without rebooting")
            return 1

        # Settle, then take the best of a few heap samples. The run's two tasks
        # are freed by the idle task some time after they end, and other tasks
        # (TLS, the web server answering these very polls) come and go, so one
        # sample can read low for reasons that have nothing to do with a leak.
        heap_a = 0
        if not stalled:
            time.sleep(SETTLE_S)
            for _ in range(HEAP_SAMPLES):
                hs = safe(reinit_status, u)
                if hs is not None:
                    heap_a = max(heap_a, int(hs.get("heap_now") or 0))
                time.sleep(1.0)
        s = reinit_status(u)
        st1 = status(u)
        up1 = int((st1.get("system") or {}).get("uptime_s") or 0)
        dg1 = safe(diag, u) or {}
        mb1 = dg1.get("modbus") or {}
        cd1 = coredump(u)
        d_reinit = int(mb1.get("reinit") or 0) - int(mb0.get("reinit") or 0)
        d_skip = int(mb1.get("reinit_skipped") or 0) - int(mb0.get("reinit_skipped") or 0)
        heap_b = int(s.get("heap_before") or 0)
        d_to = int(mb1.get("timeout") or 0) - int(mb0.get("timeout") or 0)
        # The encoder is the slave to judge: the emulated ones at addr 1 and 44
        # time out at a known rate on the dev rig (gh#68) whatever this test does.
        enc0 = (dg0.get("s%d" % ENCODER_ADDR) or {})
        enc1 = (dg1.get("s%d" % ENCODER_ADDR) or {})
        d_enc_fail = sum(int(enc1.get(k) or 0) - int(enc0.get(k) or 0)
                         for k in ("to", "crc", "ex", "fr"))
        eg1_0 = int(sysb.get("eg1") or 0)
        eg1_1 = int((st1.get("system") or {}).get("eg1") or 0)

        print("\n--- run ---")
        print("re-inits done       : %s of %s in %.1f s" % (s.get("done"), s.get("requested"),
                                                           (s.get("elapsed_ms") or 0) / 1000.0))
        print("driver counters     : reinit +%d, reinit_skipped +%d" % (d_reinit, d_skip))
        print("companion reads     : ok %s, fail %s (busy %s)"
              % (s.get("hammer_ok"), s.get("hammer_fail"), s.get("hammer_busy")))
        print("bus timeouts        : +%d, all slaves (the emulated ones time out at a known rate)" % d_to)
        print("encoder failures    : +%d (addr %d: timeouts, CRC, exceptions, framing)"
              % (d_enc_fail, ENCODER_ADDR))
        if heap_a:
            print("free internal heap  : %d before -> %d settled (%+d)"
                  % (heap_b, heap_a, heap_a - heap_b))
        else:
            print("free internal heap  : not measured (the run did not finish)")
        print("uptime              : %d -> %d s" % (up0, up1))
        print("eg1                 : 0x%02x -> 0x%02x" % (eg1_0, eg1_1))
        print("coredump stored     : %s" % cd1.get("present"))

        fails = []
        if stalled:
            fails.append("the run stopped making progress (%s of %d)" % (s.get("done"), a.count))
        if not enc0:
            fails.append("no counter row for the encoder at addr %d -- nothing to judge" % ENCODER_ADDR)
        elif d_enc_fail:
            fails.append("%d encoder transactions failed during the run" % d_enc_fail)
        if up1 < up0 + (time.time() - t_up0) - 5:
            fails.append("the unit rebooted during the run")
        if cd1.get("present"):
            fails.append("a coredump was stored")
        if s.get("done") != a.count:
            fails.append("only %s of %d re-inits ran" % (s.get("done"), a.count))
        if d_reinit != a.count:
            fails.append("the driver counted %d re-inits, not %d" % (d_reinit, a.count))
        if d_skip:
            fails.append("%d re-inits were skipped (bus held > 2 s)" % d_skip)
        if not a.no_hammer and not s.get("hammer_ok"):
            fails.append("the companion completed no reads -- nothing overlapped")
        if not a.no_hammer and s.get("hammer_fail"):
            fails.append("%s companion reads failed" % s.get("hammer_fail"))
        if heap_a and heap_a + HEAP_TOLERANCE < heap_b:
            fails.append("free heap fell by %d bytes" % (heap_b - heap_a))

        if fails:
            print("\nRESULT: FAIL")
            for f in fails:
                print("  *  %s" % f)
            return 1
        if locked is False:
            print("\nRESULT: SURVIVED ON THE FAIL-FIRST BUILD -- this test did NOT catch the"
                  " unlocked re-init, so a pass on the real build proves nothing yet")
            return 1
        print("\nRESULT: PASS -- %d re-inits under traffic, no crash, nothing skipped" % a.count)
        return 0
    finally:
        safe(u.logout)
        print("(logged out)")


if __name__ == "__main__":
    sys.exit(main())

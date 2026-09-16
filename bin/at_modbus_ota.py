#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""gh#70 acceptance: Modbus transactions must survive an OTA's flash writes.

WHY THIS EXISTS
---------------
During OTA uploads the wire encoder (addr 40) stopped answering long enough for
T17's presence gate to close. Root cause, measured 2026-09-16: the driver
released the RS485 transceiver's DE/RE line from TASK context, after
`uart_wait_tx_done()` and a 2 ms guard. A flash erase or write stalls both
cores and held back the UART's TX-done interrupt (then not in IRAM), so the
release came late -- up to 665 ms during a firmware upload -- while the encoder
answers ~4.5 ms after the request. The transceiver was still driving when the
reply arrived, and the reply was lost (0 of 7 bytes) or clipped (6 of 7, CRC).

The fix hands DE/RE to the UART (RS485 half-duplex mode, RTS on the DE/RE pin,
released in the TX-done interrupt) and puts that interrupt in IRAM. The unit
reports which it runs as `de_ctrl`: "uart+iram" is the fix, "task" the old way.

WHAT IT DOES
------------
Starts back-to-back encoder reads on a bench build (`POST /api/diag/modbus`,
`{"action":"traffic"}`) and lays an OTA upload over them:

  --phase fw        firmware POST at +3 s. The unit then commits that firmware
                    on its own ~120 s later and REBOOTS.
  --phase assets    web-asset POST at +3 s. The unit REBOOTS ~1 s after the
                    extraction, so the last snapshot before that is judged.
  --phase baseline  no upload: the control.

The files uploaded are the unit's own build, `bin/<fw_ver>/`, unless `--bin` is
given -- an OTA test must not change what the unit runs.

VERDICT
-------
PASS when no encoder transaction timed out or failed CRC/framing during the
run. The driver's listen-latency counters say when it started listening after
each request. With `de_ctrl` "task" that moment IS the DE/RE release, and a
failure after a late one (> 4 ms) is this mechanism. With "uart+iram" the UART
released DE/RE on time in its interrupt and a late listen is harmless -- the
counters then only show how stalled the task was. Failures of the emulated
slaves (addr 1 and 44) are reported but not judged: on the dev rig they fail
under dense traffic whatever the DE timing (gh#68).

FAIL-FIRST
----------
A build that releases DE/RE in task context must FAIL `--phase fw` (measured on
the pre-fix driver: 16 and 13 failures, every one after a late release). Build
one by defining `MODBUS_FAILFIRST_TASK_DE` (one line near the top of
`drivers/modBus/src/modbus_rtu.cpp`); it reports `de_ctrl` "task". Only then
trust a PASS from the real build, which must report "uart+iram".

**SIDE EFFECTS ON THE DEV RIG -- read before running.** The traffic is denser
than anything the product generates, and the rig's emulated sensors fail under
it: expect T5 sensor faults and a WIND OVERRIDE safe-fail (T3 closes the
windows). The test therefore refuses to run unless the unit is in STANDBY with
all windows CLOSED (`--force` overrides). Both phases that upload REBOOT the
unit. Return it to AUTOMATIC afterwards.

USAGE
-----
    python bin/at_modbus_ota.py --host 192.168.20.169 --phase baseline
    python bin/at_modbus_ota.py --host 192.168.20.169 --phase fw
    python bin/at_modbus_ota.py --host 192.168.20.169 --phase assets

Exit 0 = pass, 1 = fail, 2 = could not run.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import os
import pathlib
import sys
import threading
import time

# The script's own directory is on sys.path, so the shared helpers import as-is.
from at_wp_ramp import Unit, DEFAULT_HOST, DEFAULT_PIN
import ota_push

ENCODER = "s40"
UPLOAD_AT_S = 3.0


def snap(u):
    """One sample of the bus diagnostics and the traffic run, or None."""
    try:
        sc, d = u._req("GET", "/api/diag/windowpos")
        sc2, s = u._req("POST", "/api/diag/modbus", {"action": "reinit_status"})
    except Exception:                                          # noqa: BLE001
        return None
    if sc != 200 or sc2 != 200 or not isinstance(d, dict) or not isinstance(s, dict):
        return None
    return {"t": time.time(), "d": d, "s": s}


def delta(a, b, key, field):
    x, y = a["d"].get(key) or {}, b["d"].get(key) or {}
    return int(y.get(field) or 0) - int(x.get(field) or 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--phase", choices=("baseline", "fw", "assets"), required=True)
    ap.add_argument("--secs", type=int, default=0,
                    help="traffic duration; default 30 (baseline, assets) or 40 (fw)")
    ap.add_argument("--bin", help="greenhouse-controller-<ver>.bin to upload "
                                  "(default: bin/<the unit's fw_ver>/)")
    ap.add_argument("--force", action="store_true",
                    help="run although the unit is not in STANDBY with all windows CLOSED")
    a = ap.parse_args()
    secs = a.secs or (40 if a.phase == "fw" else 30)

    u = Unit(a.host, a.pin)
    try:
        st = u.status()
        sysb = st.get("system") or {}
        win = st.get("windows") or {}
        mode = (st.get("mode") or {}).get("current")
        flags = (st.get("mode") or {}).get("flags") or []
        print("unit %s  fw %s  uptime %s s  mode %s %s  windows M1 %s M2 %s M3 %s"
              % (sysb.get("unit_id"), sysb.get("fw_ver"), sysb.get("uptime_s"), mode, flags,
                 win.get("M1"), win.get("M2"), win.get("M3")))
        # The STANDBY *flag*, not the displayed mode: a wind override -- which
        # this very test tends to cause -- displays over STANDBY without ending it.
        if not a.force and ("standby" not in flags or
                            any(win.get(k) != "CLOSED" for k in ("M1", "M2", "M3"))):
            print("refusing: put the unit in STANDBY with every window CLOSED first "
                  "(the traffic trips the rig's emulated sensors), or pass --force")
            return 2
        if not a.force and "wind_override" in flags:
            print("refusing: a wind override is active -- wait for it to clear, so the "
                  "run starts from a quiet bus (or pass --force)")
            return 2

        bin_path = zip_path = None
        if a.phase != "baseline":
            ver = sysb.get("fw_ver")
            here = os.path.dirname(os.path.abspath(__file__))
            bin_path = a.bin or os.path.join(here, ver, "greenhouse-controller-%s.bin" % ver)
            bin_path, zip_path, _v = ota_push.derive_artifacts(pathlib.Path(bin_path))
            if not bin_path.exists() or not zip_path.exists():
                print("missing %s or %s" % (bin_path, zip_path))
                return 2
            print("upload: %s" % (bin_path.name if a.phase == "fw" else zip_path.name))

        a0 = snap(u)
        if a0 is None or "listen_late" not in (a0["d"].get("modbus") or {}):
            print("this build has no listen-latency counters or no traffic mode -- "
                  "it predates the gh#70 fix")
            return 2
        de_ctrl = a0["d"]["modbus"].get("de_ctrl")
        print("DE/RE driven by: %s%s" % (de_ctrl, {
            "uart+iram": "  (the gh#70 fix)",
            "uart": "  (UART-driven, but its interrupt is not in IRAM -- a flash write still delays it)",
            "task": "  (task-driven: the pre-fix behaviour -- a FAIL is expected)"}.get(de_ctrl, "")))
        if not a0["d"].get(ENCODER):
            print("no counter row for the encoder at addr 40 -- nothing to judge")
            return 2
        sc, r = u._req("POST", "/api/diag/modbus", {"action": "traffic", "duration_ms": secs * 1000})
        if sc != 200 or not (isinstance(r, dict) and r.get("ok")):
            print("the unit refused the traffic run: HTTP %s %s" % (sc, r))
            return 2
        t0 = time.time()
        print("traffic: %d s of back-to-back encoder reads, phase %s" % (secs, a.phase))

        up = {}
        if a.phase != "baseline":
            def upload():
                time.sleep(UPLOAD_AT_S)
                cookie = ota_push.login(a.host, a.pin)
                if a.phase == "fw":
                    path, data, ctype = "/api/ota/firmware", bin_path.read_bytes(), "application/octet-stream"
                else:
                    path, data, ctype = "/api/ota/assets", zip_path.read_bytes(), "application/zip"
                up["t0"] = time.time() - t0
                up["code"], up["resp"] = ota_push.post_bytes(a.host, path, data, cookie, ctype, timeout=180)
                up["t1"] = time.time() - t0
            threading.Thread(target=upload, daemon=True).start()

        last, lost = a0, False
        while time.time() - t0 < secs + 5:
            b = snap(u)
            if b is None:
                if a.phase == "assets":
                    lost = True
                    print("  t+%5.1fs  the unit stopped answering -- judging the last sample"
                          % (time.time() - t0))
                    break
                time.sleep(1.0)
                continue
            last = b
            mb = b["d"].get("modbus") or {}
            s = b["s"]
            print("  t+%5.1fs  reads ok %4s fail %3s | encoder fail %3d | late listen %s (failed %s) worst %s us"
                  % (time.time() - t0, s.get("hammer_ok"), s.get("hammer_fail"),
                     delta(a0, b, ENCODER, "to") + delta(a0, b, ENCODER, "crc") + delta(a0, b, ENCODER, "fr"),
                     int(mb.get("listen_late") or 0) - int(a0["d"]["modbus"].get("listen_late") or 0),
                     int(mb.get("listen_late_failed") or 0) - int(a0["d"]["modbus"].get("listen_late_failed") or 0),
                     mb.get("listen_lat_max_us")))
            if not s.get("running") and time.time() - t0 > UPLOAD_AT_S + 1:
                break
            time.sleep(0.3 if a.phase == "assets" else 1.0)

        m0, m1 = a0["d"]["modbus"], last["d"]["modbus"]
        enc_to = delta(a0, last, ENCODER, "to")
        enc_crc = delta(a0, last, ENCODER, "crc")
        enc_fr = delta(a0, last, ENCODER, "fr")
        enc_fail = enc_to + enc_crc + enc_fr
        late = int(m1.get("listen_late") or 0) - int(m0.get("listen_late") or 0)
        late_failed = int(m1.get("listen_late_failed") or 0) - int(m0.get("listen_late_failed") or 0)
        print("\n--- %s ---" % a.phase)
        if up:
            print("upload              : HTTP %s, t+%.1f s .. t+%.1f s"
                  % (up.get("code"), up.get("t0", 0), up.get("t1", 0)))
        print("companion reads     : ok %s, fail %s (busy %s)"
              % (last["s"].get("hammer_ok"), last["s"].get("hammer_fail"), last["s"].get("hammer_busy")))
        print("encoder (addr 40)   : ok +%d, timeout +%d, CRC +%d, framing +%d"
              % (delta(a0, last, ENCODER, "ok"), enc_to, enc_crc, enc_fr))
        print("emulated addr 1/44  : timeout +%d / +%d (reported, not judged -- gh#68)"
              % (delta(a0, last, "s1", "to"), delta(a0, last, "s44", "to")))
        print("listening started   : %d late (> 4 ms), %d of them followed by a failure; worst %s us%s"
              % (late, late_failed, m1.get("listen_lat_max_us"),
                 "  [= the DE/RE release]" if de_ctrl == "task"
                 else "  [task wake-up only: the UART released DE/RE]"))
        print("last timeout        : %s of %s bytes (addr %s)"
              % (m1.get("to_received"), m1.get("to_expected"), m1.get("last_fail_addr")))
        if a.phase == "fw":
            print("NOTE: the unit will commit this firmware on its own in ~2 minutes and reboot.")
        if lost:
            print("NOTE: the unit rebooted after the extraction; samples end there.")

        if enc_fail:
            why = ""
            if late_failed and de_ctrl == "task":
                why = " (%d after a late DE/RE release: the gh#70 mechanism)" % late_failed
            print("\nRESULT: FAIL -- %d encoder transactions failed%s" % (enc_fail, why))
            return 1
        if de_ctrl == "task":
            print("\nRESULT: PASSED ON A TASK-DRIVEN BUILD -- this run did not reproduce the"
                  " gh#70 failure, so a pass on the fixed build proves nothing yet")
            return 1
        print("\nRESULT: PASS -- no encoder transaction failed during the %s phase" % a.phase)
        return 0
    finally:
        try:
            u.logout()
        except Exception:                                      # noqa: BLE001
            pass
        print("(logged out)")


if __name__ == "__main__":
    sys.exit(main())

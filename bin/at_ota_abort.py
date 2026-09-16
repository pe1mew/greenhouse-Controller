#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Acceptance: an interrupted OTA upload must release the session.

WHY THIS EXISTS
---------------
2026-09-16, FDA4: a firmware upload cut off at 60 % by a lossy WiFi link left
the OTA state machine in `fw_writing` until the unit was rebooted. The upload
handler's receive-failure exit sent a 500 and returned without releasing the
session, so `EG1_BIT_OTA_IN_PROGRESS` stayed set: every later upload was
refused (the client saw a connection reset), ROTA skipped every check, the
status flags said `ota_in_progress`, and the LCD showed OTA. There is no remote
reboot path other than completing an OTA, which was exactly what was refused.

The fix releases the session on every exit that does not install, bounds how
long a silent sender can hold the (single) httpd task, and writes a
LOG_SYSTEM value_a=32 row saying why nothing was installed.

WHAT IT DOES
------------
Each phase breaks one upload on purpose, then checks the unit recovers:

  fw-cut      POST /api/ota/firmware with the real Content-Length, send
              --cut-pct of the body, close the socket (--close fin|rst).
  fw-stall    The same, but the socket stays open and SILENT. A GET on a
              second connection is served only once the upload handler gives
              up, because httpd runs one handler at a time: it must answer
              within the stall bound (30 s after the last byte) plus margin.
  assets-cut  fw-cut on /api/ota/assets (an asset-only session).
  paired-cut  A COMPLETE firmware POST (the unit verifies it and waits for
              the assets), then a cut asset POST. The verified firmware must be
              discarded with it: nothing installed, and no firmware-only
              commit when the 120 s fallback would have fired.

After the break, every phase requires:

  1. GET /api/ota/status leaves the busy state (fw_writing / fw_verifying /
     assets_buffering / assets_writing) within --settle seconds;
  2. /api/status `eg1` no longer carries OTA_IN_PROGRESS (0x10);
  3. the SD log has a SYSTEM row value_a=32 for this break (skipped with a
     note when no SD card is mounted);
  4. unless --no-push: a complete PAIRED upload of the unit's own build is
     accepted (firmware 200 + awaiting_assets, assets 202), the unit reboots,
     and fw_ver AND asset_version both equal the uploaded version.

The files are the unit's own build, `bin/<fw_ver>/`, unless --bin is given.

FAIL-FIRST
----------
Firmware without the fix FAILS fw-cut at step 1 (the state stays fw_writing)
and at step 4 (the complete upload is refused), and fw-stall with the second
GET timing out: the silent upload holds the web server for as long as the
socket stays open. Run a phase against such a build and see it fail before
trusting a pass from the fixed one.

**A FAIL LEAVES THE UNIT STUCK until it is rebooted** -- that is the defect.
Press RESET or power-cycle it before pushing anything.

SIDE EFFECTS
------------
Step 4 reboots the unit (so does a successful push of anything). The run
refuses to start while an OTA is in progress, while a teach is running on a
bench build, while a window is moving, or on a unit whose id is not
--expect-unit (default FDA4). --force skips the teach/window checks, never the
unit check.

USAGE
-----
    python bin/at_ota_abort.py --phase fw-cut
    python bin/at_ota_abort.py --phase fw-cut --close rst --cut-pct 20
    python bin/at_ota_abort.py --phase fw-stall
    python bin/at_ota_abort.py --phase assets-cut
    python bin/at_ota_abort.py --phase paired-cut
    python bin/at_ota_abort.py --phase fw-cut --bin bin/2.8.0-bench/greenhouse-controller-2.8.0-bench.bin

Exit 0 = pass, 1 = fail, 2 = could not run.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import csv
import http.client
import io
import json
import os
import pathlib
import re
import socket
import struct
import sys
import threading
import time

# The script's own directory is on sys.path, so the shared helpers import as-is.
from at_wp_ramp import Unit, DEFAULT_HOST, DEFAULT_PIN
import ota_push

EG1_OTA_IN_PROGRESS = 0x10
BUSY_STATES = ("fw_writing", "fw_verifying", "assets_buffering", "assets_writing")
STALL_BOUND_S = 30          # OTA_UPLOAD_STALL_S in web_server.cpp
FALLBACK_S = 120            # FW_DONE_FALLBACK_MS in ota_manager.cpp
CUT_SETTLE_S = 3.0          # after the last byte leaves us, before closing
SYS_ROW_OTA_END = 32


def log(msg):
    # Unit text can carry non-ASCII (an em dash); the console is cp1252.
    msg = str(msg).encode("ascii", "replace").decode("ascii")
    print("  %s  %s" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def get_json(host, path, cookie, timeout):
    """GET with an explicit timeout. Returns (status, json-or-text, seconds)."""
    t0 = time.time()
    c = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        c.request("GET", path, headers={"Cookie": cookie} if cookie else {})
        r = c.getresponse()
        raw = r.read()
        try:
            body = json.loads(raw.decode("utf-8"))
        except ValueError:
            body = raw.decode("utf-8", "replace")
        return r.status, body, time.time() - t0
    finally:
        c.close()


def eg1_of(st):
    try:
        return int((st.get("system") or {}).get("eg1") or 0)
    except (TypeError, ValueError):
        return 0


# ---------------------------------------------------------------- the break --

class PartialUpload(object):
    """A POST that announces the full Content-Length and sends only part."""

    def __init__(self, host, path, cookie, body, ctype):
        self.host, self.path, self.body = host, path, body
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # A small send buffer, so "handed to TCP" is close to "reached the
        # unit": Windows otherwise buffers hundreds of KB, and the unit would
        # still be receiving long after the cut or the silence began.
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)
        self.sock.settimeout(90)
        self.sock.connect((host, 80))
        head = ("POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: %s\r\n"
                "Content-Length: %d\r\nCookie: %s\r\nConnection: close\r\n\r\n"
                % (path, host, ctype, len(body), cookie))
        self.sock.sendall(head.encode("ascii"))
        self.sent = 0

    def send_to(self, nbytes):
        """Send up to nbytes of the body. The unit erases the bank before it
        reads anything, so this blocks for seconds on the firmware path."""
        while self.sent < nbytes:
            chunk = self.body[self.sent:min(nbytes, self.sent + 4096)]
            self.sock.sendall(chunk)
            self.sent += len(chunk)

    def close(self, how):
        if how == "rst":
            # linger on, timeout 0: close() resets instead of FIN
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
        else:
            try:
                self.sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
        self.sock.close()


def cut_upload(u, path, body, ctype, pct, how):
    n = max(1, len(body) * pct // 100)
    log("POST %s: Content-Length %d, sending %d (%d %%), then %s"
        % (path, len(body), n, pct, how.upper()))
    up = PartialUpload(u.host, path, u.cookie, body, ctype)
    t0 = time.time()
    up.send_to(n)
    log("  %d bytes handed to TCP after %.1f s" % (n, time.time() - t0))
    time.sleep(CUT_SETTLE_S)
    up.close(how)
    log("  socket closed")
    return n


def stall_upload(u, path, body, ctype, pct):
    """Send pct of the body, go silent, and time how long the web server is
    held. Returns (seconds until a GET was served or None, the GET's body)."""
    n = max(1, len(body) * pct // 100)
    log("POST %s: Content-Length %d, sending %d (%d %%), then SILENCE"
        % (path, len(body), n, pct))
    up = PartialUpload(u.host, path, u.cookie, body, ctype)
    up.send_to(n)
    t_last = time.time()
    log("  %d bytes handed to TCP; the socket stays open and silent" % n)
    limit = STALL_BOUND_S + 60
    served, st = None, None
    try:
        sc, st, _dt = get_json(u.host, "/api/ota/status", u.cookie, timeout=limit)
        served = time.time() - t_last
        log("  GET /api/ota/status served %.1f s after the last byte: HTTP %s %s"
            % (served, sc, st))
    except (socket.timeout, OSError) as exc:
        log("  GET /api/ota/status NOT served within %d s (%s): the silent upload "
            "is holding the web server" % (limit, exc.__class__.__name__))
    finally:
        up.close("fin")
        log("  stalled socket closed")
    return served, st


# ---------------------------------------------------------------- the checks --

def wait_released(u, settle_s):
    """Poll /api/ota/status until the state leaves the busy set."""
    end = time.time() + settle_s
    last = None
    while time.time() < end:
        try:
            sc, st, _dt = get_json(u.host, "/api/ota/status", u.cookie, timeout=15)
        except (socket.timeout, OSError):
            time.sleep(1.0)
            continue
        if sc == 401:
            u._login(u.pin)
            continue
        if isinstance(st, dict):
            key = (st.get("state"), st.get("progress"))
            if key != last:
                log("  ota/status: state=%s progress=%s error=%r"
                    % (st.get("state"), st.get("progress"), st.get("error")))
                last = key
            if st.get("state") not in BUSY_STATES:
                return st
        time.sleep(1.0)
    return None


def sd_rows(u, unit_id):
    """All SYSTEM value_a=32 rows in this unit's newest SD file, or None if
    there is no SD log to read. The caller compares against a snapshot taken
    before the break, so no clock is involved: /api/status time is a shadow
    that can lag by up to a minute."""
    sc, files, _dt = get_json(u.host, "/api/log/files", u.cookie, timeout=15)
    names = (files or {}).get("sd_files") if isinstance(files, dict) else None
    if sc != 200 or not names:
        return None
    # Names carry the writing unit's id ("FDA4_20260916080058.csv") and the
    # card moves between modules, so pick this unit's files, newest by stamp.
    own = [n for n in names if n.upper().startswith(unit_id.upper() + "_")] or names
    newest = max(own, key=lambda n: re.sub(r"\D", "", n)[-14:])
    c = http.client.HTTPConnection(u.host, 80, timeout=60)
    try:
        c.request("GET", "/api/log/download?file=" + newest,
                  headers={"Cookie": u.cookie})
        r = c.getresponse()
        raw = r.read().decode("utf-8", "replace")
        if r.status != 200:
            return None
    finally:
        c.close()
    rows = []
    for row in csv.DictReader(io.StringIO(raw)):
        if row.get("type") == "SYSTEM" and row.get("value_a") == str(SYS_ROW_OTA_END):
            vb = int(row.get("value_b") or 0) & 0xFFFF
            rows.append({"ts": row.get("timestamp"), "stage": int(row.get("ch") or 0),
                         "reason": vb >> 8, "pct": vb & 0xFF})
    return rows


def check_sd_row(u, before, stage, reasons, unit_id):
    """A NEW value_a=32 row (not in the pre-break snapshot) for this stage and
    reason. It may take a few seconds to reach the card."""
    rows = None
    for _ in range(10):
        rows = sd_rows(u, unit_id)
        if rows is None:
            log("  SD: no log file readable -- row check SKIPPED (no card?)")
            return None
        hit = [r for r in rows if r not in (before or [])
               and r["stage"] == stage and r["reason"] in reasons]
        if hit:
            log("  SD: new value_a=32 row %s" % hit[-1])
            return True
        time.sleep(2.0)
    new = [r for r in (rows or []) if r not in (before or [])]
    log("  SD: NO new value_a=32 row with stage %d and reason in %s (new rows: %s)"
        % (stage, reasons, new))
    return False


def watch_no_reboot(u, secs):
    """True if uptime keeps rising for secs (the unit did not reboot)."""
    prev = None
    end = time.time() + secs
    while time.time() < end:
        s = ota_push.status(u.host, timeout=8)
        up = ota_push.sys_block(s).get("uptime_s") if s else None
        if up is not None:
            if prev is not None and up < prev:
                log("  uptime fell %s -> %s: the unit REBOOTED" % (prev, up))
                return False
            prev = up
        time.sleep(5.0)
    log("  no reboot in %d s (uptime %s)" % (secs, prev))
    return True


def push_paired(u, bin_path, zip_path, version):
    """Complete firmware + assets, back to back, then verify both versions.
    Returns (ok, note). ota_push's wait helpers exit on failure; that is
    turned into a failed check here so the verdict still prints."""
    try:
        return _push_paired(u, bin_path, zip_path, version)
    except SystemExit as exc:
        return False, "push did not complete: %s" % exc


def _push_paired(u, bin_path, zip_path, version):
    fw, za = bin_path.read_bytes(), zip_path.read_bytes()
    base = ota_push.sys_block(ota_push.status(u.host)).get("uptime_s") or 0
    log("complete upload: %s (%d B) then %s (%d B)"
        % (bin_path.name, len(fw), zip_path.name, len(za)))
    t0 = time.time()
    try:
        code, resp = ota_push.post_bytes(u.host, "/api/ota/firmware", fw, u.cookie,
                                         "application/octet-stream", timeout=180)
    except (OSError, http.client.HTTPException) as exc:
        return False, "firmware upload REFUSED -- connection dropped (%s: %s)" % (
            exc.__class__.__name__, exc)
    log("  firmware: HTTP %s in %.1f s  %s" % (code, time.time() - t0, resp[:120]))
    try:
        awaiting = json.loads(resp).get("awaiting_assets") is True
    except (ValueError, AttributeError):
        awaiting = False
    if code != 200 or not awaiting:
        return False, "firmware upload not accepted (HTTP %s %s)" % (code, resp[:120])
    try:
        code, resp = ota_push.post_bytes(u.host, "/api/ota/assets", za, u.cookie,
                                         "application/zip", timeout=120)
    except (OSError, http.client.HTTPException) as exc:
        return False, "asset upload dropped (%s)" % exc
    log("  assets: HTTP %s  %s" % (code, resp[:120]))
    if code not in (200, 202):
        return False, "asset upload not accepted (HTTP %s)" % code
    ota_push.wait_extract(u.host, u.cookie, deadline_s=90)
    ota_push.wait_reboot(u.host, prev_uptime=base + (time.time() - t0) + 5,
                         deadline_s=180)
    time.sleep(3)
    sb = ota_push.sys_block(ota_push.status(u.host))
    fwv, av = sb.get("fw_ver"), sb.get("asset_version")
    log("  after reboot: fw_ver=%s asset_version=%s uptime=%s"
        % (fwv, av, sb.get("uptime_s")))
    u.cookie = None                  # the reboot dropped the RAM-only session
    u._login(u.pin)
    if fwv != version or av != version:
        return False, "version MISMATCH: expected %s/%s, got %s/%s" % (
            version, version, fwv, av)
    return True, "installed %s: fw_ver and asset_version both match" % version


# ---------------------------------------------------------------------- main --

def preflight(u, a):
    st = u.status()
    sysb = st.get("system") or {}
    mode = st.get("mode") or {}
    win = st.get("windows") or {}
    uid = str(sysb.get("unit_id", "?"))
    print("unit %s  fw %s  assets %s  uptime %s s  eg1 0x%x  mode %s %s  windows %s"
          % (uid, sysb.get("fw_ver"), sysb.get("asset_version"), sysb.get("uptime_s"),
             eg1_of(st), mode.get("current"), mode.get("flags"),
             {k: win.get(k) for k in ("M1", "M2", "M3")}))
    if "5C88" in uid.upper():
        sys.exit("REFUSING: %s is the production unit." % uid)
    if uid.upper() != a.expect_unit.upper():
        print("refusing: unit is %s, expected %s (--expect-unit)" % (uid, a.expect_unit))
        return None
    sc, ota, _dt = get_json(u.host, "/api/ota/status", u.cookie, timeout=15)
    print("ota/status: %s" % ota)
    if not isinstance(ota, dict) or ota.get("state") not in ("idle", "error"):
        print("refusing: an OTA is in progress (state %s) -- a stuck one needs a reboot"
              % (ota.get("state") if isinstance(ota, dict) else ota))
        return None
    if eg1_of(st) & EG1_OTA_IN_PROGRESS:
        print("refusing: eg1 says an OTA is in progress")
        return None
    if not a.force:
        moving = [k for k in ("M1", "M2", "M3") if "MOVING" in str(win.get(k))]
        if moving:
            print("refusing: %s moving -- the push reboots the unit (--force)" % moving)
            return None
        sc, com, _dt = get_json(u.host, "/api/diag/commission", u.cookie, timeout=15)
        if sc == 200 and isinstance(com, dict) and (
                com.get("teach_armed") or com.get("state") not in ("idle", "done", "failed")):
            print("refusing: a teach is running (%s) -- do not break its unit (--force)"
                  % {k: com.get(k) for k in ("state", "teach_armed", "standby_held")})
            return None
    ver = sysb.get("fw_ver")
    here = os.path.dirname(os.path.abspath(__file__))
    bin_path = a.bin or os.path.join(here, ver, "greenhouse-controller-%s.bin" % ver)
    bin_path, zip_path, version = ota_push.derive_artifacts(pathlib.Path(bin_path))
    if not bin_path.exists() or not zip_path.exists():
        print("missing %s or %s (pass --bin)" % (bin_path, zip_path))
        return None
    if version != ver:
        print("NOTE: uploading %s while the unit runs %s -- a pass then also "
              "changes what it runs" % (version, ver))
    return bin_path, zip_path, version


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--pin", default=DEFAULT_PIN)
    ap.add_argument("--phase", required=True,
                    choices=("fw-cut", "fw-stall", "assets-cut", "paired-cut"))
    ap.add_argument("--cut-pct", type=int, default=60,
                    help="share of the body sent before the break (default 60)")
    ap.add_argument("--close", choices=("fin", "rst"), default="fin",
                    help="how a cut connection ends (default fin)")
    ap.add_argument("--settle", type=int, default=45,
                    help="seconds allowed to leave the busy state (default 45)")
    ap.add_argument("--bin", help="greenhouse-controller-<ver>.bin for the complete "
                                  "upload (default: bin/<the unit's fw_ver>/)")
    ap.add_argument("--no-push", action="store_true",
                    help="skip step 4 (the complete upload and its reboot)")
    ap.add_argument("--expect-unit", default="FDA4")
    ap.add_argument("--force", action="store_true",
                    help="run although a teach is running or a window is moving")
    a = ap.parse_args()
    if not 1 <= a.cut_pct <= 99:
        ap.error("--cut-pct must be 1..99")
    try:
        sys.stdout.reconfigure(errors="replace")   # unit text on a cp1252 console
    except (AttributeError, ValueError):
        pass

    u = Unit(a.host, a.pin)
    verdict = []                    # (check, ok-or-None, detail)
    try:
        pre = preflight(u, a)
        if pre is None:
            return 2
        bin_path, zip_path, version = pre
        sd_before = sd_rows(u, a.expect_unit)
        log("SD: %s value_a=32 row(s) before the break"
            % ("no log --" if sd_before is None else len(sd_before)))
        stage, reasons = 1, (1,)

        print("\n--- break: %s ---" % a.phase)
        if a.phase == "fw-cut":
            cut_upload(u, "/api/ota/firmware", bin_path.read_bytes(),
                       "application/octet-stream", a.cut_pct, a.close)
        elif a.phase == "assets-cut":
            stage = 2
            cut_upload(u, "/api/ota/assets", zip_path.read_bytes(),
                       "application/zip", a.cut_pct, a.close)
        elif a.phase == "fw-stall":
            reasons = (2,)
            served, st = stall_upload(u, "/api/ota/firmware", bin_path.read_bytes(),
                                      "application/octet-stream", a.cut_pct)
            ok = served is not None and served <= STALL_BOUND_S + 30
            verdict.append(("web server released a silent upload within %d s"
                            % (STALL_BOUND_S + 30), ok,
                            "never served" if served is None else "%.1f s" % served))
        else:   # paired-cut
            stage = 2
            fw = bin_path.read_bytes()
            log("complete firmware POST first (%d B)" % len(fw))
            try:
                code, resp = ota_push.post_bytes(u.host, "/api/ota/firmware", fw,
                                                 u.cookie, "application/octet-stream",
                                                 timeout=180)
            except (OSError, http.client.HTTPException) as exc:
                print("firmware POST dropped (%s) -- nothing to test" % exc)
                return 2
            log("  firmware: HTTP %s  %s" % (code, resp[:120]))
            sc, st, _dt = get_json(u.host, "/api/ota/status", u.cookie, timeout=15)
            log("  ota/status: %s" % st)
            if code != 200 or not isinstance(st, dict) or st.get("state") != "fw_done":
                print("could not reach fw_done -- nothing to test")
                return 2
            t_fw_done = time.time()
            cut_upload(u, "/api/ota/assets", zip_path.read_bytes(),
                       "application/zip", a.cut_pct, a.close)

        print("\n--- recovery ---")
        st = wait_released(u, a.settle)
        verdict.append(("OTA state left the busy set within %d s" % a.settle,
                        st is not None,
                        ("state=%s error=%r" % (st.get("state"), st.get("error")))
                        if st else "still busy"))
        if st is not None and a.phase == "paired-cut":
            said = "firmware then assets" in (st.get("error") or "")
            verdict.append(("error text says to upload firmware AND assets again",
                            said, st.get("error")))
        s = ota_push.status(u.host)
        eg1 = eg1_of(s or {})
        verdict.append(("eg1 OTA_IN_PROGRESS cleared", not (eg1 & EG1_OTA_IN_PROGRESS),
                        "eg1=0x%x" % eg1))
        sd = check_sd_row(u, sd_before, stage, reasons, a.expect_unit)
        verdict.append(("SD row value_a=32 (stage %d, reason %s)" % (stage, reasons),
                        sd, "no SD log" if sd is None else ""))
        if a.phase == "paired-cut" and st is not None:
            wait = max(0, int(FALLBACK_S + 10 - (time.time() - t_fw_done)))
            log("watching %d s past the firmware-only fallback point" % wait)
            verdict.append(("verified firmware was NOT installed alone",
                            watch_no_reboot(u, wait), ""))

        if not a.no_push:
            print("\n--- the next upload ---")
            ok, note = push_paired(u, bin_path, zip_path, version)
            verdict.append(("complete upload accepted and installed", ok, note))
            if not ok:
                sc, st2, _dt = get_json(u.host, "/api/ota/status", u.cookie, timeout=15)
                log("  ota/status now: %s" % st2)

        print("\n--- %s ---" % a.phase)
        failed = False
        for name, ok, detail in verdict:
            tag = "PASS" if ok else ("SKIP" if ok is None else "FAIL")
            failed = failed or ok is False
            print("  %-4s  %s%s" % (tag, name, ("  [%s]" % detail) if detail else ""))
        if failed:
            print("\nRESULT: FAIL -- if the OTA state is stuck, the unit needs a reboot "
                  "before anything else can be pushed")
            return 1
        print("\nRESULT: PASS")
        return 0
    finally:
        try:
            u.logout()
        except Exception:                                      # noqa: BLE001
            pass
        print("(logged out)")


if __name__ == "__main__":
    sys.exit(main())

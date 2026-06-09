#!/usr/bin/env python3
"""OTA push tool for greenhouse-controller units.

Usage:
    ota_push.py PATH_TO_BIN [--host HOST] [--pin PIN]

Where PATH_TO_BIN is e.g.:
    bin/2.0.0-rc.1.5.3-wip/greenhouse-controller-2.0.0-rc.1.5.3-wip.bin

The sibling web-assets-<version>.zip is auto-discovered in the same directory.
Version is parsed from the .bin filename.

Defaults: host 192.168.20.160, pin 12345678. Stdlib-only.

Flow:
  1. POST /api/login                                                  → cookie
  2. POST /api/ota/firmware (raw .bin body)                           → 200
  3. wait_reboot — fw_done fallback commits firmware after ~120 s
  4. POST /api/login                                                  → fresh cookie
  5. POST /api/ota/assets (raw zip body)                              → 200 or 202
  6. If 202: GET /api/ota/status loop until state in {idle, rebooting, fw_done}
  7. wait_reboot — baseline uptime is captured FRESH right before step 5
  8. GET /api/status — verify fw_ver and asset_version match the uploaded version
"""
import argparse
import http.client
import json
import pathlib
import re
import sys
import time

HOST_DEFAULT = "192.168.20.160"
PIN_DEFAULT  = "12345678"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("bin_path", type=pathlib.Path,
                   help="path to greenhouse-controller-<version>.bin "
                        "(the sibling web-assets-<version>.zip is auto-discovered)")
    p.add_argument("--host", default=HOST_DEFAULT,
                   help=f"controller IP or hostname (default {HOST_DEFAULT})")
    p.add_argument("--pin",  default=PIN_DEFAULT,
                   help="admin PIN (default 12345678)")
    return p.parse_args()


def derive_artifacts(bin_path):
    """Return (bin_path, zip_path, version) from greenhouse-controller-<v>.bin."""
    m = re.match(r"greenhouse-controller-(?P<v>.+)\.bin$", bin_path.name)
    if not m:
        sys.exit(f"bin filename must look like greenhouse-controller-<version>.bin, "
                 f"got: {bin_path.name}")
    version = m.group("v")
    zip_path = bin_path.with_name(f"web-assets-{version}.zip")
    return bin_path, zip_path, version


def login(host, pin):
    body = json.dumps({"role": "admin", "pin": pin}).encode()
    c = http.client.HTTPConnection(host, 80, timeout=15)
    c.request("POST", "/api/login", body, {"Content-Type": "application/json",
                                           "Content-Length": str(len(body))})
    r = c.getresponse()
    status_code = r.status
    payload = r.read().decode("utf-8", "replace")
    sc = r.getheader("Set-Cookie") or ""
    c.close()
    if status_code != 200:
        sys.exit(f"login failed status={status_code} body={payload}")
    cookie = sc.split(";", 1)[0]
    print(f"  login OK  cookie={cookie}")
    return cookie


def status(host, cookie=None, timeout=8):
    try:
        c = http.client.HTTPConnection(host, 80, timeout=timeout)
        hdrs = {"Cookie": cookie} if cookie else {}
        c.request("GET", "/api/status", headers=hdrs)
        r = c.getresponse()
        body = r.read()
        c.close()
        if r.status != 200:
            return None
        return json.loads(body)
    except Exception:
        return None


def ota_status(host, cookie, timeout=4):
    try:
        c = http.client.HTTPConnection(host, 80, timeout=timeout)
        c.request("GET", "/api/ota/status", headers={"Cookie": cookie})
        r = c.getresponse()
        body = r.read()
        c.close()
        if r.status != 200:
            return None
        return json.loads(body)
    except Exception:
        return None


def sys_block(payload):
    """Newer firmware names it 'system'; older payloads used 'sys'. Tolerate both."""
    if not payload:
        return {}
    return payload.get("system") or payload.get("sys") or {}


def wait_extract(host, cookie, deadline_s):
    """Poll /api/ota/status until extraction reaches a terminal/handoff state.

    Terminal states (per ota_manager.h):
      idle      — extraction complete, device fully back to baseline
      rebooting — schedule_reboot armed; reboot imminent
      fw_done   — paired-OTA edge: firmware-only path commited without assets
      error     — extraction failed; exit non-zero
    """
    TERMINAL = {"idle", "rebooting", "fw_done"}
    t_end = time.time() + deadline_s
    last_seen = None
    while time.time() < t_end:
        s = ota_status(host, cookie)
        if s is None:
            # Connection may have dropped because the device is rebooting.
            # That counts as "done" for our purposes — wait_reboot picks up next.
            print(f"  ...extract  /api/ota/status unreachable — assume rebooting")
            return
        st  = s.get("state", "?")
        pct = s.get("progress", 0)
        err = s.get("error", "") or ""
        if (st, pct) != last_seen:
            print(f"  ...extract  state={st}  progress={pct}%  err={err!r}")
            last_seen = (st, pct)
        if st == "error":
            sys.exit(f"asset extraction failed: error={err!r}")
        if st in TERMINAL:
            return
        time.sleep(1.0)
    sys.exit(f"timeout waiting for asset extraction (deadline {deadline_s}s)")


def wait_reboot(host, prev_uptime, deadline_s):
    """Poll /api/status until uptime_s drops below prev_uptime (reboot detected)."""
    t_end = time.time() + deadline_s
    last_err = "no response"
    while time.time() < t_end:
        time.sleep(2.0)
        s = status(host, timeout=4)
        if s:
            sb = sys_block(s)
            up = sb.get("uptime_s")
            fw = sb.get("fw_ver")
            if up is not None and up < prev_uptime:
                print(f"  back up   uptime={up}s  fw_ver={fw}")
                return s
            print(f"  ...still  uptime={up}s fw_ver={fw}")
        else:
            print(f"  ...probe  no response")
            last_err = "no response"
    sys.exit(f"timeout waiting for reboot — last={last_err}")


def post_bytes(host, path, body, cookie, ctype, timeout=120):
    c = http.client.HTTPConnection(host, 80, timeout=timeout)
    hdrs = {
        "Content-Type":   ctype,
        "Content-Length": str(len(body)),
        "Cookie":         cookie,
    }
    c.request("POST", path, body, hdrs)
    r = c.getresponse()
    payload = r.read().decode("utf-8", "replace")
    c.close()
    return r.status, payload


def main():
    args = parse_args()
    host = args.host
    pin  = args.pin
    bin_path, zip_path, version = derive_artifacts(args.bin_path)
    for p in (bin_path, zip_path):
        if not p.exists():
            sys.exit(f"missing artifact: {p}")
    print(f"host:     {host}")
    print(f"version:  {version}")
    print(f"firmware: {bin_path.name}  {bin_path.stat().st_size:,} bytes")
    print(f"assets:   {zip_path.name}  {zip_path.stat().st_size:,} bytes")

    # Baseline
    pre = status(host)
    if not pre:
        sys.exit("baseline /api/status failed — unit unreachable?")
    sb = sys_block(pre)
    print(f"baseline fw_ver={sb.get('fw_ver')} unit_id={sb.get('unit_id')} "
          f"uptime={sb.get('uptime_s')}s")
    pre_up = sb.get("uptime_s", 0)

    # [1] login
    print("\n[1] login")
    cookie = login(host, pin)

    # [2] POST firmware
    print(f"\n[2] POST /api/ota/firmware  ({bin_path.stat().st_size:,} bytes)")
    fw_bytes = bin_path.read_bytes()
    t0 = time.time()
    code, resp = post_bytes(host, "/api/ota/firmware", fw_bytes, cookie,
                            "application/octet-stream", timeout=180)
    dt = time.time() - t0
    print(f"  HTTP {code}  ({dt:.1f}s)  body={resp[:200]}")
    if code != 200:
        sys.exit(f"firmware POST failed status={code}")

    # [3] wait for the fw_done fallback timer to commit and reboot
    print("\n[3] wait for reboot (firmware applied via fw_done fallback timer)")
    wait_reboot(host, prev_uptime=pre_up + dt, deadline_s=180)

    # [4] re-auth
    print("\n[4] re-login")
    time.sleep(3)
    cookie = login(host, pin)

    # Capture a FRESH uptime baseline immediately before the assets POST so
    # wait_reboot() in step [7] has the real pre-OTA uptime to compare against.
    # The previous version of this script hardcoded prev_uptime=600 here —
    # which worked by accident as long as the unit's uptime stayed below it.
    pre2 = status(host, cookie=cookie)
    pre2_up = sys_block(pre2).get("uptime_s", 0)
    print(f"  baseline uptime before assets POST = {pre2_up}s")

    # [5] POST assets — endpoint returns 202 Accepted (extraction is async)
    print(f"\n[5] POST /api/ota/assets  ({zip_path.stat().st_size:,} bytes)")
    zip_bytes = zip_path.read_bytes()
    t0 = time.time()
    code, resp = post_bytes(host, "/api/ota/assets", zip_bytes, cookie,
                            "application/zip", timeout=120)
    dt = time.time() - t0
    print(f"  HTTP {code}  ({dt:.1f}s)  body={resp[:200]}")
    if code not in (200, 202):
        sys.exit(f"assets POST failed status={code}")

    # [6] poll until T13 extraction finishes (only needed for 202 responses)
    if code == 202:
        print("\n[6] poll /api/ota/status until extraction terminal")
        wait_extract(host, cookie, deadline_s=60)

    # [7] wait for reboot
    print("\n[7] wait for reboot (assets extracted)")
    wait_reboot(host, prev_uptime=pre2_up + dt + 5, deadline_s=180)

    # [8] verify
    print("\n[8] verify")
    time.sleep(2)
    final = status(host)
    if not final:
        sys.exit("post-OTA /api/status failed")
    sb_final = sys_block(final)
    fw  = sb_final.get("fw_ver")
    av  = sb_final.get("asset_version")
    ntp = sb_final.get("ntp_synced")
    print(f"  fw_ver         = {fw}")
    print(f"  asset_version  = {av}")
    print(f"  ntp_synced     = {ntp}")
    print(f"  uptime_s       = {sb_final.get('uptime_s')}")
    if fw == version and av == version:
        print(f"\nOK — unit {host} now on {version}")
        return 0
    sys.exit(f"\nMISMATCH — expected fw={version} av={version}, got fw={fw} av={av}")


if __name__ == "__main__":
    sys.exit(main() or 0)

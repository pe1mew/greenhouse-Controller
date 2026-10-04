#!/usr/bin/env python3
"""at_t13_vfs.py -- gh#89 acceptance: T13's LittleFS mounts, on the unit.

gh#89: ESP-IDF 5.5.0 refuses every VFS registration once its table has been
full, and T13's mount of the inactive LittleFS was this firmware's 8th entry
of 8. After a T13 run that ended without a reboot nothing could be mounted
until a reboot: the next asset upload failed, the SD card could not be
remounted, and T13 formatted the intact inactive partition. 2.16.1 sets
CONFIG_VFS_MAX_COUNT to 12, and formats only a partition whose CONTENTS were
refused.

Stages, run in the order given; each prints a PASS/FAIL line per verdict:

  vfs       any build.
            1. an assets-only upload of a DEFLATED zip, which T13 refuses after
               mounting the inactive partition (precondition);
            2. an SD unmount and mount: the mount must succeed;
            3. 14 s after 1, an assets-only upload of the valid zip: it must
               extract and reboot.
            Fail-first: the 2.16.0 image fails 2 and 3 (2026-10-04), and is
            left unable to mount anything until a reboot (no route does that
            on a release build: push an image).

  noformat  bench builds (GET/POST /api/diag/ota).
            1. the inactive partition mounts and holds assets (precondition);
            2. fill the VFS table: the next mount is refused with
               ESP_ERR_NO_MEM, exactly as on 2026-10-03/04 (precondition);
            3. an assets-only upload of the valid zip must fail with
               "mount refused (ESP_ERR_NO_MEM), not formatted";
            4. after a reboot the inactive partition holds the assets of 1.
            Fail-first: OTA_FAILFIRST_2161=1 formats at 3 (the old rule), so 3
            and 4 fail.

  corrupt   bench builds. Erase the inactive partition's superblock: its mount
            must say `corrupt` (ESP_FAIL); an assets-only upload must still
            format it, extract and reboot; then it holds the uploaded assets.
            The first-flash path, which gh#89 must not break.

  boot      bench builds; the GUI is gone until the stage restores it. Erase
            the ACTIVE partition's superblock and reboot: the boot mount must
            find it corrupt, format and mount it (its test file present, no
            index.html); an assets-only upload then restores the GUI.

Usage:
  python bin/at_t13_vfs.py --zip <web-assets-<ver>.zip> [--host H] STAGE [STAGE ...]

The zip is the valid one for the image on the unit (stored entries). Exit code
0 when every verdict passed. Refuses 5C88. Stdlib only, plus at_wp_ramp.Unit.
"""
import argparse
import http.client
import io
import json
import os
import sys
import time
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from at_wp_ramp import Unit                                     # noqa: E402

PIN = "12345678"
BUSY = {"assets_buffering", "assets_writing", "fw_writing", "fw_verifying"}
T0 = time.time()
RESULTS = []


def say(*a):
    print("%7.1f s " % (time.time() - T0), *a, flush=True)


def verdict(stage, what, ok, detail=""):
    RESULTS.append((stage, what, ok))
    say("%s  %-8s %s%s" % ("PASS" if ok else "FAIL", stage, what, ("  [%s]" % detail) if detail else ""))


class Rig:
    def __init__(self, host):
        self.host = host
        self.unit = None

    def public(self):
        with urllib.request.urlopen("http://%s/api/status" % self.host, timeout=6) as r:
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

    def ota_status(self):
        c = http.client.HTTPConnection(self.host, 80, timeout=6)
        c.request("GET", "/api/ota/status", headers={"Cookie": self.unit.cookie})
        r = c.getresponse()
        out = json.loads(r.read().decode("utf-8", "replace"))
        c.close()
        return out

    def diag(self):
        sc, d = self.req("GET", "/api/diag/ota")
        return d if sc == 200 and isinstance(d, dict) else None

    def wait_back(self, up_before, limit=150):
        """Wait for a reboot to finish: uptime below what it was, then log in."""
        t_end = time.time() + limit
        while time.time() < t_end:
            time.sleep(3)
            try:
                y = self.public()
            except Exception:                                   # noqa: BLE001
                continue
            if y["uptime_s"] < up_before or y["uptime_s"] < 60:
                time.sleep(5)       # let the boot settle (SD mount, T11)
                self.login()
                return y
        raise SystemExit("the unit did not come back within %d s" % limit)

    def upload(self, label, data):
        """Assets-only upload. Returns ('rebooted', '') or ('error', text)."""
        up = self.public()["uptime_s"]
        c = http.client.HTTPConnection(self.host, 80, timeout=120)
        c.request("POST", "/api/ota/assets", data, {"Content-Type": "application/zip",
                                                    "Content-Length": str(len(data)),
                                                    "Cookie": self.unit.cookie})
        r = c.getresponse()
        body = r.read().decode("utf-8", "replace")
        c.close()
        say("%s: POST %d bytes -> HTTP %s" % (label, len(data), r.status))
        if r.status not in (200, 202):
            return "error", "HTTP %s %s" % (r.status, body[:120])
        t_end = time.time() + 120
        while time.time() < t_end:
            try:
                st = self.ota_status()
            except Exception:                                   # noqa: BLE001
                break       # unreachable: rebooting
            if st.get("state") == "error":
                return "error", st.get("error", "")
            if st.get("state") == "rebooting":
                break
            if st.get("state") not in BUSY:
                return "error", "ended in state %r" % st.get("state")
            time.sleep(0.5)
        self.unit = None
        y = self.wait_back(up)
        say("%s: back up, fw %s assets %s sd_mounted %s" % (label, y["fw_ver"], y["asset_version"],
                                                            y.get("sd_mounted")))
        return "rebooted", ""


def settle(rig, need_s=40):
    """Wait for `need_s` of uptime before anything that reboots the unit.

    T13's boot-loop guard (ota_check_rollback) counts every boot that comes
    within OTA_HEALTHY_MS (30 s) of the previous one, and on the fourth it marks
    the running image invalid and boots the OTHER bank. A push already makes two
    such boots, so a stage that reboots straight after one rolls the unit back
    (2026-10-04: the bench image fell back to the release mid-run)."""
    while rig.public()["uptime_s"] < need_s:
        time.sleep(3)


def deflated_copy(data):
    zin = zipfile.ZipFile(io.BytesIO(data))
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for i in zin.infolist():
            z.writestr(i.filename, zin.read(i.filename))
    return buf.getvalue()


def zip_version(data):
    return json.loads(zipfile.ZipFile(io.BytesIO(data)).read("manifest.json"))["asset_version"]


def stage_vfs(rig, data):
    s = "vfs"
    # Step 2's SD unmount races T9's writes (a pre-existing defect, found here
    # 2026-10-04: the unmount closes FAT's lock while T9 holds it, and the unit
    # panics). A unit just booted writes the most, so let it settle first.
    while rig.public()["uptime_s"] < 60:
        time.sleep(3)
    t1 = time.time()
    st, err = rig.upload("1 deflated zip", deflated_copy(data))
    if not (st == "error" and "method 8" in err):
        verdict(s, "precondition: T13 refuses the deflated zip", False, "%s %s" % (st, err))
        return
    say("1: refused as intended: %s" % err[:70])
    _, u = rig.req("POST", "/api/sd/unmount", {})
    time.sleep(2)
    _, m = rig.req("POST", "/api/sd/mount", {})
    time.sleep(2)
    sd = rig.public().get("sd_mounted")
    verdict(s, "2: SD remount after a T13 run that did not reboot", bool(m.get("ok")) and sd is True,
            "unmount %s, mount %s, sd_mounted %s" % (u, m, sd))
    time.sleep(max(0.0, 14 - (time.time() - t1)))
    st, err = rig.upload("3 valid zip", data)
    verdict(s, "3: the next asset upload extracts and reboots", st == "rebooted", err[:90])


def stage_noformat(rig, data):
    s = "noformat"
    settle(rig)
    d0 = rig.diag()
    if d0 is None:
        verdict(s, "precondition: /api/diag/ota answers (bench build)", False)
        return
    if d0.get("failfirst_2161"):
        say("fail-first image (failfirst_2161 %s): this stage must FAIL" % d0["failfirst_2161"])
    ina0 = d0["inactive"]
    if ina0.get("mount") != "ok" or not ina0.get("assets"):
        verdict(s, "precondition: the inactive partition mounts and holds assets", False, json.dumps(ina0))
        return
    say("1: inactive %s holds assets %s" % (ina0["part"], ina0["assets"]))
    _, fill = rig.req("POST", "/api/diag/ota", {"vfs_fill": True})
    if fill.get("stopped_by") != "ESP_ERR_NO_MEM":
        verdict(s, "precondition: the VFS table fills", False, json.dumps(fill))
        return
    say("2: VFS table filled: %s" % json.dumps(fill))
    st, err = rig.upload("3 valid zip", data)
    verdict(s, "3: the upload is refused and the partition NOT formatted",
            st == "error" and "mount refused (ESP_ERR_NO_MEM), not formatted" in err, err[:90])
    up = rig.public()["uptime_s"]
    rig.req("POST", "/api/diag/ota", {"reboot": True})
    rig.unit = None
    rig.wait_back(up)
    d1 = rig.diag() or {}
    ina1 = d1.get("inactive", {})
    verdict(s, "4: after a reboot the inactive partition still holds %s" % ina0["assets"],
            ina1.get("mount") == "ok" and ina1.get("assets") == ina0["assets"], json.dumps(ina1))


def stage_corrupt(rig, data):
    s = "corrupt"
    settle(rig)
    if rig.diag() is None:
        verdict(s, "precondition: /api/diag/ota answers (bench build)", False)
        return
    _, e = rig.req("POST", "/api/diag/ota", {"erase_superblock": "inactive"})
    d0 = rig.diag() or {}
    ina0 = d0.get("inactive", {})
    verdict(s, "1: the erased inactive partition reads corrupt (ESP_FAIL)",
            bool(e.get("ok")) and ina0.get("mount") == "corrupt" and ina0.get("err") == "ESP_FAIL",
            "%s %s" % (json.dumps(e), json.dumps(ina0)))
    st, err = rig.upload("2 valid zip", data)
    verdict(s, "2: T13 formats it, extracts and reboots", st == "rebooted", err[:90])
    d1 = rig.diag() or {}
    ina1 = d1.get("inactive", {})
    want = zip_version(data)
    verdict(s, "3: the inactive partition now holds %s" % want,
            ina1.get("mount") == "ok" and ina1.get("index") is True and ina1.get("assets") == want,
            json.dumps(ina1))


def stage_boot(rig, data):
    s = "boot"
    settle(rig)
    if rig.diag() is None:
        verdict(s, "precondition: /api/diag/ota answers (bench build)", False)
        return
    up = rig.public()["uptime_s"]
    _, e = rig.req("POST", "/api/diag/ota", {"erase_superblock": "active"})
    say("1: erased the active superblock: %s" % json.dumps(e))
    rig.unit = None
    rig.wait_back(up)
    d0 = rig.diag() or {}
    act = d0.get("active", {})
    verdict(s, "1: the boot formatted and mounted the corrupt active partition",
            act.get("test_file") is True and act.get("index") is False, json.dumps(act))
    settle(rig)
    st, err = rig.upload("2 valid zip (restores the GUI)", data)
    verdict(s, "2: an assets-only upload restores the GUI", st == "rebooted", err[:90])
    d1 = rig.diag() or {}
    act1 = d1.get("active", {})
    verdict(s, "3: the active partition serves index.html again",
            act1.get("index") is True, json.dumps(act1))


STAGES = {"vfs": stage_vfs, "noformat": stage_noformat, "corrupt": stage_corrupt, "boot": stage_boot}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="192.168.20.160")
    ap.add_argument("--zip", required=True, help="the valid web-assets zip for the image on the unit")
    ap.add_argument("stages", nargs="+", choices=sorted(STAGES))
    a = ap.parse_args()
    rig = Rig(a.host)
    y = rig.public()
    if "5C88" in str(y.get("unit_id", "")).upper():
        sys.exit("REFUSED: 5C88 is production")
    data = open(a.zip, "rb").read()
    if any(i.compress_type != zipfile.ZIP_STORED for i in zipfile.ZipFile(io.BytesIO(data)).infolist()):
        sys.exit("REFUSED: %s has compressed entries; the unit takes stored ones only" % a.zip)
    say("unit %s fw %s assets %s uptime %s sd_mounted %s; zip %s" % (
        y["unit_id"], y["fw_ver"], y["asset_version"], y["uptime_s"], y.get("sd_mounted"),
        zip_version(data)))
    rig.login()
    try:
        st = rig.ota_status()
        if st.get("state") not in ("idle", "error"):
            sys.exit("REFUSED: OTA state %r" % st.get("state"))
        for name in a.stages:
            say("== stage %s" % name)
            STAGES[name](rig, data)
    finally:
        rig.logout()
    bad = [r for r in RESULTS if not r[2]]
    say("== %d verdicts, %d failed" % (len(RESULTS), len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

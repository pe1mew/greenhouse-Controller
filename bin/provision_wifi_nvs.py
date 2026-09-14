#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Build an NVS image that puts WiFi credentials on a factory-fresh board.

There is a chicken-and-egg on a bare Lolin with no LCD:

  * T10 brings the AP up only when NVS `wifi/ap_enable` is 1, and the documented
    ways to set it are the web GUI and the LCD System menu;
  * the web GUI needs the network, and the LCD needs a panel.

So a new board with no credentials and no panel is unreachable. The way in is
to write the credentials straight into the NVS partition at flash time, which
needs no firmware change and no hardware beyond the USB cable.

This only generates the image and prints the commands. It does not flash: the
operator has to choose the port anyway, and on this bench that choice matters
(the CH340 hands identical COM ports to different boards -- identify by MAC
before flashing anything).

Credentials never go in argv by default, because argv is visible to other
processes and lands in shell history. The PSK is prompted for, or taken from
an environment variable. Take it from the operator's secret store.

  ** The generated .bin contains the PSK in clear text. ** It is written to a
  directory you choose -- keep it out of the repo, and delete it afterwards.

Usage:
    python bin/provision_wifi_nvs.py --ssid MyNet --out C:\\path\\to\\scratch
    PSK=... python bin/provision_wifi_nvs.py --ssid MyNet --psk-env PSK --out ...

Stdlib only. The image itself is built by Espressif's
esp-idf-nvs-partition-gen; ESP-IDF 5.5 ships only a wrapper around that
PyPI package and PlatformIO installs it nowhere, so this script finds an
interpreter that has it or builds a throwaway venv beside --out.
"""

import argparse
import csv
import getpass
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# From firmware/partitions.csv -- read, not remembered, so a partition-table
# change cannot silently leave this pointing at the wrong offset.
PARTITIONS = os.path.join(ROOT, "firmware", "partitions.csv")


def nvs_partition():
    """(offset, size) of the `nvs` partition, straight out of partitions.csv."""
    with open(PARTITIONS, "r", encoding="utf-8") as fh:
        for line in fh:
            if line.lstrip().startswith("#"):
                continue
            f = [c.strip() for c in line.split(",")]
            if len(f) >= 5 and f[0] == "nvs":
                return f[3], f[4]
    sys.exit("could not find the `nvs` row in %s" % PARTITIONS)


def _has_generator(py):
    """Can this interpreter run `-m esp_idf_nvs_partition_gen`?"""
    try:
        subprocess.check_output([py, "-c", "import esp_idf_nvs_partition_gen"],
                                stderr=subprocess.STDOUT)
        return True
    except Exception:                                          # noqa: BLE001
        return False


def generator_python(out_dir, override):
    """An interpreter that can build an NVS image.

    ESP-IDF 5.5 turned nvs_partition_gen.py into a three-line wrapper around the
    `esp-idf-nvs-partition-gen` PyPI package, and PlatformIO does not install it
    into any environment it manages. Running the IDF script directly therefore
    fails with a bare `No module named esp_idf_nvs_partition_gen`, which says
    nothing about what to do. Find an interpreter that has it, and if none does,
    build a throwaway venv beside the output -- not in the system Python and not
    in PlatformIO's, so nothing the rest of the toolchain depends on is touched.
    """
    cands = [override] if override else []
    cands += [sys.executable,
              os.path.join(os.path.expanduser("~"), ".platformio", "penv",
                           "Scripts", "python.exe"),
              os.path.join(os.path.expanduser("~"), ".platformio", "penv",
                           "bin", "python")]
    for py in cands:
        if py and os.path.exists(py) and _has_generator(py):
            return py

    venv = os.path.join(out_dir, "nvsvenv")
    vpy = os.path.join(venv, "Scripts", "python.exe")
    if not os.path.exists(vpy):
        vpy = os.path.join(venv, "bin", "python")
    if os.path.exists(vpy) and _has_generator(vpy):
        return vpy

    print("esp-idf-nvs-partition-gen is not installed in any interpreter I can "
          "see; building a throwaway venv in %s ..." % venv)
    base = sys.executable
    try:
        subprocess.check_output([base, "-m", "venv", venv], stderr=subprocess.STDOUT)
        vpy = os.path.join(venv, "Scripts", "python.exe")
        if not os.path.exists(vpy):
            vpy = os.path.join(venv, "bin", "python")
        subprocess.check_output([vpy, "-m", "pip", "install", "--quiet",
                                 "--disable-pip-version-check",
                                 "esp-idf-nvs-partition-gen"],
                                stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as exc:
        sys.stdout.write(exc.output.decode("utf-8", "replace"))
        sys.exit("could not create the venv or install the generator. Install it "
                 "yourself and pass --python:\n"
                 "  python -m pip install esp-idf-nvs-partition-gen")
    if not _has_generator(vpy):
        sys.exit("venv built but the generator still will not import")
    return vpy


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--psk-env", metavar="VAR",
                    help="read the PSK from this environment variable instead "
                         "of prompting (never pass it on the command line)")
    ap.add_argument("--ap-enable", action="store_true",
                    help="also set wifi/ap_enable=1, so the board raises its "
                         "recovery AP even if it cannot join the network")
    ap.add_argument("--python", metavar="PATH",
                    help="interpreter that has esp-idf-nvs-partition-gen "
                         "installed; auto-detected, or a venv is built, if omitted")
    ap.add_argument("--out", required=True,
                    help="directory for the generated image -- NOT inside the repo")
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out)
    if os.path.commonpath([out_dir, ROOT]) == ROOT:
        sys.exit("refusing to write a file containing a cleartext PSK inside the "
                 "repository: %s" % out_dir)
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    psk = os.environ.get(args.psk_env, "") if args.psk_env else ""
    if args.psk_env and not psk:
        sys.exit("environment variable %s is empty" % args.psk_env)
    if not psk:
        psk = getpass.getpass("WiFi PSK for '%s' (not echoed): " % args.ssid)
    if not psk:
        sys.exit("no PSK given")

    offset, size = nvs_partition()
    csv_path = os.path.join(out_dir, "wifi_nvs.csv")
    bin_path = os.path.join(out_dir, "wifi_nvs.bin")

    # Namespace and key names are what network_manager.cpp actually reads:
    #   nvs_cfg_get_str(NVS_NS_WIFI, "ssid", ...) / "psk"   (NVS_NS_WIFI = "wifi")
    rows = [["key", "type", "encoding", "value"],
            ["wifi", "namespace", "", ""],
            ["ssid", "data", "string", args.ssid],
            ["psk", "data", "string", psk]]
    if args.ap_enable:
        rows.append(["ap_enable", "data", "i32", "1"])

    with open(csv_path, "w", encoding="utf-8", newline="") as fh:
        csv.writer(fh).writerows(rows)

    gen_py = generator_python(out_dir, args.python)
    cmd = [gen_py, "-m", "esp_idf_nvs_partition_gen",
           "generate", csv_path, bin_path, size]
    try:
        subprocess.check_output(cmd, stderr=subprocess.STDOUT, cwd=out_dir)
    except subprocess.CalledProcessError as exc:
        sys.stdout.write(exc.output.decode("utf-8", "replace"))
        sys.exit("nvs_partition_gen.py failed")
    finally:
        os.remove(csv_path)          # the CSV holds the PSK too

    print("wrote %s  (%s bytes, for offset %s)"
          % (bin_path, os.path.getsize(bin_path), offset))
    print("\nThis file contains the PSK in clear text. Delete it when done.\n")
    print("Flash it, with the first-flash rules that apply to any new board:\n")
    print("  # 1. identify the board -- the CH340 gives identical COM ports to")
    print("  #    different boards, so confirm the MAC before writing anything")
    print("  esptool.py --port COMx chip_id\n")
    print("  # 2. coredump partition MUST be erased on a new unit, or IDF reads")
    print("  #    garbage there and panics on every subsequent boot")
    print("  esptool.py --port COMx erase_region 0x620000 0x10000\n")
    print("  # 3. greenfield flash uses --flash_mode dio for the bootloader header")
    print("  #    (runtime board_build.flash_mode = qio stays as it is)")
    print("  #    build first:  pio run -e lolin_s3")
    print("  cd firmware && pio run -e lolin_s3 -t upload --upload-port COMx\n")
    print("  # 4. finally the credentials, into the nvs partition")
    print("  esptool.py --port COMx write_flash %s %s" % (offset, bin_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())

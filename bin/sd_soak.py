#!/usr/bin/env python3
"""sd_soak.py -- watch, then judge, a soak of anything that touches the SD card.

WHY
---
2.16.2 put one lock in front of every SD operation (gh#90). What a lock can
break only shows over hours, next to everything else that uses the card:
T9's writes, its 1 MB rotation and per-unit retention, T14's log uploads, the
web listing and download, and the status snapshot's free-space query. A
deadlock, a stall or a failing write shows up as a gap in T9's rows, an error
row, an unmounted card or a reboot. T1 logs the heap every 60 s (SYSTEM 7, 8
and 12), so on a healthy unit no two rows are more than a minute apart.

WHAT IT JUDGES (report)
  1. no reboot: uptime only rises from sample to sample, and the window holds
     no BOOT row (SYSTEM 5);
  2. no coredump: the coredump_available flag stays clear in every sample, so
     erase any stored dump before the start;
  3. the card is mounted in every sample;
  4. T9's rows keep coming: no gap longer than --max-gap (90 s) between two
     rows in the window, the rows sorted first (some land out of order,
     gotcha 2026-09-13);
  5. no SYSTEM -1 row: value_b 0 is an SD write failure, more is rows Q3
     dropped;
  6. retention holds: at most 30 of this unit's own files on the card;
  7. the window lasted at least --hours (12).
  T14's log uploads are CHECK, not FAIL, when one failed: its failure row does
  not say whether the card or the network failed. Rotations (SYSTEM 11 rows in
  the window), the files the window spans and the heap (steady state over the
  first and the last hour, and the floor) are reported, not judged.

USAGE
-----
    python3 bin/sd_soak.py watch [HOURS] [--host H] [--out-dir D]
    python3 bin/sd_soak.py report [--host H] [--out-dir D] [--max-gap 90] [--hours 12]

watch writes, in --out-dir (default: next to this script):
    sd_watch.json   the start: unit, fw, assets, uptime, unit time, the current file
    sd_watch.csv    every minute: pc time, unit time, uptime_s, sd_mounted,
                    sd_free_mb, flags, heap free / largest / min
    sd_files.log    every hour: the current file, this unit's file count, on_card
report downloads this unit's files, from the one current at the start onward,
into <out-dir>/sd_soak/, then prints PASS / FAIL / CHECK per item. Exit 0 =
no FAIL. Refuses 5C88. Stdlib only, plus at_wp_ramp.Unit.
"""
import argparse
import csv
import http.client
import io
import json
import os
import statistics
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from at_wp_ramp import Unit                                     # noqa: E402

PIN = "12345678"
PERIOD_S = 60
LISTING_EVERY = 60          # samples between two listings (an hour)
SD_MAX_FILES = 30           # event_logger.h, per unit (gh#82)


def now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def public(host):
    with urllib.request.urlopen("http://%s/api/status" % host, timeout=10) as r:
        return json.loads(r.read().decode())


def listing(host):
    """(current, names, on_card) from /api/log/files, in an admin session that
    ends at once."""
    u = Unit(host, PIN)
    try:
        sc, d = u._req("GET", "/api/log/files")
    finally:
        u.logout()
    if sc != 200 or not isinstance(d, dict):
        raise RuntimeError("log/files HTTP %s" % sc)
    return d.get("current"), list(d.get("sd_files") or []), d.get("on_card")


def watch(a):
    s = public(a.host)
    y = s["system"]
    unit = str(y["unit_id"])
    if "5C88" in unit.upper():
        sys.exit("REFUSED: 5C88 is production")
    cur, names, on_card = listing(a.host)
    start = {"pc_time": now(), "unit_time": y.get("time_iso"), "unit": unit, "fw": y.get("fw_ver"),
             "assets": y.get("asset_version"), "uptime_s": y.get("uptime_s"), "current": cur,
             "flags": s.get("mode", {}).get("flags") or []}
    with open(os.path.join(a.out_dir, "sd_watch.json"), "w") as f:
        json.dump(start, f, indent=1)
    print(now(), "start", json.dumps(start), flush=True)
    path = os.path.join(a.out_dir, "sd_watch.csv")
    new = not os.path.exists(path)
    out = open(path, "a", newline="")
    w = csv.writer(out)
    if new:
        w.writerow(["pc_time", "unit_time", "uptime_s", "sd_mounted", "sd_free_mb", "flags",
                    "heap_free_kb", "heap_largest_kb", "heap_min_kb"])
    flog = os.path.join(a.out_dir, "sd_files.log")
    n_samples = int(a.hours * 3600 / PERIOD_S)
    for i in range(n_samples):
        t0 = time.time()
        try:
            s = public(a.host)
            y = s["system"]
            w.writerow([now(), y.get("time_iso"), y.get("uptime_s"), y.get("sd_mounted"), y.get("sd_free_mb"),
                        "|".join(s.get("mode", {}).get("flags") or []), y.get("heap_free_kb"),
                        y.get("heap_largest_kb"), y.get("heap_min_kb")])
        except Exception as e:                                  # noqa: BLE001
            w.writerow([now(), "", "", "", "", "unreachable:" + type(e).__name__, "", "", ""])
        out.flush()
        if i % LISTING_EVERY == 0:
            try:
                cur, names, on_card = listing(a.host)
                mine = [n for n in names if n.startswith(unit + "_")]
                line = "%s current %s  %s files of %s listed, on_card %s" % (now(), cur, len(mine), len(names),
                                                                         on_card)
            except Exception as e:                              # noqa: BLE001
                line = "%s listing failed (%s)" % (now(), type(e).__name__)
            with open(flog, "a") as f:
                f.write(line + "\n")
        time.sleep(max(0.0, PERIOD_S - (time.time() - t0)))
    print(now(), "sd watch finished", flush=True)


def download(host, names, dest):
    u = Unit(host, PIN)
    got = {}
    try:
        for n in names:
            c = http.client.HTTPConnection(host, 80, timeout=120)
            c.request("GET", "/api/log/download?file=" + urllib.parse.quote(n), headers={"Cookie": u.cookie})
            r = c.getresponse()
            body = r.read()
            if r.status != 200:
                raise RuntimeError("download %s: HTTP %s" % (n, r.status))
            with open(os.path.join(dest, n), "wb") as f:
                f.write(body)
            got[n] = body.decode("utf-8", "replace")
    finally:
        u.logout()
    return got


def report(a):
    results = []

    def judge(level, what, detail=""):
        results.append(level)
        print("%-5s %s%s" % (level, what, ("  [%s]" % detail) if detail else ""), flush=True)

    start = json.load(open(os.path.join(a.out_dir, "sd_watch.json")))
    unit = start["unit"]
    if "5C88" in unit.upper():
        sys.exit("REFUSED: 5C88 is production")
    s = public(a.host)
    y = s["system"]
    print("soak report -- %s fw %s assets %s, from %s (unit time) to %s" % (
        unit, y.get("fw_ver"), y.get("asset_version"), start["unit_time"], y.get("time_iso")), flush=True)

    # The samples: reboots, the coredump flag, the mount.
    samples = list(csv.DictReader(open(os.path.join(a.out_dir, "sd_watch.csv"), newline="")))
    ok = [r for r in samples if r["uptime_s"]]
    lost = len(samples) - len(ok)
    falls = [(p["pc_time"], p["uptime_s"], q["uptime_s"]) for p, q in zip(ok, ok[1:])
             if int(q["uptime_s"]) < int(p["uptime_s"])]
    judge("PASS" if not falls else "FAIL", "1. no reboot: uptime only rises",
          "%d samples, %d unreachable%s" % (len(samples), lost,
                                            "; fell at %s" % falls[:3] if falls else
                                            "; %s s -> %s s" % (ok[0]["uptime_s"], ok[-1]["uptime_s"]) if ok else ""))
    cd = [r["pc_time"] for r in ok if "coredump_available" in r["flags"]]
    judge("PASS" if not cd and "coredump_available" not in (s.get("mode", {}).get("flags") or []) else "FAIL",
          "2. no coredump", "flag seen at %s" % cd[:3] if cd else "flag clear in every sample")
    unm = [r["pc_time"] for r in ok if r["sd_mounted"] != "True"]
    judge("PASS" if not unm else "FAIL", "3. the card mounted in every sample",
          "not mounted at %s" % unm[:3] if unm else "%d samples" % len(ok))

    # The log itself.
    cur, names, on_card = listing(a.host)
    mine = sorted(n for n in names if n.startswith(unit + "_"))
    span = [n for n in mine if n >= start["current"]]
    dest = os.path.join(a.out_dir, "sd_soak")
    os.makedirs(dest, exist_ok=True)
    texts = download(a.host, span, dest)
    t_from, t_to = start["unit_time"], y.get("time_iso")
    rows = []
    for n in span:
        rows += [r for r in csv.DictReader(io.StringIO(texts[n])) if t_from <= r["timestamp"] <= t_to]
    stamps = sorted(time.mktime(time.strptime(r["timestamp"], "%Y-%m-%dT%H:%M:%S")) for r in rows)
    gaps = [(time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(p)), int(q - p))
            for p, q in zip(stamps, stamps[1:]) if q - p > a.max_gap]
    big = max((int(q - p) for p, q in zip(stamps, stamps[1:])), default=0)
    judge("PASS" if rows and not gaps else "FAIL", "4. T9's rows keep coming: no gap over %d s" % a.max_gap,
          "%d rows in %s; largest gap %d s%s" % (len(rows), ", ".join(span), big,
                                                 "; over it: %s" % gaps[:5] if gaps else ""))
    sysrows = [r for r in rows if r["type"] == "SYSTEM"]
    boots = [r["timestamp"] for r in sysrows if r["value_a"] == "5"]
    if boots:
        judge("FAIL", "1b. a BOOT row in the window", ", ".join(boots[:3]))
    neg = [(r["timestamp"], r["value_b"]) for r in sysrows if r["value_a"] == "-1"]
    judge("PASS" if not neg else "FAIL", "5. no SYSTEM -1 row (SD write failure / Q3 drop)",
          "%s" % neg[:5] if neg else "none")
    if on_card is not None and on_card > len(names):
        judge("CHECK", "6. retention", "the listing left %d of %d files out" % (on_card - len(names), on_card))
    else:
        judge("PASS" if len(mine) <= SD_MAX_FILES else "FAIL", "6. retention: <= %d of %s's own files" % (
            SD_MAX_FILES, unit), "%d of %s's, %d on the card" % (len(mine), unit, len(names)))
    hours = (time.mktime(time.strptime(t_to, "%Y-%m-%dT%H:%M:%S")) -
             time.mktime(time.strptime(t_from, "%Y-%m-%dT%H:%M:%S"))) / 3600.0
    judge("PASS" if hours >= a.hours else "FAIL", "7. elapsed %.2f h (need >= %.1f)" % (hours, a.hours))

    # Reported, not judged.
    web = [r for r in sysrows if r["initiator"] == "WEB"]
    up_ok = sum(1 for r in web if r["value_a"] == "1" and r["value_b"] == "1")
    up_bad = [r["timestamp"] for r in web if (r["value_a"] == "0" and r["value_b"] == "1")
              or (r["value_a"].isdigit() and 100 <= int(r["value_a"]) <= 599)]
    post_bad = sum(1 for r in web if r["value_a"] == "0" and r["value_b"] == "0")
    judge("CHECK" if up_bad else "INFO", "T14 log uploads: %d ok, %d failed" % (up_ok, len(up_bad)),
          ("failed at %s: card or network, read the serial log" % up_bad[:3]) if up_bad else
          "status POST failures %d" % post_bad)
    rot = [r["timestamp"] for r in sysrows if r["value_a"] == "11"]
    judge("INFO", "rotations (SYSTEM 11) in the window: %d" % len(rot), ", ".join(rot[:4]))
    if len(ok) >= 120:
        def med(rs, k):
            return statistics.median(int(r[k]) for r in rs if r[k])
        judge("INFO", "heap, first hour: %d KB free, %d KB largest; last hour: %d KB free, %d KB largest; "
              "floor now %s KB" % (med(ok[:60], "heap_free_kb"), med(ok[:60], "heap_largest_kb"),
                                   med(ok[-60:], "heap_free_kb"), med(ok[-60:], "heap_largest_kb"),
                                   ok[-1]["heap_min_kb"]))
    fails = results.count("FAIL")
    print("\n%s: %d FAIL, %d CHECK" % ("PASS" if not fails else "FAIL", fails, results.count("CHECK")))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=("watch", "report"))
    ap.add_argument("hours", nargs="?", type=float, default=None,
                    help="watch: how long (default 14); report: see --hours")
    ap.add_argument("--host", default="192.168.20.160")
    ap.add_argument("--out-dir", default=HERE)
    ap.add_argument("--max-gap", type=int, default=90)
    ap.add_argument("--hours", dest="need_h", type=float, default=12.0)
    a = ap.parse_args()
    if a.mode == "watch":
        a.hours = 14.0 if a.hours is None else a.hours
        watch(a)
        return 0
    a.hours = a.need_h
    return report(a)


if __name__ == "__main__":
    sys.exit(main())

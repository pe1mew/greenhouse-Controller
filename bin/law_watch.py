#!/usr/bin/env python3
"""Watch what the control law does through a soak: one line a minute.

WHY
---
The SD log carries T6's decisions (MODE rows, stamped with T4's cached clock,
up to a minute early) and the RAW readings, but T6 acts on T5's AVERAGES and
the log does not record those. This samples the public /api/status once a
minute and writes the averages next to the three windows, M3's mode and the
law in force (`windows.law`, 2.15.0 on), so a decision can be read in context:
a humidity-only opening against the temperature floor, say.
`bin/law_conformance.py --law-watch` pairs this file with the SD log.

It only reads the public /api/status: no session, no settings, so it cannot
hold up a ROTA apply (the quiet gate) or disturb a harness.

Written for the 2.15.0 soak (gh#84, 2026-09-27); it runs on Shuttle2 beside the
other soak jobs, under init, like heap_watch.py:

    nohup python3 -u law_watch.py 14 > law_watch.out 2>&1 < /dev/null &

USAGE
-----
    python bin/law_watch.py [hours] [--host HOST] [--out-dir DIR]

hours defaults to 14 and --host to 192.168.20.160 (2344). Writes to --out-dir
(default: next to this script):
    law_watch.csv   every sample (the columns in COLS)
    law_watch.log   one line per change of the windows, M3's mode or the law
"""
import argparse
import json
import os
import time
import urllib.request

DEFAULT_HOST = "192.168.20.160"
PERIOD_S = 60
COLS = ("iso", "uptime_s", "temp_c", "temp_avg_c", "rh_pct", "rh_avg_pct", "t_max",
        "rh_max", "rh_min", "day", "M1", "M2", "M3", "M3_pct_x10", "M3_mode", "law",
        "mode", "flags")


def now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def row(s):
    c = s.get("climate") or {}
    w = s.get("windows") or {}
    m = s.get("mode") or {}
    sy = s.get("system") or {}
    sun = s.get("sun") or {}
    return (now(), sy.get("uptime_s"), c.get("temp_c"), c.get("temp_avg_c"), c.get("rh_pct"),
            c.get("rh_avg_pct"), c.get("temp_max_active"), c.get("rh_max_active"),
            c.get("rh_min_active"), sun.get("is_daytime"), w.get("M1"), w.get("M2"),
            w.get("M3"), w.get("M3_percent_x10"), w.get("M3_ctrl_mode"), w.get("law"),
            m.get("current"), "|".join(m.get("flags") or []))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("hours", nargs="?", type=float, default=14.0)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--out-dir", default=os.path.dirname(os.path.abspath(__file__)))
    a = ap.parse_args()
    csv_path = os.path.join(a.out_dir, "law_watch.csv")
    log_path = os.path.join(a.out_dir, "law_watch.log")

    def note(line):
        with open(log_path, "a") as f:
            f.write(line + "\n")
        print(line, flush=True)

    end = time.time() + a.hours * 3600.0
    if not os.path.exists(csv_path):
        with open(csv_path, "w") as f:
            f.write(",".join(COLS) + "\n")
    note("%s  law watch started on %s for %.1f h" % (now(), a.host, a.hours))
    last = None
    while time.time() < end:
        try:
            with urllib.request.urlopen("http://%s/api/status" % a.host, timeout=10) as r:
                s = json.loads(r.read().decode())
        except Exception as e:                                 # noqa: BLE001
            note("%s  status read failed (%s)" % (now(), type(e).__name__))
            time.sleep(PERIOD_S)
            continue
        v = row(s)
        with open(csv_path, "a") as f:
            f.write(",".join("" if x is None else str(x) for x in v) + "\n")
        key = (v[10], v[11], v[12], v[14], v[15], v[16])
        if key != last:
            note("%s  up %ss  T %s (avg %s)  RH %s (avg %s)  day=%s  M1 %s M2 %s M3 %s %s  %s  law=%s  %s"
                 % (v[0], v[1], v[2], v[3], v[4], v[5], v[9], v[10], v[11], v[12], v[13] or "",
                    v[14], v[15], v[16]))
            last = key
        time.sleep(PERIOD_S)
    note("%s  law watch finished" % now())


if __name__ == "__main__":
    main()

"""
prepare_calibration_input.py

Merge SD card log files, outdoor LoRa CSV (lht65-20), and door-sensor CSVs
(LDS01) into a single flat calibration-input CSV ready for
calibrate_plant_dynamic.py.

Row backbone: SENSOR_HR_0 timestamps (~30 s cadence from the SD log).
Outdoor and door columns are forward-filled from the last known uplink.

Columns in output CSV
---------------------
timestamp        ISO 8601 local time (Europe/Amsterdam, naive)
T_in_C           Indoor temperature  (SENSOR_HR_0 ch=0, t_c10 / 10)
RH_in_pct        Indoor humidity %   (SENSOR_HR_0 ch=0)
wind_ms          Indoor wind speed   (SENSOR_HR_1 ch=1, value_a / 10)
wind_dir_deg     Indoor wind dir     (SENSOR_HR_1 ch=1, value_b)
bitmask          Window state        (SENSOR_HR_2 ch=2, forward-filled)
T_out_C          Outdoor temperature (lht65-20, forward-filled)
RH_out_pct       Outdoor humidity %  (lht65-20, forward-filled)
lux              Outdoor lux         (lht65-20, forward-filled)
lux_stale_s      Seconds since last outdoor uplink
door1_open       0/1  lds01-5 state (forward-filled; 0 if no prior uplink)
door2_open       0/1  lds01-6 state (forward-filled; 0 if no prior uplink)
calibration_valid  1 when both doors closed AND outdoor data fresh

Option A door exclusion: calibrate_plant_dynamic.py filters on
calibration_valid == 1. Pass --keep-door-rows to suppress this flag
(e.g. for Option B covariate modelling).

Usage
-----
    # 1. Export outdoor and door CSVs first:
    python model/fetch_lora_data.py --sensor lht65-20 \\
        --start 2026-06-04 --end 2026-06-26 \\
        --output model/campaign-summer-2026/lht65_20_2026-06-04_2026-06-25.csv

    python model/fetch_lora_data.py --sensor lds01-5 \\
        --start 2026-06-04 --end 2026-06-26 \\
        --output model/campaign-summer-2026/lds01_5_2026-06-04_2026-06-25.csv

    python model/fetch_lora_data.py --sensor lds01-6 \\
        --start 2026-06-04 --end 2026-06-26 \\
        --output model/campaign-summer-2026/lds01_6_2026-06-04_2026-06-25.csv

    # 2. Merge:
    python model/prepare_calibration_input.py \\
        --logs    model/campaign-summer-2026/ \\
        --outdoor model/campaign-summer-2026/lht65_20_2026-06-04_2026-06-25.csv \\
        --door1   model/campaign-summer-2026/lds01_5_2026-06-04_2026-06-25.csv \\
        --door2   model/campaign-summer-2026/lds01_6_2026-06-04_2026-06-25.csv \\
        --output  model/campaign-summer-2026/calibration_input_2026-06-04_2026-06-25.csv
"""

import argparse
import csv
import sys
from pathlib import Path
from datetime import datetime


LUX_STALE_DEFAULT_S = 1800   # 3 outdoor intervals — matches §7.3 of campaign plan


def parse_log_ts(s):
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S")


def parse_csv_dt(s):
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S"):
        try:
            return datetime.strptime(s, fmt)
        except ValueError:
            pass
    raise ValueError(f"Unrecognised datetime: {s!r}")


def load_sd_rows(log_dir):
    """Return SENSOR_HR_0/1/2 events from all *.log files in log_dir.

    Format per firmware/src/event_logger/event_logger.cpp:
      timestamp,type,initiator,ch,param,value_a,value_b
    """
    hr0, hr1, hr2 = [], [], []
    for path in sorted(Path(log_dir).glob("*.log")):
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            reader = csv.reader(f)
            try:
                hdr = next(reader)
            except StopIteration:
                continue
            if hdr[:2] != ["timestamp", "type"]:
                continue
            for row in reader:
                if len(row) != 7:
                    continue
                try:
                    dt = parse_log_ts(row[0])
                    ch, va, vb = int(row[3]), int(row[5]), int(row[6])
                except ValueError:
                    continue
                if row[1] != "SENSOR_HR":
                    continue
                if ch == 0:
                    hr0.append((dt, va, vb))
                elif ch == 1:
                    hr1.append((dt, va, vb))
                elif ch == 2:
                    hr2.append((dt, va & 0xFFFF))

    dedup = lambda lst: sorted(set(tuple(r) for r in lst))
    return dedup(hr0), dedup(hr1), dedup(hr2)


def load_outdoor_csv(path):
    """Return [(dt, T_out_C, RH_out_pct, lux), ...] from lht65-20 CSV."""
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                dt  = parse_csv_dt(r["dateTime"])
                t   = float(r["T_out_C"])   if r.get("T_out_C")    not in (None, "None", "") else None
                rh  = float(r["RH_out_pct"]) if r.get("RH_out_pct") not in (None, "None", "") else None
                lux = int(r["lux"])          if r.get("lux")         not in (None, "None", "") else None
            except (ValueError, KeyError):
                continue
            rows.append((dt, t, rh, lux))
    return sorted(rows)


def load_door_csv(path):
    """Return [(dt, door_status_0_or_1), ...] from an LDS01 CSV."""
    if path is None:
        return []
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                dt = parse_csv_dt(r["dateTime"])
                s  = int(r["doorStatus"]) if r.get("doorStatus") not in (None, "None", "") else 0
            except (ValueError, KeyError):
                continue
            rows.append((dt, s))
    return sorted(rows)


def make_ffiller(series):
    """Return a closure that forward-fills `series` at query timestamps.

    series: sorted list of (dt, *values)
    Returns (values_tuple_or_scalar, age_seconds) at query time.
    age_seconds is None if no prior reading exists.
    """
    idx = [0]
    last_vals = [None]
    last_dt   = [None]

    def ffill(target_dt):
        while idx[0] < len(series) and series[idx[0]][0] <= target_dt:
            row = series[idx[0]]
            last_dt[0]   = row[0]
            last_vals[0] = row[1] if len(row) == 2 else row[1:]
            idx[0] += 1
        if last_vals[0] is None:
            return None, None
        age = (target_dt - last_dt[0]).total_seconds()
        return last_vals[0], age

    return ffill


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--logs",    required=True, help="Directory with *.log SD card files")
    ap.add_argument("--outdoor", required=True, help="lht65-20 CSV (from fetch_lora_data.py)")
    ap.add_argument("--door1",   default=None,  help="lds01-5 CSV (harvest door 1)")
    ap.add_argument("--door2",   default=None,  help="lds01-6 CSV (harvest door 2)")
    ap.add_argument("--output",  required=True, help="Output merged CSV path")
    ap.add_argument("--lux-stale-limit", type=int, default=LUX_STALE_DEFAULT_S,
                    help=f"Max outdoor data age in seconds before row is excluded "
                         f"(default {LUX_STALE_DEFAULT_S})")
    ap.add_argument("--keep-door-rows", action="store_true",
                    help="Set calibration_valid=1 even during door-open periods "
                         "(use for Option B covariate modelling)")
    args = ap.parse_args()

    print("Loading SD log files ...")
    hr0, hr1, hr2 = load_sd_rows(args.logs)
    if not hr0:
        sys.exit("[error] No SENSOR_HR_0 rows found. Check --logs path and log filenames.")
    print(f"  SENSOR_HR_0: {len(hr0)} rows  (backbone)")
    print(f"  SENSOR_HR_1: {len(hr1)} rows  (wind)")
    print(f"  SENSOR_HR_2: {len(hr2)} rows  (bitmask)")

    print("Loading outdoor CSV ...")
    outdoor = load_outdoor_csv(args.outdoor)
    print(f"  lht65-20: {len(outdoor)} rows")

    door1_data = load_door_csv(args.door1)
    door2_data = load_door_csv(args.door2)
    print(f"Loading door CSVs ...")
    print(f"  door1 (lds01-5): {len(door1_data)} rows{'  [NOT PROVIDED]' if not args.door1 else ''}")
    print(f"  door2 (lds01-6): {len(door2_data)} rows{'  [NOT PROVIDED]' if not args.door2 else ''}")

    ffill_hr1 = make_ffiller(hr1)
    ffill_hr2 = make_ffiller(hr2)
    ffill_out  = make_ffiller(outdoor)
    ffill_d1   = make_ffiller(door1_data) if door1_data else lambda dt: (0, None)
    ffill_d2   = make_ffiller(door2_data) if door2_data else lambda dt: (0, None)

    COLS = [
        "timestamp",
        "T_in_C", "RH_in_pct",
        "wind_ms", "wind_dir_deg",
        "bitmask",
        "T_out_C", "RH_out_pct", "lux", "lux_stale_s",
        "door1_open", "door2_open",
        "calibration_valid",
    ]

    n_total = n_valid = n_door_excl = n_stale_excl = 0

    with open(args.output, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(COLS)

        for dt, t_c10, rh_pct in hr0:
            n_total += 1

            T_in  = round(t_c10 / 10.0, 2)
            RH_in = rh_pct

            hr1_val, _ = ffill_hr1(dt)
            if hr1_val is not None:
                wind_ms      = round(hr1_val[0] / 10.0, 2)
                wind_dir_deg = hr1_val[1]
            else:
                wind_ms = wind_dir_deg = ""

            hr2_val, _ = ffill_hr2(dt)
            bitmask = hr2_val if hr2_val is not None else ""

            out_val, lux_age_s = ffill_out(dt)
            outdoor_ok = out_val is not None and (lux_age_s is not None) and lux_age_s <= args.lux_stale_limit
            if outdoor_ok:
                T_out, RH_out, lux = out_val
                lux_stale_s = round(lux_age_s)
            else:
                T_out = RH_out = lux = lux_stale_s = ""

            d1_raw, d1_age = ffill_d1(dt)
            d2_raw, d2_age = ffill_d2(dt)
            door1_open = int(d1_raw) if d1_raw is not None else 0
            door2_open = int(d2_raw) if d2_raw is not None else 0
            doors_open = bool(door1_open) or bool(door2_open)

            if not outdoor_ok:
                calibration_valid = 0
                n_stale_excl += 1
            elif doors_open and not args.keep_door_rows:
                calibration_valid = 0
                n_door_excl += 1
            else:
                calibration_valid = 1
                n_valid += 1

            w.writerow([
                dt.strftime("%Y-%m-%dT%H:%M:%S"),
                T_in, RH_in,
                wind_ms, wind_dir_deg,
                bitmask,
                T_out if outdoor_ok else "",
                RH_out if outdoor_ok else "",
                lux if outdoor_ok else "",
                lux_stale_s,
                door1_open, door2_open,
                calibration_valid,
            ])

    pct = lambda n: f"{n*100//n_total}%" if n_total else "n/a"
    print(f"\nWrote {n_total} rows -> {args.output}")
    print(f"  calibration_valid=1 : {n_valid:6d}  ({pct(n_valid)})")
    print(f"  excluded door open  : {n_door_excl:6d}  ({pct(n_door_excl)})")
    print(f"  excluded stale out  : {n_stale_excl:6d}  ({pct(n_stale_excl)})")


if __name__ == "__main__":
    main()

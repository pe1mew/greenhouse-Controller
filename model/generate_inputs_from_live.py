#!/usr/bin/env python3
"""
Generate input_S1..S5 weather CSVs from live sensor data in srcData/.

The five scenarios are 24-hour slices of the outdoor sensor (lht65-20),
chosen from the LHT65-02 / LHT65-03 / lht65-20 archive
(2026-03-17 .. 2026-05-07). Each slice is selected to exercise a specific
control situation, using outdoor T, outdoor RH, and outdoor lumosity
(sun detector) plus the indoor sensors (LHT65-02/-03) to confirm the
greenhouse experienced the expected behaviour during the chosen day.

Usage:
    python generate_inputs_from_live.py

Outputs five input_S*.csv files in this directory, in simulation.py
"Format A" (dateTime, airTemperature, airHumidity).
"""

import csv
from datetime import datetime, time
from pathlib import Path
from typing import List, Tuple, Optional

ROOT = Path(__file__).parent
SRC  = ROOT / "srcData"

OUTDOOR = SRC / "greenhouseClimate-lht65-20_2026-03-17_to_2026-05-07.csv"

# Scenario -> (filename, picked-day, rationale shown in console).
# Day picks come from the per-day survey of the source archive.
SCENARIOS: List[Tuple[str, str, str]] = [
    ("input_S1_Daytime_Solar_Gain.csv",
     "2026-04-29",
     "Clean sunny spring day, T_out 3.9->19.3 C, lux peak 33548. "
     "Strong solar drive, low RH (avg 45%) -> exercises step ladder + RH<rh_min."),
    ("input_S2_High_Humidity_Mild_Day.csv",
     "2026-05-03",
     "Mild humid day, T_out 11.8->20.3 C, RH_out avg 89.6%, lux peak 29925. "
     "Tests RH-driven ventilation at moderate T."),
    ("input_S3_Full_24h_Day-Night_Cycle.csv",
     "2026-05-01",
     "Wide day-night swing, T_out 2.4->25.1 C (22.7 C range), lux peak 34227. "
     "Exercises the full diurnal cycle including night close-down."),
    ("input_S4_T_Below_Setpoint_RH_Critical.csv",
     "2026-04-02",
     "Cold saturated day, T_out 0.6->11.2 C, RH_out avg 84%, indoor RH 94.8% (LHT65-03). "
     "Tests the T<T_min AND RH>RH_critical override path."),
    ("input_S5_Motor_Stall_M2.csv",
     "2026-05-02",
     "Hottest archive day, T_out 3.9->25.2 C, indoor T peak 39.1 C (LHT65-03). "
     "High thermal stress -- baseline for motor-stall stress tests."),
]


def load_outdoor() -> List[Tuple[datetime, float, float, Optional[float]]]:
    rows = []
    with open(OUTDOOR, newline="") as f:
        for r in csv.DictReader(f):
            try:
                dt = datetime.strptime(r["dateTime"], "%Y-%m-%d %H:%M:%S")
                t  = float(r["airTemperature"])
                h  = float(r["airHumidity"])
            except (KeyError, ValueError):
                continue
            lx_raw = r.get("lumosity", "")
            lx = float(lx_raw) if lx_raw not in (None, "", "NULL") else None
            rows.append((dt, t, h, lx))
    rows.sort(key=lambda x: x[0])
    return rows


def slice_day(rows, day_str: str):
    day = datetime.strptime(day_str, "%Y-%m-%d").date()
    start = datetime.combine(day, time(0, 0, 0))
    end   = datetime.combine(day, time(23, 59, 59))
    return [r for r in rows if start <= r[0] <= end]


def write_input(path: Path, slice_rows) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["dateTime", "airTemperature", "airHumidity"])
        for dt, t, h, _lx in slice_rows:
            w.writerow([dt.strftime("%Y-%m-%d %H:%M:%S"),
                        f"{t:.2f}", f"{h:.1f}"])


def main() -> None:
    rows = load_outdoor()
    print(f"Loaded {len(rows)} outdoor rows from {OUTDOOR.name}")
    for fname, day, note in SCENARIOS:
        sl = slice_day(rows, day)
        if not sl:
            print(f"  ! {fname}: no data for {day}")
            continue
        out = ROOT / fname
        write_input(out, sl)
        ts = [r[1] for r in sl]
        hs = [r[2] for r in sl]
        ls = [r[3] for r in sl if r[3] is not None]
        print(f"  -> {fname}  ({day}, {len(sl)} rows; "
              f"T {min(ts):.1f}..{max(ts):.1f} C, RH {min(hs):.0f}..{max(hs):.0f}%, "
              f"lux peak {max(ls) if ls else 0:.0f})")
        print(f"     {note}")


if __name__ == "__main__":
    main()

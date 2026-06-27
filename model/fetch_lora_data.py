"""
Fetch LoRa sensor data from the Wenumseveld MySQL database and export to CSV.

Usage
-----
    python model/fetch_lora_data.py [OPTIONS]

Options
-------
    --host      DB host          (default: 192.168.20.232)
    --user      DB user          (default: wenumseveld)
    --password  DB password      (default: wenumseveld)
    --database  DB name          (default: wenumseveld)
    --table     DB table         (default: wenumseveld)
    --sensor    Sensor name      (required, e.g. lht65-20; use --list to see all)
    --start     Start date/time  (required, ISO: YYYY-MM-DD or YYYY-MM-DD HH:MM:SS)
    --end       End date/time    (required, exclusive upper bound)
    --output    Output CSV path  (default: <sensor>_<start>_<end>.csv)
    --list      List available sensors with their date ranges and exit

All datetime values are in the database's local time (Europe/Amsterdam).

Examples
--------
    # List sensors
    python model/fetch_lora_data.py --list

    # Outdoor sensor for summer-2026 campaign
    python model/fetch_lora_data.py \\
        --sensor lht65-20 --start 2026-06-04 --end 2026-06-26 \\
        --output model/campaign-summer-2026/lht65_20_2026-06-04_2026-06-25.csv

Data-source note
----------------
The authoritative measurement source for the thermal campaign is the SD card
log files (model/campaign-summer-2026/*.log).  The only MySQL source used
for the model is lht65-20 (outdoor T, RH, lux).
"""

import argparse
import csv
import sys

try:
    import mysql.connector
except ImportError:
    sys.exit("mysql-connector-python not installed. Run: pip install mysql-connector-python")


DB_DEFAULTS = {
    "host":     "192.168.20.232",
    "user":     "wenumseveld",
    "password": "wenumseveld",
    "database": "wenumseveld",
}
TABLE_DEFAULT = "wenumseveld"

# Columns to skip (internal DB artefacts not useful for analysis)
SKIP_COLUMNS = {"id"}

# Friendly column renames per sensor context
RENAMES = {
    "lht65-20": {
        "airTemperature": "T_out_C",
        "airHumidity":    "RH_out_pct",
        "lumosity":       "lux",
        "sensorBattery":  "battery_V",
        "sensorLatitude": "lat",
        "sensorLongitude":"lon",
    },

}


def connect(args):
    return mysql.connector.connect(
        host=args.host, user=args.user, password=args.password,
        database=args.database, connect_timeout=15,
    )


def list_sensors(cn, table):
    cur = cn.cursor()
    cur.execute(f"""
        SELECT sensor, COUNT(*) AS n, MIN(dateTime), MAX(dateTime)
        FROM `{table}`
        GROUP BY sensor
        ORDER BY sensor
    """)
    print(f"{'Sensor':<25}  {'Rows':>7}  {'From':<20}  {'To':<20}")
    print("-" * 75)
    for row in cur.fetchall():
        print(f"{row[0]:<25}  {row[1]:>7}  {str(row[2]):<20}  {str(row[3]):<20}")
    cur.close()


def fetch(cn, table, sensor, start, end):
    cur = cn.cursor()
    cur.execute(f"""
        SELECT * FROM `{table}`
        WHERE sensor = %s
          AND dateTime >= %s
          AND dateTime <  %s
        ORDER BY dateTime
    """, (sensor, start, end))
    cols = [d[0] for d in cur.description]
    rows = cur.fetchall()
    cur.close()
    return cols, rows


def write_csv(path, cols, rows, sensor, drop_all_null=True):
    renames = RENAMES.get(sensor, {})

    # Build candidate column list (skip internal IDs)
    candidates = [(i, renames.get(c, c)) for i, c in enumerate(cols) if c not in SKIP_COLUMNS]

    # Detect all-null columns
    if drop_all_null and rows:
        n_rows = len(rows)
        all_null_indices = {i for i, _ in candidates if all(row[i] is None for row in rows)}
        candidates = [(i, c) for i, c in candidates if i not in all_null_indices]
        dropped_null = [renames.get(cols[i], cols[i]) for i in all_null_indices]
    else:
        dropped_null = []

    col_indices = [i for i, _ in candidates]
    out_cols    = [c for _, c in candidates]

    n_null_cols = {}
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(out_cols)
        for row in rows:
            out_row = [row[i] for i in col_indices]
            for j, v in enumerate(out_row):
                if v is None:
                    n_null_cols[out_cols[j]] = n_null_cols.get(out_cols[j], 0) + 1
            writer.writerow(out_row)

    return out_cols, n_null_cols, dropped_null


def default_output(sensor, start, end):
    s = start[:10].replace("-", "")
    e_dt = end[:10]
    # end is exclusive; subtract 1 day for the display name
    from datetime import datetime, timedelta
    e = (datetime.strptime(e_dt, "%Y-%m-%d") - timedelta(days=1)).strftime("%Y-%m-%d")
    name = sensor.lower().replace("-", "_").replace(" ", "_")
    return f"{name}_{start[:10]}_{e}.csv"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host",     default=DB_DEFAULTS["host"])
    ap.add_argument("--user",     default=DB_DEFAULTS["user"])
    ap.add_argument("--password", default=DB_DEFAULTS["password"])
    ap.add_argument("--database", default=DB_DEFAULTS["database"])
    ap.add_argument("--table",    default=TABLE_DEFAULT)
    ap.add_argument("--sensor")
    ap.add_argument("--start")
    ap.add_argument("--end")
    ap.add_argument("--output")
    ap.add_argument("--list", action="store_true", help="List sensors and exit")
    ap.add_argument("--keep-null-cols", action="store_true",
                    help="Keep always-null columns (dropped by default)")
    args = ap.parse_args()

    cn = connect(args)

    if args.list:
        list_sensors(cn, args.table)
        cn.close()
        return

    if not args.sensor:
        ap.error("--sensor is required (use --list to see available sensors)")
    if not args.start or not args.end:
        ap.error("--start and --end are required")

    # Normalise dates: bare YYYY-MM-DD -> YYYY-MM-DD 00:00:00
    start = args.start if " " in args.start or "T" in args.start else args.start + " 00:00:00"
    end   = args.end   if " " in args.end   or "T" in args.end   else args.end   + " 00:00:00"

    out_path = args.output or default_output(args.sensor, start, end)

    print(f"Fetching  sensor={args.sensor!r}  {start} .. {end} (exclusive)")
    cols, rows = fetch(cn, args.table, args.sensor, start, end)
    cn.close()

    if not rows:
        print("No rows returned for this sensor / date range.")
        return

    drop = not args.keep_null_cols
    out_cols, n_null, dropped_null = write_csv(out_path, cols, rows, args.sensor, drop_all_null=drop)
    print(f"Wrote     {len(rows)} rows -> {out_path}")
    print(f"Columns:  {', '.join(out_cols)}")
    if dropped_null:
        print(f"Dropped (all-NULL for this sensor): {', '.join(dropped_null)}")
    if n_null:
        partial = {c: n for c, n in n_null.items() if n < len(rows)}
        if partial:
            print(f"Partial nulls: " + ", ".join(f"{c}={n}" for c, n in partial.items()))


if __name__ == "__main__":
    main()

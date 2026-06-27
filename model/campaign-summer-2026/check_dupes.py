"""Check log files for duplicate / overlapping time ranges."""
import csv
from datetime import datetime
from pathlib import Path

LOG_DIR = Path(__file__).parent

def parse_ts(s):
    try:
        return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S")
    except ValueError:
        return None

files = sorted(LOG_DIR.glob("*.log"))
ranges = []  # (filename, first_ts, last_ts, row_count)

for f in files:
    first = last = None
    count = 0
    with open(f, newline="") as fh:
        for row in csv.reader(fh):
            if len(row) < 2 or row[0] == "timestamp":
                continue
            ts = parse_ts(row[0])
            if ts is None:
                continue
            if first is None:
                first = ts
            last = ts
            count += 1
    if first:
        ranges.append((f.name, first, last, count))

# Print ranges
print(f"{'File':<30} {'First':<20} {'Last':<20} {'Rows':>6}")
print("-" * 82)
for name, first, last, count in ranges:
    print(f"{name:<30} {str(first):<20} {str(last):<20} {count:>6}")

# Detect REAL overlaps — touching at one boundary point is normal log rotation
# Overlap requires file B's start to be STRICTLY BEFORE file A's end (and vice versa)
print()
print("=== Real overlap check (excludes touch-at-boundary) ===")
found = False
for i, (na, fa, la, ca) in enumerate(ranges):
    for j, (nb, fb, lb, cb) in enumerate(ranges):
        if j <= i:
            continue
        # Real overlap: intervals share more than a single point
        if fa < lb and fb < la:
            # compute overlap span
            ov_start = max(fa, fb)
            ov_end   = min(la, lb)
            if ov_start == ov_end:
                continue  # single-point touch — normal rotation boundary
            print(f"  DUPLICATE DATA: {na}")
            print(f"             AND: {nb}")
            print(f"    Overlap span: {ov_start} -- {ov_end}")
            found = True
if not found:
    print("  None found — all ranges are sequential (no data duplication).")

# Identify chains: group files into sequential chains
print()
print("=== Chain analysis ===")
# Sort by first_ts
sorted_r = sorted(ranges, key=lambda x: x[1])
chains = []
for name, first, last, count in sorted_r:
    placed = False
    for chain in chains:
        prev_name, prev_first, prev_last, prev_count = chain[-1]
        # This file continues the chain if it starts at or just after prev_last
        # Allow a few seconds of tolerance for boundary timestamp sharing
        gap = (first - prev_last).total_seconds()
        if -60 <= gap <= 3600:  # starts within 1h after (or at) last file's end
            chain.append((name, first, last, count))
            placed = True
            break
    if not placed:
        chains.append([(name, first, last, count)])

for i, chain in enumerate(chains, 1):
    cstart = chain[0][1]
    cend   = chain[-1][2]
    print(f"Chain {i}: {len(chain)} file(s)  {cstart} -- {cend}")
    for name, first, last, count in chain:
        print(f"    {name}  ({first} → {last}, {count} rows)")

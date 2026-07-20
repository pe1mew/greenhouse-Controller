#!/usr/bin/env python3
"""heap_soak.py — FR-BK04 / FR-BK07 soak evidence from SD event logs.

Answers the question the FRS defers to soak data:

    FRS 5.15: "If the long-running soak test confirms that the simplified stack
    is sufficient on its own, these requirements [FR-BK03 circuit breaker,
    FR-BK04 supervisor task, FR-BK06 breaker surfacing] may be withdrawn from
    the specification at the next revision."

and runs the fragmentation test FR-BK07 was written to make possible:

    FR-BK07: "The largest-contiguous-block metric shall be sampled at the same
    cadence as total-free so that heap fragmentation (largest block falling
    while total free remains stable) is observable from the log alone."

Reads the raw SD CSV directly (no logparser run needed). Relevant rows are
LOG_SYSTEM with:

    value_a=7   value_b = internal free heap, KB
    value_a=8   value_b = PSRAM free heap, KB
    value_a=12  value_b = largest contiguous internal block, KB
    value_a=5   value_b = esp_reset_reason  -> segment boundary

WHY IT SEGMENTS ON BOOTS
------------------------
A firmware change shifts the heap baseline permanently: adding T16 (ROTA) with
its 16 KB stack dropped 5C88's internal free heap by ~18 KB across the
2.1.3 -> 2.2.14 update on 2026-07-17. Averaged over the whole window that reads
as a "leak" of ~11 KB; segmented per boot it is visibly a step followed by a
flat plateau. **A leak is a slope within one segment, not a step between
segments** — so always read the per-segment slope, never the global drift.

USAGE
    python log/heap_soak.py <logfile> [<logfile> ...]
    python log/heap_soak.py model/campaign-summer-2026/*.log

Output is ASCII-only (Windows console is cp1252 — see memory/gotcha-log.md).
"""

import csv
import sys
from collections import OrderedDict

VA_FREE_INT = 7
VA_FREE_PSRAM = 8
VA_LARGEST_BLOCK = 12
VA_BOOT = 5

# A per-segment slope steeper than this is worth a human look. The gh#24
# accumulator tripped at 64 KB; a genuine leak large enough to matter would
# show as sustained downward movement across a multi-day segment.
LEAK_SLOPE_KB_PER_DAY = 2.0
# Minimum samples before a segment's slope is worth quoting at all.
MIN_SEGMENT_SAMPLES = 120


def load(paths):
    """Return (samples, boots). samples = [(ts, va, kb)], boots = [ts]."""
    samples, boots = [], []
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
                for row in csv.DictReader(fh):
                    if row.get("type") != "SYSTEM":
                        continue
                    try:
                        va = int(row["value_a"])
                        vb = int(row["value_b"])
                    except (TypeError, ValueError, KeyError):
                        continue
                    ts = row.get("timestamp", "")
                    if va == VA_BOOT:
                        boots.append(ts)
                    elif va in (VA_FREE_INT, VA_FREE_PSRAM, VA_LARGEST_BLOCK):
                        samples.append((ts, va, vb))
        except OSError as exc:
            print("  SKIP %s (%s)" % (path, exc))
    samples.sort(key=lambda r: r[0])
    boots.sort()
    return samples, boots


def segment(samples, boots):
    """Split samples into [(boot_ts_or_None, [(ts, va, kb)])] at boot markers."""
    if not boots:
        return [(None, samples)]
    segs, idx = [], 0
    bounds = [None] + boots
    for i, start in enumerate(bounds):
        end = bounds[i + 1] if i + 1 < len(bounds) else None
        chunk = []
        while idx < len(samples) and (end is None or samples[idx][0] < end):
            chunk.append(samples[idx])
            idx += 1
        if chunk:
            segs.append((start, chunk))
    return segs


def day_of(ts):
    return ts[:10]


def slope_kb_per_day(series):
    """Least-squares slope over daily means. Robust enough for a trend check."""
    byday = OrderedDict()
    for ts, kb in series:
        byday.setdefault(day_of(ts), []).append(kb)
    pts = [(i, sum(v) / len(v)) for i, (_d, v) in enumerate(byday.items())]
    n = len(pts)
    if n < 2:
        return None
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    denom = sum((p[0] - mx) ** 2 for p in pts)
    if denom == 0:
        return None
    return sum((p[0] - mx) * (p[1] - my) for p in pts) / denom


def describe(label, series, unit="KB"):
    if not series:
        print("      %-24s no samples" % label)
        return None
    vals = [v for _t, v in series]
    sl = slope_kb_per_day(series)
    sl_txt = "n/a (needs 2+ days)" if sl is None else "%+.2f %s/day" % (sl, unit)
    print("      %-24s n=%-5d min=%-5d max=%-5d mean=%7.1f  slope=%s"
          % (label, len(vals), min(vals), max(vals), sum(vals) / len(vals), sl_txt))
    return sl


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    samples, boots = load(argv[1:])
    if not samples:
        print("No heap samples found. Are these raw SD CSV logs "
              "(timestamp,type,initiator,ch,param,value_a,value_b)?")
        return 1

    print("=== heap soak: %d samples, %s .. %s, %d boot(s) ==="
          % (len(samples), samples[0][0], samples[-1][0], len(boots)))
    for b in boots:
        print("  boot marker: %s" % b)

    segs = segment(samples, boots)
    print()
    verdicts = []
    for i, (boot_ts, chunk) in enumerate(segs, 1):
        free = [(t, v) for t, va, v in chunk if va == VA_FREE_INT]
        blok = [(t, v) for t, va, v in chunk if va == VA_LARGEST_BLOCK]
        psram = [(t, v) for t, va, v in chunk if va == VA_FREE_PSRAM]
        span = "%s .. %s" % (chunk[0][0], chunk[-1][0])
        print("  segment %d  (from %s)  %s" % (i, boot_ts or "start of data", span))
        s_free = describe("internal free", free)
        describe("largest contig block", blok)
        describe("PSRAM free", psram)
        if s_free is not None and len(free) >= MIN_SEGMENT_SAMPLES:
            verdicts.append((i, s_free, len(free)))
        print()

    print("=== verdict ===")
    if not verdicts:
        print("  INSUFFICIENT DATA — no segment has >= %d internal-free samples "
              "spanning 2+ days. Collect a longer soak before citing this as "
              "FR-BK04 withdrawal evidence." % MIN_SEGMENT_SAMPLES)
        return 0
    leaky = [(i, s) for i, s, _n in verdicts if s <= -LEAK_SLOPE_KB_PER_DAY]
    if leaky:
        for i, s in leaky:
            print("  SEGMENT %d SLOPES DOWN: %+.2f KB/day — investigate before "
                  "withdrawing FR-BK04." % (i, s))
    else:
        print("  No segment slopes below %.1f KB/day; within-segment heap is flat."
              % -LEAK_SLOPE_KB_PER_DAY)
    print()
    print("  CAVEAT — read this before quoting the result. gh#24's failure mode was")
    print("  a per-call transient that left AGGREGATE heap looking healthy (122-126 KB,")
    print("  stable) while T14's per-call accumulator climbed to the 64 KB reboot")
    print("  threshold. This script measures aggregate heap only, so a flat result")
    print("  is consistent with the transient still existing. What it does show is")
    print("  that the consequence FR-BK04 guards against -- real exhaustion or")
    print("  fragmentation -- is not occurring. See gh#27 (closed) and gh#44.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

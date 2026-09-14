#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Check that every gotcha-log entry has an index line, and vice versa.

memory/gotcha-log.md says of its own index:

    **Adding an entry means adding its index line too**; the pair is checked
    by counting `^## 20` headings against `^- \\*\\*20` index lines.

Nothing was checking it. When this script was written the file held **86
entries and 57 index lines** -- 29 entries reachable only by scrolling, in a
file whose header says the index is the fast path and "54 entries is too many
to scan". A claim in prose that nothing enforces is the same shape as
`event_logger.h` documenting `value_a` 0..21 while 22/23/24 had shipped
(gh#59), and as `cfg_limits.h` asserting agreement with T5 in a comment while
publishing a wider range (gh#57 part 2). This is the check the file already
claims to have.

It matches on DATE, not on title: the index hook is deliberately the symptom
rather than the heading, so the text will not match and must not be compared.
A date with entries but no index line is the failure that matters; the reverse
(an index line for a date with no entry) means the entry was archived or the
line is stale.

Usage:
    python bin/check_gotcha_index.py          # quiet unless something is wrong
    python bin/check_gotcha_index.py -v       # list every date and its counts

Exit 0 = paired, 1 = a gap, 2 = the file could not be read.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import argparse
import collections
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOG = os.path.join(ROOT, "memory", "gotcha-log.md")

ENTRY_RE = re.compile(r"^## (20\d\d-\d\d-\d\d)\b", re.M)
INDEX_RE = re.compile(r"^- \*\*(20\d\d-\d\d-\d\d)\*\*", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    try:
        with io.open(LOG, "r", encoding="utf-8") as fh:
            text = fh.read()
    except Exception as exc:                                   # noqa: BLE001
        sys.stderr.write("cannot read %s: %s\n" % (LOG, exc))
        return 2

    entries = collections.Counter(ENTRY_RE.findall(text))
    index = collections.Counter(INDEX_RE.findall(text))
    if not entries:
        sys.stderr.write("no entries parsed from %s -- has the heading format "
                         "changed? Refusing to report a pass.\n" % LOG)
        return 2

    missing = {d: n for d, n in entries.items() if index.get(d, 0) < n}
    orphan = {d: n for d, n in index.items() if entries.get(d, 0) == 0}

    if args.verbose:
        for d in sorted(set(entries) | set(index), reverse=True):
            print("  %s  entries=%d  index=%d" % (d, entries.get(d, 0), index.get(d, 0)))
        print("")

    if missing:
        print("UNINDEXED -- %d date(s) have more entries than index lines:"
              % len(missing))
        for d in sorted(missing, reverse=True):
            print("  %s  %d entr%s, %d index line(s)"
                  % (d, entries[d], "y" if entries[d] == 1 else "ies", index.get(d, 0)))
        print("\nThe index is the file's own fast path. An entry that is not in "
              "it is found only by scrolling, which is what the index exists to "
              "avoid.")
    if orphan:
        print("\nSTALE -- index line(s) for a date with no entry (archived?):")
        for d in sorted(orphan, reverse=True):
            print("  %s  %d index line(s)" % (d, orphan[d]))

    if missing or orphan:
        return 1

    print("gotcha log OK: %d entries across %d dates, every date indexed"
          % (sum(entries.values()), len(entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

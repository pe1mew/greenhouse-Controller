#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Resolve a markdown rebase conflict where each side edited DIFFERENT lines.

CLAUDE.md names rebasing `ropeSensor` onto `main` as the integration path, so
this conflict recurs every time. Its shape is always the same and never
genuinely contested: both branches edit rows of the same markdown table, with
no blank line between them, so git cannot separate them and marks the whole
block. Resolved by hand it is tedious and easy to botch -- dropping one row of
a 40-row table is invisible in review.

**The rule.** For each line, if exactly one side changed it, take that side. If
a line was changed by BOTH, or the three stages disagree on line count, REFUSE
and leave the conflict in place. A script that guessed there would be worse
than no script: that is the case a human has to look at.

Reads git's own index stages (1 = merge base, 2 = ours / rebased-so-far,
3 = theirs / the commit being applied), so it needs no knowledge of which
commit is in flight and works identically for rebase, cherry-pick and merge.

Usage, at each stop:
    python bin/resolve_md_rebase.py && git add CLAUDE.md && git rebase --continue
    python bin/resolve_md_rebase.py memory/gotcha-log.md     # any other file

Exit 0 = resolved and written, 1 = refused (nothing written), 2 = not conflicted.
Stdlib only. ASCII output only (Windows consoles here are cp1252).
"""

import io
import subprocess
import sys


def stage(n, path):
    # Swallow git's own stderr: outside a conflict it prints a "did you mean
    # :0:" hint that buries this script's own, clearer message.
    return subprocess.check_output(
        ["git", "show", ":%d:%s" % (n, path)],
        stderr=subprocess.DEVNULL).decode("utf-8").split("\n")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "CLAUDE.md"

    try:
        base, ours, theirs = (stage(n, path) for n in (1, 2, 3))
    except subprocess.CalledProcessError:
        sys.stderr.write("%s is not conflicted (no index stages)\n" % path)
        return 2

    if not (len(base) == len(ours) == len(theirs)):
        sys.stderr.write(
            "refusing: line counts differ (base=%d ours=%d theirs=%d). The "
            "per-line rule only holds when both sides edited lines in place; "
            "an insertion or deletion needs a human.\n"
            % (len(base), len(ours), len(theirs)))
        return 1

    out, contested, took_theirs, kept_ours = [], [], 0, 0
    for i, (b, o, t) in enumerate(zip(base, ours, theirs), 1):
        if o == t:
            out.append(o)
        elif b == o:                      # only the incoming commit changed it
            out.append(t)
            took_theirs += 1
        elif b == t:                      # only the rebased-so-far side changed it
            out.append(o)
            kept_ours += 1
        else:
            contested.append(i)
            out.append(o)

    if contested:
        sys.stderr.write(
            "refusing: line(s) changed on BOTH sides: %s\n"
            "Resolve those by hand; nothing has been written.\n"
            % ", ".join(str(c) for c in contested))
        return 1

    with io.open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write("\n".join(out))
    print("resolved %s: %d line(s) from the incoming commit, %d kept from the "
          "rebased branch, 0 contested" % (path, took_theirs, kept_ours))
    return 0


if __name__ == "__main__":
    sys.exit(main())

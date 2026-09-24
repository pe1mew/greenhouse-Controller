# Release 2.12.2

**Date:** 2026-09-24
**Built on:** 2.12.1
**Fixes:** [gh#82](https://github.com/pe1mew/greenhouse-Controller/issues/82) — the SD log listing hid the newest files, and retention had stopped

**Patch.** One root cause, three symptoms, all in the logging path. No config key, no NVS change, no
log-encoding change, nothing in the ventilation path.

> **Nothing in this release has run on a greenhouse.** Production (5C88) is on 2.3.1 and unaffected.

## What changed

### Every decision about log files now comes from a scan that cannot truncate

**The root cause.** `storage_sd_list_csv()` fills the caller's buffer, **drops whatever does not fit, and
returns `STORAGE_OK` anyway**. FAT lists roughly in creation order, so the names it drops are the **newest**.
Every decision T9 made about files came from such a list, sized for 30 names:

| Decision | What it did past ~30 files on the card |
|---|---|
| **Retention** | the count saturated at exactly `SD_MAX_FILES`, so `count > SD_MAX_FILES` was **never true** — nothing was ever deleted again, and the card grew without bound |
| **The listing** (`/api/log/files`) | returned the **oldest** 30 names; the file the unit was writing could not be seen or downloaded |
| **The boot resume** | took the largest name from the same partial view, so it could append today's rows to a file named weeks ago — or, on a shared card, to the other module's file |
| **T14's upload enumerators** | chose the "newest closed" and "next pending" from the same partial view; sizing them to 30 names (gh#42) only moved the cliff from ~21 files to ~30 |

**The fix.** A new driver call, `storage_sd_foreach_csv()` — one callback per matching file, constant memory,
no buffer to overflow — and every caller above now **aggregates** what it needs during that pass: the count,
the oldest, the newest, or a bounded newest-N. The list-based helpers are deleted rather than left available,
because a second, truncating path is the defect.

### Ordering, retention and the listing follow the unit prefix

Log files are named `<unit>_YYYYMMDDHHMMSS.csv`, and one card can hold more than one controller's history —
the dev rig's modules are swappable and the card stays with the rig. **Sorting is on unit first, then
date-time** (operator decision, 2026-09-24), from which the rest follows:

- **Retention is per unit.** Each unit keeps up to 30 of *its own* files and deletes only its own oldest. A
  module never evicts another module's history. On a production card — one unit — this is the old rule
  exactly. FR-LG06 carries the clarification; the TSDS carries the detail.
- **The resume takes this unit's newest file**, not the largest name on the card.
- **The listing is grouped, newest first** — this unit's files, then any other module's — and it now reports
  **which file is being written**: `{"current": "<name>", "sd_files": [...]}`. The GUI marks that entry
  "(current)" and selects it, because "which file is the unit writing to?" was the question that started
  gh#82 and nothing answered it. The listing is bounded at the newest 64, so a card that outgrows it loses
  its **oldest** names from the list, never its newest.

The web GUI needed one change (the marker); the mock server returns the same shape so GUI work stays honest.

## Verification

| Check | Status |
|---|---|
| `pio test -e native` in `drivers/sdCard` | **14/14 pass**, including two new cases: with **40 files** on the card the iterator sees all 40 while `storage_sd_list_csv()` with the logger's own 871-byte buffer sees fewer **and still returns OK** — the defect, demonstrated; plus extension filtering and the refusals |
| Release image builds | yes — RAM 19.9 %, flash 67.2 % |
| Bench image builds | yes |
| On the rig: the listing shows the active file, newest first | **PASS** — 2344, 2026-09-24: newest first throughout, `current` always this unit's newest and tracking each rotation. The card held **113** files against a cap of 30, which the reply now states (`on_card`) instead of implying by length. Once retention brought the card to 38 files, **all 38 were listed — 30 of this unit's and the other module's 8** — the steady state the grouped listing is for |
| On the rig: retention deletes this unit's oldest past the cap | **PASS** — 2344, 2026-09-24, on a bench build with `-DSD_ROTATE_TEST_BYTES=2048`: **113 files → 38 in 90 minutes**, four per rotation, settling at **30 of this unit's own plus the other module's 8, untouched**. The last step was −3, the trim stopping at the cap rather than past it. Before the fix the count had been stuck above the cap for weeks |
| T14's upload path, against the live status server | **PASS** — 2344, 2026-09-24. The rig's persisted watermark was **`FDA4_20260916192045.csv`**, another module's file: the old enumerator took the lexicographic maximum across every unit, and `FDA4_` sorts above `2344_`. With the enumerators restricted to this unit's files, a NAME comparison would then have found nothing newer for ever — every `2344_*` name sorts below it — so the backlog would have dead-ended silently. **The watermark is a point in time, so it compares the 14-digit timestamps.** Measured over 20 minutes: the watermark advanced chronologically (17:53 → 18:50 → 19:18 → 19:22 → 19:26), every upload reported OK, and the status server went from **113 files to 132** |
| Regression: log download | **PASS** — the active file (6 786 B), a rotated one (2 054 B) and the oldest listed, which is the other module's 1 048 580-byte file, all download with an intact header. That last case was impossible before the fix |
| Regression: the three rig suites | **ALL PASS** — `at_wp_target.py` 9/9, `at_wp_fallback.py` 5/5, `at_wp_confirm.py` 9/9 (2026-09-24) |
| The GUI Log tab against the mock | **PASS** — in a browser: the active file is marked "(current)" and pre-selected, this unit's files come first and newest-first, then the other module's. With the reply's `on_card` raised to 113 — the number the rig really had this morning — the tab adds a disabled row, "— 108 older file(s) on the card, not listed —", so a bounded list reads as one |

## Upgrading

- **No partition change, no NVS migration, no new key.**
- **Existing cards are not migrated.** A card that has accumulated more than 30 files keeps them until
  rotation trims them back — up to five per rotation — so nothing is bulk-deleted on upgrade.
- **The listing's shape changed**: `sd_files` is now newest-first and grouped, and `current` is new. Any
  consumer that assumed ascending order should be checked — in this repo the GUI was the only one.

## Known limitations

- The listing is bounded at 64 names. A card holding more than that shows the newest 64.
- **A card far above the cap converges over a few rotations**, not at once: each rotation trims up to five
  files (`SD_TRIM_PER_ROTATION`). That is deliberate — one rotation deleting dozens of files would be a worse
  surprise than a card that comes back to the cap over a few rotations. Trimming only one per rotation would
  not converge at all, since each rotation also creates one.

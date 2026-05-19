# 2.0.0-a.6.35.7 — Diagnostics panel moved to the bottom of the Log tab

Tiny GUI-only follow-up to a.6.35.6. The new Diagnostics (coredump) panel originally sat at the top of the Log tab, above the SD-card controls and the log-file download. Operator feedback: routine SD operations are touched far more often than the post-mortem coredump panel, so the routine controls should stay at eye-level. Moved Diagnostics to the bottom — still visible without scrolling, but no longer competing for attention.

## What changed

```
Log tab pre-a.6.35.7:                  Log tab post-a.6.35.7:
─── Diagnostics ────────              ─── SD Card ───────────
Coredump: ...                          [Mount] [Unmount]
[Download] [Erase]                     ───────────────────────
─── SD Card ───────────                ─── Download log ──────
[Mount] [Unmount]                      [select] [↻]
─── Download log ──────                [Download CSV]
[select] [↻]                           ───────────────────────
[Download CSV]                         ─── Diagnostics ───────
                                       Coredump: ...
                                       [Download] [Erase]
```

Pure DOM-reorder in `firmware/data/index.html`. No JS / CSS / firmware logic changes.

## What changed (files)

- **`firmware/data/index.html`** — moved the `<h3>Diagnostics</h3>` block + its single row from before the SD Card section to after the Download log section.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.35.7`. The asset bundle pairs with firmware version via the manifest checksum, so the change deploys as a paired firmware+assets OTA.

## Build delta vs a.6.35.6

| Metric | a.6.35.6 | a.6.35.7 | Delta |
|---|---:|---:|---:|
| Firmware bin | 1 354 176 B | ≈ same | ~0 (only the FIRMWARE_VERSION string changed) |
| RAM static | 60 568 B | 60 568 B | 0 |

Functionally identical to a.6.35.6. The bin diff is only the version string in the build.

## Phase 7 readiness

Unchanged from a.6.35.6 — still green. The Diagnostics panel placement doesn't affect the coredump retrieval workflow, just where the operator's eye lands first when opening the Log tab during daily-review.

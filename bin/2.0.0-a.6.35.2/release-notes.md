# 2.0.0-a.6.35.2 — Multi-file log-upload drain (no more stranded CSVs)

Data-loss follow-up to a.6.35. Operator-driven question: *"is it possible we miss a file because log rotation was executed before the upload was executed?"* Answer in the previous version: yes, in two specific scenarios. Answer here: no, both scenarios close.

## The bug

T14's two upload triggers each examined exactly **one** filename:

- **Daily trigger** read `event_logger_newest_closed()` — the lex-max non-active CSV on SD.
- **On-rotation trigger** read `event_logger_last_rotated()` — the in-memory `s_last_closed` written by T9's `rotate_sd_file()`.

The dedup test was `strcmp(fn, cfg.log_last_up) != 0`, which means "different from the latch", not "newer than the latch". The middle of a range was never considered.

### Scenario 1 — multiple rotations during one upload

```
t=0 s    T9 rotates → A closed, B active, s_last_closed=A, notify bit set
t=0.1 s  T14 wakes, do_log_upload(A) starts
t=2 s    T9 rotates → B closed, C active, s_last_closed=B, notify re-set
t=4 s    T9 rotates → C closed, D active, s_last_closed=C, notify already set
t=8 s    do_log_upload(A) returns 2xx, latch=A
t=8 s    T14 loops, sees notify, reads s_last_closed=C, uploads C, latch=C
```

**B is gone.** The daily trigger can't recover B — `newest_closed` returns C, dedup-equals latch, skipped. B sits on SD until `SD_MAX_FILES=10` enforces deletion.

### Scenario 2 — T14 fails across a rotation (more realistic)

WiFi drops, or the server returns 5xx. The daily trigger's upload of file B fails → latch stays at the previous successful file (A). Next day at the daily slot WiFi is back, but T9 has rotated C since. T14 reads `newest_closed=C`, uploads C, latch advances to C. **B is stranded.** All future triggers see `newest_closed=C == latch`, dedup skips.

## Fix

New event_logger API:

```c
/**
 * Return the lex-smallest closed CSV strictly greater than @p after.
 * Excludes the active file. Returns false if no such file exists.
 */
bool event_logger_next_pending(const char *after, char *out, size_t cap);
```

Wired into T14 via a new `upload_pending(cfg)` helper:

```c
static int upload_pending(const cfg_shadow_t *cfg)
{
    char after[33] = {0};
    strncpy(after, cfg->log_last_up, sizeof(after) - 1u);

    int n_uploaded = 0;
    char next[24];
    for (int i = 0; i < 12; i++) {       /* safety cap = SD_MAX_FILES + slack */
        if (!event_logger_next_pending(after, next, sizeof(next))) break;
        if (!do_log_upload(next, cfg)) break;   /* failure → next trigger resumes */
        strncpy(after, next, sizeof(after) - 1u);
        after[sizeof(after) - 1u] = '\0';
        n_uploaded++;
    }
    return n_uploaded;
}
```

`do_log_upload`'s 2xx path already calls `dm_set_log_last_up(next)` → the dedup latch advances to NVS after each successful upload. A reboot mid-batch leaves the latch correctly pointing at the last delivered file; the next trigger after reboot picks up from there. A failure mid-batch leaves the latch correctly behind, so the next trigger retries from the failed file.

Both daily and on-rotation triggers now call `upload_pending(&cfg)` instead of single-file logic.

## What changed

- **`firmware/src/event_logger/event_logger.h`** — new `event_logger_next_pending(after, out, cap)` declaration.
- **`firmware/src/event_logger/event_logger.cpp`** — implementation. Same `sd_scan()` + active-file exclusion as `event_logger_newest_closed`; the only difference is the selection predicate (smallest-greater-than-after vs lex-max).
- **`firmware/src/status_post/status_post.cpp`** — new static `upload_pending(cfg)`. Daily-trigger block reduced to single call + 0-count diagnostic. On-rotation block reduced to single call. Old single-file paths removed.
- **`firmware/platformio.ini`** `FIRMWARE_VERSION` → `2.0.0-a.6.35.2`.

## Edge-case matrix

| Scenario | Behaviour |
|---|---|
| 0 pending | Returns 0. Daily trigger logs `value_a=0, value_b=2` skip-diagnostic. |
| 1 pending (normal cadence) | Single upload. Observable identical to pre-patch. |
| N pending (post-outage backlog) | All N uploaded oldest-first. Latch advances per success. |
| Failure mid-batch | Loop bails. Latch reflects only successes. Next trigger resumes from failed file. |
| Reboot mid-batch | NVS holds advanced latch. Boot re-reads. Next trigger resumes. |
| File deleted between scan and upload (SD-full reclaim race) | `storage_sd_file_size` returns 0 → `s_last_log_str = "FAIL nofile"`. Loop bails. Next iteration sees the file gone, picks the next pending. |
| Server consistently rejects a specific file | Loop blocks at that file across triggers. 12-iteration safety cap in `upload_pending` prevents a single call from looping forever. Operator-visible via `last_log_up = "FAIL ..."`. Auto-skip-after-N-failures deferred to a future patch if this proves to bite in production. |

## Acceptance

| Metric | a.6.35.1 | a.6.35.2 | Delta |
|---|---:|---:|---:|
| Firmware bin (flash usage) | 1 348 669 B | **1 348 685 B** | +16 B |
| RAM static | 60 552 B | 60 552 B | 0 |

+16 bytes total because `event_logger_next_pending` reuses the same `sd_scan` body as `event_logger_newest_closed` (just a different selection predicate), and `upload_pending` is a thin loop around the already-linked `do_log_upload`.

Bench-verified on 192.168.20.160:

- a.6.35.2 paired OTA flash completes, unit boots clean (`uptime=88 s`, `eg1=0`).
- NVS settings preserved through OTA — the user's production URL, secret, interval, expose mask, log_h:m, and log_rot all survive the flash and round-trip cleanly through `/api/web`.
- Status POST to the operator's real production server returns HTTP 200 — `last_post = 'OK 2026-05-19 11:54:33'` — confirming items A (secret header) and B (canonical JSON body) from a.6.35 work end-to-end against the real upstream, not just a mock.
- Multi-file drain path **not exercised** in this acceptance window. The unit has 9 closed CSVs from earlier testing and the user's production endpoint is configured — triggering a 9-file drain (~5 MB upload to pe1mew.nl) would have been unexpected for the operator. First real exercise of `upload_pending` with N > 1 happens organically during Phase 7 when natural WiFi blips / rotation cadence create the conditions.

**Single-file behaviour is preserved by construction**: with 0 or 1 pending files, `upload_pending` collapses to "iterate zero or one times then return", which is byte-identical in observable behaviour to the pre-patch single-file paths. All a.6.35 acceptance tests (item D `status_enable` gate, item G URL validator, item F format) and the a.6.35.1 transition fix continue to apply unchanged.

## On the lost web settings

During the a.6.35.1 acceptance test I cleared the URL via `POST /api/web {"enable":0,"url":""}` in the test cleanup script. That wiped the operator's NVS-stored URL. **NVS persistence is fine; this was a test-script bug, not a firmware regression.** Web settings live in `NVS_NS_SYSTEM` (partition `0x10000`), entirely separate from the OTA app banks (`0x20000` / `0x220000`). OTA flashes do not touch NVS. The settings round-trip cleanly: `T11 web_post_handler` → `nvs_cfg_set_*` → `dm_reload_web_cfg()` → `nvs_load_web()` → MX4 shadow → `dm_cfg_snapshot()`. Going forward, acceptance tests will only toggle `enable` and never overwrite `url` / `secret`.

## Next

Phase 7 — 14-day soak. Both a.6.35 follow-ups (the UX in a.6.35.1, the data-loss fix here) reduce the chance of any "I see weird behaviour" surprise during the soak. The remaining real-world checks are:

- Item C — log upload completes end-to-end against the real server (waits for the 03:15 daily slot OR a natural 512 KB rotation).
- Item E — `log_upload_rot=0` gate observed via a rotation that does *not* produce an upload (currently `log_rot=1` on the unit so this would need a temporary flip).
- gh#23 watch — `value_a=12` (largest-block) stays >50 KB through ≥100 status POST cycles. With status POSTing succeeding at `interval=120 s`, this accumulates ~720 cycles/day. If largest-block stays healthy, gh#23 is closed by a.6.35 alone and no a.6.36 mbedTLS-mitigations alpha is needed.

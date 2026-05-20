# 2.0.0-rc.1.2.1 — T14 log-upload heap-overrun fix

Patch release on top of rc.1.2. **One-line C/C++ change** in `status_post.cpp` (4096 → 4097 byte allocation in the chunk reader) plus the version bump. Fixes the heap-corruption panic that knocked the unit into a boot loop at 03:15 on day 0 of the rc.1.2 soak.

Supersedes rc.1.2 as the Phase 7 soak candidate; the 14-day clock restarts at day 0 (third reset — rc.1 → rc.1.1 wind-fix → rc.1.2 OTA-reboot-fix → rc.1.2.1 log-upload-fix).

## The operator's report (verbatim)

> *"greenhouse controller did not upload log at 3:15 to status page, resetted 4 hours ago and has a coredump available."*

## What actually happened

Timeline from the SD CSV log on the bench unit:

| Time (local) | Event |
|---|---|
| 2026-05-19 23:21 | rc.1.2 OTA cleanly applied (`ESP_RST_SW`). Unit ran fine. |
| 2026-05-20 03:14:21 | Routine 120 s status POST OK. |
| 2026-05-20 03:14:52 | Last pre-crash heartbeat — `free=105 KB, largest=31 KB` (healthy). |
| 2026-05-20 03:15:03 | **PANIC**. T14 fires at log_h:log_m=3:15, multi-file drain (a.6.35.2) picks up older stranded file `20260518233026.csv` first, starts streaming over HTTPS. TLSF heap assertion fires inside a WiFi TX-buffer alloc. Coredump captured. |
| 03:15:08 → 03:15:59 | **Boot loop** — 11 × `ESP_RST_PANIC` + 1 × `ESP_RST_SW`, one every ~5 s. Each boot re-detected the dump (`value_a=18` rows) and re-attempted the upload. |
| 03:15:59 | Last panic — boot #12 stabilised. (Likely cause: the corrupted block was either reaped or `dm_set_log_last_up` advanced enough that the next attempt didn't re-trigger the same chunk stream.) |
| 03:16 onwards | Stable run. Status POSTs continued at 120 s; log upload never re-attempted (latch behaviour). Uptime at the time of the report: 4 h 17 m. |

So the post never made it to the status page (`last_log_up=""` per `/api/web`) but the device looked outwardly healthy aside from the persistent `coredump_available` flag.

## The bug — `firmware/src/status_post/status_post.cpp`

The 4 KB-chunk streaming reader in `do_log_upload`:

```c
// before — 4096-byte allocation
uint8_t *chunk = heap_caps_malloc(LOG_UPLOAD_CHUNK_BYTES, MALLOC_CAP_INTERNAL);
//                                ↑ = 4096

storage_sd_read(sd_path, offset, (char *)chunk,
                want + 1u,   // = 4097 for every full chunk
                &got);
```

`storage_sd_read`'s contract: write up to `(buf_len - 1)` data bytes, NUL-terminate at offset `got`. The reader passes `buf_len = want + 1u` expecting the NUL inside the allocation, but the allocation is only `want` bytes. Every full-chunk read writes the NUL **one byte past the end** of `chunk`. That byte lands in the metadata header of the *next* TLSF block and corrupts the `block_is_free` flag.

TLSF doesn't assert immediately — the failure mode is "next allocator visit to the corrupted block". Under WiFi+TLS heap churn (each `esp_http_client_write(4 KB)` allocates several mbedtls + WiFi-TX buffers), the assertion eventually catches. On the bench it took the entire 4 KB chunk loop of the 76-KB stranded log file before a 1494-B WiFi TX-buffer alloc hit the corrupt block and panicked.

The original author left a TODO comment at lines 420-423 calling out exactly this hazard ("Instead pass want bytes…") — the code below was never updated to match the comment.

### Decoded backtrace (key threads)

Panicking task (the unlucky one that visited the corrupt block):
```
panic_abort  "assert failed: block_trim_free … block_is_free(block)"
  __assert_func ← heap/tlsf_control_functions.h:548
  tlsf_malloc → multi_heap_malloc_impl
  heap_caps_malloc(size=1630, caps=2060)
  esp_coex_common_malloc_internal_wrapper(1630)
  esf_buf_alloc_dynamic → ieee80211_output_do → esp_wifi_internal_tx
  wifi_transmit_wrap(len=1494)
```

The task that actually caused the corruption (parallel thread):
```
esp_http_client_write(client, buffer, len=4096)
do_log_upload(filename="20260518233026.csv", status_post.cpp:432)
upload_pending(cfg, status_post.cpp:529)
task_status_post     ← T14
```

## The fix

One-character allocation bump:

```diff
-    uint8_t *chunk = (uint8_t *)heap_caps_malloc(LOG_UPLOAD_CHUNK_BYTES,      MALLOC_CAP_INTERNAL);
+    uint8_t *chunk = (uint8_t *)heap_caps_malloc(LOG_UPLOAD_CHUNK_BYTES + 1u, MALLOC_CAP_INTERNAL);
```

The 4097-byte allocation accommodates the NUL `storage_sd_read` writes at offset `got` (≤ 4096 for full chunks). The wire `esp_http_client_write(chunk, got)` below still clamps to `got` data bytes, so each on-wire chunk stays at 4096 bytes — no throughput change. The dangling TODO comment was rewritten to reflect the now-correct semantics:

```diff
-        /* storage_sd_read NUL-terminates → buf_len must include the NUL.
-         * We pass want+1 and discard the NUL — but the +1 might exceed our
-         * allocation. Instead pass want bytes, accept the truncated NUL
-         * inside that count, and clamp the actual write to (got). */
+        /* storage_sd_read writes up to (buf_len - 1) data bytes and appends
+         * a NUL at offset `got`. Buf is allocated LOG_UPLOAD_CHUNK_BYTES+1
+         * so the NUL always lands inside the allocation. The wire write
+         * below clamps to `got` (excludes the NUL). */
```

## Build-artefact preservation — `bin/build_release.ps1`

A second problem surfaced during this investigation: the rc.1.2 binary deployed on the bench had a different sha256 than what a fresh `pio run` produced from the (clean) working tree — build-time non-determinism (likely `__DATE__`/`__TIME__` macros). The original rc.1.2 `firmware.elf` had been clobbered by a later build, so `esp_coredump info_corefile` refused to load the captured dump (SHA mismatch). Decoding required temporarily monkey-patching `esp_coredump/corefile/loader.py` to downgrade the SHA-check to a warning.

To prevent this trap for every future release, `bin/build_release.ps1` now archives the matching `firmware.elf`, `firmware.map`, `bootloader.bin`, and `partitions.bin` to `bin/<version>/` alongside the `.bin`:

```
bin/2.0.0-rc.1.2.1/
├── greenhouse-controller-2.0.0-rc.1.2.1.bin    (OTA upload)
├── firmware-2.0.0-rc.1.2.1.elf                 (symbols — needed for coredump decode)
├── firmware-2.0.0-rc.1.2.1.map                 (linker map — function/section addresses)
├── bootloader-2.0.0-rc.1.2.1.bin               (full-flash recovery)
├── partitions-2.0.0-rc.1.2.1.bin               (partition table snapshot)
├── web-assets-2.0.0-rc.1.2.1.zip               (paired OTA web bundle)
└── release-notes.md
```

All five binary outputs are gitignored (`bin/**/*.{bin,elf,zip}` already covers them) so the repo stays clean. The per-version directory keeps the matching debug-info set against the local checkout regardless of subsequent rebuilds.

## Forensic artefacts from the rc.1.2 failure

Preserved at `bin/2.0.0-rc.1.2/post-mortem/`:

- `coredump-rc.1.2-post-reset.bin` — raw 43 172 B dump
- `coredump.core.elf` — converted ELF (decodable against a same-source rebuild with the SHA-check downgraded; reproducible per the procedure recorded in this entry)
- `sd-log-20260519111829.csv` — full SD log including the 56-second boot-loop window (lines 6115-6250)
- `patch_dump_sha.py` — Python helper for future cross-build decodes if the same trap recurs

## What did NOT change

- Web GUI, web assets, the rc.1.1 wind-direction surface fix, the rc.1.2 OTA-reboot carve-off.
- Other firmware C/C++ (canonical JSON shape, LCD code, every task graph, every endpoint behaviour, the rc.1.1 / rc.1.2 fixes).
- Static RAM footprint (a one-byte heap allocation difference is below measurement resolution).

## Phase 7 soak — acceptance criteria

Unchanged from rc.1. Day-counter resets at day 0 against rc.1.2.1.

## Pre-soak verification checklist on the bench

1. Boot → confirm `fw_ver=2.0.0-rc.1.2.1`, `asset_version=2.0.0-rc.1.2.1`, `eg1=0`, `mode=AUTOMATIC`, `flags=[]`.
2. Confirm `/api/coredump/status` returns `present:false`.
3. Wait through the next `log_h:log_m` window (03:15 local) — confirm `last_log_up = OK <ts>` and `log_last_up = <ts>` in `/api/web`, with the `value_a=1, value_b=1, initiator=WEB` row in the SD CSV at that minute.
4. Confirm the multi-file drain catches up — both `20260518233026.csv` (the file that crashed rc.1.2) and `20260519111829.csv` should upload successfully on the first post-fix run.
5. Walk the GUI smoke test (login, view, change setpoint, download log, OTA push) at the normal cadence.

## Status

Day 0 of Phase 7 soak. Day 14 = `v2.0.0` if green across the board.

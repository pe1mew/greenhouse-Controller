# 2.0.0-rc.1 — Phase 7 soak candidate

Release candidate 1. The maturation work that started at `2.0.0-a.6.32` and walked through eleven alpha sub-iterations (a.6.32 → a.6.33 → a.6.34 → a.6.35 → 6.35.1 → .2 → .3 → .4 → .5 → .6 → .7) is **complete**. Tag bumped from `2.0.0-a.6.35.7` to `2.0.0-rc.1` ahead of the 14-day Phase 7 verification soak. No firmware code changes vs a.6.35.7 — only the version string + paired-asset manifest + companion-doc references.

If the soak passes its acceptance criteria, the next tag is `v2.0.0` on a fast-forward merge into `main`.

## What's in rc.1

Every feature shipped through the maturation series. Operator-facing summary:

### Logging + diagnostics
- T1 watchdog full instrumentation — NeoPixel + heap rows + integrity sweep + stack-HWM (a.6.32)
- CSV row timestamps in **local time** matching the filename convention (a.6.35.3)
- Every config change audit-logged with operator attribution (LCD Farmer / LCD Admin / Web UI); sensitive fields use `(set)`/`(changed)` sentinel — no PIN/credential leak (a.6.35.5)
- New SD-CSV event subtypes 14–17 (OTA stages), 18–20 (coredump lifecycle), `ch=4/5` for sensor faults — all decoded by `log/logparser.py` v1.6
- Coredump retrieval via web GUI: Log → Diagnostics → Download → offline `idf.py coredump-info` → Erase. Admin-only, rate-limited 1 op/10 s, audit-logged (a.6.35.6)

### Operator visibility
- New `mode.flags[]` entries surfaced as badges in the Alarms card AND on the public status dashboard:
  - 🟡 `wind_protect_off` — wind safety subsystem disabled (a.6.35.4)
  - 🔵 `humidity_ctrl_off` — RH-driven control disabled (a.6.35.4)
  - 🔵 `coredump_available` — panic dump waiting in flash (a.6.35.6)
- Audit row `value_a=18` at boot if a coredump was captured in the previous session (a.6.35.6)
- `Wind protect off` / `Humidity ctrl off` flow through T14 status POST → public dashboard

### Network / status-website
- HTTPS-only enforcement — controller rejects `http://` URLs at `/api/web` to prevent shared-secret leakage on the wire (a.6.35 item G)
- 24-hour NTP re-sync cadence with cfg-TZ preservation across sync (a.6.33)
- T10 STA up/down + NTP timeout/synced edge events to the SD log (a.6.35.3)
- Multi-file log upload drain — one outage no longer strands intermediate CSVs (a.6.35.2)
- Status POST sources `cfg.status_secret` and attaches as `sourceidentifier` header on every POST + log upload (a.6.35 item A)
- Canonical JSON body via `build_canonical_status_json` (a.6.35 item B)

### OTA + safety
- T13 firmware-only fallback timer — verified firmware commits after 120 s if no paired asset upload arrives (a.6.34)
- Status-POST `status_enable` master flag with immediate operator feedback on enable/disable (a.6.35 item D + a.6.35.1 UX)
- `log_upload_rot` rotation-trigger gate (a.6.35 item E)
- Distinct OTA stage codes `value_a = 14/15/16/17` (no collision with T14 outcome / T10 STA / T10 NTP — a.6.35.3)

## Pre-soak housekeeping done in this build

- `firmware/platformio.ini` `FIRMWARE_VERSION` `2.0.0-a.6.35.7` → `2.0.0-rc.1`
- `webUiMock/mock_server.py` `cfg["fw_ver"]` bumped to match
- `manual/beheerderHandleiding.md` header v1.16 / a.6.35.7 → v1.17 / rc.1
- `design/maturationPlan_alpha6.32-6.35.md` status marked as "complete; rc.1 cut for Phase 7"
- Built + paired-deployed `bin/2.0.0-rc.1/greenhouse-controller-2.0.0-rc.1.bin` + `web-assets-2.0.0-rc.1.zip` to the bench unit at 192.168.20.160
- **Clean-state post-deploy verification**: `fw_ver=2.0.0-rc.1, asset_version=2.0.0-rc.1, eg1=0, mode=AUTOMATIC, flags=[]`
- **Archived pre-existing 45 KB coredump** (residual from panic-test work during the alpha series) to the local workstation at `bin/2.0.0-rc.1/pre-soak-artifacts/coredump-pre-soak-cleanup.bin`. The file is *not* committed (gitignored alongside firmware binaries) but persists in the local checkout for cross-reference. Partition then erased on the unit — day-1 of soak starts with a clean coredump slot. If a soak-time regression surfaces that resembles the archived dump, the operator can decode the local archive via `idf.py coredump-info` to compare.

## Phase 7 soak — acceptance criteria

The soak runs **minimum 14 days** against the operator's production status server (`https://pe1mew.nl/hbwv/api.php`) at `status_interval_s = 120`. Pass = green to tag `v2.0.0` and merge to main. Fail on any criterion = halt + a.6.36 (or rc.2) patch + restart the 14-day clock.

| Criterion | Target | Where to verify |
|---|---|---|
| Zero unplanned reboots | No `value_a=5` rows with `value_b ∈ {5, 6, 7, 9}` (INT_WDT, TASK_WDT, WDT, BROWNOUT) | `grep ",SYSTEM,SYS,0,0,5," /tmp/log.csv` |
| Zero coredumps | `/api/coredump/status` returns `present:false` every daily check; Alarms-card never shows the blue badge | Browser dashboard + `curl /api/coredump/status` |
| **gh#23 — heap fragmentation watch (primary signal)** | `value_a=12` (largest contiguous internal block) stays > 30 KB across ≥ 100 status POST cycles. Stable or rising trend; sustained fall triggers a.6.36 (mbedTLS mitigations) | `awk -F',' '$2=="SYSTEM" && $6==12 {print $1, $7}' /tmp/log.csv` |
| Heap baseline stable | `value_a=7` (free internal) + `value_a=8` (PSRAM) drift < 5 KB over 14 days | Same `awk` filter on params 7 / 8 |
| Status POST success rate | > 95 % of cycles return 2xx; no extended `Net backoff` badge | `awk -F',' '$2=="SYSTEM" && $3=="WEB" && $6==1 && $7==0' /tmp/log.csv` |
| Daily log upload | At least one `value_a=1, value_b=1, initiator=WEB` row per 24 h at the configured `log_upload_h:log_upload_m` | Confirms gh#25 dedup latch + multi-file drain are operational |
| Climate-control responsiveness | Window behaviour matches what 1.20.3 would have done under identical conditions; no spurious wind-override; mode rarely leaves AUTOMATIC | Operator visual + RELAY-row review |
| Flash usage | Bin stays ≤ 1.40 MB (current rc.1 = 1.354 MB); no growth unless a patch alpha lands | Bin size unchanged |
| GUI smoke test | Daily: login + view status + change a setpoint + download a log + see audit rows fire | Operator dashboard walkthrough |

## Pre-soak sanity check (recommended, ~30 min)

Before leaving the unit alone for 14 days, **deliberately trigger a panic on the bench** to prove the full coredump retrieval workflow works end-to-end against this rc.1 build. A one-line `*(volatile int*)0 = 0;` in a `/api/__test/panic` handler is the cheapest way; revert immediately after the test. Walk:

1. Trigger panic → unit reboots → `value_a=5, value_b=4` BOOT row → `value_a=18, value_b=N` coredump-detected row.
2. Open dashboard → see **Coredump available** blue badge on Alarms card.
3. Open Log tab → Diagnostics panel shows size + capture fw_ver.
4. Click Download → browser saves `coredump-2.0.0-rc.1-<unix_ts>.bin`.
5. Run `idf.py coredump-info -t raw -c <file> bin/2.0.0-rc.1/firmware-2.0.0-rc.1.elf` → readable backtrace.
6. Click Erase → confirm dialog → partition wiped → badge disappears.

That walks every link in the diagnostics chain before any of them are needed for real during the soak.

## Build delta vs a.6.35.7

| Metric | a.6.35.7 | rc.1 | Delta |
|---|---:|---:|---:|
| Firmware bin | 1 354 176 B | **1 354 160 B** | −16 B (FIRMWARE_VERSION string shortened: `a.6.35.7` → `rc.1`) |
| RAM static | 60 568 B | 60 568 B | 0 |

No code changes. Final flash usage **64.6 %** of the 2 MB OTA bank.

## After the soak passes

- Tag `v2.0.0` on the merge commit
- Fast-forward merge `dev/2.0.0-esp-idf` → `main`
- Run `bin/build_release.ps1` from main → publishes `bin/2.0.0/`
- Operator deploys to Unit 2 first (7-day observation), then Unit 1

## Known deviations from the original migration plan

- **Flash budget**: original Phase 7 target was ≤ 1.30 MB. Current rc.1 = 1.354 MB (~50 KB over). The maturation work landed real operator-facing value (audit log, coredump retrieval, operator-aware badges) that wasn't in the original budget. Treating as accepted; the 2 MB OTA bank has plenty of headroom (64.6 % used → 35 % free).
- **`largest_block` baseline**: original target was > 50 KB. Bench unit currently runs at ~31 KB in-flight (largely-unchanged-since-Phase-4 baseline). If the soak's 100-cycle watch shows a falling trend below 30 KB, a.6.36 brings mbedTLS mitigations (max_frag_len, single cipher, session-ticket reuse). If the value stays steady around 31 KB through the soak, gh#23 is closed by the existing implementation and a.6.36 isn't needed.
- **T15 supervisor**: still dormant (source on disk, not in `CMakeLists.txt SRCS`). Re-enabled paired with a.6.36 only if gh#23 mitigations land. Without it, the soak runs without circuit-breaker + planned-reboot protection — observed risk if a hidden heap leak surfaces under the new T14 traffic.

## Status

Day 0 of Phase 7 soak. Day 14 = `v2.0.0` if green across the board.

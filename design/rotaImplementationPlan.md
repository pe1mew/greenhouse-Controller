# Remote OTA (ROTA) — Implementation Plan

*2026-07-12 · branch `rota` · firmware target 2.2.0 · implements [rota_tds.md](rota_tds.md) v0.1 · derives from [remoteOTAstudy.md](remoteOTAstudy.md)*

This plan sequences the work behind the ROTA TDS. It is a build order, not a re-statement of requirements — each work item cites the TDS requirements it satisfies (`R-*`) and the verification cases it must pass (`TC-*`). Effort scale: **S** < 8 h · **M** 1–3 days · **L** 1–2 weeks.

## Repository split

ROTA spans **two git repositories** (TDS R-T06). The server repository already exists (`pe1mew/greenhouse-Controller-FOTA-server`); it is developed and deployed independently of the firmware.

| Repo | Contents | Language |
|---|---|---|
| `greenhouse-Controller` (this repo, branch `rota`) | Firmware client (T16), GUI, config, client-side tooling (`build_release.ps1` publish step, provisioning, device simulator), the TDS and this plan | C++ / ESP-IDF, PowerShell, Python |
| `greenhouse-Controller-FOTA-server` (separate repo — `git@github.com:pe1mew/greenhouse-Controller-FOTA-server.git`, local `C:\Users\drasv\github\greenhouse-Controller-FOTA-server`) | `manifest.php`, `download.php`, registry/channel schema, nginx server-block config, deploy script, PHP unit tests | PHP, shell, nginx conf |

**The interface between them is §4 of the TDS** (endpoints, `X-OTA-Auth`, manifest schema, audit codes, store layout) — frozen before either side codes against it. The `greenhouse-Controller-FOTA-server` repo pins the TDS version/commit it implements (R-T06). Neither repo stores secrets, private keys, or device credentials (R-T07).

## Phase 0 — Contract freeze and prerequisites (S)

| # | Task | Satisfies | Done when |
|---|---|---|---|
| 0.1 | Freeze TDS §4 (endpoints, header formula, manifest schema incl. reserved `key_id`, audit codes 22–24, store layout). Tag the TDS commit; the server repo references this tag. **Status: §4 precision-passed and marked frozen 2026-07-13** (id = full MAC 12-hex; manifest request carries `fw=`/`res=`; 204 reserved for auth-fail only; HMAC field encodings pinned; audit code 21 verified taken → 22–24 confirmed free). Awaiting operator commit + tag `rota-contract-v1.0`. | R-A05, R-A10, R-T06 | TDS §4 tagged; no open questions on the wire format. |
| 0.2 | Scaffold the existing `greenhouse-Controller-FOTA-server` repo (README naming the TDS tag, `.gitignore` excluding any `*.pem`/`*.secret`/`devices.json`/`.deploy.env`, PHP-lint CI). **Status: scaffolded 2026-07-13** — README pins `rota-contract-v1.0`; endpoint stubs (501) carrying the contract as header comments; `nginx/ota.conf` fragment; `tools/deploy.sh` (SSH-key auth, StrictHostKeyChecking, `.deploy.env` pattern); CI = PHP lint + R-T07 credential scan. | R-T06, R-T07 | Repo scaffolded, CI lints PHP. |
| 0.3 | Generate the long-lived self-signed **server** certificate (e.g. 20 y) for the OTA hostname; keep the private key off both repos (operator's secret store). Decide the OTA URL/vhost name. **Status: done 2026-07-13** — OTA host decided: **`ota.rfsee.net`** (dedicated SNI vhost, port 443; DNS record to be added by operator); cert CN/SAN `ota.rfsee.net`, RSA-3072, valid 20 y (to 2046-07), in the operator's secret store. `ota_url` will be `https://ota.rfsee.net/` (endpoints at the vhost root — no `hbwv/ota/` prefix; fully separate from the status site). | R-A02, R-A04 | Cert + key generated; public cert earmarked for the firmware-embedded default and GUI test. |
| 0.4 | Generate per-unit `ota_secret`s for 2344 and 5C88 (`openssl rand -hex 32`), distinct from `status_secret`. Store in the operator's secret store, not in git. **Status: done 2026-07-13** — two 64-hex secrets generated into the operator's secret store; values never surfaced in any log or document. | R-A06, R-T07 | Two secrets recorded out-of-repo. |

## Phase 1 — Server (`greenhouse-Controller-FOTA-server` repo) (M) — no firmware dependency

Buildable and fully testable against the device simulator before any client code exists.

> **Status: 1.1–1.5 implemented 2026-07-13** (FOTA-server repo). `public/lib/rota_lib.php` (auth, HMAC verify, ±300 s skew, flat-file nonce cache, atomic registry/check-in), `public/manifest.php` (pinned/mainstream resolution → stored manifest), `public/download.php` (manifest-named artefact → `X-Accel-Redirect`, with `ROTA_NO_XACCEL` dev path); `tools/init-store.sh` + `tools/prune-releases.sh` (R-S08 retention); `examples/` schema reference. 1.4 nginx block done in the scaffold. 1.6 done earlier. **PHP-runtime verified 2026-07-13** (PHP 8.5 local): all sources lint clean; the device simulator passes **8/8** against `php -S` + a local ota-store (version resolution mainstream→2.2.0 & pinned→2.1.9; four 204 auth-failure modes; nonce replay; artefact download); check-in logging + atomic registry update + nonce cache confirmed working; certificate pinning validated **2/2** against an HTTPS server presenting the real `ota_server.pem` (correct cert accepted, wrong fingerprint refused). **Remaining:** stale-check-in alert (R-S10, Could — deferred).

| # | Task | Satisfies | Verify |
|---|---|---|---|
| 1.1 | `manifest.php`: parse `X-OTA-Auth`, HMAC-SHA256 verify with `hash_equals`, ±300 s skew check, nonce cache (flat file, prune > 10 min); on pass resolve unit → `pinned_version` else `channels/<unit-type>.json`; emit manifest JSON; append `checkins.csv`; failed auth → 204. | R-A05, R-A07, R-A08, R-S01, R-S04, R-S06, R-O04 | TC-02, TC-11 |
| 1.2 | `download.php`: same auth gate; resolve `file=fw\|assets&v=` to `ota-store/releases/<v>/…`; serve via nginx `X-Accel-Redirect` (PHP authenticates, nginx streams). Range support optional. | R-A05, R-S01, R-S05 | TC-11 |
| 1.3 | Store schema: `ota-store/` outside webroot — `releases/<version>/`, `channels/<unit-type>.json`, `devices.json` (opaque-string key: secret, unit_type, channel, `pinned_version`, enabled, last_seen, fw_ver), `checkins.csv`. Registry writes atomic (tmp + `rename()`). | R-S02, R-S03, R-S07, R-I02, R-I03 | TC-11 |
| 1.4 | nginx server block: pinned self-signed server cert, `internal` location for `X-Accel-Redirect`, PHP-FPM pass. Committed as config to the server repo (no secrets inline). | R-A02, R-S01, R-S05 | TC-11 |
| 1.5 | Retention: keep ≥ 5 releases/type; optional stale-check-in alert (`now − last_seen > 2·ota_check_h`). | R-S08, R-S10 | review |
| 1.6 | **Deployment by git clone in `$HOME` on rfsee.net, deployed from there** (operator convention, not push): `tools/bootstrap.md` (read-only GitHub deploy key with pinned host key, clone at `~/greenhouse-Controller-FOTA-server`, git-ignored `.server.env` for local targets, runtime dirs + cert/key placed out of band) + `tools/server-update.sh` (fast-forward `git pull` → local copy of `public/` into WEBROOT → `nginx -t` → reload). nginx serves only the deployed copy; never touches `ota-store/`; `.git/` unreachable under the OTA host. No key material in the repo. | R-T07 | TC-12; repo credential scan clean; `.git/` probe returns 404 |

## Phase 2 — Server acceptance harness (this repo) (S) — gates Phase 1 "done"

| # | Task | Satisfies | Verify |
|---|---|---|---|
| 2.1 | Python **device simulator** (`bin/rota_sim.py`): builds correct `X-OTA-Auth`, pins the server cert, walks manifest→download→verify. Exercises negatives: wrong secret, bit-flipped MAC, replayed nonce, ±skew, non-pinned server cert (MITM), pinned-version vs mainstream resolution. **Status: done 2026-07-13** — stdlib-only, SHA-256 fingerprint pinning (faithful to esp_http_client `cert_pem`); 9 cases; compiles; HMAC formula cross-checked against PHP. Runs against a deployed instance or local `php -S`. | R-T04 | TC-02 |
| 2.2 | Wire the simulator suite as the server's acceptance gate. **Status: DONE — 8/8 green on LIVE production `ota.rfsee.net` 2026-07-13** (deployed cert fingerprint matches the pin; HMAC auth + all four 204 rejection modes; nonce replay; version resolution → 2.2.0; artefact download via nginx `X-Accel-Redirect`). TC-02 closed on the production config → **Phase 1 accepted.** (Live test used the example device + a hand-placed dummy 2.2.0 release; real device rows + a real published release come with Phase 3 provisioning / Phase 4 publish tooling.) | R-T04 | TC-02 green = Phase 1 accepted |

## Phase 3 — Firmware client (this repo, `rota` branch) (L) → 2.2.0

Ordered so each layer is testable before the next stacks on it.

| # | Task | Satisfies | Verify |
|---|---|---|---|
| 3.1 | **Config plumbing** — NVS `system` keys `ota_enable`(0), `ota_check_h`(24), `ota_url`, `ota_secret`, `ota_win_lo`(2), `ota_win_hi`(4) via the standard 5-spot data_manager pattern; `cfg_defaults.h` + `cfg_limits.h`; `cfg_shadow_t` fields; LOG_PARAM ids; **`logparser.py` in the same changeset**. **Status: DONE 2026-07-13** — K_ constants, `nvs_load_web` (web-reloadable alongside `status_*`), `cfg_clamp` + `ns_key_to_log_id` branches, `cfg_shadow_t` fields, defaults/limits; `LOG_PARAM_OTA_*` = **39–44** (38 reserved for gh#35 avg_win_wind); logparser decodes 39–44 (+ url/secret "set" sentinels, ota_enable boolean). Firmware builds clean (+545 B flash). | R-F01, R-F04, R-O02 | TC-14 |
| 3.2 | **Server-cert storage** — NVS blob or LittleFS file (≤ 4 KB) + firmware-embedded default; resolver prefers uploaded, falls back to embedded. **Status: DONE 2026-07-13** — `ota_cert_default.h` (embedded public `ota.rfsee.net` cert, raw literal) + `rota_cert_get/set/clear/is_custom` in `ota_client` (NVS `system/ota_cert` as a string, PEM-shape + ≤2 KB validation, default fallback via `looks_like_pem`). Builds clean. | R-A03, R-A04 | TC-03 |
| 3.3 | **`/api/ota/config` endpoint + GUI group** — admin-only transaction endpoint (validate-then-write, empty-secret/cert = keep); System-tab group: enable, interval, URL, secret, night-window, PEM upload, read-only last-check line; farmer allowlist excludes all keys. **Status: endpoint DONE 2026-07-13** — GET/POST `/api/ota/config` (admin-only, validate-then-write, empty secret/cert = keep, secret never echoed R-A09), POST notifies T16 to re-check now (R-F04); `max_uri_handlers` bumped. **GUI System-tab group still pending.** | R-F02, R-F03, R-F05, R-A03, R-A09 | TC-14 |
| 3.4 | **HMAC + HTTPS request layer** — `X-OTA-Auth` builder (`mbedtls_md_hmac`), pinned-cert `esp_http_client` config (reuses the T14 pattern but `cert_pem` instead of the bundle). **Status: DONE 2026-07-13** — new `ota_client` module: `rota_build_auth_header()` (HMAC-SHA256 over `id\|ts\|nonce\|request_uri`, SNTP-gated, full-MAC id via new `system_mac_str()`) + `rota_https_get()` (pinned `cert_pem`, PSRAM body accumulator, X-OTA-Auth attached). CMakeLists + `mbedtls` REQUIRES; builds clean. HMAC matches the server-verified vector by construction; live end-to-end check happens when T16 (3.5) issues its first request. | R-A01, R-A02, R-A05 | TC-02 (against staging) |
| 3.5 | **T16 task skeleton** — priority 3, 8 KB stack, interval+jitter scheduler, precondition gate (enable/URL/WiFi/NTP/no-OTA), backoff ladder, audit rows 22. Shares one TLS mutex with T14. **Status: DONE + VERIFIED 2026-07-13** — T16 created (prio 3, 8 KB, `tskNO_AFFINITY`); interval±jitter scheduler with interruptible sleep (`ulTaskNotifyTake`) for config-change/manual re-check (R-F04); precondition gate (enable/url/secret/SNTP-synced/no-OTA-in-progress); exponential backoff 1h→24h (R-C09); audit `value_a=22` sub 0..4; shares `MX_TLS` with T14 (R-C07). **Verified on FDA4 (192.168.20.169):** live manifest check against `ota.rfsee.net` → **204/auth_fail while unregistered**, **200/up_to_date/2.2.0 once registered** — confirms pinned-cert TLS (R-A02/A04) + per-unit HMAC both accept and reject (R-A05). Download/verify/apply (3.6–3.8) still stubbed. | R-C01, R-C02, R-C03, R-C07, R-C09, R-O01 | TC-04, TC-09 |
| 3.6 | **Manifest fetch + decision** — GET manifest, parse, SemVer + `seq` vs NVS `fw_hiwater`, `min_version` guard; audit rows 23. **Status: DONE (impl) 2026-07-13** — full manifest parse (ad-hoc/house style: version, seq, min_version, fw_*/assets_*); SemVer + `seq` vs NVS `fw_hiwater` gate (R-V01/V02), `min_version` refusal (R-V03); audit `value_a=23` (0 ok · 1 TLS/pin · 2 SHA/size · 3 downgrade/seq · 4 min_version). **VERIFIED 2026-07-13** — FDA4 pulled 2.2.1→2.2.2 (`seq 32` accepted vs hiwater 0; `fw_hiwater` persisted → steady-state up-to-date). See test_10_rota.md Test 9. | R-V01, R-V02, R-V03, R-O01 | TC-08 |
| 3.7 | **Download + verify** — both artefacts staged in PSRAM, SHA-256 + size checked before any flash write; abort frees buffers. **Status: DONE (impl) 2026-07-13** — `rota_download_verify()` GETs `download.php?file=fw\|assets&v=<v>` (pinned cert + HMAC, `MX_TLS`-serialised); stages each in PSRAM (reuses `rota_https_get`), checks size + SHA-256 (mbedtls) before any flash write; BOTH verified before apply (R-C04/C05/R-R06); artefact-size ceilings guard the alloc. **VERIFIED 2026-07-13** — FDA4 downloaded+verified 1.37 MB fw + 108 KB assets (`dl:0`) on the fixed build. | R-C04, R-C05, R-R06 | TC-05 |
| 3.8 | **Apply via T13 + policy gate** — night-window ∩ quiet-gate (re-checked < 5 s pre-reboot), feed `ota_firmware_begin/write/end → assets`, existing `schedule_reboot()`; deferral logic; audit rows 24. **Status: DONE (impl) 2026-07-13** — night-window (R-P01: local time, `lo==hi` disables, wraps midnight) ∩ quiet gate (R-P02: no `WIN_MOVING_*`, WIND_OVERRIDE/MOTOR_ALARM/CALIBRATING clear, no web session via new `web_any_active_session()`, no LCD PIN session via new `ui_pin_session_active()`); final re-check < 5 s before commit (R-P03) with new `ota_firmware_abort()` for a clean deferral (no stranded FW_DONE); feeds T13 `ota_firmware_*` → `ota_assets_*` back-to-back (paired-commit safe — `ota_assets_begin` cancels the 120 s fallback before its alloc); persists `fw_hiwater` (R-V02); reboot via existing `schedule_reboot()` (R-P06); audit `value_a=24` (0 committed · 1 deferred · 2 failed). On deferral the scheduler wakes near the next window start (or +5 min in-window) instead of a full check-interval later, else a 24 h interval anchored outside 02–04 would never coincide with the window (R-P04). **Known limitation:** a deferred update re-downloads both artefacts on each retry (no cross-cycle PSRAM cache) — bandwidth-wasteful if the quiet gate stays blocked through a window; optimise later if it bites. **VERIFIED 2026-07-13** — FDA4 apply deferred on an active web session (`apply:1`), then committed on the next 5-min retry once the gate cleared; rebooted to 2.2.2 with paired fw+assets. **Crash fixed first:** the initial pull stack-overflowed T16 (8 KB) → 16 KB + heap certs (see gotcha-log). | R-C06, R-G04, R-G06, R-P01–P06, R-R04 | TC-06, TC-07 |
| 3.9 | **Observability wiring** — status JSON `rota_state`/`rota_last_check`/`rota_last_result`; confirm 13–17 still fire. **Status: DONE 2026-07-13** — exposed as a dedicated admin endpoint `/api/ota/check`: GET returns `{id, last_check, result, result_code, http, checks, offered, running}`; POST triggers a check now. `rota_status_t` snapshot written by T16. Enabled the FDA4 verification above. Note: dedicated endpoint rather than folded into `/api/status`; push-OTA audit 13–17 non-regression not re-checked. | R-O03, R-O05 | TC-10 |
| 3.10 | **Docs same changeset** — OTAimplementation.md addendum, beheerder manual (config surface), boer manual one-liner, changelog `## [2.2.0]`, `bin/2.2.0/release-notes.md`. | R-F06 | TC-14 |

## Phase 4 — Release tooling (this repo) (S)

| # | Task | Satisfies | Verify |
|---|---|---|---|
| 4.1 | **`build_release.ps1` publish step** (new Step 4, after the Step 3.5 placeholder restore): compute SHA-256s, emit `manifest-<v>.json`, upload artefacts + manifest to `ota-store/releases/<v>/` and point the **soak** channel — over **SSH key auth, host-key checking on**, host from git-ignored `.deploy.env`. | R-T01, R-T07 | TC-12 |
| 4.2 | **`bin/ota_promote.ps1`** — soak → mainstream (`channels/<type>.json`); per-unit `pin`/`unpin` subcommands. Separate command from publish (human soak gate). | R-T02 | TC-12 |
| 4.3 | **Provisioning tool** — write `ota_secret` (+ cert if non-default) to a unit via the admin `/api/ota/config` endpoint over the bench LAN, and create/update its `devices.json` row in one documented step (row edit shipped as a server-repo helper). | R-T03, R-I02 | TC-12 |

## Phase 5 — Verification on soak unit 2344 (M, over a soak week)

Runs the TDS §3 case matrix against a **staging channel**. Each row is a gate, not a nicety.

| # | Task | Cases |
|---|---|---|
| 5.1 | Feature-off regression + push-OTA still works on 2.2.0. | TC-01 |
| 5.2 | Cert lifecycle: embedded default → GUI upload → reboot persistence. | TC-03 |
| 5.3 | Scheduler/precondition/backoff soak; no-update cycles cost nothing. | TC-04 |
| 5.4 | Fault injection: corrupted/truncated artefacts → no flash write. | TC-05 |
| 5.5 | Kill-power matrix (mid-download / mid-flash / post-commit). | TC-06 |
| 5.6 | Window + quiet-gate: forced sessions, wind override, motion during window. | TC-07 |
| 5.7 | Downgrade / replay / min_version from staging. | TC-08 |
| 5.8 | Heap-floor (≥ 20 KB) + control cadence during full update. | TC-09 |
| 5.9 | Full log parse + dashboard field check; zero `value_a=13` from pull cycles. | TC-10, TC-13 |
| 5.10 | 48 h server-down soak; crash-loop rollback. | TC-13 |
| 5.11 | ≥ 1 week nightly-check soak before production exposure. | (soak gate) |

## Phase 6 — Production rollout (5C88)

| # | Task | Satisfies |
|---|---|---|
| 6.1 | Site visit: install 2.2.0 locally (`ota_push.py`), provision `ota_secret` + cert, create the registry row, enable ROTA **pinned to 5C88's current version**. The visit that ends visits. | R-T05 |
| 6.2 | First real remote update = the next release (2.2.1): publish → soak channel → 2344 overnight pull → `ota_promote` to mainstream → watch 5C88's dashboard `rota_*` fields next morning. | R-T02, R-O03 |
| 6.3 | Update CLAUDE.md release cycle step 8 (production no longer queues for a farm visit); keep `ota_push.py` as bench/recovery path. | — |

## Phase 7 — Deferred hardening (explicitly out of scope for 2.2.0)

Firmware/manifest signing (R2/R-A10; `key_id` reserved), mTLS client certs (R-A11; nginx path confirmed), DS-peripheral key storage, NVS encryption, device-initiated secret rotation, GitHub-Releases artefact offload.

## Critical path and risks

**Critical path:** 0.1 (contract freeze) → 1.x + 2.x (server, in parallel with firmware) → 3.4–3.8 (client core) → 5.x (soak) → 6.1 (site visit). Server and firmware proceed in parallel once §4 is frozen; the device simulator (2.1) lets the server reach "done" with no ESP32.

**Top risks, watch early:**
1. **TLS heap serialization with T14** (R-C07/R-R05, gh#23 ~31 KB budget) — validate in task 3.4/3.5, not at soak. The sharpest technical risk.
2. **FW_DONE fallback must never fire on the pull path** (R-R04) — a `value_a=13` row during Phase 5 means the pairing logic has a bug; treat as a stop-ship.
3. **Clock validity load-bearing** for the night window (R-P01) — mitigated by the 2.1.3 RTC-over-NTP fix already on main; T16's NTP precondition (R-C03) is the backstop.
   - **Decision (2026-07-13):** T16's R-C03 gate stays on the strict `nm_is_sntp_synced()` latch (not a relaxed "wall-clock ≥ 2025" check). The operator confirmed both the soak and the 5C88 production networks currently allow outbound NTP (UDP/123), so a valid clock is always reached within ≤5 min of boot (rc.1.5.6 retry). The device-side gate is only a "don't waste a request on an untrusted clock" optimisation — the actual auth-freshness control is server-side (`ROTA_SKEW_S=300` + nonce + HMAC). **Revisit trigger:** a future deployment site that blocks outbound NTP but allows HTTPS → then relax the gate to accept RTC-backed time (degrades gracefully to a harmless 204).
4. **Two-repo drift** — the frozen §4 contract + the server repo pinning the TDS tag (R-T06) is the guard; the device simulator catches contract mismatches before hardware.

## Effort summary

| Phase | Effort | Blocking? |
|---|---|---|
| 0 Contract + prereqs | S | gates everything |
| 1 Server | M | parallel with 3 |
| 2 Simulator | S | gates 1-accept |
| 3 Firmware client | L | critical path |
| 4 Release tooling | S | before 5.9 rehearsal |
| 5 Soak verification | M (calendar: ≥ 1 wk) | gates 6 |
| 6 Production | site visit + 1 release cycle | — |

Roughly **3–4 calendar weeks** for one engineer, dominated by Phase 3 and the Phase 5 soak.

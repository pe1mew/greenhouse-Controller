# Remote OTA study — internet-pull OTA for the greenhouse controller

**Status:** study / decision document — no implementation yet
**Date:** 2026-07-04
**Related:** [OTAimplementation.md](OTAimplementation.md) (current push OTA), [tls_leak_audit.md](tls_leak_audit.md) (pre-migration TLS audit), `memory/architecture.md` (task graph)

Effort scale used throughout: **S** < 8 h · **M** 1–3 days · **L** 1–2 weeks · **XL** multi-week or hardware change.

---

## 1. Executive summary

The controller today can only be updated by an operator PC on the same LAN (push OTA via `bin/ota_push.py`). This study designs the pull counterpart: a new FreeRTOS task that periodically asks an OTA server on the internet "is there a newer release for me?", downloads it, verifies it, and applies it — with the same paired firmware+assets atomicity the push path enforces.

Direct answers to the commissioning questions:

- **New task:** yes — **T16**, a permanent low-priority task that *reuses* the existing T13 ingestion machinery, preserving the paired-commit invariant by construction (§4).
- **GUI:** an admin-only "Internet OTA" group in the existing System-tab OTA section — enable checkbox, check-interval (hours), server URL, device secret (§4.5).
- **Mutual identification — is asymmetric key an option?** Yes, three ways (mTLS with software key, with the ESP32-S3 DS peripheral, with a secure element). But in stage 1 the highest-value use of asymmetric crypto is the **artefact signature, not the transport**: a signed release manifest + signed app image, with the private key offline on the build PC, protects the fleet even if the VPS itself is compromised. Recommended stage-1 combo: **O6 (signed artefacts) + O2 (per-device HMAC identity)** (§5).
- **Firmware signing — possible?** Yes, twice over: ESP-IDF signed-app verification on OTA write (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`) — software-only, zero eFuse burns, fully reversible, effort S — plus the signed release manifest that also covers the web-assets ZIP, which today has no integrity check at all (§7).
- **Server — PHP or dedicated software?** **PHP is entirely adequate** at 2–20 units and matches the stack already running at rfsee.net; two small PHP files next to the existing `api.php`, artefacts outside the webroot, flat-file device registry. A dedicated service is not justified below ~50 units; the only capability PHP cannot deliver alone is mTLS termination (web-server/vhost config — access level unconfirmed, open question) (§8).
- **Key management for a fleet:** per-device HMAC keys in a server-side registry (revocation = one row edit); a tiny in-house CA only when mTLS arrives; the firmware-signing key lives on the build PC and **never on the VPS** (§9).
- **Scalability / maintainability:** nothing structural changes until ~50 units; the stage-1 design (manifest format, channels, registry, publish pipeline) survives later upgrades of transport auth and server runtime (§9.5).

**Phased roadmap:**

| Stage | Content | Effort | Prereq |
|---|---|---|---|
| **0 — harden now** | Enable cert-date checking (`CONFIG_MBEDTLS_HAVE_TIME_DATE`); signed-apps-on-update Kconfig + offline key + `build_release.ps1` sign/verify step. Hardens the *existing push path* too; independent of everything else | S–M | none |
| **1 — pull OTA** | T16 task + manifest flow + signed release manifest + per-device HMAC + PHP server (manifest/download/registry/channels) + NVS/GUI + publish & promote scripts + full test plan on 2344 | L (client) + M (server) | Stage 0 |
| **1.5 — hardware-backed identity** | DS-peripheral-held mTLS client key (RSA-3072; no BOM change, bench provisioning per unit) + NVS encryption (HMAC-eFuse scheme); mTLS contingent on VPS vhost access | M | Stage 1, bench access |
| **2 — hardware roadmap** | ATECC608 secure element (next board revision), Secure Boot V2, flash encryption | XL | new hardware / changed threat model |

---

## 2. Current state

- **Push OTA only.** `ota_push.py` logs in with the admin PIN, POSTs the app binary (~1.36 MB) to `/api/ota/firmware`, then the web-assets ZIP (~108 KB) to `/api/ota/assets`. T13 (`ota_manager`) stages, verifies size/SHA-integrity, extracts assets into the passive LittleFS partition and atomically swaps both banks. A 120 s `FW_DONE` fallback timer commits firmware alone if assets never arrive — the known stranded-assets failure mode.
- **No cryptographic authenticity.** `esp_ota_end()` checks the image header and appended SHA-256 digest — integrity, not authenticity. Anyone with the admin PIN can flash arbitrary code ([OTAimplementation.md](OTAimplementation.md) says this explicitly). The assets ZIP has no check at all: `extract_zip_store()` (ota_manager.cpp:749-834) parses local-file headers but never reads or verifies the CRC-32 field.
- **Outbound HTTPS already works.** T14 POSTs status JSON every ~120 s and uploads SD logs daily to `https://rfsee.net/hbwv/api.php` using `esp_http_client` + the mbedTLS CA bundle. Certificate **expiry is not checked** (`CONFIG_MBEDTLS_HAVE_TIME_DATE` unset).
- **Device auth today is one fleet-wide secret** (`sourceidentifier` header) — no per-unit identity, no replay protection. The production unit 5C88 is outbound-only (NAT); rfsee.net is its only communication path.
- **Fleet:** 5C88 (production) + 2344 (soak). Release discipline: build → OTA soak → overnight → production.

## 3. Requirements traceability

| # | Requirement (user ask) | Answered in |
|---|---|---|
| 1 | FreeRTOS pull-OTA task | §4 |
| 2 | GUI: enable checkbox, interval, server URL | §4.5 |
| 3 | Mutual identification; asymmetric keys; stage 1 sw / stage 2 secure element | §5, §7.5–7.6 |
| 4 | Options and risks | §5, §6 |
| 5 | CIA triad + best practices | §6 |
| 6 | Effort ratings | every option + §10 |
| 7 | Firmware signing | §7 |
| 8 | Controller impact analysis | §4.7 |
| 9 | Server impact; PHP vs dedicated | §8 |
| 10 | Key management, multiple controllers | §9 |
| 11 | Scalability, small-company maintainability | §8, §9.5 |

---

## 4. Client-side design: the remote-OTA task

### 4.1 Task placement — new T16, not an extension of T13 or T14

**Recommendation: a new permanent task T16 (`T16-rota`).** T16 is the next free task number (T1–T15 are all assigned; T12 declared-but-disabled, T15 dormant). Rationale against the alternatives:

- **T13 is a worker, not a scheduler.** It has no global handle and is spawned on demand by `ota_assets_end()` (ota_manager.cpp:622-629) to do one ZIP extraction and die. Turning it into a permanent polling task would change the lifecycle every push-OTA code path assumes. Instead T16 *reuses* T13's machinery as its backend (§4.3).
- **T14 extension couples the wrong cadences.** T14 wakes every `status_interval_s` (60–300 s); OTA checks run on an hours scale. Worse, T14's failure-escalation ladder (5 fails → DHCP renew, 10 → reassociate, status_post.cpp:141-174) would start counting OTA-server outages as status-post failures. Failure isolation argues for a separate task.

**Proposed creation block** (mirrors T14, main.cpp:1475-1491): stack **8192 B** — T14 does its TLS work on 8192 B with a documented ~5 KB handshake peak + 3 KB margin (main.cpp:1457-1459), and T16's HTTP work is the same shape; priority **3** (network band, alongside T10/T13/T14 — below all control tasks); `tskNO_AFFINITY`. Handle `task_t16` in system_globals.cpp, extern in app_types.h. Like T14, heap-allocate working buffers rather than growing the stack.

### 4.2 Check-update flow

```
sleep(interval ± jitter) → preconditions gate → GET manifest → semver decision
  → download BOTH artefacts to PSRAM → verify → feed T13 machinery → quiet-window reboot
```

1. **Interval + jitter.** `cfg.ota_check_h` hours, ±10 % uniform jitter per cycle so a growing fleet never checks in lockstep against rfsee.net. Config changes take effect via an `xTaskNotify(task_t16, T16_NOTIFY_CFG_CHANGED)` hook cloned from `dm_reload_web_cfg()` (data_manager.cpp:1540-1564).
2. **Preconditions gate (check phase):** `ota_enable==1` and `ota_url[0]!=0`; WiFi STA up (T10 state); `nm_is_sntp_synced()` true (network_manager.cpp:321) — mandatory once cert-expiry checking is enabled (§4.4 step 1); `EG1_BIT_OTA_IN_PROGRESS` (bit 4, app_types.h:525) clear — a push OTA in flight wins. Any gate failure → audit "skipped" row, retry next cycle.
3. **GET `<ota_url>/manifest.php`** using the exact `do_status_post()` client config (status_post.cpp:369-379): `HTTP_TRANSPORT_OVER_SSL`, `esp_crt_bundle_attach`, `timeout_ms=10000`, `skip_cert_common_name_check=false`, plus the per-device HMAC auth header (`X-OTA-Auth`, §5 O2). The manifest is the signed release manifest of §7.7.
4. **Decision:** semver-compare manifest `fw_version` against the compile-time `FIRMWARE_VERSION` (platformio.ini). Equal or older → log and sleep (anti-downgrade, enforced again later). Newer → proceed.
5. **Download to PSRAM, never straight to flash.** Firmware (~1.36 MB) and asset ZIP (~108 KB) both staged in `heap_caps_malloc(MALLOC_CAP_SPIRAM)` buffers — the push path already stages the ZIP this way (ota_manager.cpp:575) and 8 MB PSRAM makes 1.5 MB trivial. Download loop mirrors `do_log_upload()`'s 4 KB-chunk pattern in reverse (status_post.cpp:593-618).

### 4.3 Integration shape — (a) internal client feeding T13's entry points. Recommended.

| | (a) Reuse T13 ingestion | (b) Parallel `esp_https_ota` path |
|---|---|---|
| Paired-commit invariant | Preserved **by construction**: T16 calls the same `ota_firmware_begin/write/end` → `ota_assets_begin/accumulate/end` sequence the HTTP handlers call; `ota_assets_begin` already accepts entry from `FW_DONE` (ota_manager.cpp:551-553); the deferred boot-swap at end of T13 extraction (cpp:1002-1013) activates fw+assets atomically | **Bypassed**: `esp_https_ota_finish()` calls `esp_ota_set_boot_partition()` itself — new fw would activate with old assets; no asset concept at all, pairing logic must be reimplemented |
| Mutual exclusion with push OTA | Free: `ota_firmware_begin()` rejects state ≠ IDLE/ERROR (cpp:427-433), `EG1_BIT_OTA_IN_PROGRESS` set/cleared as today | Must be re-built |
| Audit codes 13-17, `/api/ota/status`, LCD " OTA" suffix | All keep working unchanged | Parallel state to surface |
| What we give up | Ranged/resumable download and the library's app-header version peek | — |

The give-ups are cheap: at farm-WiFi/cell rates a 1.36 MB download takes 1–5 min, so "retry from scratch" beats maintaining resume state; and the version check is done better by our own manifest semver compare plus reading the staged image's `esp_app_desc_t`. Because both artefacts are fully staged in PSRAM *before* `ota_firmware_begin()`, feeding fw-then-assets back-to-back takes seconds — the 120 s `FW_DONE_FALLBACK_MS` timer (cpp:100) can never realistically fire on the pull path; it stays armed as a safety net, and its `value_a=13` audit row would flag the anomaly.

### 4.4 Verification pipeline (client side)

Ordered; any failure aborts **before flash is touched** (no-partial-apply rule):

1. **Transport TLS** — CA-bundle chain + CN check as today. Gap to close: `CONFIG_MBEDTLS_HAVE_TIME_DATE` is currently **not set** (sdkconfig.lolin_s3:1887-1889), so certificate expiry is not checked. Enable it in `firmware/sdkconfig.defaults`; the DS1307 pre-seed + SNTP quick-sync make wall time reliable, and the gate in §4.2 step 2 requires NTP sync anyway.
2. **Manifest authenticity** — RSA-3072 signature over the manifest, made with the *same offline key* as the app-image signing key (§7.7), verified with `mbedtls_pk_verify` against the public key compiled into the firmware. HMAC-SHA256 with the per-device secret is the weaker fallback if signing is deferred.
3. **Per-artefact SHA-256** — computed over each staged PSRAM buffer, compared to the manifest values, before any `ota_firmware_begin()`.
4. **App image signature** — with `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y` (currently not set, sdkconfig.lolin_s3:488) the Secure-Boot-V2-format signature block appended to the .bin is verified during `esp_ota_end()` (cpp:493) — the same call that does the SHA/header check today. Details in §7.1.
5. **Anti-downgrade** — manifest `seq`/semver check plus cross-check of the staged image's embedded `esp_app_desc_t.version`, with an NVS high-water mark (`system/fw_hiwater`, §7.4). eFuse-counter anti-rollback is deliberately **not** used (§7.4).

**Failure handling:** abort → free PSRAM buffers → audit row (§4.6) → exponential backoff 1 h → 2 h → 4 h … capped at 24 h, reset on next success. Persistent verify-failures are visible on the rfsee.net dashboard via the status-JSON field (§4.6) — essential for 5C88, which has no inbound path.

### 4.5 Reboot policy

Apply (flash feed + commit) and reboot are gated on a **quiet window**, re-checked immediately before `schedule_reboot()`:

- `EG1_BIT_CALIBRATING` clear and all channels reported idle by `t2_get_window_states()` (no `WIN_MOVING_*`) — rebooting mid-travel leaves NVS state ≠ CLOSED and forces the ~171 s live CLOSE_ALL sweep at next boot;
- `EG1_BIT_WIND_OVERRIDE` clear (an active protective close must finish first) and `EG1_BIT_MOTOR_ALARM` clear;
- no active web session slot and no LCD PIN session (T8) — minimal "manual session" check.

If not quiet: poll every 60 s; if still busy after 30 min, release the OTA state (stay on old bank — nothing was committed) and retry next check cycle, audit "deferred". Once committed by T13, reboot goes through the existing two-stage `schedule_reboot()` → `reboot_worker_task` pattern (ota_manager.cpp:195-238) with the 1 s asset-success delay — no new mechanism. Farmer notification: none beyond the existing LCD " OTA" suffix driven by `EG1_BIT_OTA_IN_PROGRESS` (ui_display.cpp:926); an update applied in a quiet window is invisible by design.

Rollback after a bad update is the existing 3-strike NVS counter (`ota_check_rollback()`, cpp:352-410) + `ota_mark_healthy()` at 30 s uptime; bank-paired LFS rolls assets back with the bank. No new code (§7.4).

### 4.6 New NVS params + GUI (admin-only)

Namespace **`system`**, following the T14 `status_*` precedent (data_manager.cpp:114-123). All keys ≤ 15 chars:

| Key | Type | Default | Clamp | LOG_PARAM |
|---|---|---|---|---|
| `ota_enable` | i32 0/1 | 0 | 0–1 | 39 (old→new) |
| `ota_check_h` | i32 hours | 24 | 1–168 | 40 (old→new) |
| `ota_url` | str ≤128 (`CFG_MAX_URL_LEN`) | "" (= disabled) | `https://` only | 41 ("set" marker, value never logged) |
| `ota_secret` | str 16–64 | "" | — | 42 ("set" marker) |

`ota_secret` is the per-device HMAC key of §5 O2/§9.2 — **one location, one name**: `system/ota_secret`, provisioned via the endpoint below (see §9.2 for the provisioning flow; the firmware has no serial console, so a console-based injection path would be new scope).

Touch points, exactly per the established pattern: `K_` constants + load lines in a `nvs_load_ota()` called beside `nvs_load_web()` (data_manager.cpp:518-532); `cfg_shadow_t` fields cloned from the `status_*` group (data_manager.h:146-155); defaults in `firmware/config/cfg_defaults.h`; limits in `cfg_limits.h`; `cfg_clamp()` + `ns_key_to_log_id()` + `apply_config_update()` branches for the two ints; `LOG_PARAM_*` 39–42 in app_types.h (next free = 39) **plus the matching `log/logparser.py` update in the same changeset** (CLAUDE.md hard rule).

**Write path:** a dedicated admin transaction endpoint `GET/POST /api/ota/config`, cloned from `web_get_handler`/`web_post_handler` (web_server.cpp:2325-2360 / 2383-2532) — mandatory because `ota_url` (128) exceeds the 79-char `str_value` cap of `POST /api/config` (web_server.cpp:1257). Same discipline: validate all fields before any write, `https://`-only URL check (server-side clone of :2424-2446, client-side clone of `validateStatusUrl()`, app.js:1009-1017), "empty secret = keep", per-field audit rows, then shadow reload + `xTaskNotify(task_t16, …)`.

**GUI:** extend the existing System-tab OTA section (index.html:427-448) with an "Internet OTA" group — checkbox (`cfg-web-enable` idiom, index.html:502), number input for interval hours, `<input type="url" maxlength="128">` + password-style secret field (`cfg-web-url`/secret idiom, :489-495), one Apply button via a `postOtaCfg()` clone of `postWebCfg()` (app.js:1019-1057), and a read-only "last check / result" line fed from `/api/ota/status`. Everything carries `class="admin-only"`; server-side the new endpoint uses `admin_only_or_send_error()`, and none of the keys enter `FARMER_KEYS`/`FARMER_WIND_KEYS` (web_server.cpp:1030-1036) — farmers can neither see nor set any of it.

### 4.7 Observability

New `LOG_SYSTEM` `value_a` codes — next free is **21** (highest allocated today is 20, event_logger.h:110-133):

| value_a | Event | value_b |
|---|---|---|
| 21 | Remote-OTA check ran | 0=no update, 1=update found, 2=server unreachable/HTTP error, 3=preconditions skip |
| 22 | Download/verify outcome | 0=ok, 1=TLS fail, 2=SHA mismatch, 3=manifest sig/HMAC fail, 4=downgrade rejected |
| 23 | Apply outcome | 0=committed, reboot scheduled, 1=deferred (motors/session busy), 2=apply failed |

Codes 13–17 keep firing unchanged since the T13 machinery is reused. Table lives in event_logger.h; `log/logparser.py` learns 21–23 in the same commit. Status JSON (`build_canonical_status_json()`, status_json.cpp:131) gains `rota_state`, `rota_last_check` (epoch), `rota_last_result` — the only way the engineer sees 5C88's remote-OTA health.

### 4.8 Controller impact analysis

| Resource | Cost | Notes |
|---|---|---|
| Flash (code) | ~5–8 KB task + manifest/semver code; **no `esp_https_ota`** under shape (a); +~10–15 KB when signed-app verify is enabled | Bank is 1.36 MB / 2 MB (~66 %) — ample |
| Internal RAM | 8 KB T16 stack (permanent) + ~30–40 KB transient per TLS handshake, largest-block dip −36 KB observed (gh#23 forensics) | **Must serialise with T14**: steady-state largest block is only ~31 KB with T14 in flight. One `s_tls_mx` (or T16 skipping its slot while T14 posts) — never two concurrent handshakes |
| PSRAM | ~1.4 MB fw staging + ~108 KB ZIP, transient | Against ~8 MB free — negligible; matches push-path precedent |
| Scheduling | Download 1.36 MB at 50–500 KB/s (cell-NAT uplink) ≈ 3 s–5 min; flash feed ~10–20 s | Prio 3 keeps T2/T3/T6 unaffected; T16 does not register with TWDT (T14 precedent); chunked loop yields naturally on socket reads. T14 status posting continues except during the serialised TLS sections |
| Push-OTA coexistence | Free | `ota_firmware_begin()` state guard + `EG1_BIT_OTA_IN_PROGRESS`; push always wins if first |

Failure modes:

| Failure | Outcome | Mitigation |
|---|---|---|
| Power loss mid-download | PSRAM only; old bank boots | Retry next cycle, nothing to clean |
| Power loss mid-flash-write | Passive bank half-written, boot slot never swapped | Old bank boots; next attempt's `esp_ota_begin()` re-erases |
| Power loss after commit, before reboot | Boot slot already swapped, assets already extracted (T13 order: extract → swap) | Boots new fw + new assets — safe by T13's ordering |
| Stranded assets (fw commits alone) | Eliminated by construction (both artefacts staged pre-begin) | `FW_DONE` 120 s timer retained as net; `value_a=13` flags anomaly |
| Bad TLS / cert failure | Check or download aborts | Audit 22/1, exponential backoff, dashboard visibility |
| Server down | Check fails | Audit 21/2, backoff cap 24 h; zero impact on climate control |
| Tampered image (SHA/sig fail) | Rejected before flash touch | Audit 22/2-3; no partial apply |
| New fw crash-loops | 3-strike counter reverts bank + paired LFS | Existing `ota_check_rollback()`; T15-planned-reboot exemption unaffected |

### 4.9 Testing plan and effort

All on soak unit 2344 before any 5C88 exposure (CLAUDE.md rule), against a staging URL on rfsee.net:

1. Happy path: publish manifest, verify post-reboot `/api/status` shows **both** `fw_ver` and `asset_version` updated.
2. Kill-power at three points: mid-download, mid-flash-write, post-commit/pre-reboot — assert the outcomes in the failure table.
3. Wrong signature / corrupted SHA / tampered manifest → flash untouched, audit 22/x rows present.
4. Downgrade: serve an older `fw_version` → rejected (21/0, or 22/4 if forced past the manifest gate).
5. Server-unreachable soak ≥ 48 h: backoff cadence in SD log, heap drift < 5 KB via T1 rows (`value_a` 7/8/12).
6. Concurrency: start a push OTA during a pull check and vice versa → busy-guard rejects cleanly.
7. Deferral: assert `value_a=23/1` when wind override or calibration is active during the apply window.

| Scope | Effort |
|---|---|
| Stage 1 — T16 + check/download/apply flow reusing T13 | M–L |
| Stage 1 — NVS params, `/api/ota/config`, GUI, logparser | M |
| Stage 1 — verify pipeline (manifest sig, SHA, signed-app Kconfig) + full test plan on 2344 | M |
| **Stage 1 client total** | **L** |
| Stage 1.5 — DS-peripheral key storage (`CONFIG_ESP_TLS_USE_DS_PERIPHERAL=y` already compiled), NVS encryption | +M–L incremental |

---

## 5. Mutual authentication options

The requirement splits into two independent questions, and keeping them separate is the backbone of this section:

1. **Transport authentication** — is the device talking to the real server, and is the server talking to a real fleet unit? (Options O1–O5, a ladder of increasing strength.)
2. **Artefact authentication** — is this binary genuine RFSee firmware, regardless of what pipe it arrived through? (O6, orthogonal to the ladder.)

The distinction matters because the signing key for O6 lives **offline on the build PC** and never touches the VPS. Even with the weakest transport option, a fully compromised rfsee.net cannot produce runnable firmware. Conversely, even perfect mTLS does not help if the server itself is hostile. [RFC 9019](https://www.rfc-editor.org/info/rfc9019) (IETF SUIT architecture) makes exactly this split: end-to-end security lives in the signed manifest, not the transport.

Baseline for calibration: today's `api.php` device-auth is a single fleet-wide `sourceidentifier` header (status_post.cpp:393-395) — no per-unit identity, no replay protection — and the OTA endpoints have no internet exposure at all. Every option below is an improvement.

### O1 — HTTPS server-auth + static per-device API key (baseline)

- **How:** keep the existing `esp_crt_bundle_attach` TLS client (the T14 pattern, status_post.cpp:369-379); add header `X-Device-Key: <unit_id>:<key>`. Server looks the key up in a per-device table.
- **Client stores:** 32-byte random key in NVS. **Server stores:** `unit_id → key-hash` table.
- **CIA:** C — key protected in transit by TLS; I — server identity via CA bundle only; device identity is bearer-token grade (whoever holds the key *is* the device); A — no impact.
- **Effort: S.** **Stage 1: yes.** Strictly better than today because the key is per-device → revocable per-unit.

### O2 — HTTPS server-auth + per-device HMAC request signing

- **How:** device signs the request with HMAC-SHA256 (`mbedtls_md_hmac`, already in the build) and sends one header: `X-OTA-Auth: <unit_id>:<ts>:<nonce>:<mac>` where `mac = HMAC-SHA256(key, unit_id|ts|nonce|request-URI)`. Server recomputes against its registry, rejects clock skew > ±5 min, and caches nonces (pruned after 10 min) to kill replays. This exact wire format is what the PHP sketch in §8.3 verifies. Device wall time is already trustworthy (DS1307 pre-seed + SNTP, network_manager.cpp:311-393).
- **Client stores:** 32-byte HMAC key in NVS (`system/ota_secret`, §4.6). **Server stores:** per-device key table + small nonce cache (flat file).
- **CIA:** C — key never appears on the wire at all (unlike O1's bearer key); I — requests are tamper-evident and replay-proof, genuine mutual *identification*; A — no impact.
- **Effort: M.** **Stage 1: yes.**

### O3 — Mutual TLS, per-device client certificate, software-stored key

- **How:** yes — **asymmetric keys are an option today, in software.** A tiny in-house CA (§9.3) issues one cert per unit, CN = `unit_id`. Device presents it via `client_cert_pem`/`client_key_pem` in `esp_http_client_config_t`; web server enforces `SSLVerifyClient require` (Apache) / `ssl_verify_client on` (nginx); PHP reads `SSL_CLIENT_S_DN_CN`.
- **Client stores:** device private key + cert in NVS — plaintext at rest today; the mitigation is **NVS encryption** (HMAC-eFuse scheme, §7.3 — flash encryption does *not* cover NVS). **Server stores:** the CA cert + an allowlist; no per-device secrets (a real advantage — a server breach leaks no device credentials).
- **CIA:** C/I — strongest transport story, identity bound to a key the server never holds; A — caveat: no heap audit exists for client-cert paths in the current esp-tls stack. The only TLS heap audit on file ([tls_leak_audit.md](tls_leak_audit.md):95-99) predates the ESP-IDF migration and covered the retired Arduino WiFiClientSecure stack — a fresh audit is required before enabling mTLS, given the ~31 KB largest-block budget (gh#23).
- **Effort: M–L** (CA tooling, cert lifecycle, web-server config — actual Apache/nginx stack on the VPS is unconfirmed). **Stage 1: yes.**

### O4 — mTLS with key in the ESP32-S3 DS peripheral (stage 1.5)

- **How:** same as O3, but the RSA private key is stored AES-encrypted in flash, decryptable only by a key the HMAC peripheral derives from a read-protected eFuse key block; the Digital Signature peripheral signs internally and the plaintext key never exists in readable flash or RAM. `CONFIG_ESP_TLS_USE_DS_PERIPHERAL=y` is **already compiled in** (sdkconfig.lolin_s3:764); glue via `esp_secure_cert_mgr` and `ds_data` in esp-tls. **Note: the DS peripheral is RSA-only** — client certs on this path must be RSA-3072, not ECDSA (§9.3).
- **Client stores:** encrypted key blob + cert; eFuse HMAC key (write-only). **Server stores:** as O3.
- **CIA:** as O3, plus the key survives full flash dumps — hardware-backed with **zero BOM change**.
- **Effort: M–L incremental over O3.** Provisioning burns eFuses over serial → physical access; fine for 2344, needs a site visit for 5C88. **Stage 1: no (stage 1.5).**

### O5 — ATECC608 secure element (stage 2)

- **How:** I2C ATECC608 holds an ECDSA P-256 key generated inside the element; `CONFIG_ESP_TLS_USE_SECURE_ELEMENT` + `esp-cryptoauthlib`. Adds tamper resistance, monotonic counters, and a factory-provisioning chain the DS peripheral lacks.
- **Client stores:** nothing extractable — key never leaves the element. **Server stores:** as O3.
- **CIA:** marginal gain over O4 for this threat model.
- **Effort: XL** — hardware change, and fielded units need rework. **Stage 1: no.** Honest recommendation: with the DS peripheral free on every S3, the ATECC608 is only worth designing in at the **next board revision**; do not retrofit for a 2-unit fleet.

### O6 — Signed manifest + signed firmware (orthogonal, applies under any of O1–O5)

- **How:** `build_release.ps1` gains a signing step: the release manifest (§7.7 — version, monotonic `seq`, SHA-256 + size of **both** the app bin and the assets zip, channel, min-version) is signed with the **offline RSA-3072 key** — the same key that signs the app image, so there is exactly one key to custody. Verified on-device with `mbedtls_pk_verify` (already in the build; mbedTLS has no Ed25519, and a second algorithm buys nothing here). Structure per [RFC 9124](https://www.rfc-editor.org/rfc/rfc9124.html). Complemented by ESP-IDF app-image signing (§7.1): `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` verifies a Secure Boot V2 RSA-PSS signature block on every OTA write **without burning any eFuse**; full Secure Boot V2 can follow later, irreversibly. The app signature covers only the app image — the manifest is what covers the assets zip and binds the pair.
- **Client stores:** one public key (in the image). **Server stores:** nothing secret — it is a dumb artefact host.
- **CIA:** I — end-to-end firmware authenticity independent of transport, server, and DNS; this is the only option that survives a compromised VPS. **This answers "is firmware signing possible?" — yes, twice over.**
- **Effort: M.** **Stage 1: yes.**

### Summary and recommendation

| Option | Client stores | Server stores | Blocks rogue server? | Effort | Stage 1 |
|---|---|---|---|---|---|
| O1 API key | key in NVS | key table | No | S | Yes |
| O2 HMAC signing | key in NVS | key table + nonce cache | No | M | Yes |
| O3 mTLS (sw key) | key+cert in flash | CA cert + allowlist | No | M–L | Yes |
| O4 mTLS (DS periph) | encrypted blob + eFuse | CA cert + allowlist | No | M–L | Stage 1.5 |
| O5 ATECC608 | nothing extractable | CA cert + allowlist | No | XL | Stage 2 |
| O6 signed artefacts | 1 public key | nothing | **Yes** | M | Yes |

**Recommendation — stage 1: O6 + O2**, plus enabling `CONFIG_MBEDTLS_HAVE_TIME_DATE` (cert expiry checking is compiled out today, sdkconfig.lolin_s3:1889). O6 carries the actual security load; O2 gives per-device identity and replay protection with no server TLS reconfiguration and no CA to operate. The direct answer to "is asymmetric key an option": yes — and in stage 1 the highest-value use of asymmetric crypto is the **artefact signature**, not the transport. Defer mTLS until there is a reason (fleet growth, per-device artefact entitlements). **Stage 1.5:** if mTLS is adopted, put the key in the DS peripheral (O4) at the next physical touch of each unit. **Stage 2:** ATECC608 only with a board respin.

---

## 6. Risk analysis and CIA evaluation

Framing per [NIST SP 800-193](https://csrc.nist.gov/pubs/sp/800/193/final): *protect* (signatures, auth), *detect* (digest/signature verification, audit rows), *recover* (dual bank, rollback). The dominant assets are **integrity** of what runs in the greenhouse and **availability** of ventilation control; firmware confidentiality is a distant third.

### Threat table

(Threats numbered TH1… to avoid collision with task numbers T1–T16.)

| # | Threat | Path / impact | Primary mitigation | Residual |
|---|---|---|---|---|
| TH1 | Compromised VPS / rogue OTA server | Attacker with rfsee.net access serves malicious firmware to the whole fleet | **O6** — signing key is offline; server cannot mint runnable images | Build PC becomes the crown jewel (see checklist) |
| TH2 | Malicious insider at hosting provider | Same as TH1, plus theft of server-side device-key table (O1/O2) → device impersonation *to the server* | O6 for firmware; per-device keys bound the blast radius; O3/O4 remove server-held secrets entirely | Insider can still drop/serve stale updates (availability) |
| TH3 | MITM on path | Intercept/modify download | Existing CA-bundle TLS + CN check — but **cert expiry is not verified today** (`MBEDTLS_HAVE_TIME_DATE` off); enable it, making DS1307/SNTP load-bearing | O6 backstops any TLS failure |
| TH4 | DNS hijack of rfsee.net | Redirect device to attacker host | Attacker still needs a cert from one of ~200 bundle CAs; optional custom-bundle pinning trades agility for surface | O6 backstops; pinning deferred |
| TH5 | Stolen device credential | Today: one fleet-wide secret = whole-fleet impersonation | Per-device key (O1/O2): revoke one registry row; mTLS: allowlist edit | Physical flash dump reads NVS keys until O4 (DS-held key) or NVS encryption (§7.3) |
| TH6 | Downgrade attack (replay of old, validly-signed firmware) | Old vulnerable release re-served | Manifest monotonic `seq` + device rule: never accept `seq` ≤ running, high-water mark in NVS (§7.4). eFuse anti-rollback exists but is irreversible — software check suffices for stage 1 | Requires the version check to be in the *verified* manifest, not the URL |
| TH7 | Bricked unit mid-update (power loss) | Flash write interrupted | Dual bank: download lands in the passive slot; `esp_ota_set_boot_partition` is atomic. Recovery is the existing 3-strike NVS rollback (`ota_check_rollback()`, ota_manager.cpp:352-410); IDF pending-verify rollback (`CONFIG_APP_ROLLBACK_ENABLE`) is deliberately **not** enabled — its semantics collide with the custom counter (§7.4) | A unit that boots but can't reach WiFi still relies on the local 3-strike fallback; bench-verify the currently-inert `esp_ota_mark_app_invalid_rollback_and_reboot()` call (:398) on 2344 |
| TH8 | Stranded assets (fw committed, assets not) | Existing known failure mode: 120 s `FW_DONE` fallback (ota_manager.cpp:88) | Pull flow fixes this structurally: download **both** artefacts, verify both digests against the manifest, then commit — the manifest binds the pair; post-boot verify `fw_ver == asset_version` exactly as `ota_push.py` does | Keep the fallback timer as last resort, but it should now never fire |
| TH9 | DoS of update server | Devices can't fetch updates | Availability-only: units keep running current firmware. Jittered check interval + exponential backoff (reuse T14's failure counter / ladder pattern, status_post.cpp:141-174) | rfsee.net is already a single point for status; accepted |
| TH10 | Firmware binary disclosure | Attacker downloads the .bin | **Honest assessment: the binary is not a meaningful secret.** No credentials in the image — WiFi PSK, PINs, `status_secret` live in NVS. Risk is vulnerability-hunting convenience. Gate downloads behind O1/O2 auth (cheap) and stop there | The real confidentiality asset is **NVS under physical access** → NVS encryption (§7.3), a separate decision |

### CIA matrix

| Configuration | Confidentiality | Integrity | Availability |
|---|---|---|---|
| Today (fleet secret, no signing, local push only) | Weak — shared secret, plaintext NVS | Weak — SHA-256 only, any admin-PIN holder flashes anything | Good — dual bank, but stranded-assets mode open |
| O1 only | Adequate — TLS + bearer key | Poor — trusts server entirely | Good |
| O2 only | Good — key never on wire | Fair — transport tamper-proof, server still trusted for content | Good |
| **O2 + O6 (stage-1 pick)** | Good | **Strong — end-to-end signed pair, anti-downgrade** | Good — pair-atomic commit closes TH8 |
| O3 + O6 | Good — no server-held device secrets | Strong | Fair — un-audited mTLS heap vs ~31 KB budget |
| O4/O5 + O6 (stage 1.5/2) | Strong — key survives flash dump | Strong | Fair — same heap caveat |

### Best-practice checklist mapped to this project

- **SUIT-style signed manifest** covering the fw+assets pair, monotonic sequence number, offline key ([RFC 9019](https://www.rfc-editor.org/info/rfc9019) / [RFC 9124](https://www.rfc-editor.org/rfc/rfc9124.html)).
- **App-image signature on update:** set `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` now (no eFuse cost); revisit full Secure Boot V2 deliberately (§7.2).
- **Enable `CONFIG_MBEDTLS_HAVE_TIME_DATE`** so TLS actually checks certificate validity windows.
- **Recovery: keep the existing 3-strike NVS rollback** as the recover mechanism; IDF pending-verify rollback stays off (§7.4). Bench-verify the currently-inert `esp_ota_mark_app_invalid_rollback_and_reboot()` call on 2344.
- **Anti-downgrade** enforced from the verified manifest (`seq` + NVS high-water mark), not URL or filename.
- **Per-device credentials** with a server-side registry and a one-line revocation path.
- **Staged rollout channels** (`soak` → `production`) — formalises the existing 2344-overnight-then-5C88 discipline in server config rather than operator memory.
- **Serialize the pull-OTA TLS session with T14** — two concurrent handshakes against a ~31 KB largest-block budget is the known failure shape from gh#23.
- **Audit every OTA state transition** as `LOG_SYSTEM` rows to SD (check, download, verify-pass/fail, commit, reject-downgrade) and teach `log/logparser.py` the new codes in the same changeset (CLAUDE.md rule).
- **Jittered polling + backoff** on the check interval — protects the VPS and the cell link.
- **Post-update verification** of both `fw_ver` and `asset_version` via the next status POST — the existing honesty rule, now automated.
- **Offline signing key hygiene:** passphrase-protected on the build PC, one offline backup; this key, not the VPS, is now the asset that must never leak. Addresses [OWASP IoT Top 10](https://owasp.org/www-project-internet-of-things/) item 4, *Lack of Secure Update Mechanism*, end to end.

---

## 7. Firmware signing and platform security (ESP32-S3 / ESP-IDF 5.5)

Today the device performs **no cryptographic authentication of firmware images**. `esp_ota_end()` (ota_manager.cpp:493) checks only the image header and the appended SHA-256 *digest* — an integrity check, not an authenticity check. Anyone who can reach an OTA path with admin rights can flash arbitrary code. The moment OTA moves from the LAN to the internet, that gap becomes the single largest risk in this study. The good news: the ESP32-S3 + IDF 5.5 stack has a clean, staged ladder from "software-only signing" to "hardware root of trust", and stage 1 is cheap and fully reversible.

### 7.1 Stage 1 (recommended): signed apps *without* Secure Boot

ESP-IDF can verify an RSA-3072-PSS signature appended to the app image **in software, at OTA-write time**, with no eFuse burns and no bootloader changes:

```
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y          # S3: pick RSA — same block format as Secure Boot V2, forward-compatible with stage 2
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y # verify on OTA write, not on boot
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y      # sign inside `pio run`
CONFIG_SECURE_BOOT_SIGNING_KEY="C:/Users/drasv/keys/hbwv_fw_signing.pem"  # absolute path, OUTSIDE the repo
```

Add these to `firmware/sdkconfig.defaults` (delete `firmware/sdkconfig.lolin_s3` once to force regeneration). Key generation, one-time:

```
espsecure.py generate_signing_key --version 2 --scheme rsa3072 hbwv_fw_signing.pem
# equivalently: idf.py secure-generate-signing-key, or: openssl genrsa -out hbwv_fw_signing.pem 3072
```

Mechanics and consequences, verified against the IDF 5.5 docs:

- The build appends a 4 KB signature sector (1216 bytes used) containing the signature **and the public key**. Image grows 1.36 MB → ~1.37 MB; still ~68 % of the 2 MB bank.
- **One pinned key, no in-band rotation.** In this mode only the *first* signature block is verified — the multi-key story (up to 3 keys) belongs to full Secure Boot V2 with its three eFuse digest slots (§7.2). Rotating the stage-1 key means either a cable flash, or a two-release dance: ship a transition release *signed with the old key* that has verification disabled, then ship the new-key release, then re-enable verification.
- Verification happens in `esp_ota_end()` (and again at `esp_ota_set_boot_partition()`), using the public key embedded in the **currently running app's** signature block. Both call sites are already in T13 (ota_manager.cpp:493, :1002) and in the FW_DONE fallback commit (:292) — so the existing local push OTA, the FW_DONE fallback, and the future pull task all funnel through the same enforcement point with **zero code changes**. An unsigned or wrongly-signed image fails with `ESP_ERR_OTA_VALIDATE_FAILED` → `OTA_STATE_ERROR`, device keeps running the old bank.
- **Trust-on-first-flash**: the first signed release is pushed with today's non-verifying firmware (which accepts anything). From the *next* release onward, only images signed with the same key are accepted over any OTA path. Cable flashing via esptool is untouched — the bootloader does not check signatures in this mode.
- `bin/build_release.ps1` needs one addition: a post-build sanity step `espsecure.py verify_signature --version 2 --keyfile <pubkey> <bin>` so a mis-signed artefact never reaches `bin/X.Y.Z/`. If the key file is missing the build fails loudly — an accidental unsigned release becomes impossible.
- **What it protects against:** malicious or corrupted images arriving over the network — a leaked admin PIN, a compromised or impersonated OTA server, a MITM that somehow beats TLS. **What it does not:** a physical attacker with a USB cable (esptool writes flash freely), rollback to an *older signed* image (§7.4), or extraction of secrets from flash (§7.3).
- **Reversibility: total.** No eFuse is touched; brick risk is zero. One transition gotcha: once a verifying build is running, you cannot OTA back to a plain unsigned build — either cable-flash, or build the "verification off" firmware and *sign it anyway* (the old firmware verifies it, accepts it, and then stops checking).

Key custody for a one-engineer company: the PEM lives outside the repo (it must never land on GitHub), with a copy in a password manager and one offline backup. **Losing it in stage 1 means no OTA until each unit is cable-flashed** (bench for 2344, site visit for 5C88); in stage 2 it would mean units can never accept new firmware at all — one more reason stage 2 is deferred.

### 7.2 Stage 2+: Secure Boot V2

ESP32-S3 Secure Boot V2 uses **RSA-3072-PSS** (SHA-256, MGF1, 32-byte salt — RFC 8017 §8.1.1; the S3 does *not* support ECDSA secure boot, hence the RSA scheme choice above). `CONFIG_SECURE_BOOT=y` makes the ROM verify the (now signed) bootloader against a SHA-256 digest of the public key burned into one of **3 eFuse key-digest slots** (with per-slot `KEY_REVOKEx` revocation bits — this is where true multi-key rotation lives), and the bootloader verifies the app on every boot. This closes the cable-flash hole: esptool can still write flash, but unsigned images will not boot.

Honest assessment for this fleet:

- **Irreversible.** `SECURE_BOOT_EN` is a one-way eFuse. A lost signing key means the unit can never accept new firmware; revoking all three digests after a bad burn bricks it permanently.
- **ROM download mode:** Secure Boot V2 itself leaves UART download mode usable for recovery; optionally harden further by burning `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE` (middle ground) or `CONFIG_SECURE_DISABLE_ROM_DL_MODE` (removes the recovery path an attacker — and you — would use).
- **Dual-bank OTA:** unaffected; both banks simply hold signed images and stage-1 on-update verification remains active on top.
- **Greenfield-flash interaction (project-specific):** the repo's `--flash_mode dio` gotcha relies on esptool *rewriting the bootloader header byte at flash time*. Under Secure Boot V2 the bootloader is signed — a flash-time header patch invalidates the signature and the ROM refuses to boot. The dio setting must move into the build (`CONFIG_ESPTOOLPY_FLASHMODE_DIO`) and esptool must flash with `--flash_mode keep`. This must be sorted *before* any eFuse is burned.
- **Verdict:** for 2 units in a locked greenhouse cabinet where the threat is network-borne, the brick risk and process burden outweigh the marginal gain over stage 1. Revisit if fleet size grows or units land in less controlled locations. Effort M, but the cost is really the permanent operational liability, not the hours.

### 7.3 Flash encryption and NVS encryption

**Flash encryption** (`CONFIG_SECURE_FLASH_ENC_ENABLED=y`; S3 supports XTS-AES-128/256) encrypts the app image and partitions marked `encrypted` with an eFuse-held key. **It does not cover NVS**: the NVS partition cannot be flash-encrypted (the NVS library is incompatible with transparent flash encryption and has its own scheme). So flash encryption protects firmware code and the `esp_secure_cert` partition at rest — it does *not* protect the WiFi PSK, admin PIN, `status_secret` or a future OTA key sitting in NVS. Development mode allows re-flashing plaintext; release mode is one-way and changes the greenfield procedure. Not justified while the units sit in a locked cabinet. Effort M, moderate irreversibility risk.

**NVS encryption is the actual at-rest protection for secrets.** IDF 5.5 offers two key-protection schemes: `CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC` (needs an extra 4 KB `nvs_keys` partition *and* flash encryption) and — the right fit here — **`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`** with `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID`, which derives the NVS XTS keys from an eFuse HMAC key and **works without flash encryption**. That gives encrypted credential storage (WiFi PSK, PINs, `ota_secret`, a software mTLS key) for one burned eFuse key block and no partition-table change. The natural stage-1.5 companion to the DS peripheral. Effort S–M.

### 7.4 Anti-rollback

The eFuse mechanism (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, embedding a secure-version into the app descriptor, compared against a monotonic eFuse counter) gives at most **16 increments ever** on the S3, burns eFuses, and — critically — requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, which switches `esp_ota_set_boot_partition()` to pending-verify semantics and would collide head-on with the custom NVS 3-strike rollback in `ota_check_rollback()` (ota_manager.cpp:352-410). **Do not enable it in stage 1** — the threat table (TH7) and best-practice checklist reflect this same position. (Side finding: T13 already calls `esp_ota_mark_app_invalid_rollback_and_reboot()` with `CONFIG_APP_ROLLBACK_ENABLE` compiled out — its behaviour in that configuration should be bench-verified on 2344 regardless of this study.)

The right-sized alternative is **software anti-rollback in the pull client**: the signed release manifest (§7.7) carries the version and a monotonic `seq`; the client refuses any manifest whose `seq`/version is not strictly newer than the running `FIRMWARE_VERSION`, and persists a high-water mark in NVS (`system/fw_hiwater`) so even a compromised server replaying an *old, validly signed* release is rejected. Effort S, no eFuses, covers the realistic threat.

### 7.5 The ESP32-S3 DS (Digital Signature) peripheral — stage 1.5 for client identity

The S3 has a hardware Digital Signature peripheral: an **RSA** private key (up to 4096 bits; **RSA only — no ECDSA**) is stored AES-encrypted in flash, decryptable only by a key the HMAC peripheral derives from a **read-protected eFuse key block**. Software can *use* the key to sign (e.g. a TLS client-auth handshake) but can never read it — malware exfiltrating flash gets ciphertext.

Integration is largely already paid for: `CONFIG_ESP_TLS_USE_DS_PERIPHERAL=y` is **already set** in `firmware/sdkconfig.lolin_s3:764`. Provisioning uses the `esp-secure-cert-tool` (PyPI; `configure_esp_secure_cert.py --configure_ds`), which burns the HMAC key, encrypts the RSA key parameters, and writes them plus the device certificate into a small `esp_secure_cert` flash partition (a few KB — the ~9.8 MB unused region above 0x630000 has ample room, one `firmware/partitions.csv` edit). At runtime the `esp_secure_cert_mgr` component hands the DS context to esp-tls (`esp_tls_cfg_t.ds_data`), making mutual-TLS client auth work with the existing `esp_http_client` pattern in T14. Two caveats to state plainly: (1) the provisioning host sees the private key once, in plaintext — the DS peripheral protects the key *on the device*; it is not on-die key generation; (2) **provisioning is a bench/serial operation** (eFuse burn) — no BOM change, but plan it for the next physical touch of each unit, which for 5C88 means a site visit. That is exactly why it is the meaningful middle stage: per-device unforgeable identity, no hardware respin, re-provisioning possible at the bench.

### 7.6 External secure element (stage 2): ATECC608B + esp-cryptoauthlib

An ATECC608B on I2C (driven by the `espressif/esp-cryptoauthlib` component, `CONFIG_ESP_TLS_USE_SECURE_ELEMENT=y`) adds what the DS peripheral cannot: **on-die key generation** (the ECC P-256 private key never exists outside the chip, not even at provisioning), tamper-hardened storage, monotonic counters, and — with the Trust&Go variant — a Microchip-signed device certificate chain usable for zero-touch fleet attestation. Note the algorithm shift: ATECC608 does ECDSA P-256, not RSA, so server-side TLS config must accept ECDSA client certs. Provisioning is either "buy pre-provisioned Trust&Go" or a bench workflow via `esp_cryptoauth_utility`.

Say it plainly: **the two fielded units have no secure element and no spare I2C header designed for one.** Adding an ATECC608B means a board respin or a piggyback board, plus a physical visit to 5C88 at the farm. For a 2-unit fleet whose identity problem the DS peripheral already solves in silicon, this is deferred hardware-roadmap material — worth designing the piggyback footprint into the *next* hardware revision, not retrofitting the current one.

### 7.7 Project-specific gap: signing covers the app image only — the assets ZIP is naked

All of the above authenticates only the app binary. The web-assets ZIP that fills the paired LittleFS partition has **no integrity or authenticity check at all today** — `extract_zip_store()` (ota_manager.cpp:749-834) parses local-file headers but never reads or verifies the CRC-32 field, and `manifest.json` ships an empty `checksum`. A signed firmware paired with attacker-controlled assets is still a compromise (the assets are the admin GUI: a hostile `app.js` can capture the PIN and drive every admin API).

Proposal — a **signed release manifest** as the unit of trust, which also happens to solve the paired-commit invariant for the pull path. This is the single authoritative manifest schema; §4 and §8 reference it:

```json
{
  "version": "2.2.0",
  "seq": 27,
  "channel": "soak",
  "min_version": "2.1.0",
  "key_id": "hbwv-fw-2026",
  "fw_file": "greenhouse-controller-2.2.0.bin",
  "fw_sha256": "…", "fw_size": 1425408,
  "assets_file": "web-assets-2.2.0.zip",
  "assets_sha256": "…", "assets_size": 110243,
  "released_at": "2026-07-04T12:00:00Z"
}
```

`seq` is the monotonic anti-rollback counter (TH6, §7.4); `min_version` lets a release refuse to install over too-old firmware (e.g. across an NVS schema change); `key_id` is reserved now so a future key rotation has an in-band handle. Detached signature over the canonical JSON bytes, made in `bin/build_release.ps1` with the *same* offline RSA-3072 key as the app image (`openssl dgst -sha256 -sign hbwv_fw_signing.pem`), verified on-device with `mbedtls_pk_verify` (already in the build). Pull-client order: fetch manifest → verify signature → `seq`/version anti-rollback check → download firmware, SHA-256 must match → download assets ZIP into PSRAM, SHA-256 must match **before** extraction → hand both to T13 → single atomic bank switch. Firmware and assets are then cryptographically bound into one release: the pull path can never reach the FW_DONE 120 s fallback because it does not start applying until both artefacts are verified in hand — the stranded-asset failure mode becomes a push-path-only legacy concern.

### 7.8 Effort summary

| Measure | Effort | Justification |
|---|---|---|
| Signed apps on update (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`) | **S** | Kconfig + key file + one `build_release.ps1` verify step; no code changes, fully reversible — do this first |
| Signed release manifest incl. assets SHA-256 (§7.7) | **S–M** | ~1 script step + ~200 LOC of mbedTLS verify in the pull client; designed together with the pull task |
| Secure Boot V2 (RSA-3072-PSS, eFuse digests) | **M** | Hours are modest, but irreversible eFuse burns, dio-header rework, and permanent key-custody liability make it stage-2+ |
| Flash encryption | **M** | XTS-AES + eFuse key burns, greenfield-flash procedure changes; does not cover NVS |
| NVS encryption (HMAC-eFuse scheme) | **S–M** | The actual at-rest protection for credentials; one eFuse key block, no partition change |
| DS peripheral device identity (mTLS client key) | **M** | Provisioning tooling + `esp_secure_cert` partition + server-side mTLS; esp-tls hook already compiled in; bench access per unit |
| ATECC608B secure element | **XL** | Hardware respin or piggyback board, field visit to 5C88, new provisioning workflow, ECDSA migration — next hardware revision |

**Bottom line:** enable signed-apps-on-update and the signed release manifest now (stage 1, ~2–3 days total, zero brick risk); provision the DS peripheral when mutual TLS for the pull server is built (stage 1.5); hold Secure Boot V2, flash encryption, and the ATECC608B until the fleet or the threat model outgrows a locked cabinet.

---

## 8. Server-side design (rfsee.net VPS)

### 8.1 Can it be done in PHP?

**Yes.** Everything the pull-OTA server must do — serve a per-channel JSON manifest, stream two static binaries, verify a per-device HMAC, append an audit row — sits comfortably in PHP next to the `hbwv/api.php` that already exists. The one capability that is *not* a PHP problem is **mTLS**: client-certificate verification terminates in the web server (Apache `SSLVerifyClient require` / nginx `ssl_verify_client on`), which needs vhost-level and probably root access. SSH/SCP deploy access is confirmed, but the actual web-server stack (Apache vs nginx) and root access are **unconfirmed — open question to resolve before committing to mTLS**. Recommendation: **stage 1 in PHP with per-device HMAC; mTLS in stage 2, contingent on confirming vhost access.** Dedicated software is not needed at 2–20 units.

### 8.2 Required capabilities

| # | Capability | Notes |
|---|---|---|
| 1 | Manifest per device/channel | The signed release manifest of §7.7, served per channel; the check-in doubles as fleet telemetry |
| 2 | Binary hosting | Plain full-body GET is all the recommended client (§4.3 shape a) needs. Range/206 support is optional future-proofing (~30 lines parsing the general `bytes=N-M` form) — cheap and standard, required only if a future client switches to `esp_https_ota` |
| 3 | Device authentication | Per chosen option: HMAC header (stage 1), client cert (stage 2) |
| 4 | Check-in / result audit | Who polled, when, running what, and the last update result |
| 5 | Publish workflow | One command from the build PC: upload binaries + rewrite manifest |
| 6 | Staged rollout | Channels `soak` (2344) → `production` (5C88) — mirrors today's manual soak gate exactly |

### 8.3 Option 1 — PHP under the existing hbwv/ site (recommended) — effort **M**

```
httproot/hbwv/
  api.php                  # existing status/log ingest — untouched
  ota/
    manifest.php           # GET → channel manifest; doubles as check-in recorder
    download.php           # GET ?file=fw|assets&v=… → binary stream, same HMAC gate
ota-store/                 # OUTSIDE the webroot
  releases/2.2.0/greenhouse-controller-2.2.0.bin
  releases/2.2.0/web-assets-2.2.0.zip
  releases/2.2.0/manifest-2.2.0.json          # incl. detached signature
  channels/soak.json       # which release each channel points at
  channels/production.json
  devices.json             # registry: unit_id → {key, channel, enabled, last_seen, fw_ver}
  checkins.csv             # append-only audit
```

- **Registry:** flat `devices.json` with atomic tmp-write + `rename()` — the same pattern the status site already uses. SQLite is the upgrade path once concurrent writes exist; unnecessary at ≤20 units (and PHP SQLite3 extension availability on this VPS is unverified anyway).
- **HMAC verification** — verifies exactly the `X-OTA-Auth` wire format of §5 O2 (`hash_hmac` + `hash_equals`, both ancient PHP, constant-time compare):

```php
list($unit, $ts, $nonce, $mac) = array_pad(explode(':', $_SERVER['HTTP_X_OTA_AUTH'] ?? ''), 4, '');
$key = $registry[$unit]['key'] ?? null;
$msg = $unit . '|' . $ts . '|' . $nonce . '|' . ($_SERVER['REQUEST_URI'] ?? '');
$ok  = $key !== null && ($registry[$unit]['enabled'] ?? false)
    && abs(time() - (int)$ts) < 300
    && !nonce_seen($nonce)                       /* flat-file cache, prune > 10 min */
    && hash_equals(hash_hmac('sha256', $msg, $key), $mac);
if (!$ok) { http_response_code(204); exit; }     /* silent drop, same policy as api.php */
```

  Authenticated requests get **real** status codes (200/404/416) — unlike api.php's everything-is-204 posture, because the OTA client state machine needs them. Only failed auth gets the silent 204. Note the HMAC authenticates the client to the server (gates downloads and registry writes); firmware *integrity* comes from the offline signature, not from this.
- **Downloads:** `fopen` + 64 KB `fread`/`echo` loop from `ota-store/` (never `readfile()` into memory limits). The optional Range handler (capability #2) parses `bytes=N-M` and emits `206`/`Content-Range`/`Accept-Ranges`; `mod_xsendfile` / `X-Accel-Redirect` would be nicer but depends on the unconfirmed stack — don't design around it.
- **Why mTLS is the awkward part in PHP:** PHP never sees the TLS handshake; the client cert is verified by Apache/nginx config and surfaced to PHP only as `SSL_CLIENT_*` variables if the vhost exports them. Feasible only with root/vhost access — **open question**, see §8.1.

**Verdict:** at 2–20 units PHP is entirely adequate, matches the maintainer's existing stack, and is running the moment `scp` completes. Effort **M** including registry tooling.

### 8.4 Option 2 — dedicated service (Go or Python behind a reverse proxy) — effort **L**

Buys: first-class mTLS handling in-process, long-running state (rollout percentages, rate limits), proper unit tests in CI, one static binary (Go). Costs: a daemon to babysit — systemd unit, restarts, log rotation, a deploy pipeline, a second toolchain on the VPS. For a one-person company each of those is a recurring cost that PHP files simply don't have. Not justified below ~50 units; revisit if mTLS becomes mandatory and vhost access is denied.

### 8.5 Option 3 — off-the-shelf

- **Mender** — open source, real rollback and fleet UI, hosted or self-hosted. But self-hosting is a multi-service Docker stack (**XL**) and the client targets embedded Linux; ESP32/MCU support is community-grade, so you'd write nearly as much device code as the DIY path while inheriting a server an order of magnitude bigger than the problem. Fit: poor. No.
- **Eclipse hawkBit** — the DDI polling API is actually a good protocol match, but it's a JVM + database stack to run and patch for a two-unit fleet. Effort **XL**. No.
- **GitHub Releases as artefact store** + thin manifest on rfsee.net — the pragmatic hack. Binaries get TLS, CDN, and Range support for free; `manifest.php` stays the auth + channel brain and points `fw_url` at GitHub. Costs: a public repo means public firmware images (probably acceptable — WiFi creds and OTA keys live in NVS, not the image — but decide consciously); a private repo needs a token on every device, which is worse than the HMAC key it replaces. Effort **S** on top of option 1. Fit: fine as a later bandwidth offload; unnecessary now.

### 8.6 Publishing workflow — one command from build PC to fleet

Extend [bin/build_release.ps1](../bin/build_release.ps1) with a **Step 4 — publish** (after Step 3.5's manifest-placeholder restore): (1) sha256 over bin + zip, (2) sign firmware and release manifest with the **offline** signing key (§7.1, §7.7), (3) emit `manifest-<v>.json`, (4) `scp` the artefacts to `ota-store/releases/<v>/`, (5) point `channels/soak.json` at `<v>`. Promotion after soak passes is a deliberately separate command (`bin/ota_promote.ps1 <v> production`) that rewrites `channels/production.json` — preserving today's human soak gate.

**The signing key lives on the build PC and never on the VPS.** Stated twice because it is the crux of the defence-in-depth argument: with signing offline, a fully compromised VPS can serve stale or garbage firmware (an availability problem) but can never produce an image the devices will accept (integrity survives). The VPS holds artefacts and HMAC keys — never signing keys.

### 8.7 VPS impact analysis

| Resource | Impact |
|---|---|
| Disk | ~1.5 MB/release (1.36 MB bin + 108 KB zip + manifest); retain last 10 → ~15 MB. `bin/<version>/` in the repo stays the master copy; server copies are expendable |
| Bandwidth | 1.5 MB × fleet per update (3 MB today; 300 MB at 200 units) + ~1 KB manifest poll × fleet × checks/day — trivial |
| CPU | HMAC over a short string + static-file streaming: negligible |
| PHP config | Downloads, not uploads — `post_max_size` isn't in the path (releases arrive via scp). The ≥2 MB raise of 2026-06-28 already covers any future HTTP-upload variant |
| Backup | Add `ota-store/` to whatever backs up `httproot/`; `devices.json` is the only file not reproducible from the repo |
| Monitoring | Optional cron: alert if an enabled device's `last_seen` exceeds 2× its check interval — replaces noticing 5C88 went quiet by accident |

---

## 9. Key management and fleet provisioning

### 9.1 Device identity

`unit_id` already exists — 4 hex chars from eFuse MAC bytes 4+5 (`firmware/src/system_id/system_id.cpp`). Today it appears only *inside* the status JSON body and is suppressible via `status_expose` bit 5; there is no transport-level identity at all. The OTA path must promote it to a first-class lookup key: sent on every manifest/download request, never suppressible, and used as the HMAC key id. Caveat: 16 bits collide eventually — birthday odds ≈ 2 % at 50 units, ≈ 25 % at 200 — so provisioning must reject duplicate `unit_id`s and fall back to the full MAC for the colliding unit.

### 9.2 Stage 1 — one secret per device

- **Generate on the build PC**: `openssl rand -hex 32`, one per unit, at provisioning time.
- **Inject at first flash / bench**: via the admin-PIN'd `/api/ota/config` endpoint (§4.6) over the unit's AP or the bench LAN — the write path already exists in the design, logs a "set" audit row without echoing the value, and the key crosses only the local bench link once. (The firmware has no serial console/REPL; adding one just for key injection would be new scope for no security gain at bench distance.) Stored as `system/ota_secret` — the same key `/api/ota/config`'s "empty = keep" path protects.
- **Server side**: the same key lands in `devices.json` (outside webroot, mode 0600) with `channel` and `enabled`. HMAC means the server holds the raw key — accepted for stage 1; that is the same trust level as today's fleet-wide secret, with two improvements: a leaked registry compromises device *authentication* but never firmware *integrity* (signing key is offline), and compromise is per-device — delete one row instead of rotating the whole fleet.

### 9.3 Stage 1.5/2 — a tiny CA for mTLS

A real CA is a few openssl commands on the build PC. **Algorithm choice follows the key store:** the DS peripheral path (O4, stage 1.5) is RSA-only, so device keys/certs on that path are RSA-3072; ECDSA P-256 arrives only with the ATECC608 (stage 2). The CA itself can sign both.

```
# CA (once):
openssl req -x509 -newkey rsa:3072 -days 7300 -keyout ca.key -out ca.pem \
        -subj "/CN=RFSee Greenhouse OTA CA"
# per device (DS-peripheral path — RSA-3072):
openssl req -newkey rsa:3072 -keyout 5C88.key -out 5C88.csr -subj "/CN=5C88"
openssl x509 -req -in 5C88.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 3650 -out 5C88.pem
# per device (ATECC608 path, stage 2 — key born on-chip, CSR exported): ECDSA P-256
```

- Cert CN = `unit_id`. Stage 1.5: private key generated on the build PC and provisioned into the DS peripheral (`configure_esp_secure_cert.py --configure_ds`, §7.5). Stage 2: key born inside the secure element — it then never exists outside the chip.
- **Lifetimes**: CA 20 y, device certs 10 y. Long on purpose — short-lived device certs are a fleet-wide expiry incident waiting to happen to a company of one; their dates are enforced server-side where the clock is trustworthy. Note the flip side after stage 0: with `MBEDTLS_HAVE_TIME_DATE` enabled, the device **does** check the *server* certificate's validity dates — so rfsee.net's own cert renewal (e.g. Let's Encrypt 90-day certs) must be automated and monitored, or the whole fleet loses its update path (and its status posting) on a silent expiry.
- **Revocation at this scale**: skip CRLs and OCSP entirely. The web server authenticates "some cert signed by our CA"; `devices.json` authorizes the specific CN/serial. Revoking a unit = `enabled:false` or delete the row. *Authenticate by CA, authorize by allowlist* is what keeps a tiny CA maintainable.

### 9.4 Key rotation

- **HMAC keys**: rotate on suspicion, not on schedule. Bench units: reprovision at the bench. Remote units (5C88 is outbound-only — there is no push path): add device-initiated rotation — device generates a new key, POSTs it over the existing HMAC-authenticated TLS channel, server swaps the registry entry atomically. Effort **S** in firmware, and worth building in stage 1 because it is the only way to rotate a remote unit short of a site visit.
- **Firmware-signing key**: stage-1 mode verifies only the first signature block (§7.1), so there is **no dual-key transition window** — rotation is the two-release dance (transition release signed with the old key with verification disabled, then the new-key release) or a cable flash. Reserve the `key_id` manifest field now (§7.7); true multi-key rotation arrives with Secure Boot V2's three eFuse digest slots.
- **CA**: dual-trust pattern — device trust store carries old + new CA across the transition.

### 9.5 Scaling

| Fleet | Registry | Auth | Server | What actually changes |
|---|---|---|---|---|
| 2 (today) | `devices.json` | per-device HMAC | PHP | Nothing — this design as written |
| 10 | `devices.json` | HMAC (mTLS if vhost access confirmed) | PHP | A provisioning script replaces manual openssl/NVS steps |
| 50 | SQLite | mTLS + secure element on new builds | PHP still fine | `unit_id` collisions plausible → full-MAC ids; rollout percentages become nice-to-have |
| 200 | SQLite/Postgres | real PKI (intermediate CA, issuance tooling) | dedicated service (§8.4) | The structural change lands **here**, not before |

Nothing structural changes until ~50 units, and the stage-1 design is not throwaway: the manifest format, channel model, registry semantics, and publish pipeline all survive a later swap of transport auth (HMAC → mTLS) and server runtime (PHP → Go).

### 9.6 Maintainability by a one-engineer company

- The stack is identical to what already runs: PHP files next to `api.php`, deployed by scp. Zero new daemons, zero new languages on the VPS.
- Disaster recovery: re-scp artefacts from `bin/<version>/` + restore one `devices.json`. The status/dashboard path is untouched and independent.
- Exactly two artefacts must never leak or be lost: the firmware-signing key and (stage 2) the CA key. Both live on the build PC with one offline backup — an encrypted USB stick stored physically elsewhere. Everything else (device keys, certs, releases) is regenerable or revocable.
- Steady-state burden: publishing = one build command + one promote command; provisioning a new unit adds ~5 minutes to first flash; revocation is a one-line registry edit. Sustainable for a company of one.

---

## 10. Consolidated effort and roadmap

| Stage | Work item | Effort |
|---|---|---|
| 0 | `CONFIG_MBEDTLS_HAVE_TIME_DATE` + signed-apps-on-update + build script signing/verify | S–M |
| 1 | T16 task, check/download/apply via T13 | M–L |
| 1 | NVS params + `/api/ota/config` + GUI + logparser | M |
| 1 | Verify pipeline (manifest sig, SHA, downgrade guard) + test plan on 2344 | M |
| 1 | PHP server: manifest.php, download.php, registry, channels, publish + promote scripts | M |
| **1** | **Total stage 1** | **~3–4 weeks calendar, one engineer** |
| 1.5 | NVS encryption (HMAC-eFuse) | S–M |
| 1.5 | mTLS: tiny CA + DS-peripheral provisioning + vhost config (contingent on access) | M–L |
| 2 | Secure Boot V2 / flash encryption / ATECC608 (next hardware revision) | M / M / XL |

Version impact: stage 1 is a feature release → **minor bump** (new NVS keys, new API endpoint, new status-JSON fields). The signed-apps Kconfig flip (stage 0) is also minor — it changes what future OTA accepts.

## 11. Open questions

1. **VPS access level** — Apache or nginx? root/vhost access available? Determines whether mTLS (stage 1.5/2) is possible at all on rfsee.net, and whether `mod_xsendfile`-style download offload is available. Resolve before committing to O3/O4.
2. **Reboot window policy** — is "motors idle + no wind override + no active session" sufficient, or should remote updates additionally be restricted to a configured time window (e.g. 02:00–04:00)? A `ota_window_h` NVS param would be a trivial addition.
3. **Should the existing push path also require signatures once stage 0 lands?** Yes by mechanism (same `esp_ota_end()` enforcement) — but the team must remember: after stage 0, `ota_push.py` can only push signed binaries. `build_release.ps1` signing makes this automatic; ad-hoc dev builds need the key available.
4. **`ota_secret` vs `status_secret` unification** — T14's status POST auth and T16's OTA auth could share one per-device credential and one server registry. Attractive (one provisioning step), but couples the two failure domains; decide at implementation time.
5. **Firmware binary confidentiality** — accepted as non-secret in this study (TH10). If that changes (licensing, IP), revisit GitHub-Releases hosting and add O1/O2 gating to downloads (already designed in).

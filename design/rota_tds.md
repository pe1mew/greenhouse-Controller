# Remote OTA (ROTA) — Technical Design Specification

*Version 0.1 (draft for review) · 2026-07-12 · branch `rota` · firmware target 2.2.0 · derives from [remoteOTAstudy.md](remoteOTAstudy.md) §3.1 (R1–R5) and §11 (Q1–Q5, all resolved)*

## 1. Background

The controller today is updated only by a push from an operator PC on the same LAN ([OTAimplementation.md](OTAimplementation.md)). Production unit 5C88 is behind NAT and outbound-only, so every production update requires a site visit. ROTA adds a pull path: a new FreeRTOS task periodically asks an OTA server on rfsee.net whether a newer release exists for this unit, downloads and verifies it, and applies it in a night window — reusing the existing T13 ingestion machinery so the firmware+assets paired-commit invariant is preserved. Mutual authentication between controller and server before any firmware is offered is the governing Must (study §3.1 R1); firmware signing is deferred (R2, Would).

Requirement keywords **shall / should / may** follow RFC 2119/RFC 8174. Priorities follow MoSCoW. Document conventions follow ISO/IEC/IEEE 29148 §5.2 and the [vmodel.eu style guide](https://vmodel.eu/style-guide.html). Referenced technical standards: RFC 2104 (HMAC), FIPS 180-4 (SHA-256), RFC 8446/5246 (TLS), RFC 5280 (X.509), SemVer 2.0.0, ISO 8601. Requirement IDs are category-suffixed (`R-G01` general, `R-A` authentication/security, `R-I` identity, `R-C` client, `R-V` versioning, `R-P` apply policy, `R-F` configuration, `R-O` observability, `R-R` reliability, `R-S` server, `R-T` tooling/operations).

## 2. Requirements

### 2.1 General (R-G)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-G01 | ROTA **shall** be delivered as firmware release 2.2.0 (SemVer 2.0.0 minor bump: new NVS keys, new API endpoint, new status fields). | Must | `fw_ver` and `asset_version` report `2.2.0` post-OTA. |
| R-G02 | With `ota_enable = 0` (factory default) the firmware **shall** behave identically to 2.1.3: no network requests to the OTA server, no new task activity beyond an idle wait. | Must | 24 h packet capture with feature off shows 0 requests to the OTA URL; SD log shows no ROTA audit rows. |
| R-G03 | The existing local push OTA path (`/api/ota/firmware`, `/api/ota/assets`, `ota_push.py`) **shall** remain functional and unchanged as the recovery path. | Must | `ota_push.py` full cycle passes on 2.2.0. |
| R-G04 | A pull update **shall** apply firmware and web-assets as one transaction: after any reboot the unit runs either the complete old pair or the complete new pair. | Must | Kill-power matrix (R-R01) never yields `fw_ver ≠ asset_version`. |
| R-G05 | ROTA **shall** require no hardware change on fielded units. | Must | Feature runs on unmodified 2344 and 5C88. |
| R-G06 | A concurrent local push OTA and pull update **shall** be mutually exclusive; the first to enter the OTA state machine wins. | Must | Starting either while the other is in progress is rejected/deferred cleanly; no interleaved writes. |

### 2.2 Mutual authentication and transport security (R-A)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-A01 | Controller and OTA server **shall** mutually authenticate each other before any firmware is offered, listed, or downloadable (study R1). | Must | Device with wrong secret receives no manifest content; device rejects a server presenting a non-pinned certificate. |
| R-A02 | Server authentication **shall** use TLS (≥ 1.2, RFC 5246/8446) with a certificate pinned on the controller; a self-signed certificate **shall** be accepted when pinned (study R5). | Must | Connection succeeds only against the pinned cert; a MITM with a publicly-CA-signed cert for the same hostname is rejected. |
| R-A03 | The pinned server certificate **shall** be uploadable via the controller's admin web GUI (PEM, ≤ 4 KB) and stored persistently. | Must | Upload → reboot → OTA connection still verifies against the uploaded cert. |
| R-A04 | The firmware **should** carry an embedded default server certificate used when no uploaded certificate is present. | Should | Factory-fresh unit with empty cert storage connects using the embedded cert. |
| R-A05 | Device authentication **shall** use per-unit HMAC-SHA256 (RFC 2104, FIPS 180-4) request signing: `mac = HMAC(ota_secret, id | ts | nonce | request-URI)`, sent as one header `X-OTA-Auth: <id>:<ts>:<nonce>:<mac>` on every OTA request. | Must | Server accepts a correctly signed request and rejects a bit-flipped signature. |
| R-A06 | The device credential **shall** be a dedicated `ota_secret` (16–64 chars), independent of `status_secret` (study Q4). | Must | Changing either secret does not affect the other path's authentication. |
| R-A07 | The server **shall** reject requests with clock skew > ±300 s and **shall** reject a reused nonce within a 10 min cache window. | Must | Replayed capture of a valid request within 10 min is rejected; a 6-min-old timestamp is rejected. |
| R-A08 | The server **should** answer failed authentication with HTTP 204 and no body (silent-drop posture of the existing `api.php`); authenticated requests receive real status codes. | Should | Wrong-secret probe receives 204; valid client receives 200/404/416 as applicable. |
| R-A09 | Neither secret nor certificate private material **shall** ever appear in logs, status JSON, GUI read-back, or SD audit rows; configuration writes log a "set" marker only. | Must | Grep of SD log, serial log, and `/api/*` responses after provisioning shows no secret material. |
| R-A10 | Firmware/manifest signing **may** be added later (study R2); the manifest format **shall** reserve a `key_id` field so signing can be introduced without schema break. | Would | Manifest schema contains `key_id`; no verification code required in 2.2.0. |
| R-A11 | The design **should** permit a later upgrade to mTLS (nginx `ssl_verify_client`, confirmed available) without changing the manifest or endpoint schema. | Could | Design review: endpoints carry no assumption that breaks under client-cert termination. |

### 2.3 Device identity (R-I)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-I01 | The short unit id **shall** be the last 2 bytes of the WiFi MAC, rendered as 4 hex characters (existing `system_id` convention). | Must | Reported id matches MAC bytes 4–5 on both units. |
| R-I02 | The full WiFi MAC **shall** be the default unique device identifier in all OTA requests and in the server registry (study R3). | Must | Registry lookups key on the full-MAC string; requests carry it. |
| R-I03 | The identifier **may** later be overridden or replaced by a longer provisioned key; the server registry **shall** key on an opaque string so replacement needs no schema change. | Should | Inserting a unit with a non-MAC identifier string works without server code change. |

### 2.4 Client task (R-C)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-C01 | ROTA **shall** run as a dedicated FreeRTOS task T16 (priority 3, network band; 8 KB stack; heap-allocated work buffers), not inside T13 or T14. | Must | Task list shows T16; stack high-water ≥ 1 KB margin during a full update. |
| R-C02 | T16 **shall** check for updates every `ota_check_h` hours (configurable 1–168, default 24) with ±10 % uniform jitter per cycle. | Must | Observed check intervals over 5 cycles fall within `ota_check_h` ± 10 %. |
| R-C03 | A check **shall** run only when all preconditions hold: `ota_enable=1`, `ota_url` non-empty, WiFi STA connected, SNTP synced (`nm_is_sntp_synced()`), no OTA in progress (`EG1_BIT_OTA_IN_PROGRESS` clear). A skipped check **shall** be audited. | Must | Each precondition individually blocks the check and produces the audit row. |
| R-C04 | Firmware image (~1.4 MB) and web-assets ZIP **shall** both be fully staged in PSRAM and verified before the first flash write (no streaming to flash). | Must | Heap trace shows SPIRAM allocations; no `esp_ota_begin` before both SHA checks pass. |
| R-C05 | Each downloaded artefact **shall** be verified against its manifest SHA-256 (FIPS 180-4) and size before apply; any mismatch aborts with no flash write. | Must | Corrupting one byte of either artefact on the server → audit row, flash untouched, old version boots. |
| R-C06 | Apply **shall** reuse the existing T13 ingestion sequence (`ota_firmware_begin/write/end` → `ota_assets_begin/accumulate/end`) so audit codes 13–17, `/api/ota/status`, and the LCD OTA indicator keep working unchanged. | Must | Pull update produces the same T13 state transitions and audit rows as a push update. |
| R-C07 | T16 and T14 **shall** never run TLS handshakes concurrently (shared mutex): at most one TLS session exists at any moment. | Must | Instrumented soak shows no overlapping handshakes; internal-heap largest block stays above the R-R05 floor. |
| R-C08 | Downloads **shall** be chunked (≤ 8 KB reads) and must not degrade control: T2/T3/T6 cycle cadence **shall** remain within normal bounds during a full download. | Must | SD log SENSOR_HR cadence stays 30 ± 2 s throughout a download on soak. |
| R-C09 | On any failure T16 **shall** free all staged buffers and retry with exponential backoff 1 h → 2 h → 4 h → … capped at 24 h, reset on success. | Must | Server-down soak shows the exact backoff ladder in audit timestamps. |
| R-C10 | A completed check with no newer version **shall** cost no flash writes and no reboot. | Must | "No update" cycles leave uptime and flash-write counters untouched. |

### 2.5 Version selection and anti-downgrade (R-V)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-V01 | The client **shall** accept only a manifest whose version is strictly newer than the running `FIRMWARE_VERSION` (SemVer compare) **and** whose `seq` is strictly greater than the persisted high-water mark. | Must | Serving an equal or older version/seq produces a "rejected downgrade" audit row and no download. |
| R-V02 | The accepted `seq` **shall** be persisted in NVS (`fw_hiwater`) at commit time, surviving reboot and power loss. | Must | Replay of a previously-installed manifest after reboot is rejected. |
| R-V03 | The client **shall** honor the manifest `min_version` field: if the running firmware is older than `min_version`, the update is refused with a distinct audit code (manual intervention path). | Should | Manifest with `min_version` above running version → refusal row, no apply. |
| R-V04 | After the post-update reboot the unit **shall** verify `fw_ver == asset_version` and report the result in the next status POST. | Must | Dashboard shows matching pair after every soak update. |

### 2.6 Apply and reboot policy (R-P)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-P01 | Flash feed, commit, and reboot **shall** occur only inside the configured night window `ota_win_lo`–`ota_win_hi` (local time; default 02–04 h; equal values disable the window) (study Q2). | Must | Update downloaded at 14:00 applies no earlier than `ota_win_lo`; timestamps in audit rows prove it. |
| R-P02 | Within the window, apply **shall** additionally require the quiet gate: all windows idle (no `WIN_MOVING_*`), `EG1_BIT_WIND_OVERRIDE` clear, `EG1_BIT_MOTOR_ALARM` clear, `EG1_BIT_CALIBRATING` clear, no active web session, no active LCD PIN session. | Must | Forcing each condition during the window defers the apply with the corresponding audit row. |
| R-P03 | The quiet gate **shall** be re-evaluated immediately (< 5 s) before the reboot is scheduled. | Must | Opening an LCD session between gate-pass and reboot aborts the reboot. |
| R-P04 | If the gate is not satisfied before the window closes, T16 **shall** release the OTA state (old bank keeps running, nothing committed) and retry the next night; each deferral is audited. | Must | Blocked window → no commit, retry next night, audit row per deferral. |
| R-P05 | Check, download, and verify **may** run at any time of day; only apply/commit/reboot is window-restricted. | Must | Download timestamps outside window + apply inside window in the same cycle. |
| R-P06 | The reboot **shall** use the existing `schedule_reboot()`/reboot-worker path (no new reboot mechanism). | Must | Code review + reboot audit row identical to push path. |

### 2.7 Configuration and GUI (R-F)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-F01 | ROTA **shall** add exactly these NVS keys (namespace `system`): `ota_enable` (0/1, def 0), `ota_check_h` (1–168, def 24), `ota_url` (str ≤ 128, `https://` only), `ota_secret` (str 16–64), `ota_win_lo` (0–23, def 2), `ota_win_hi` (0–23, def 4), plus certificate storage (NVS blob or LittleFS, ≤ 4 KB). | Must | `/api/config`-family GET reflects all keys with defaults on first boot after OTA. |
| R-F02 | All ROTA configuration **shall** be admin-only: served/accepted via a dedicated `GET/POST /api/ota/config` transaction endpoint; none of the keys appear in farmer allowlists; the GUI group carries the admin-only class. | Must | Farmer session receives 403 on POST and does not see the GUI group. |
| R-F03 | The endpoint **shall** validate the full payload before any write (URL scheme, ranges, secret length) and **shall** treat an empty secret/cert field as "keep current". | Must | Invalid field → nothing written; empty secret in a valid payload leaves the stored secret unchanged. |
| R-F04 | Config changes **shall** take effect without reboot (task notify), and each changed key **shall** produce a config audit row (values of secrets as "set" marker only). | Must | Interval change observed at next cycle; SD log has one row per changed key. |
| R-F05 | The web GUI **shall** present: enable checkbox, interval, URL, secret (password-style), night window (two hour fields), certificate upload, and a read-only "last check / last result" line. | Must | All controls present and functional in an admin session; absent for farmer. |
| R-F06 | The beheerder manual **shall** document the configuration surface, and the boer manual **shall** state that the controller may update itself at night (same changeset as the firmware). | Must | Manual diffs present in the release commit. |

### 2.8 Observability and audit (R-O)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-O01 | T16 **shall** write SD audit rows (`LOG_SYSTEM`) with new `value_a` codes: 22 = check outcome, 23 = download/verify outcome, 24 = apply outcome, each with defined `value_b` sub-codes (see §4.4). | Must | Every cycle produces exactly the specified rows; codes match the table. |
| R-O02 | `log/logparser.py` **shall** decode codes 22–24 in the same changeset (repo hard rule). | Must | Parser output labels the new rows on a soak log. |
| R-O03 | The status JSON **shall** gain `rota_state`, `rota_last_check` (epoch), `rota_last_result`, visible on the rfsee.net dashboard (the only observation channel for 5C88). | Must | Dashboard shows the fields within one status interval of a check. |
| R-O04 | The server **shall** append one check-in record per authenticated manifest request (unit, timestamp, running version, result). | Must | `checkins.csv` row count equals manifest request count in a test run. |
| R-O05 | The existing T13 audit codes 13–17 **shall** continue to fire unchanged during pull updates. | Must | Soak log contains the same 13–17 sequence as a push update. |

### 2.9 Reliability and failure handling (R-R)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-R01 | Power loss at any moment of a pull update **shall** leave the unit bootable on the complete old pair or the complete new pair. | Must | Kill-power at ≥ 3 points (mid-download, mid-flash-write, post-commit/pre-reboot) — all boot cleanly with matching `fw_ver`/`asset_version`. |
| R-R02 | An unreachable or failing OTA server **shall** have zero effect on climate control, safety functions, and status posting. | Must | 48 h server-down soak: T2/T3/T6 behaviour and T14 posting statistically unchanged. |
| R-R03 | The existing 3-strike NVS rollback (`ota_check_rollback`) **shall** cover pull-updated firmware unchanged. | Must | Deliberately crash-looping test build rolls back to the previous bank within 3 boots. |
| R-R04 | The `FW_DONE` 120 s fallback timer **shall** never fire on the pull path (both artefacts staged before apply); an audit code 13 during pull testing is a defect. | Must | Full test campaign contains zero `value_a=13` rows from pull cycles. |
| R-R05 | Internal-heap largest free block **shall** remain ≥ 20 KB at all times during a pull update (gh#23 budget). | Must | T1 heap rows (`value_a` 7/8/12) during soak updates never report below 20 KB. |
| R-R06 | A verify failure (TLS, HMAC, SHA, size, version) **shall** abort before any flash write and free all staged memory; heap returns to pre-check level ± 1 KB. | Must | Fault-injection runs show flash-write counter unchanged and heap restored. |

### 2.10 Server (R-S)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-S01 | The server **shall** run as PHP under the existing rfsee.net nginx (full root confirmed, study Q1) at `/ghc/rota/` with two endpoints: `manifest.php` and `download.php`. | Must | Both endpoints functional over the pinned-cert TLS host. |
| R-S02 | Release artefacts, the device registry, and channel pointers **shall** live outside the webroot; only the two PHP endpoints are web-reachable. | Must | Direct URL probes to `ota-store/` paths return 404/403. |
| R-S03 | The registry **shall** key on the opaque identifier string (R-I02/03) and hold at least: secret, unit type, channel, `pinned_version`, `enabled`, `last_seen`, last reported version. | Must | Registry review; disable flag blocks a unit immediately. |
| R-S04 | The server **shall** resolve a unit's offer as: `pinned_version` if set, else the mainstream release of the unit's type (`channels/<type>.json`) (study R4). | Must | Pinning a unit yields the pinned manifest while mainstream differs. |
| R-S05 | Binary downloads **should** be served via nginx `X-Accel-Redirect` after PHP authentication (PHP never streams the file body). | Should | Response headers/timings show nginx sendfile path; PHP memory stays flat. |
| R-S06 | HMAC verification **shall** use constant-time comparison (`hash_equals`) and the nonce cache **shall** be pruned (> 10 min entries). | Must | Code review + replay test (R-A07). |
| R-S07 | Registry mutations **shall** be atomic (tmp-write + `rename()`); a killed writer never corrupts the registry. | Must | Kill-during-write test leaves a parseable registry. |
| R-S08 | The store **shall** retain at least the last 5 releases per unit type; the repo's `bin/<version>/` remains the master copy. | Should | Disk layout review; re-publish from repo restores any release. |
| R-S09 | `ota-store/` (registry + artefacts) **shall** be included in the VPS backup routine. | Must | Backup listing contains the store. |
| R-S10 | The server **should** support a per-unit "stale check-in" alert: flag when `now − last_seen > 2 × ota_check_h`. | Could | Silencing a test unit raises the flag within the bound. |

### 2.11 Tooling and operations (R-T)

| ID | Requirement | Priority | Pass/Fail criterion |
|------|---|---|---|
| R-T01 | `build_release.ps1` **shall** gain a publish step: compute SHA-256s, emit the manifest, upload artefacts + manifest via scp, and point the **soak** channel at the new release — one command from build to soak-offered. | Must | Fresh build reaches the soak channel with a single script invocation. |
| R-T02 | Promotion to mainstream **shall** be a separate deliberate command (`ota_promote`), preserving the human soak gate; a per-unit pin/unpin command **shall** exist. | Must | Soak channel update never changes mainstream implicitly. |
| R-T03 | Provisioning tooling **shall** write `ota_secret` (+ cert if non-default) to a unit at the bench and create/update its registry row in one documented step. | Must | Provisioning a fresh unit takes ≤ 10 min following the doc. |
| R-T04 | A Python device simulator **shall** exercise the server contract (auth, replay, skew, pinning, channel/pin resolution) as the server acceptance suite, runnable without any ESP32. | Must | Simulator suite green on the production server config. |
| R-T05 | Production enablement on 5C88 **shall** follow: 2.2.0 installed locally on a site visit, unit initially pinned to its current version, unpinned only after ≥ 1 successful observed soak-unit pull cycle. | Must | Rollout checklist in the release notes, ticked. |
| R-T06 | The ROTA server **shall** be developed and versioned in its own git repository (`pe1mew/greenhouse-Controller-FOTA-server`), separate from the firmware; the wire contract (§4 of this TDS) is the interface between the repositories, and the server repository **shall** pin the TDS version/commit it implements. | Must | Server repo `greenhouse-Controller-FOTA-server` holds no firmware sources; its README names the implemented TDS version/tag. |
| R-T07 | All deployment/publishing scripts (server deploy from the server repo; artefact publish from this repo) **shall** authenticate to the VPS using SSH public-key authentication with host-key verification enabled; no passwords, private keys, or device secrets **shall** be stored in either repository. | Must | Deploy succeeds only with the operator's key; repo scan finds no credential material; `StrictHostKeyChecking` is active in the scripts. |

## 3. Verification overview (V-model right side)

| TC | Verifies | Method |
|----|---|---|
| TC-01 | R-G02, R-G03 | Regression test on 2344: feature-off capture + push OTA cycle |
| TC-02 | R-A01/02/05/07, R-S06 | Server acceptance via device simulator (R-T04): wrong secret, bit-flip, replay, skew, MITM cert |
| TC-03 | R-A03/04 | Cert lifecycle demonstration: embedded default → GUI upload → reboot persistence |
| TC-04 | R-C02/03/09/10 | Soak observation of intervals, precondition blocking, backoff ladder |
| TC-05 | R-C04/05, R-R06 | Fault injection: corrupted artefact bytes, truncated download |
| TC-06 | R-G04, R-R01 | Kill-power matrix at 3+ points |
| TC-07 | R-P01–P05 | Window/quiet-gate test: forced sessions, wind override, motion during window |
| TC-08 | R-V01–V03 | Downgrade/replay/min_version served from staging channel |
| TC-09 | R-C07/08, R-R05 | Heap + cadence instrumentation during full update on soak |
| TC-10 | R-O01–O05 | Log parse of a complete soak cycle; dashboard field check |
| TC-11 | R-S01–S07 | Server code review + endpoint probes (unauth 204, path probes, atomic write kill test) |
| TC-12 | R-T01/02/05/06/07 | End-to-end release rehearsal across both repos: build → publish (key-auth) → soak pull → promote → (pinned) production; credential-scan both repos |
| TC-13 | R-R02/03/04 | 48 h server-down soak; crash-loop rollback; zero code-13 audit scan |
| TC-14 | R-F01–F06 | Config/GUI review incl. farmer-session negative tests; manual diffs present |

## 4. Interface definitions (normative)

> **Wire contract v1.0 — FROZEN (plan task 0.1).** This section is the interface between the firmware repo and `greenhouse-Controller-FOTA-server`. After the freeze tag is set, changes require a new contract version and coordinated updates on both sides. Freeze tag: `rota-contract-v1.0` (set on the commit containing this section).

### 4.1 Endpoints

| Endpoint | Method | Auth | Function |
|---|---|---|---|
| `/hbwv/ota/manifest.php?fw=<running-version>[&res=<a>.<b>]` | GET | `X-OTA-Auth` | Resolve unit → offered release; return manifest JSON (HTTP 200, always the full resolved manifest — the *client* decides whether it is newer); record check-in with the reported running version and optional last audit outcome (`res` = last `value_a.value_b` pair, e.g. `24.0`) |
| `/hbwv/ota/download.php?file=fw\|assets&v=<version>` | GET | `X-OTA-Auth` | Stream artefact (nginx `X-Accel-Redirect`) |

HTTP status semantics: **204 is reserved exclusively for failed authentication** (silent drop, R-A08). Authenticated requests receive 200 (success), 404 (unknown unit/file/version), 500 (server fault). "No newer version" is *not* an HTTP condition — the manifest is always returned and compared client-side.

### 4.2 Authentication header

```
X-OTA-Auth: <id>:<ts>:<nonce>:<mac>
mac = HMAC-SHA256(ota_secret, id + "|" + ts + "|" + nonce + "|" + request_uri)
```

| Field | Definition |
|---|---|
| `id` | Full WiFi station MAC (R-I02): 12 lowercase hex chars, no separators, e.g. `a0b1c2d3e4f5` |
| `ts` | Unix seconds, decimal ASCII |
| `nonce` | 8 random bytes, 16 lowercase hex chars |
| `request_uri` | Path **and** query string exactly as sent, e.g. `/hbwv/ota/manifest.php?fw=2.2.0` |
| `mac` | 64 lowercase hex chars (SHA-256 output) |
| `ota_secret` | The per-unit secret (R-A06), used as raw ASCII bytes |

Server checks: window ±300 s, nonce cache 10 min, constant-time compare (`hash_equals`), any failure → 204 empty.

### 4.3 Manifest schema

```json
{
  "version": "2.2.1",
  "seq": 31,
  "unit_type": "ghc1",
  "min_version": "2.1.0",
  "key_id": "",
  "fw_file": "greenhouse-controller-2.2.1.bin",
  "fw_sha256": "…", "fw_size": 1360544,
  "assets_file": "web-assets-2.2.1.zip",
  "assets_sha256": "…", "assets_size": 108073,
  "released_at": "2026-07-20T12:00:00Z"
}
```

`seq` is a strictly monotonic release counter (anti-downgrade, R-V01/02). `key_id` is reserved for later signing (R-A10). Timestamps are ISO 8601 UTC.

### 4.4 Audit codes (`LOG_SYSTEM`, extends the table in event_logger.h)

| value_a | Event | value_b |
|---|---|---|
| 22 | Check outcome | 0 no update · 1 update found · 2 server unreachable/HTTP error · 3 preconditions skip · 4 auth failure |
| 23 | Download/verify outcome | 0 ok · 1 TLS/pin fail · 2 SHA/size mismatch · 3 downgrade/seq rejected · 4 min_version refusal |
| 24 | Apply outcome | 0 committed, reboot scheduled · 1 deferred (window/quiet gate) · 2 apply failed |

### 4.5 Server store layout

```
/hbwv/ota/                 manifest.php, download.php        (webroot)
ota-store/                 (outside webroot)
  releases/<version>/      bin + zip + manifest-<version>.json
  channels/<unit-type>.json  mainstream pointer per unit type
  devices.json             registry (R-S03)
  checkins.csv             append-only audit
```

## 5. Explicitly out of scope (deferred per study §3.1/§11)

Firmware/manifest signing (R2 — Would; `key_id` reserved), mTLS client certificates (upgrade path kept open, R-A11), DS-peripheral key storage, NVS encryption, device-initiated secret rotation, GitHub-Releases artefact offload (binary confirmed non-secret, Q5).

## Appendix A — Glossary

| Term | Meaning |
|---|---|
| ROTA | Remote OTA — this feature; also the development branch name |
| Manifest | Per-release JSON (§4.3) describing the offered firmware+assets pair |
| Mainstream | The release a unit type's channel pointer currently offers (study R4) |
| Pinned version | Per-unit override that fixes the offered release regardless of mainstream |
| Quiet gate | Motors idle + no wind override/motor alarm/calibration + no active sessions |
| Night window | `ota_win_lo`–`ota_win_hi` local-time interval in which apply/reboot is allowed |
| Paired-commit invariant | Firmware and web-assets always activate together (OTAimplementation.md) |
| Soak / mainstream channel | Staged rollout: 2344 receives releases first; promotion is manual |

## Appendix B — Traceability to the study decisions

| Study decision | Requirements |
|---|---|
| R1 mutual auth (Must) | R-A01–A09 |
| R2 signing (Would) | R-A10, §5 |
| R3 identity (MAC) | R-I01–I03 |
| R4 mainstream/pinned | R-S04, R-T02 |
| R5 self-signed pinned cert | R-A02–A04 |
| Q1 nginx + root | R-S01, R-S05 |
| Q2 night window | R-P01–P05, R-F01 |
| Q3 uniform signing later | R-A10 |
| Q4 separate secret | R-A06 |
| Q5 binary non-secret | §5, R-A08 posture |

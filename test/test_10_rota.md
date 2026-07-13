# TC-01…14 — ROTA (internet-pull OTA) — Verification Results

**Feature:** ROTA / T16 pull-OTA client (branch `rota`, firmware 2.2.0)
**Test cases:** defined in [design/rota_tds.md](../design/rota_tds.md) §7; plan mapping in [design/rotaImplementationPlan.md](../design/rotaImplementationPlan.md)
**Automated:** Partial — device driven over the REST API from `bin/ota_push.py` + ad-hoc `urllib` scripts; server side via the `bin/rota_sim.py` device simulator
**Status of feature under test:** check path (auth → manifest → decision → observability) implemented (tasks 3.1–3.5, 3.9). Download/verify/apply (tasks 3.6–3.8) **not yet implemented** — see "Not yet covered".

---

## Environment

| Item | Value |
|---|---|
| Device under test | **FDA4** — Lolin S3, id `30eda0a0fda4`, `192.168.20.169`, greenfield-flashed then OTA-updated to **2.2.0** |
| OTA server | `https://ota.rfsee.net/` (production PHP/nginx host; pinned self-signed cert) |
| Server channel offered | `mainstream` / `unit_type=ghc1` → **2.2.0** (dummy release) |
| Device config | `ota_enable=1`, `ota_url=https://ota.rfsee.net/`, per-unit `ota_secret` (64-char), embedded default cert (`cert_custom=false`) |
| Soak/training unit 2344 | **Untouched** — reserved for plant-model training; never flashed with ROTA firmware |
| Date | 2026-07-13 |

> **Note on 2344:** the operator constraint is that 2344 stays on non-ROTA firmware for training. All device-side ROTA verification is therefore on FDA4, not the usual soak unit.

---

## Results summary

| # | What was tested | TC (slice) | Requirements | Result |
|---|---|---|---|---|
| 1 | Push-OTA of 2.2.0 still works | TC-01 | R-G03 | ✅ PASS |
| 2 | `/api/ota/config` set + readback, secret never echoed | TC-14 | R-F02, R-F03, R-A09 | ✅ PASS |
| 3 | T16 precondition gate blocks on untrusted clock | TC-04 | R-C03 | ✅ PASS |
| 4 | `/api/ota/check` observability + manual trigger | TC-10 | R-O03, R-O05 | ✅ PASS |
| 5 | Live manifest check — **unregistered device rejected** | TC-02 | R-A02, R-A04, R-A05 | ✅ PASS |
| 6 | Live manifest check — **registered device accepted** | TC-02, TC-08 | R-A01, R-A02, R-A05, R-V01 | ✅ PASS |
| 7 | Embedded-default pinned cert used for TLS | TC-03 | R-A04 | ✅ PASS (default path only) |
| S | Server acceptance matrix via device simulator | TC-02, TC-11 | R-A01/02/05/07, R-S* | ✅ 8/8 (server repo) |

---

## Detail

### Test 1 — Push-OTA regression on 2.2.0 (R-G03, TC-01 slice)

**Method:** `python bin/ota_push.py bin/2.2.0/greenhouse-controller-2.2.0.bin --host 192.168.20.169`

**Result:** upload → assets extract → reboot → verify. Post-reboot `/api/status.system`:
```
fw_ver = 2.2.0   asset_version = 2.2.0   (matched)
```
Push-OTA path unaffected by the addition of T16. ✅

_Not covered by this slice:_ the 2344 feature-off capture half of TC-01 (2344 is reserved).

### Test 2 — Config endpoint (R-F02/F03/A09, TC-14 slice)

**Method:** admin login (`POST /api/login`), then `POST /api/ota/config {enable:1, url, secret}`, then `GET /api/ota/config`.

**Result:**
```
POST -> {"ok":true}
GET  -> {"ok":true,"enable":1,"check_h":24,"url":"https://ota.rfsee.net/",
         "secret_set":true,"win_lo":2,"win_hi":4,"cert_custom":false}
```
`secret_set:true` with **no secret value echoed** (R-A09). Validate-then-write accepted a well-formed config. ✅

_Not covered:_ farmer-session negative test (403), URL/range rejection cases, System-tab GUI (TC-14 full).

### Test 3 — Precondition gate on untrusted clock (R-C03, TC-04 slice)

**Method:** immediately after a warm reboot, with `system.ntp_synced=false` (SNTP flag not re-armed after rapid reboots), `POST /api/ota/check` then `GET /api/ota/check`.

**Result:**
```
result="skipped"  result_code=3  http=0  checks=0
```
T16 audited a skip (`value_a=22, value_b=3`) and made **no** network request on a clock it does not yet trust. Correct R-C03 behaviour. ✅

> Design note (recorded in the plan): the gate uses the strict `nm_is_sntp_synced()` latch. Both soak and production networks pass outbound NTP, so a valid clock is reached within ≤5 min (rc.1.5.6 retry). Kept strict deliberately.

### Test 4 — Observability endpoint (R-O03/O05, TC-10 slice)

**Method:** `GET /api/ota/check` (admin).

**Result:** returns the T16 last-check snapshot, including the device's own signing id:
```
{"ok":true,"id":"30eda0a0fda4","last_check":<epoch>,"result":"...","result_code":N,
 "http":N,"checks":N,"offered":"...","running":"2.2.0"}
```
`POST /api/ota/check` queues an immediate check (`{"ok":true,"queued":true}`). The self-reported `id` matched the credentials-repo filename exactly — this is what an operator reads to provision `devices.json`. ✅

_Not covered:_ full log-parse of a complete cycle + push-OTA audit 13–17 non-regression (TC-10 full).

### Test 5 — Live check, unregistered device rejected (R-A02/A04/A05, TC-02 slice)

**Preconditions:** SNTP synced; FDA4 **not** in `devices.json`.

**Method:** `POST /api/ota/check`; poll `GET /api/ota/check`.

**Result:**
```
last_check=1783948232  result="auth_fail"  result_code=4  http=204  offered=""  checks=1
```
`http=204` (not a transport failure `http=0`) proves: the pinned self-signed cert **validated** (TLS handshake completed — R-A02/A04), the `X-OTA-Auth` HMAC header was transmitted and parsed, the server reached its registry lookup, and an unknown device was correctly rejected. T16 mapped 204 → `auth_fail`. ✅

### Test 6 — Live check, registered device accepted (R-A01/A02/A05/V01, TC-02 + TC-08 slice)

**Preconditions:** SNTP synced (`time_iso=2026-07-13T15:28:36`); FDA4 added to `devices.json` (`enabled`, matching `secret`, `unit_type=ghc1`, `channel=mainstream`).

**Method:** `POST /api/ota/check`; poll `GET /api/ota/check` until `checks` increments.

**Result:**
```
id="30eda0a0fda4"  last_check=1783949334  result="up_to_date"  result_code=0
http=200  offered="2.2.0"  running="2.2.0"  checks=1
```
`http=200` means the server recomputed `HMAC-SHA256(secret, id|ts|nonce|uri)` and `hash_equals` matched — the **full per-unit auth** (R-A05), not just reachability. Server resolved `mainstream/ghc1` → offered `2.2.0`; T16 parsed the manifest version and SemVer-compared equal → `up_to_date`, audit `value_a=22, value_b=0`. ✅

**Tests 5 + 6 together** confirm both sides of the auth gate on the live server.

### Test 7 — Pinned cert, embedded default (R-A04, TC-03 slice)

**Result:** all live checks (Tests 5, 6) succeeded with `cert_custom=false` — i.e. using the firmware-embedded `ota.rfsee.net` PEM as the sole trust anchor. Confirms the embedded-default resolver path. ✅

_Not covered:_ GUI cert upload → NVS persistence → reboot survival (TC-03 full).

### Test S — Server acceptance matrix (server repo)

Executed earlier with `bin/rota_sim.py` against `ota.rfsee.net`: **8/8** — TLS + pinned-cert, HMAC accept, wrong-secret reject, nonce replay reject, `ROTA_SKEW_S` clock-skew reject, MITM/other-cert reject, version resolution, `X-Accel-Redirect` download, unauthenticated `204`. Full results live in the `greenhouse-Controller-FOTA-server` repo (TC-02 / TC-11 server side).

---

## Not yet covered (open)

| Area | TC | Blocked by |
|---|---|---|
| Manifest decision: `seq`/`fw_hiwater`, `min_version` guard | TC-08 | task 3.6 **implemented (2026-07-13), builds clean** — hardware verify pending (needs higher version published) |
| Download + SHA-256/size verify; corrupted/truncated artefact | TC-05 | task 3.7 **implemented (2026-07-13), builds clean** — hardware verify pending |
| Apply via T13; kill-power matrix; night-window ∩ quiet-gate | TC-06, TC-07 | task 3.8 **implemented (2026-07-13), builds clean** — hardware verify pending |
| Heap + cadence during a full pull-update | TC-09 | verify with 3.7–3.8 on hardware |
| Re-check of the CHECK path on the 3.6+ build (full manifest parse) | TC-08 | **blocked now:** FDA4 SNTP rate-limited from repeated reboots → T16 skips; retry when SNTP re-syncs |
| Interval/jitter + backoff ladder over a real soak | TC-04 (full) | Phase 5 soak |
| Custom cert upload + reboot persistence | TC-03 (full) | GUI (task 3.3) |
| Farmer-session config negatives; System-tab GUI | TC-14 (full) | GUI (task 3.3) |
| Full-cycle log parse; audit 13–17 non-regression | TC-10 (full) | Phase 5 soak |
| 2344 feature-off regression capture | TC-01 (full) | 2344 reserved for training |
| Server-down / crash-loop rollback; zero code-13 scan | TC-13 | tasks 3.7–3.8 + soak |
| Release rehearsal across both repos (publish → promote) | TC-12 | Phase 4 tooling |

**Headline:** the ROTA **check path** is verified end-to-end on live hardware against the live server (auth both ways, pinned TLS, manifest fetch, SemVer decision, observability). A real pull-**install** cannot be demonstrated until tasks 3.6–3.8 land and the server offers a higher version than the unit runs.

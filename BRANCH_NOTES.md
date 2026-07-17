# Branch: `rota` — remote (internet-pull) OTA

> **STATUS — CLOSED (2026-07-17).** ROTA shipped across the 2.2.x series; the
> `rota` branch was **merged to `main` (fast-forward) and deleted**. ROTA is now
> mainline and development continues on `main`. This file is kept as the
> historical branch record — for current ROTA reference use
> [`design/rota_tds.md`](design/rota_tds.md), [`changelog.md`](changelog.md), and
> the FOTA-server repo's `documentation/documentation.md`. **Superseded below:**
> the "never flash 2344 with ROTA" rule — 2344 and FDA4 are now both ROTA
> `soak`-channel units, and 5C88 runs ROTA on `mainstream`.

## What this branch is

This branch adds **ROTA** — remote, internet-pull OTA — to the
greenhouse-Controller firmware. A new FreeRTOS task (**T16**) periodically
checks a configured OTA server over the internet, downloads and verifies a
release (firmware + web assets), and installs it during a night window — no
farm visit and no LAN push required. It runs **alongside** the existing
push-OTA (web-GUI upload) path, which is unchanged.

- Technical design: [`design/rota_tds.md`](design/rota_tds.md)
- Implementation plan: [`design/rotaImplementationPlan.md`](design/rotaImplementationPlan.md)
- Feasibility study: [`design/remoteOTAstudy.md`](design/remoteOTAstudy.md)
- Operator-facing summary: the `[2.2.0]` section of [`changelog.md`](changelog.md)

This work builds on the **completed v2.0.0 ESP-IDF migration** (arduino-esp32 →
pure ESP-IDF, `framework = espidf`), which shipped and is the baseline on
`main`. The current firmware line is **2.2.x**.

## Three-repository split

ROTA spans three repositories:

| Repo | Role |
|---|---|
| **greenhouse-Controller** (this repo) | Firmware T16 pull client + System-tab web-GUI config |
| **greenhouse-Controller-FOTA-server** | OTA server — PHP under nginx (`manifest.php`, `download.php`) |
| Operator's secret store (private) | Per-unit `ota_secret`s and the server key/certificate material |

## Why a separate branch

- **5C88** is production at Herenboeren Willemshoeve (Soest). It must not be
  disturbed by in-development remote-update code. 5C88 is behind NAT
  (outbound-only) with no remote push path — production updates queue until
  someone is on-site.
- ROTA is a multi-part feature — device client, server, and a mutual-auth
  security model — developed and soak-tested on a bench unit before any
  production exposure.
- Branch protection on `main` rejects merge commits and force-pushes; this
  branch **rebases + fast-forwards** into `main` when ROTA is production-ready.

## Security model (summary — full detail in the TDS)

- The server's self-signed certificate is **pinned by SHA-256**; a server
  presenting any other certificate is rejected.
- The device authenticates with a **per-unit HMAC-SHA256** header (`X-OTA-Auth`,
  ±300 s skew window + nonce replay cache). Wrong secret → no manifest.
- **Anti-downgrade**: a release is accepted only if its version is newer **and**
  its `seq` beats the persisted NVS high-water mark; `min_version` is honored.
- Both artefacts are downloaded into PSRAM and checked against the manifest
  SHA-256 + size **before any flash write** (mismatch aborts; the old bank boots).
- Apply/commit/reboot happen **only** inside the night window behind a quiet gate
  (no window motion; no wind override / motor alarm / calibration; no active web
  or LCD session), reusing the T13 push-OTA machinery and its 3-fail rollback.
- Firmware signing is deferred (a `key_id` field is reserved).

## Test / soak discipline

- Dev + soak units: **FDA4** and **2344** (both on the ROTA `soak` channel).
  *(Historical: 2344 was originally reserved for plant-model training only; it
  is now a ROTA soak unit, back in active service.)*
- Latest release: **2.2.15** (gh#42 SD-scan buffer fix — see [`changelog.md`](changelog.md)).
- **Paired-commit invariant**: firmware and web assets must publish/commit
  together — a firmware-only push strands the asset partition. Verify a ROTA
  install by reading **both** `fw_ver` and `asset_version` from `/api/status`.

## Working policy — `main` vs this branch

| Concern | `main` (2.2.x baseline) | `rota` (this branch) |
|---|---|---|
| Active development | Maintenance / production fixes | ROTA feature work |
| New features | No | ROTA + supporting changes |
| Forward merge | — | Rebase + fast-forward into `main` when production-ready |
| Force-pushes | Blocked by branch protection | Blocked by branch protection |
| Linear history | Required | Required |

## Predecessor — the v2.0.0 ESP-IDF migration

The immediately-prior major effort was the arduino-esp32 → ESP-IDF migration
(the `dev/2.0.0-esp-idf` branch), now shipped and merged. Its rollback anchor —
the last arduino-esp32 build — remains the annotated tag
`v1.20.3-arduino-final`. The phase-by-phase migration narrative is in the
`[2.0.0]` section of `changelog.md`.

## How to switch back to the released line

```bash
git checkout main          # 2.2.x baseline (ESP-IDF, production line)
```

## See also

- [`design/OTAimplementation.md`](design/OTAimplementation.md) — push-OTA reference (11 sections) + §12 ROTA addendum
- [`design/rota_tds.md`](design/rota_tds.md) — ROTA technical design spec
- `changelog.md` — `[2.2.0]` (ROTA feature) and `[2.2.12]` (ROTA hardening test build)

# `rota_release.py` — ROTA release toolchain

Publishes a built firmware release to the internet OTA server so field units can
pull it (ROTA). This is the client side of the **FROZEN wire contract
`rota-contract-v1.1`** ([design/rota_tds.md](../design/rota_tds.md) §4); the
server lives in the separate `greenhouse-Controller-FOTA-server` repo.

Implements **R-T01** (publish → soak, one command from a built `bin/<version>/`
to soak-offered) and the **`ota_promote`** role (soak → mainstream), over
SSH-key transport with host-key verification (**R-T07**). Stdlib Python only.

## Release flow

```
bin/build_release.ps1                 # build bin/<version>/{.bin,.zip}
python bin/rota_release.py publish <version>     # -> soak channel
#   ... soak on FDA4: let it pull, verify fw_ver AND asset_version ...
python bin/rota_release.py promote <version>     # -> mainstream (after soak)
```

`publish` computes SHA-256 + size of both artefacts, assigns the next `seq`,
emits `manifest-<version>.json` (§4.3), uploads all three into
`ota-store/releases/<version>/`, and points `channels/soak.json` at the release.
`promote` points `channels/mainstream.json` at an already-published release.
`status` prints what each channel currently offers.

A device on the `soak` channel (e.g. FDA4) then pulls it on its next check; a
device on `mainstream` (production) pulls it only after `promote` — and only if
not held by a `pinned_version` in `devices.json` (R-T05: production stays pinned
until a good soak cycle, unpinned on-site).

## Pull-based deploy via GitHub Releases (recommended)

Instead of scp-ing straight to the VPS, `release` publishes to a **GitHub
Release** and the FOTA server pulls it (public repo → tokenless fetch). This
keeps every VPS-write key off GitHub — the server pulls with its existing
read-only access, mirroring how the server code already deploys.

```
bin/build_release.ps1
python bin/rota_release.py release <version>              # -> GitHub Release (points soak)
python bin/rota_release.py release <version> --prerelease # -> staged, soak NOT pointed
```

`release` authors the same `manifest-<version>.json` (correct `seq` from the
repo ledger), creates the release at tag `v<version>` on the current HEAD, and
uploads the `.bin`, `.zip` and manifest as assets. A **full release** signals
the server to point `soak`; a **`--prerelease`** stages the bytes without
pointing any channel. Promotion to mainstream stays a manual server-side step.
Creating the release needs a token in `.github/token.local` with **Contents:
Read and write** on the repo — the Issues-scoped `gh_issue.py` token is *not*
sufficient (add the Contents permission to that fine-grained PAT, or point
`GITHUB_TOKEN` at one that has it). The VPS-side retriever
(`ota-store-update.sh`) is step 2.

> Security note: until firmware signing lands (`key_id`/R-A10), write access to
> a release == ability to ship firmware to the *soak* bench. Protect the tag/
> release path (branch protection); mainstream is never auto-pointed.

## One-time setup

1. Copy the config template and fill it in (git-ignored):
   ```
   cp bin/rota_release.env.example bin/.rota_release.env
   ```
2. Add an SSH alias for the VPS to `~/.ssh/config` so the host key, user and key
   are pinned there (R-T07 — no secrets in the repo):
   ```
   Host ota-vps
     HostName <vps-host>
     User <deploy-user>
     IdentityFile ~/.ssh/<key>
     IdentitiesOnly yes
   ```
   Set `ROTA_SSH=ota-vps` in `bin/.rota_release.env`. The per-unit `ota_secret`s
   and the server cert/key live on the VPS / in the operator's secret store — the
   tool never touches them.

## Commands

```
python bin/rota_release.py release 2.2.12            # GitHub Release -> soak (pull deploy)
python bin/rota_release.py release 2.2.12 --dry-run  # preview, no token/network
python bin/rota_release.py publish 2.2.12            # scp to ota-store -> soak (push deploy)
python bin/rota_release.py publish 2.2.12 --dry-run  # preview, no changes
python bin/rota_release.py promote 2.2.12            # promote soak -> mainstream
python bin/rota_release.py status                    # show both channels
```

- `--dry-run` — print every planned action, make no changes. Always dry-run first.
- `--seq N` — override the auto-assigned seq (normally unnecessary).
- `--min-version X` — set the anti-downgrade floor in the manifest.
- `--yes` — skip the confirmation prompt (for scripting).
- `--unit-type ghc1` — the unit type key (default `ghc1`).

## seq (anti-downgrade, R-V01/02)

`seq` is a strictly-monotonic release counter; a device rejects any manifest
whose `seq` is not greater than its NVS high-water mark. The tool assigns
`seq = max(existing) + 1`, taking the maximum across the server's release
manifests **and** the repo's `bin/*/manifest-*.json` master copies (R-S08).
Re-publishing the same version reuses its seq (idempotent). A publish that would
not strictly advance the current soak seq is refused unless `--seq` forces it.
Current ledger: 2.2.0 → seq 30 … 2.2.11 → 37, so 2.2.12 → 38.

## Local test (no VPS) — proven end-to-end

Publish to a local store and pull it with the device simulator against the real
PHP server (`../greenhouse-Controller-FOTA-server`):

```bash
STORE=/tmp/ota-store
python bin/rota_release.py publish 2.2.12 --local "$STORE" --yes
ROTA_STORE="$STORE" ROTA_NO_XACCEL=1 \
    php -S 127.0.0.1:8099 -t ../greenhouse-Controller-FOTA-server/public &
python bin/rota_sim.py --base-url http://127.0.0.1:8099 \
    --id <full-mac> --secret <hex> --fw 2.1.3        # expect 7/7 passed
```

(`--local <dir>` writes straight into a local `ota-store/` instead of scp.)

## See also

- [design/rota_tds.md](../design/rota_tds.md) §4 — wire contract v1.1 (endpoints, manifest, store layout)
- `greenhouse-Controller-FOTA-server/examples/README.md` — store schema + role split
- [bin/rota_sim.py](rota_sim.py) — device-side acceptance suite (verify a publish)
- [BRANCH_NOTES.md](../BRANCH_NOTES.md) — the `rota` branch overview

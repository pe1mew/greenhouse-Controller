# Release 2.16.1

**Date:** 2026-10-04
**Built on:** 2.16.0
**Changes:** a failed asset upload no longer blocks every later mount, and T13 no longer wipes an
intact partition ([gh#89](https://github.com/pe1mew/greenhouse-Controller/issues/89))

**Patch. Bug fixes only: no setting, key, payload or GUI change.** A unit behaves exactly as 2.16.0
until an asset upload fails without rebooting, or a LittleFS mount is refused for something other
than the partition's contents.

> **Not soaked** (the operator's decision). The fix is in OTA's failure paths and a build setting, and
> every acceptance row below ran on hardware, with a fail-first for each part.
> **Published to the ROTA soak channel as seq 62** on 2026-10-04, and 2344 pulled it by itself.

## What changed

- **The VFS table: 8 → 12 entries** (`CONFIG_VFS_MAX_COUNT`).
  - **The bug, in ESP-IDF 5.5.0:** the table refuses every registration once it has been full, even
    with entries freed. This firmware holds 7 entries, and T13's mount of the inactive LittleFS was the
    8th.
  - **What it caused:** after any T13 run that ended without a reboot (a refused zip, a write failure,
    a failed bank switch, by push or by ROTA), nothing could be mounted until a reboot:
    - every later asset upload failed (`inactive LittleFS remount after format failed`);
    - **the SD card could not be remounted** after an unmount or a re-insertion, so SD logging
      stopped.
  - **A stale per-env sdkconfig cannot ship without it:** `ota_manager.cpp` refuses to compile below 9.
- **T13 formats only a partition whose CONTENTS were refused.**
  - **Before:** T13 formatted on any mount failure, so on 2026-10-03 and 2026-10-04 it wiped an intact
    inactive partition, the GUI of the rollback bank, because the VFS table was full.
  - **The driver** now reports a refused contents (corrupt, unformatted, erased) as `LFS_ERR_CORRUPT`
    and keeps the `esp_err_t` behind a failed mount.
  - **Any other refusal** fails the session as `inactive LittleFS mount refused (<esp_err>), not
    formatted` and leaves the partition untouched.
  - **The boot mount follows the same rule** for the active partition. Erased flash on a new unit
    still reads as corrupt and is formatted.
- **Bench builds only, none of it in this image:**
  - `GET/POST /api/diag/ota`;
  - the fail-first mask `types/failfirst_2161.h`;
  - `bin/at_t13_vfs.py`.
- **Version 2.16.1.**

## Verification

| Check | Result |
|---|---|
| Release image | 1 431 344 B, sha256 `b38f293ce7199efb…`, version `2.16.1`, no bench suffix or routes; RAM 20.1 % (65 724 B), flash 68.2 %; assets stamped `2.16.1`, STORE |
| The image is the one tested | **byte-identical** to the image the release stage ran on (`b38f293c…`); `build_release.ps1`'s zip has the tested zip's content |
| The setting in the build | `CONFIG_VFS_MAX_COUNT 12` in the generated `sdkconfig.h`, release and bench |
| Host tests, `drivers/littleFS` | **18 of 18** (UT-LFS-013..018 new). Three mutations each caught by two tests: every refusal as `LFS_ERR_MOUNT` (the old mapping); "a format cures any failure" (the old T13 rule); a success that keeps the last error |
| **Part 1, `vfs`** (refused upload, SD remount, next upload) | **2.16.0: FAILS both** (`mount failed`; `remount after format failed`), the fail-first. **2.16.1 release: PASSES both** |
| **Part 2, `noformat`** (VFS table filled for real, then an upload) | **2.16.1-bench: PASS.** Refused with `mount refused (ESP_ERR_NO_MEM), not formatted`, and the inactive partition still holds `2.16.1-bench` after a reboot. **Fail-first, `OTA_FAILFIRST_2161=1`: FAILS both.** It formats, `remount after format failed (ESP_ERR_NO_MEM)`, and the partition comes back empty |
| `corrupt` (inactive superblock erased) | **PASS:** reads `corrupt (ESP_FAIL)`; T13 formats it, extracts and reboots; it then holds the uploaded assets |
| `boot` (active superblock erased, reboot) | **PASS:** the boot formats and mounts it (its test file present, no GUI), and an assets-only upload restores the GUI |
| 2344 afterwards | 2.16.1-bench (fw and assets), bank A, accepted; both partitions hold the GUI; nine rig settings and the measured 30 mm band unchanged |
| **ROTA publish** | **verified**: GitHub Release `v2.16.1`, published 2026-10-04 17:49:52. Not a draft or prerelease, marked latest, the tag on `01c3462`; all three files downloaded back hash-identical; manifest seq 62 |
| **ROTA pull on 2344** | **PASS**, from the 2.16.0 release (pushed at 18:02), so **2.16.0's own client ran this apply, the first it has done.** Its first check, 18:02:58, downloaded and verified 2.16.1 in 10 s, 13 min after the publish. It then **deferred the apply on its quiet gate, because M1 was opening** (T6's first step after the boot calibration). The 300 s retry's download failed (sub-code 1, TLS/transport). A forced check at 18:20:10 downloaded and verified it, and the apply committed at 18:20:33. After the 18:20:39 boot, `fw_ver` **and** `asset_version` read **2.16.1**, bank A, accepted; the check at 18:21:14 read up to date. Lineair (`graded v2`), the nine rig settings and the measured 30 mm band are unchanged. Heap at 61 s: 68 KB free, 26 KB largest block |

## Upgrading

- **Nothing to do.** No setting, key or NVS record changed.
- **A rollback to 2.16.0 or earlier** brings the bug back, and nothing else changes.

## Known limitations

- **Not soaked** (see above).
- **Found while testing, not fixed: `POST /api/sd/unmount` can panic the unit.**
  - **The race:** the unmount closes FAT's lock while T9 holds it during a write, and the assert reboots
    the unit (decoded from the coredump of 2026-10-04).
  - **How often:** the window is one write, a few ms. It is most likely just after a boot, and it hit
    at 15 s of uptime.
  - **Scope:** every build with the route has it, 2.16.0 and 2.3.1 included. Gotcha 2026-10-04.
- **Rebooting a freshly pushed image too quickly rolls it back,** by design. The boot-loop guard counts
  every boot within 30 s (`OTA_HEALTHY_MS`) of the last, and on the fourth it boots the other bank. A
  push already makes two such boots. The TSDS and the beheerder manual now say so.
- **2.16.1's own ROTA apply has not run yet.** The pull above ran 2.16.0's client, so watching 2.16.1's
  needs the next release. **It ran on 2026-10-05:** 2344 on 2.16.1 pulled and applied 2.16.2 (seq 63);
  see 2.16.2's notes.

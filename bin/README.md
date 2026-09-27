# Greenhouse Controller — Releases and Tools

This directory holds the release build script, the release and test tools, and one directory per
version.

```
bin/
  README.md                <- this file
  build_release.ps1        <- builds a release into bin/<version>/
  ota_push.py              <- pushes firmware + web assets to a unit over the LAN
  rota_release.py (+ .md)  <- publishes a release to the internet-pull OTA (ROTA) channels
  gh_issue.py              <- minimal GitHub Issues client
  check_cfg_desc.py        <- config-descriptor checker (the pre-commit hook runs it on config changes)
  at_*.py                  <- acceptance harnesses for the development rig
  2.14.1/
    greenhouse-controller-2.14.1.bin    <- firmware image
    web-assets-2.14.1.zip               <- web UI (STORE-only ZIP)
    bootloader-2.14.1.bin, partitions-2.14.1.bin
    firmware-2.14.1.elf, firmware-2.14.1.map
    release-notes.md                    <- also the GitHub release text when published over ROTA
    manifest-2.14.1.json                <- ROTA sequence-ledger entry, written when published
  ...
```

---

## 1. Building a release

### Prerequisites

| Tool | Install |
|------|---------|
| PlatformIO Core | `pip install platformio`  or via VS Code extension |
| Python 3.x | https://python.org (must be on PATH for esptool) |
| PowerShell 5.1+ | Pre-installed on Windows 10/11 |

### One-time per clone — enable the repo's git hooks

The repo ships a `.githooks/pre-commit` hook (tracked, no Python framework dependency). It blocks the gh#9
stamped-manifest regression. On a commit that touches the configuration tables, it also runs
`bin/check_cfg_desc.py`, so that a configuration key cannot drift out of its descriptor. Enable it once after cloning:

```powershell
git config core.hooksPath .githooks
```

To check it's active:

```powershell
git config --get core.hooksPath        # should print  .githooks
```

If the hook blocks a commit over `firmware/data/manifest.json`, the fix is almost always:

```powershell
git restore --staged firmware/data/manifest.json   # un-stage the stamped form
git checkout firmware/data/manifest.json           # restore the placeholder from HEAD
```

then re-add whatever else you intended to commit. See gh#9 for the full context.

### Bump the version

> **The build script does not increment the version.** Edit it by hand before running it.

`firmware/platformio.ini` states `FIRMWARE_VERSION` in two environments:

| Environment | Value | Purpose |
|---|---|---|
| `lolin_s3` | `X.Y.Z` | the release build; the script reads this, the first match |
| `lolin_s3_bench` | `X.Y.Z-bench` | the bench build, with development-only diagnostics (`MODBUS_BENCH`) |

`lolin_s3_mbprobe` inherits `lolin_s3`'s flags. The `-bench` suffix is deliberate. ROTA's version
comparison ignores the suffix, so a unit running `X.Y.Z-bench` is never offered `X.Y.Z`, and a pull
test first needs an older plain build pushed. Feature releases bump the minor version and bug fixes
the patch (see `CLAUDE.md`).

### Run the build script

From the **project root** (the directory that contains `firmware/`, `bin/`, etc.):

```powershell
powershell -ExecutionPolicy Bypass -File .\bin\build_release.ps1
```

The script:

0. Stamps the version into `firmware/data/manifest.json`.
1. Builds `lolin_s3`, then copies the firmware image, bootloader, partition table, ELF and map into
   `bin\<version>\`.
2. Builds a filesystem image of `firmware/data/` as a completeness check. This is a check only and is
   never flashed: `pio buildfs` produces a **SPIFFS** image in this project, while the firmware mounts
   LittleFS.
3. Packs `firmware/data/` into a **STORE-only ZIP** (no compression — required by the on-device
   extractor).
3.5. Restores the `manifest.json` placeholder (gh#9).

> **Important:** Do not re-compress the web-assets ZIP with a standard tool.
> The on-device OTA extractor only handles ZIP STORE (method 0).
> DEFLATE entries (method 8) are rejected at flash time with a diagnostic error.

Then add a `## [X.Y.Z]` section at the top of `changelog.md`, and write
`bin\<version>\release-notes.md` in the previous release's structure. When the release is published
over ROTA, that file becomes the GitHub release text.

---

## 2. Putting a release on a unit

**After any update, verify both `fw_ver` AND `asset_version` in `/api/status`.** Neither alone proves
the update: a mismatch means the web-asset partition was left behind.

### Path A — web GUI (a unit on the network)

1. Log in as **Admin** and open the **System** tab → OTA update section.
2. Upload `bin\<version>\greenhouse-controller-<version>.bin`. When it is written, the status reads
   *Firmware ready — please upload the web assets ZIP*. The unit waits for the assets; it does not
   reboot yet.
3. **Within 120 s**, upload `bin\<version>\web-assets-<version>.zip`. The unit writes it and reboots
   once, into both. If more than 120 s pass, the firmware is committed on its own and the unit reboots
   on the old web UI. Upload the assets again after that reboot.
4. Reload the page and check both versions in the footer.

### Path B — scripted push over the LAN

```bash
python bin/ota_push.py bin/<version>/greenhouse-controller-<version>.bin --host <ip>
```

It finds the ZIP beside the image, logs in as admin, pushes both, and reads `fw_ver` and
`asset_version` back from `/api/status`. **Check the target first.** The default host is
`192.168.20.169` (the FDA4 module), so pass `--host` for any other unit, and read `unit_id` from
`/api/status` before pushing.

### Path C — ROTA, the internet pull (no site visit)

```bash
python bin/rota_release.py release <version> --yes
```

This creates the GitHub Release from `bin/<version>/`. The release tags `HEAD`, so **push `HEAD`
first**; an unpushed commit fails with HTTP 422. The FOTA server fetches new releases on a 10-minute
schedule and points the **soak** channel at them. Units then pull on their own hourly check.

- A unit applies an update only **inside its apply window** and **while no web session is open** (the
  quiet gate), so close the GUI while waiting.
- The first download in the minutes after publishing can fail, because the server points the channel
  before it has finished fetching. The unit retries on its next check.
- Promotion to the production (`mainstream`) channel is a separate, deliberate step:
  `rota_release.py promote <version>`.

Full reference: [`rota_release.md`](rota_release.md).

### Path D — first flash over USB (a new unit, or recovery)

A new unit needs two things a normal update never does:
- the **coredump partition** (`0x620000`) erased once, because IDF panics on every boot when it reads
  garbage there;
- the bootloader written with `--flash_mode dio`. The ROM needs dio in the header; the runtime
  `qio` in `platformio.ini` stays, and mixing them boot-loops in `ets_loader.c`.

The offsets and flash settings are the ones in the build's `flash_args`. Its file paths are IDF's,
though, so name PlatformIO's copies directly. Build `lolin_s3`, then from
`firmware\.pio\build\lolin_s3\`:

```powershell
python -m esptool --chip esp32s3 --port COM8 erase_flash
python -m esptool --chip esp32s3 --port COM8 --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB `
    0x0 bootloader.bin 0x8000 partitions.bin 0xe000 ota_data_initial.bin 0x20000 firmware.bin
```

`erase_flash` clears the whole chip, the coredump partition included.

Replace `COM8` with the actual port (Device Manager → Ports).

**Web assets**, either way:

- **Over the network (simplest):** once the unit is on WiFi, upload the ZIP (Path A or B).
- **By cable:** build a **LittleFS** image with `mklittlefs`. Do not use `pio buildfs`, which makes
  SPIFFS here, so the firmware would mount nothing.

  ```bash
  printf '{"asset_version":"X.Y.Z","checksum":""}' > firmware/data/manifest.json
  ~/.platformio/packages/tool-mklittlefs/mklittlefs.exe -c firmware/data -b 4096 -p 256 -s 0x100000 lfs_assets.bin
  python -m esptool --chip esp32s3 --port COM8 write_flash 0x420000 lfs_assets.bin
  printf '{"asset_version":"{{ASSET_VERSION}}","checksum":""}' > firmware/data/manifest.json
  ```

  `mklittlefs` builds a perfectly valid image of an **empty** directory, so check that the image
  contains the files before flashing it. On the serial console, success reads
  `littlefs_mount(A (lfs0)) returned 0 (OK)` and `/index.html exists`.

Leave the unit running for **more than 30 s** before unplugging it (see §3).

---

## 3. Rollback

The firmware rolls back a bad update by itself:

- Every boot increments a fail counter in NVS.
- T1 resets the counter to 0 after **30 s** of uptime.
- A boot that **starts** with the counter at 3 marks the running image invalid and boots the previous
  firmware bank.

So three boots that each die before 30 s make the fourth roll back. If an update leaves the unit in a
boot loop, do nothing: it recovers on its own. The web assets are not rolled back; re-upload the
matching version after recovery.

**On the bench, every short USB session counts as a failed boot**, because unplugging after 15 s looks
exactly like a crash. Let the board run past 30 s: the console then shows
`[OTA] Boot marked healthy - fail counter reset to 0`.

To force recovery immediately, flash a known-good image over USB (Path D).

---

## 4. Partition layout reference

| Name     | Offset     | Size    | Contents |
|----------|------------|---------|----------|
| otadata  | 0x0000E000 | 8 KB    | OTA bank selector |
| nvs      | 0x00010000 | 64 KB   | NVS configuration — survives firmware update |
| app0     | 0x00020000 | 2 MB    | Firmware Bank A |
| app1     | 0x00220000 | 2 MB    | Firmware Bank B |
| lfs0     | 0x00420000 | 1 MB    | Web assets Bank A |
| lfs1     | 0x00520000 | 1 MB    | Web assets Bank B |
| coredump | 0x00620000 | 64 KB   | Crash dump — **erase once on a new unit** |

Banks A and B are always switched together by the OTA manager. NVS is never erased by an OTA update.
The authority is [`firmware/partitions.csv`](../firmware/partitions.csv).

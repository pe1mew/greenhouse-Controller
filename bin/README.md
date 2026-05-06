# Greenhouse Controller — Release Artefacts

This directory holds the release build script and versioned release packages.

```
bin/
  README.md               <- this file
  build_release.ps1       <- release builder script
  1.15.0/
    greenhouse-controller-1.15.0.bin
    web-assets-1.15.0.zip
  1.15.1/
    greenhouse-controller-1.15.1.bin
    web-assets-1.15.1.zip
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

### Bump the version

> **The build script does not increment the version automatically.**
> You must edit the version manually before running it.

Open `firmware/platformio.ini` and update `FIRMWARE_VERSION` in **both** build environments (`lolin_s3` and `test_t2_relay`):

```ini
build_flags =
    ...
    -DFIRMWARE_VERSION=\"1.15.2\"
```

Make sure both occurrences are updated — the script reads only the first match and will build whichever version it finds.

### Run the build script

From the **project root** (the directory that contains `firmware/`, `bin/`, etc.):

```powershell
powershell -ExecutionPolicy Bypass -File .\bin\build_release.ps1
```

Or from inside the `bin\` directory:

```powershell
cd bin
powershell -ExecutionPolicy Bypass -File .\build_release.ps1
```

The script will:

1. Read `FIRMWARE_VERSION` from `firmware/platformio.ini` automatically.
2. Compile the firmware (`pio run -e lolin_s3`) and copy `firmware.bin`.
3. Build the LittleFS image (`pio run -e lolin_s3 -t buildfs`) to validate `data/`.
4. Package `firmware/data/` into a **STORE-only ZIP** (no compression — required by the on-device extractor).
5. Write both files to `bin\<version>\` with the version in the filename.

Output on success:

```
bin\1.15.1\greenhouse-controller-1.15.1.bin   <- firmware binary
bin\1.15.1\web-assets-1.15.1.zip              <- web UI assets (STORE ZIP)
```

> **Important:** Do not re-compress the web-assets ZIP with a standard tool.
> The on-device OTA extractor only handles ZIP STORE (method 0).
> DEFLATE entries (method 8) are rejected at flash time with a diagnostic error.

---

## 2. Uploading firmware

There are two upload paths depending on whether the device already has v1.14.0 or later running.

---

### Path A — OTA via web GUI (device running v1.14.0+)

This is the normal upgrade path for a device already in the field.

**Step 1 — Open the web GUI**

Navigate to the device IP in a browser (shown on the LCD network page or in the serial monitor on boot).  Log in as **Admin**.

**Step 2 — Go to the System tab → OTA update section**

The OTA section is visible to Admin only.

**Step 3 — Upload the firmware binary**

1. Click **Choose File** next to *Firmware (.bin)*.
2. Select `bin\<version>\greenhouse-controller-<version>.bin`.
3. Click **Upload firmware**.
4. The progress bar advances to 100%.  The device reboots automatically.
5. Wait ~10 seconds, then reload the page.  The footer version number should show the new version.

**Step 4 — Upload the web assets**

After the firmware reboot the device is running the new firmware but still serving the old web UI from the previous LittleFS partition.  Upload the new assets to complete the upgrade.

1. Log in again as Admin (session was cleared by the reboot).
2. Go to System tab → OTA update section.
3. Click **Choose File** next to *Web assets (.zip)*.
4. Select `bin\<version>\web-assets-<version>.zip`.
5. Click **Upload assets**.
6. The status field changes to *assets\_write* then *assets\_end*.  The device reboots automatically (~5–10 seconds after the upload completes).
7. Reload the page.  Both the footer version and the served files are now the new version.

> **Order matters:** always flash firmware first, then web assets.
> Flashing assets first onto the wrong firmware bank is a no-op at best.

---

### Path B — Initial flash via USB (first-time or recovery)

Use this path when the device has no firmware yet, the OTA system is not reachable, or you need to recover from a bad state.

**Requirements:** USB-C cable connected to the LOLIN S3 native USB port, PlatformIO installed.

#### Flash everything in one step

```powershell
cd firmware
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e lolin_s3 -t upload
```

Then flash the web assets to **lfs0** (0x420000) — `pio run -t uploadfs` always targets lfs1 and must not be used here:

```powershell
& "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" `
    --chip esp32s3 --port COM8 --baud 460800 `
    write_flash 0x420000 .pio\build\lolin_s3\littlefs.bin
```

Replace `COM8` with the actual COM port (check Device Manager → Ports).

#### Flash using pre-built artefacts

If you have the release files but not the full build environment, flash with esptool directly:

```powershell
# Firmware (app0 at 0x20000)
python -m esptool --chip esp32s3 --port COM8 --baud 460800 `
    write_flash 0x20000 bin\1.15.1\greenhouse-controller-1.15.1.bin

# Web assets (lfs0 at 0x420000)
# The .zip cannot be flashed directly -- you need the raw littlefs.bin from the build artefacts.
# Use Path A (OTA) or rebuild with pio run -t buildfs.
```

> The pre-built `.zip` is for OTA upload only.  Direct flash requires the raw
> `littlefs.bin` produced by `pio run -t buildfs`, not the OTA ZIP.

---

## 3. Rollback

The firmware implements a **3-fail automatic rollback**:

- On every boot the fail counter in NVS is incremented.
- After 30 seconds of stable operation the counter is reset to 0.
- If the counter reaches 3 (three consecutive failed boots) the device automatically reverts to the previous firmware bank and reboots.

If a bad firmware update leaves the device in a boot loop, do nothing — it will roll back on its own after the third attempt.  The web assets are not rolled back automatically; re-upload the matching version after recovery.

To force recovery immediately, use Path B (USB flash) to write a known-good binary.

---

## 4. Partition layout reference

| Name    | Offset     | Size    | Contents |
|---------|------------|---------|----------|
| otadata | 0x0000E000 | 8 KB    | OTA bank selector (written by esptool on every flash) |
| nvs     | 0x00010000 | 64 KB   | NVS configuration — survives firmware update |
| app0    | 0x00020000 | 2 MB    | Firmware Bank A (default boot target) |
| app1    | 0x00220000 | 2 MB    | Firmware Bank B (OTA target) |
| lfs0    | 0x00420000 | 1 MB    | Web assets Bank A |
| lfs1    | 0x00520000 | 1 MB    | Web assets Bank B |

Banks A and B are always switched together by the OTA manager.
NVS is never erased by an OTA update.

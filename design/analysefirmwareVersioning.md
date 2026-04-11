# Firmware Versioning & Update Analysis

**Project:** Greenhouse Controller  
**Date:** 2026-04-11  
**Status:** Analysis (no changes made)

---

## 1. Introduction

A complete firmware release for the greenhouse controller consists of three distinct components:

| Component | Storage | Description |
|-----------|---------|-------------|
| **Firmware binary** | Flash — OTA partition (Bank A or Bank B) | Compiled ESP32-S3 application image |
| **Web GUI assets** | Flash — LittleFS A or LittleFS B (paired with firmware bank) | HTML, CSS, JavaScript files served over HTTP |
| **NVM configuration data** | Flash — NVS partition | Setpoints, credentials, system config, event log |

Each component lives in its own flash partition and has independent lifecycle management. A version is only considered fully deployed when all three components are consistent with one another.

---

## 2. Flash Partition Layout

```
+---------------------+   <-- Flash start
|  Bootloader         |
+---------------------+
|  Partition table    |
+---------------------+
|  NVS partition      |   <- Configuration & event log (persistent across updates)
+---------------------+
|  OTA Bank A         |   <- Firmware image slot A (active or standby)
+---------------------+
|  OTA Bank B         |   <- Firmware image slot B (active or standby)
+---------------------+
|  LittleFS A         |   <- Web assets paired with firmware Bank A (~50 KB)
+---------------------+
|  LittleFS B         |   <- Web assets paired with firmware Bank B (~50 KB)
+---------------------+
```

The NVS partition is intentionally isolated from the OTA banks and LittleFS partitions so that configuration data survives firmware updates. Each LittleFS partition is permanently paired with its same-letter firmware bank. The active LittleFS is always the one matching the active firmware bank (Bank A → LittleFS A, Bank B → LittleFS B). Both switch together on activation.

---

## 3. Versioning Scheme

### 3.1 Firmware Binary Versioning

The project uses **Semantic Versioning** (semver.org): `MAJOR.MINOR.PATCH`

Current release history (from `changelog.md`):

| Version | Date | Notes |
|---------|------|-------|
| Unreleased | — | In-progress work |
| 0.2.0 | 2026-03-26 | — |
| 0.1.1 | 2026-03-07 | — |
| 0.1.0 | 2026-03-06 | — |
| 0.0.1 | 2026-03-05 | Initial release |

All individual driver libraries (LIB-1 through LIB-7) are separately versioned at `0.1.0` and follow the same scheme.

### 3.2 NVS Schema Versioning

The NVS driver (`drivers/nvs/src/nvs_config.h`) introduces a dedicated **schema version** key stored in the `system` NVS namespace under the key `schema_ver`:

```c
#define NVS_SCHEMA_VERSION  1   // current NVS layout version
```

On every boot, `nvs_cfg_init()` reads the stored schema version. If it differs from the compile-time `NVS_SCHEMA_VERSION`:

1. All configuration namespaces (`climate`, `wind`, `motor`, `access`, `wifi`, `mqtt`, `system`) are erased.
2. All keys are repopulated with compile-time defaults.
3. The `log` namespace is **preserved** (event history is not erased during migration).
4. The function returns `NVS_CFG_ERR_MIGRATION` so the calling task can log the event.

This mechanism ensures that a firmware update which changes the configuration layout never leaves the device in an inconsistent or undefined NVS state.

### 3.3 Web GUI Versioning

Web asset versioning is tracked via a `manifest.json` file written into the root of each LittleFS partition by T13 as the final step of every successful web asset update:

```json
{
  "asset_version": "MAJOR.MINOR.PATCH",
  "checksum":      "<hex string — CRC32 or SHA-256 of the zip archive>"
}
```

`asset_version` matches the firmware release the assets were built for. Because LittleFS partitions are permanently paired with firmware banks and both are activated together, a version mismatch between `asset_version` and `system/fw_version` is not expected after a clean update. T11 reads the manifest on startup; an absent manifest or version mismatch is logged and surfaced as a dashboard warning.

---

## 4. OTA Update Mechanism (Task T13)

### 4.1 Overview

Firmware updates are handled entirely by **Task T13 — OTA Manager**, running on **Core 0** alongside the network tasks. Updates are delivered over the local network via the admin web interface. Physical access to device internals is not required (requirements TR-SW02, TR-IF05).

### 4.2 Dual-Bank (A/B) Strategy

The ESP32-S3 flash holds two firmware image banks. At any time one bank is **active** and the other is **standby**:

```
Normal operation:  [Bank A: active firmware] [Bank B: empty/previous]

During update:     [Bank A: running]          [Bank B: receiving new image]

After success:     [Bank A: previous/standby] [Bank B: new active firmware]
                                              (reboot into Bank B)

After 3 failures:  [Bank A: restored active]  [Bank B: bad image]
                   (automatic rollback)
```

### 4.3 Update Procedure — Step by Step

1. **Administrator authentication** — Admin session (8-digit PIN, SHA-256 salted hash) required to access the OTA update page.
2. **Upload firmware binary** — Administrator uploads the new `.bin` image via the web interface. T13 receives it and writes it to the inactive flash bank.
3. **Integrity check** — T13 verifies the image checksum/hash before marking it valid.
4. **Upload web assets** (if applicable) — Administrator uploads a `.zip` archive of HTML/CSS/JS files. T13 buffers it in PSRAM, mounts the **inactive** LittleFS partition, and extracts each file into it. Existing files are overwritten; new files are created. T13 writes `manifest.json` last. The **active** LittleFS partition is never touched; T11 continues serving from it uninterrupted. MX5 is not acquired during this phase.
5. **Activation** — If both components verify successfully, T13 marks the new firmware bank as the next boot target.
6. **Reboot** — System restarts into the new firmware.
7. **NVS schema check** — `nvs_cfg_init()` compares `NVS_SCHEMA_VERSION` with the stored value. If they differ, automatic migration runs (erase config, restore defaults; preserve log).
8. **Health check** — New firmware must pass startup health checks (all critical tasks start, sensors respond, watchdog fed). If not, a failure is recorded.
9. **Rollback on failure** — If the new firmware fails the health check on **3 consecutive boots**, the bootloader automatically reverts to the previous bank.

### 4.4 Rollback Mechanism

The 3-consecutive-fail rollback provides automatic recovery from a defective firmware image with no operator intervention:

```
Boot attempt 1 → health check fails → fail counter = 1
Boot attempt 2 → health check fails → fail counter = 2
Boot attempt 3 → health check fails → fail counter = 3 → rollback to previous bank
Boot attempt 4 → running previous (known-good) firmware + matching LittleFS partition
```

Because each LittleFS partition is permanently paired with its firmware bank, rolling back the firmware bank automatically restores the matching web assets. The previous LittleFS partition was never written during the update and remains intact.

All update and rollback events are logged to the event queue Q3 and persisted via Task T9 (Event Logger) into the NVS `log` ring buffer.

### 4.5 Synchronization During Updates

T13 sets **EG1.OTA_IN_PROGRESS** for the duration of an update. Because T13 writes only to the **inactive** LittleFS partition and T11 reads only from the **active** partition, there is no flash-level conflict between them during a web asset update. T11 is not blocked and continues to serve requests normally while the update is in progress.

MX5 continues to serialise concurrent HTTP file-serve requests within T11 itself, but is not acquired by T13.

EG1.OTA_IN_PROGRESS is used to suppress OTA-page interactions in T11 (preventing a second simultaneous upload) and to signal system state to other tasks.

---

## 5. NVM Configuration Data Across Updates

### 5.1 Data That Persists

The NVS partition is never erased by the OTA process itself. All namespaces survive a firmware update. The `schema_ver` key in the `system` namespace is used to detect a layout change and log it, not to trigger a data wipe.

| Namespace | Keys |
|-----------|------|
| `climate` | T_min, T_max, RH_min, RH_max, rh_ctrl_en |
| `wind` | v_max, dir_excl_low, dir_excl_high, wind_prot_en |
| `motor` | dwell_open_m1–m3, dwell_close_m1–m3 |
| `access` | PIN hashes (farmer & admin), lockout config |
| `wifi` | SSID, PSK hash, AP credentials, IP settings |
| `mqtt` | Broker URL, port, auth, topic prefix, publish interval |
| `system` | poll_interval, session_timeout, ap_timeout, language, log_pointer |
| `log` | Ring-buffer of up to 1000 event records |

### 5.2 Behaviour Per Key on Firmware Update

| Situation | Outcome |
|-----------|---------|
| Key exists and is still used by new firmware | Read as-is — **user setting is preserved** |
| Key absent (new setting added in new firmware) | First `_or_default` call writes the factory default |
| Key exists but is no longer used by new firmware | Never read — orphaned entry; no functional impact |
| Key's storage type has changed | ESP-IDF returns `ESP_ERR_NVS_TYPE_MISMATCH`; T4 must erase and rewrite that key explicitly |

### 5.3 Schema Migration Flow

```
Power-on / reboot after firmware update
      |
      v
nvs_cfg_init()
      |
      +-- Read system/schema_ver from NVS
      |
      +-- [Match]  --> normal startup, all namespaces intact
      |
      +-- [Mismatch or missing]
               |
               v
         Write new NVS_SCHEMA_VERSION to system/schema_ver
         All namespaces left intact
               |
               v
         Return NVS_CFG_ERR_MIGRATION to caller
               |
               v
         T4 (Data Manager) logs migration event via Q3 → T9
               |
               v
         Normal operation:
           existing keys  --> user values read normally
           new/absent keys --> _or_default writes factory default on first access
           orphaned keys  --> ignored, never read
```

---

## 6. Current Implementation State vs. Design

| Feature | Designed | Implemented |
|---------|----------|-------------|
| Dual-bank OTA (A/B) | Yes (T13 design) | Not yet — T13 not implemented |
| 3-fail rollback | Yes (T13 design) | Not yet — T13 not implemented |
| LittleFS web asset update via OTA | Yes (T13 design) | Not yet — T13 not implemented |
| NVS driver with schema versioning | Yes | **Yes — `drivers/nvs/`** |
| GPIO driver (LIB-1) | Yes | **Yes — `drivers/gpio/`** |
| I2C bus driver (LIB-2) | Yes | **Yes — `drivers/i2c/`** |
| DS1307 RTC driver (LIB-3) | Yes | **Yes — `drivers/DS1307_RTC/`** |
| LCD1602 I2C driver (LIB-4) | Yes | **Yes — `drivers/LCD1602_I2C/`** |
| Keypad matrix driver (LIB-5) | Yes | **Yes — `drivers/keyPad/`** |
| Web server (T11) | Yes (design) | Not yet implemented |
| OTA manager (T13) | Yes (design) | Not yet implemented |

The NVS driver is the most relevant implemented component for firmware versioning. It already handles the data-layer versioning and migration that must occur on every firmware update that changes the configuration schema.

---

## 7. Gaps and Observations

1. ~~**No firmware version stored in NVS.**~~ **Resolved.** A `system/fw_version` key (string `"MAJOR.MINOR.PATCH"`) is written on every boot by `nvs_cfg_init()`. It always reflects the currently running firmware and is available to T11 (web dashboard) and T9 (event logger) without requiring the version to be parsed from the binary image.

2. ~~**Web asset version not tracked.**~~ **Resolved.** A `manifest.json` file in the LittleFS partition root records the `asset_version` and a checksum. T13 writes it as the last step of a successful asset update; its absence signals an interrupted transfer. T11 compares `asset_version` against `system/fw_version` on startup and logs/warns on any mismatch.

3. **T13 not yet implemented.** The OTA manager exists only as a design specification. All update handling described in Section 4 is planned behaviour, not running code. Until T13 is implemented, firmware updates require physical USB access via PlatformIO (`pio run -t upload`).

4. **PIN hashes survive firmware updates.** Because namespaces are not erased on schema version mismatch, the `access` namespace (PIN hashes, salt, lockout config) is preserved across updates. The farmer and administrator do not need to re-enter their PINs after a firmware update unless a factory reset is explicitly performed.

5. **No staged / canary update path.** The current design targets a single-device deployment. There is no mechanism for staged rollout across multiple devices. This is appropriate for the current scope but is noted for completeness.

---

## 8. Summary

A firmware version in this project spans three storage regions:

- **Binary** — versioned with semver, deployed to OTA Bank A or B, with automatic dual-bank rollback after 3 consecutive failures.
- **Web GUI** — HTML/CSS/JS files in LittleFS A or B, always paired with the same-letter firmware bank. Updated via T13 OTA as a zip archive buffered in PSRAM and extracted to the inactive LittleFS partition. Activated together with the firmware bank switch; rolled back together with a firmware rollback.
- **NVM data** — persisted in the NVS partition across updates; protected from schema incompatibility by the `NVS_SCHEMA_VERSION` mechanism, which erases and restores defaults when the schema changes while preserving the event log.

The NVS driver (the only versioning-related code currently implemented) correctly handles the data-migration aspect of a firmware update. The OTA transport mechanism (T13) and the web server (T11) are designed but not yet implemented.

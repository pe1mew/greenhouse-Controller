# OTA implementation specification

A field-tested reference for building an over-the-air update system on an embedded device with a web UI, dual-bank firmware, and a separate web-asset partition. Written for LLMs implementing OTA in similar projects; assumes ESP-IDF / FreeRTOS today but the *protocol*, *state machine*, *build contract*, and *verification methodology* sections are platform-agnostic.

This spec captures the design that has shipped 30+ releases on the Greenhouse Controller project (ESP32-S3, 16 MB flash, dual app banks + dual LittleFS) with zero recovered bricks in production. Each design choice carries the rationale that motivated it — adapt freely, but understand why before deviating.

---

## 1. What this OTA system delivers

A network-only software update path that:

1. **Updates firmware and web assets atomically as a paired unit.** A new firmware version and its matching web-UI bundle are flashed and activated together, so a refreshed browser never lands on a UI built for a different API version.
2. **Survives a power loss at any single byte of the flash write.** Both firmware and assets are written to the *inactive* partition; the active partition keeps serving until the boot-slot pointer is swapped atomically at the end.
3. **Auto-rolls back firmware that crashes its first boot.** A consecutive-fail counter in NVS triggers the IDF's built-in `esp_ota_mark_app_invalid_rollback_and_reboot()` after 3 failed boots without a marked-healthy signal.
4. **Returns from a bad upload to the same on-disk state it started from.** Failed asset extractions release the PSRAM buffer, clear state, and emit an error string; no half-written partition is left mounted.
5. **Reports state machine + progress over the same web API used to push the update,** so a CLI/dashboard observer can drive the update and see live progress.

It does **not**:
- Deliver updates from a cloud distribution server (the device is the upload target, not a downloader).
- Support delta updates (full firmware bin, full asset zip, every time).
- Handle multiple concurrent OTA sessions (a single mutex serialises).
- Support resumable uploads (a dropped connection mid-flash forces a restart of that phase).

If your project needs cloud-pull-style updates, this spec is the wrong starting point — look at `esp_https_ota` for that pattern instead. If your device has plenty of flash but no PSRAM, the PSRAM-buffering choice in §5.2 will need a streaming-extractor variant.

---

## 2. The five architectural pillars

### 2.1 Dual-bank firmware partitions

Two equal-sized app partitions (`app0`, `app1`). At any moment exactly one is active (running) and the other is the OTA target. An out-of-band `otadata` partition records which bank to boot from; the bootloader atomically reads it on every reset.

**Why dual-bank, not single + staging:**
- Atomicity for free: the boot-slot swap is a single page write in `otadata`. Either the new image is active, or it isn't. No corruption window.
- Crash recovery comes for free too — the bootloader can fall back to the previous slot if the new one fails to boot enough times.

**Rule:** firmware bin size must fit within one bank, with margin. We size banks at 2 MB and run at 65 % full; that gives headroom for at least a year of feature growth.

### 2.2 Coupled asset partitions

Two LittleFS partitions (`lfs0`, `lfs1`), paired 1:1 with the app banks:

```
app0 active → lfs0 mounted
app1 active → lfs1 mounted
```

The currently-running firmware always mounts the LittleFS partition tied to its bank. The OTA writer always writes the *other* LittleFS partition. The active filesystem is never touched during an update.

**Why pair-bound, not shared:**
- Eliminates an entire class of "firmware version expects asset key X but the live partition has version N-1" bugs.
- Lets you confidently switch firmware versions without invalidating the running UI.
- Trivializes asset rollback: a firmware rollback automatically points the running image at its own paired LittleFS partition, which already contains the assets that match.

**Cost:** doubles the LittleFS storage requirement. Acceptable for web UIs (≪1 MB each); not acceptable if the asset payload is multi-megabyte media.

### 2.3 Atomic paired commit

The boot-slot swap happens **after** both the firmware verification and the asset extraction have succeeded. There is no intermediate state where firmware-N is active but the LittleFS partition contains assets-(N-1).

**Implementation:** `ota_firmware_end()` verifies the new firmware image but does *not* call `esp_ota_set_boot_partition()`. The asset writer is what eventually flips the boot pointer, after the new LittleFS partition contents are durable.

**Fallback path:** if the operator pushes firmware only and never sends the asset bundle, a 120 s timer commits the firmware-only update (boot-slot swap with no asset write). This handles legitimate firmware-only flashes and prevents a half-finished session from blocking future OTA attempts forever.

### 2.4 Streaming firmware, buffered assets

Firmware bytes are written to the inactive bank chunk-by-chunk as the HTTP request body streams in. No staging.

Asset bytes are accumulated in PSRAM until the full ZIP is buffered, then a background task extracts the ZIP entries to the inactive LittleFS partition.

**Why the asymmetry:**
- Firmware images can be large (1-2 MB). Streaming keeps RAM cost flat and lets the IDF's `esp_ota_write` handle erase/program scheduling.
- ZIP archives must be parsed back-to-front (Central Directory is at the end). Streaming a ZIP would require either two passes through the network or a much more complex extractor. PSRAM buffering is simpler and the asset bundles are small (~100 KB-1 MB).

If your device lacks PSRAM, replace this with a streaming ZIP parser or a tar archive instead.

### 2.5 Three-fail rollback with a healthy-marking signal

Every boot increments an NVS counter (`system/ota_fail_cnt`). If a boot survives long enough to be considered stable, a separate task calls `ota_mark_healthy()` which resets the counter to zero. If the counter reaches 3 *before* a healthy mark, the IDF's `esp_ota_mark_app_invalid_rollback_and_reboot()` is called, flipping the boot slot back to the previous app bank.

**The two parameters that matter:**
- `OTA_HEALTHY_MS` — uptime after which "this boot is healthy". 30 s is a workable default; long enough that a crash inside a slow startup task still counts as a failed boot, short enough that legitimate operations get credit quickly.
- Fail threshold — 3 consecutive failures means a single bad reboot doesn't roll back, but two stuck-in-bootloop boots will. Tune higher only if your boot path has known flaky transient failures (you should fix those instead).

**Exempt from the counter:** any deliberate restart your firmware triggers (planned reboots, scheduled maintenance restarts) should set a "this is intentional" NVS flag before calling reset, and the boot-counter logic should skip the increment when it sees that flag with a reset reason of "software reset". Otherwise repeated planned reboots accumulate into a false rollback.

---

## 3. HTTP protocol specification

Three endpoints, all on the device:

| Method | Path | Auth | Body | Response |
|---|---|---|---|---|
| `POST` | `/api/ota/firmware` | Admin | Raw firmware bin (`application/octet-stream`) | `200 {ok:true, rebooting:false, awaiting_assets:true}` |
| `POST` | `/api/ota/assets` | Admin | Raw ZIP (`application/zip`) | `202 {ok:true, message:"extracting — poll GET /api/ota/status"}` |
| `GET` | `/api/ota/status` | Farmer or Admin | — | `200 {ok:true, state, progress, error, bank, accepted}` |

### 3.1 Request requirements

- **`Content-Length` is required** on both POST endpoints. The handler returns `400 Bad Request` if absent, because the device needs the size up front to allocate the PSRAM buffer (for assets) and to validate against partition size (for firmware).
- **`Content-Type`** is informational only; the body is read as raw bytes regardless. The push client should still set it correctly (`application/octet-stream` for the firmware bin, `application/zip` for the asset zip).
- **Cookie-based session auth.** A `POST /api/login` call gates the entire OTA flow. The cookie expires; the canonical pattern is one login per partition push (firmware POST → reboot → fresh login → assets POST).

### 3.2 The 200 vs 202 distinction

`/api/ota/firmware` returns **200** when the firmware bytes have been written and verified to the inactive bank. The response intentionally says `rebooting:false` because the device deliberately waits up to 120 s for the asset bundle before committing.

`/api/ota/assets` returns **202** because the body upload completes before the extraction does. The body arrives, the device puts the ZIP in its PSRAM buffer, the handler returns immediately, and a background task starts extracting. The client must poll `/api/ota/status` to observe extraction progress.

### 3.3 Status response shape

```json
{
  "ok": true,
  "state": "assets_writing",
  "progress": 73,
  "error": "",
  "bank": "B",
  "accepted": true
}
```

| Field | Meaning |
|---|---|
| `state` | One of `idle`, `fw_writing`, `fw_verifying`, `assets_buffering`, `assets_writing`, `rebooting`, `error`, `fw_done`. |
| `progress` | Percent (0-100) within the current state's scope. Resets to 0 at each state transition. |
| `error` | Empty when `state != "error"`. Free-form message when in error state — describes what failed. |
| `bank` | `'A'` or `'B'` — the currently-running app slot. Lets the client see which bank an update will target. |
| `accepted` | `true` if the current boot has been marked healthy (3-fail counter at zero). Useful for verifying rollback safety before issuing another OTA. |

### 3.4 Terminal states for polling

The client polls until `state ∈ {idle, rebooting, fw_done, error}`. Anything else is a transient. Notable:

- **`idle`** after asset extraction = success, ready for next OTA or normal use.
- **`rebooting`** = boot-slot swap done, reboot scheduled, connection imminent loss.
- **`fw_done`** = firmware-only path took the fallback timer (no asset upload arrived in 120 s). Cosmetically distinct from `idle` so the client knows the firmware-only path was taken.
- **`error`** = poll terminated; read `error` for the reason.

A graceful client treats "GET /api/ota/status connection refused" during polling as "device probably rebooting" (not an error) and switches to waiting-for-reboot mode.

---

## 4. Device-side state machine

```
                         ┌──────┐
              ┌──────────│ IDLE │──────────┐
              │          └──────┘          │
       firmware POST                  assets POST
              ↓                            ↓
        ┌──────────┐               ┌──────────────────┐
        │FW_WRITING│               │ASSETS_BUFFERING  │
        └──────────┘               └──────────────────┘
              ↓                            ↓
        ┌──────────────┐            ┌────────────────┐
        │FW_VERIFYING  │            │ ASSETS_WRITING │
        └──────────────┘            └────────────────┘
              ↓                            ↓
        ┌──────────┐                 ┌────────────┐
        │ FW_DONE  │───assets POST──►│            │
        └──────────┘                 │  REBOOTING │
              │                      │            │
       120 s timeout                  └────────────┘
              ↓                            ↓
        ┌──────────┐                 (device resets, slot swapped)
        │REBOOTING │
        └──────────┘

  Any state on error → ERROR (with error string set)
  ERROR → IDLE on next valid POST
```

**Invariants:**

1. `IDLE` is the only state in which a fresh POST is accepted without surprise. From `ERROR` and `FW_DONE`, fresh POSTs are also accepted (these are "graceful retry" entries).
2. Exactly one OTA session can be in any non-IDLE/ERROR/FW_DONE state at a time. The state mutex enforces this.
3. The OTA-in-progress flag (the project uses an EG1 event-group bit) is set when entering any active state and cleared on every exit path, including error. Other tasks read this bit to defer non-essential work (in our case: WiFi reconnect timers, status pushes).
4. The state mutex must NOT be held during the actual flash erase/program calls — those can take seconds and would lock out the status endpoint.

### 4.1 Why FW_DONE is a separate state

The naive design is `FW_WRITING → FW_VERIFYING → REBOOTING`, with `esp_ota_set_boot_partition()` happening in `FW_VERIFYING`. That works for firmware-only OTA but creates a race for the paired model:

- Firmware uploaded and verified
- Boot-slot is now pointing to the new app
- Asset upload begins
- Asset extraction fails (corrupt ZIP, partition error, out of memory)
- Device reboots into new firmware against the *old* LittleFS partition → API mismatch

The `FW_DONE` state defers the boot-slot swap until after the asset extraction succeeds. The 120 s fallback timer handles the legitimate firmware-only case while preserving the paired-commit invariant when assets are coming.

### 4.2 Why ASSETS_WRITING is in a separate task

The web-server task that handles `/api/ota/assets` must return the 202 response quickly. ZIP extraction can take 5-30 s depending on asset count and partition speed. Doing extraction inline:
- Holds the HTTP socket open for the entire extraction.
- Blocks other web requests behind the OTA handler.
- Risks the client timing out the socket and retrying the upload.

A dedicated background task takes ownership of the PSRAM buffer, frees the web-server task to return immediately, and frees its own resources on exit.

---

## 5. Build artifact contract

A release produces six files in a per-version directory:

```
bin/<version>/
  greenhouse-controller-<version>.bin    ← firmware binary, the OTA payload
  web-assets-<version>.zip               ← STORE-only ZIP of all assets
  bootloader-<version>.bin               ← for greenfield serial flashes (not OTA)
  partitions-<version>.bin               ← for greenfield serial flashes (not OTA)
  firmware-<version>.elf                 ← for coredump decoding
  firmware-<version>.map                 ← for symbol lookup
```

### 5.1 Firmware bin requirements

- **Verified by SHA-256** by `esp_ota_end()`. No app-level integrity check needed beyond what the IDF already does.
- **Size fits within the inactive app partition.** Catch this at build time by failing the build if the .bin exceeds 90 % of partition size; that leaves headroom for the next few patches.
- **Built from the exact source tree the .elf+.map archive captures.** Coredump decoding fails silently if the .elf is from a different build (IDF's `esp_core_dump_check` verifies a SHA over the app image).

### 5.2 Asset ZIP requirements — STORE method only

The on-device ZIP extractor handles **method=0 (STORE) only**. DEFLATE-compressed entries are rejected at flash time with a diagnostic error. This is non-negotiable:

- Web assets (HTML, CSS, JS) are typically already minified and gzip-served at runtime by the device. Re-compressing them in transit wastes time on both ends.
- DEFLATE inside an OTA path means shipping zlib in the firmware OR a third-party decompressor. Both add code size and attack surface.
- STORE means the device just memcpys each entry; the parser is ~200 lines of straightforward code.

**Build-side hazard:** standard zip libraries write method=8 by default. PowerShell's `System.IO.Compression.ZipArchive` with `CompressionLevel.NoCompression` still writes method=8 with a level-0 stream on .NET Framework 4.x (PowerShell 5.1). The fix is to write the ZIP bytes manually — Local File Header, Central Directory, EOCD — with method=0 hardcoded. Verify after build by inspecting offset 8 of the file (the compression method byte) and refusing to ship a non-zero value.

**Why STORE is also faster:** the asset payload is already small (~100 KB), the OTA transfer is on a LAN (megabits/sec), and the per-byte extraction cost on the device is dominated by flash write speed, not by parser CPU. Compression saves nothing useful here.

### 5.3 Asset version stamping — the placeholder dance

The asset bundle ships with an `asset_version` field that the running firmware reads at boot and surfaces in the status JSON. The placeholder pattern:

1. The in-tree `manifest.json` source file contains a literal placeholder: `{"asset_version":"{{ASSET_VERSION}}","checksum":""}`.
2. A pre-commit hook rejects any commit where `manifest.json` does NOT contain the placeholder (catches forgotten un-stamping after a build).
3. The build script (Step 0) overwrites the placeholder with the literal version BEFORE running the LittleFS image build, so the LittleFS image gets the literal.
4. Both the LittleFS image (for greenfield flashes) and the asset ZIP (for OTA) carry the literal version.
5. The build script (Step 3.5, post-build) writes the placeholder back so the in-tree file matches the pre-commit hook's expectation.

This survives release builds without polluting the source tree and catches the "I forgot to bump the asset version" mistake at commit time, not at OTA time.

### 5.4 Versioning convention

SemVer. The firmware version is embedded as a `-DFIRMWARE_VERSION=\"X.Y.Z\"` compile-time string in the build config; the asset version comes from the placeholder dance above. **Both must equal the release tag** for the verification step to pass.

The push client compares both numbers post-OTA and exits non-zero if they don't match. This catches: stale partition reads, wrong-version asset bundle paired with a firmware bin, a pre-release version stamp that escaped into a production release.

---

## 6. Push client responsibilities

The push client (`bin/ota_push.py` in this project) is the operator's tool for shipping an update. Implement these properties in any port:

### 6.1 Baseline capture

Before any POST, hit `GET /api/status` to capture:
- Current firmware version
- Current uptime
- Unit identifier (so logs say "OTA'd unit 5C88", not "OTA'd 192.168.20.150")

Print these. They're the "before" half of the success report.

### 6.2 Single login per phase

The cookie-based session has a timeout. The naïve "login once, push everything" pattern fails between firmware-POST and assets-POST because the device reboots in between. Pattern:

```
login → POST firmware → wait_reboot → login again → POST assets → wait_extract → wait_reboot
```

Re-login after every observed reboot. This makes the script resilient to slow extractions and to network stalls.

### 6.3 Wait-for-reboot via uptime drop

After each reboot, the device's `uptime_s` resets to 0 and starts climbing again. Use that as the reboot signal: poll `/api/status` and watch for `uptime_s < pre_uptime`. Add a small safety margin to `pre_uptime` (the POST took time too) to avoid spurious "no reboot detected" false negatives.

Tolerate `GET /api/status` failing during the gap — the device is offline, that's the whole point of waiting. Print a "no response" dot and keep polling.

### 6.4 Async extraction polling

For the assets POST, the response is `202` and extraction happens asynchronously. Poll `/api/ota/status` every 1 s; print state + progress on every change. If the GET starts failing connection-refused, that's the reboot — switch to wait_for_reboot mode.

Set a generous extraction timeout (60 s typical for ~100 KB payloads on slow flash) and exit with a clear error if hit.

### 6.5 Auto-discovery of paired artifacts

Take only the firmware bin path on the CLI. Derive:
- Version from the filename (`greenhouse-controller-<version>.bin`)
- Asset ZIP path (`web-assets-<version>.zip` in the same directory)

This prevents the "OTA'd firmware A with assets B" version-mismatch mistake.

### 6.6 Post-OTA verification

Final `GET /api/status` must show:
- `fw_ver == <expected_version>`
- `asset_version == <expected_version>`
- `ntp_synced == true` (sanity check that network came back up)
- `uptime_s` small (recent reboot)

Mismatch on any → exit non-zero with a specific error. The verification is what turns a "OTA succeeded" claim into an evidenced fact.

---

## 7. Verification methodology

A release is verified by **OTA pushing it to a soak unit, watching its serial console + SD logs for an extended uptime window, and comparing the post-OTA behavior to a known baseline.**

### 7.1 Pre-push checks

1. SHA-256 of every artifact recorded in the release notes.
2. Build delta vs previous release published (flash bytes, RAM bytes) — sudden growth is a regression signal.
3. Push client's auto-discovery succeeds locally.

### 7.2 Push & immediate verify

1. Push runs end-to-end with no manual intervention.
2. `fw_ver` and `asset_version` both match the target version.
3. First post-OTA `/api/status` shows expected values (NTP, network, sensors).

### 7.3 Soak

For non-trivial changes (anything touching tasks, networking, or persistence): leave the soak unit running for **at least overnight** before pushing the same release to production units. Watch:

- Serial console for unexpected log lines.
- SD log for unexpected `LOG_SYSTEM` rows.
- Status JSON for stuck or oscillating values.
- Memory usage (heap free) for downward drift.

Soak length scales with risk. A version-bump-only release needs ~10 minutes (verify boot + first POST + first NTP). A new task or new partition-write path warrants 24+ hours.

### 7.4 Production rollout

Push to a single production unit first. Watch its remote status push for 24+ hours. If healthy, push to remaining production units in batches.

For high-risk changes, stagger across batches so a discovered regression has a bounded blast radius.

---

## 8. Edge cases and the lessons that produced them

These are the corners that bit us at least once; each is worth implementing before they bite you.

### 8.1 Firmware-only OTA must work cleanly

Some operators legitimately want to push firmware without re-flashing assets (during fast iteration, asset-side hasn't changed). The 120 s fallback timer after `FW_DONE` makes this work without a separate API endpoint.

### 8.2 Asset-only OTA writes to the ACTIVE LittleFS partition

If no firmware was uploaded in this session, the boot-slot swap should not happen. Asset extraction in that case should target the active LittleFS partition, not the inactive one. Otherwise the new assets land on a partition that won't be mounted until a firmware OTA happens — and if the operator then does a serial-cable greenfield flash (which resets to bank A), the assets become stranded on lfs1 forever.

### 8.3 Pre-commit hook for the placeholder dance

Without this, the placeholder gets accidentally committed as the literal version (Bob built locally and forgot to revert). The next OTA goes out with a stale asset_version. The fix is the hook, not "remember to revert" — humans don't remember.

### 8.4 Coredump partition must be erased on first boot

The IDF reads the coredump partition unconditionally on every boot. If the partition was added to a layout that previously didn't have one, the IDF reads garbage and logs a "Core dump flash config is corrupted" panic. First flash on every new unit needs:

```
esptool.py --chip esp32s3 --port COMx erase_region <coredump_offset> <coredump_size>
```

Bake this into the greenfield-flash procedure, not into the OTA path (OTA can't reach the coredump partition).

### 8.5 The reboot timer can overflow the timer service stack

If your "schedule a reboot" callback runs in the FreeRTOS timer service task and calls `esp_restart()` directly, the IDF's `esp_wifi_stop()` teardown path consumes several KB of stack and overflows. Spawn a small worker task with a 4 KB stack to actually call `esp_restart()`. The timer just creates the task.

### 8.6 Asset partition needs format-on-mount-failure for first boot

When a new firmware version mounts its paired LittleFS partition for the first time after a greenfield flash, the partition has random flash contents. The mount call fails. The OTA writer should detect that specific failure mode and format the partition before writing. Otherwise the first asset push to a fresh unit fails mysteriously.

### 8.7 Cookies survive cross-boot in some browsers, fail predictably in others

Don't rely on a browser's cookie staying valid across the device's reboot. The push client should re-login after each reboot. The web UI gets the same treatment — show a re-login prompt when a 401 returns post-reboot, don't try to silently retry.

### 8.8 `Content-Length` is non-optional

Chunked transfer encoding without Content-Length: the device doesn't know how much PSRAM to allocate for the asset buffer. The handler must reject these with `400 Bad Request` rather than guess. Document this in the API spec, validate it in the client.

### 8.9 Build artifacts must be archived per-release, not just at HEAD

The `.elf` and `.map` files are needed for coredump decoding for the lifetime of that release in the field — which can be months. PIO/CMake overwrite these on every build. The build script must `cp` them into the per-version directory at release time, otherwise a coredump captured from a unit running v2.0.0 becomes undecodable as soon as anyone builds v2.0.1 on the same checkout.

### 8.10 The OTA-in-progress flag must be respected by other tasks

If a periodic status push is running concurrently with the flash write, the flash write goes 3-5× slower because of bus contention and interrupt latency. Other tasks should check the OTA-in-progress flag and defer non-essential work for the duration. We do this via an event-group bit; any cross-task signal works.

---

## 9. Things this design deliberately does NOT do

- **Resumable uploads.** A dropped connection mid-firmware means restarting the firmware POST from byte 0. Trade-off chosen to keep the protocol stateless; in our LAN scenario, dropped connections are rare and the retry cost is acceptable.
- **Authenticated firmware signing.** The device verifies SHA-256 of the OTA bin but does not check a signature. Anyone who can authenticate to the device's admin role can flash arbitrary firmware. This is intentional given the deployment context (private LAN, single operator); change it if your threat model is different.
- **HTTPS.** The HTTP endpoints are plaintext. TLS adds significant code size and certificate-management overhead; we accept the trade-off for a LAN-only deployment.
- **Background download.** The device is the upload target, not a downloader. Adding a "pull from URL" mode would mean adding HTTP client + TLS + signature verification + retry/resume logic — call it a separate project.
- **A/B testing or staged rollouts coordinated at the cloud level.** The push client targets one device at a time; orchestration across many devices is the operator's responsibility.
- **In-place upgrades during active operations.** The device defers non-critical work during OTA but does not interrupt critical control loops (motor control, sensor reads). The OTA window is a brief degraded mode, not a complete pause.

---

## 10. Reference implementation checklist

Use this as a build-order TODO when porting to another project:

### Device side

- [ ] **Partition table** with dual app banks + dual asset partitions + otadata + coredump.
- [ ] **OTA state machine module** with the 8 states from §4, mutex-protected accessors.
- [ ] **Firmware streaming writer** (`ota_firmware_begin/_write/_end`) using IDF's `esp_ota_*`.
- [ ] **Asset accumulator** (`ota_assets_begin/_accumulate/_end`) writing to a PSRAM buffer.
- [ ] **Background extraction task** that owns the PSRAM buffer, extracts to inactive LittleFS, sets boot partition, schedules reboot.
- [ ] **STORE-only ZIP parser** (~200 lines, no external dependencies).
- [ ] **Format-on-mount-failure** for the first-boot path on a paired LittleFS partition.
- [ ] **Three-fail rollback** with `ota_check_rollback()` at boot, `ota_mark_healthy()` after uptime threshold, exempt for planned-reboot flag.
- [ ] **Reboot scheduling** via a worker task spawned by a timer callback (not direct from the timer service task).
- [ ] **OTA-in-progress flag** that other tasks honor.
- [ ] **HTTP endpoints** (`POST /api/ota/firmware`, `POST /api/ota/assets`, `GET /api/ota/status`) with admin auth and required-Content-Length checks.
- [ ] **Status JSON** carrying `fw_ver`, `asset_version`, `uptime_s`, `unit_id` for the push client to verify against.

### Build side

- [ ] **Version stamping into the firmware bin** via compile-time `-D` flag.
- [ ] **Manifest placeholder dance** with pre-commit hook enforcing the placeholder in source.
- [ ] **STORE-only ZIP construction** (manual header writing if your default zip lib defaults to method=8).
- [ ] **Per-release archive** of bin + zip + bootloader + partitions + elf + map under `bin/<version>/`.
- [ ] **Build delta reporting** (flash bytes, RAM bytes, vs previous release).
- [ ] **SHA-256 of artifacts** captured in release notes.

### Push side

- [ ] **Auto-discovery of paired artifacts** from the firmware bin path.
- [ ] **Baseline capture** via `/api/status` before any writes.
- [ ] **Login per phase**, not login-once.
- [ ] **Wait-for-reboot via uptime drop**, tolerant of GET failures.
- [ ] **Async extraction polling** with terminal-state detection.
- [ ] **Post-OTA verification** comparing both fw_ver and asset_version.
- [ ] **Exit non-zero on any mismatch** so CI/scripts can detect failure.

### Documentation

- [ ] **Release notes per version** including SHA-256 of artifacts, build delta vs previous, what changed and why.
- [ ] **Changelog with a SemVer entry per release** including server-side and operator-visible behavior changes.

---

## 11. Provenance

This spec captures the design that landed in the Greenhouse Controller project across the 1.17.x → 2.0.x release cycles, codified after ~30 production OTA pushes with zero rollbacks needed in the field. Notable issues that motivated specific sections:

- **gh#9** — manifest.json placeholder dance and pre-commit guard (§5.3).
- **gh#21 / 1.19.0** — coredump partition addition + erase-on-first-flash requirement (§8.4).
- **gh#22 / 2.0.0-alpha.6.5** — retirement of an NVS-backed event log in favor of SD CSVs (informs §2.1's "fit with margin" rule).
- **2.0.0-rc.1.2** — reboot stack-overflow that motivated the worker-task carve-off (§8.5).
- **2.0.0-rc.1.2.1** — ELF/map archiving requirement after a coredump became undecodable (§5.1, §8.9).
- **alpha.6.24** — format-on-mount-failure for new LittleFS partitions (§8.6).
- **gh#33 / 2.0.3** — independently relevant: T14 → T10 L3 recovery ladder ensures status pushes survive upstream outages, which is what gives OTA observability in the wild.

The version of this spec lives at `temp/OTAimplementation.md`; the canonical implementation lives in `firmware/src/ota_manager/`, `firmware/src/web_server/web_server.cpp` (lines ~2100-2310 for the HTTP handlers), `bin/build_release.ps1`, and `bin/ota_push.py`.

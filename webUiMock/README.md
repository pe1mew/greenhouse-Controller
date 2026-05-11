# Greenhouse Controller — Web UI Mock Server

A lightweight Flask development server that serves the web GUI directly from
`firmware/data/` and emulates every REST and WebSocket endpoint that the real
ESP32-S3 firmware exposes.  
Use it to iterate on the HTML/CSS/JS without flashing the board.

---

## Prerequisites

| Requirement | Version |
|-------------|---------|
| Python      | ≥ 3.10  |
| pip         | any     |

---

## Setup

```bash
# From the project root:
cd webUiMock

# Create and activate a virtual environment (recommended):
python -m venv .venv
# Windows PowerShell:
.venv\Scripts\Activate.ps1
# macOS / Linux:
source .venv/bin/activate

# Install dependencies:
pip install -r requirements.txt
```

---

## Running the server

```bash
# From inside webUiMock/ (virtual environment active):
python mock_server.py
```

Then open **http://localhost:5000** in a browser.

| Role    | PIN        |
|---------|------------|
| Farmer  | `1234`     |
| Admin   | `12345678` |

Stop the server with **Ctrl+C**.

---

## What is emulated

Targets firmware **1.17.20** — canonical nested status-JSON shape, status-website POST configuration (Web tab), and the OTA-version-mismatch diagnostic surfaces.

| Endpoint             | Method | Auth    | Description |
|----------------------|--------|---------|-------------|
| `/`                  | GET    | none    | Serves `firmware/data/index.html` with `?v=<fw_ver>` cache-busters injected on `app.js` / `style.css` references |
| `/style.css`         | GET    | none    | Serves `firmware/data/style.css` |
| `/app.js`            | GET    | none    | Serves `firmware/data/app.js` |
| `/manifest.json`     | GET    | none    | `{"asset_version":"<fw_ver>","checksum":""}` |
| `/api/whoami`        | GET    | cookie  | Returns `{ok, role}` or 401 |
| `/api/login`         | POST   | —       | `{role, pin}` → `{ok, role}` + sets cookie |
| `/api/logout`        | POST   | cookie  | Clears session cookie |
| `/api/status`        | GET    | farmer+ | Full status JSON in canonical nested shape (`climate`, `wind`, `windows:{M1,M2,M3}`, `mode:{current,flags[]}`, `sun`, `system{asset_version,uptime_s,…}`, `update_interval_s`) — same payload as WebSocket push |
| `/api/config`        | GET    | farmer+ | All configuration parameters |
| `/api/config`        | POST   | farmer+ | Write one NVS key; farmer restricted to climate/wind keys |
| `/api/wifi`          | POST   | admin   | Update WiFi SSID (PSK accepted but not stored) |
| `/api/pin`           | POST   | admin   | Change farmer or admin PIN for this session |
| `/api/web`           | GET    | admin   | Current status-website (Web tab) settings; secret never echoed |
| `/api/web`           | POST   | admin   | Update status-website settings; same validation as firmware (URL must end `api.php`, secret ≥ 16 chars, interval 60–300, etc.) |
| `/api/history`       | GET    | farmer+ | `?n=N` — last N synthetic sensor readings |
| `/api/sd/status`     | GET    | farmer+ | `{mounted, free_mb, size_mb}` |
| `/api/sd/mount`      | POST   | admin   | Set SD mounted state to `true` |
| `/api/sd/unmount`    | POST   | admin   | Set SD mounted state to `false` |
| `/ws`                | WS     | none    | Push status JSON every 2 s |

### Simulated data

* **Sensor readings** — temperature, relative humidity, and wind speed are
  generated with slow sine-wave variation so dashboard tiles update visibly.
* **Window states** — always `CLOSED`; system mode always `AUTOMATIC` with empty `flags[]`.
* **`asset_version` always equals `fw_ver`** — the mock has no LittleFS partitions to drift apart, so the `MISMATCH` badge never fires. That's deliberate; the badge is only meaningful on real hardware where an OTA bank flip can leave the new firmware paired with old assets.
* **SD card** — starts mounted (7.5 GB / 7.1 GB free); toggled by mount/unmount.
* **History** — synthetic ring-buffer rows matching the real firmware's format.

### Access control

Matches the firmware exactly:

* Unauthenticated requests to API endpoints return **401**.
* Farmer sessions may only write the keys allowed by `FARMER_WRITABLE` in
  `mock_server.py` (climate thresholds and wind-protection enable).
* Admin-only endpoints return **403** for farmer sessions.

---

## Configuration

The default port is **5000**. To change it, edit the `app.run(...)` call at the
bottom of `mock_server.py`.

The in-memory config state (default threshold values, motor travel times, etc.)
is defined in the `cfg` dict near the top of `mock_server.py`. Changes made via
the web UI are held in memory for the lifetime of the server process.

---

## Differences from the real firmware

| Aspect | Mock | Real firmware |
|--------|------|---------------|
| Session storage | Python dict (in-memory) | FreeRTOS session table |
| Config persistence | In-memory only (reset on restart) | NVS flash |
| WiFi PSK / AP PSK | Accepted but discarded | Written to NVS |
| SD card | Simulated flag | Real SPI SD via SD.h |
| Sensor data | Sine-wave generator | DHT22 + anemometer |
| Window states | Always CLOSED | T2 relay state machines |
| NTP / time | Host system clock | SNTP |
| OTA / firmware version | Single `cfg["fw_ver"]` string in `mock_server.py` (currently `"1.17.20"`) | NVS `system/fw_version`, set on every boot from `FIRMWARE_VERSION` |
| `manifest.json` / `asset_version` | Always equal to `cfg["fw_ver"]` (no real LFS to drift) | Written by `bin/build_release.ps1` into the ZIP; T13 preserves it on the inactive LFS partition during OTA |
| OTA cross-bank routing | Not simulated (single-instance Python process) | Dual OTA banks + dual LittleFS partitions; T13 writes assets to the LFS paired with the inactive bank |

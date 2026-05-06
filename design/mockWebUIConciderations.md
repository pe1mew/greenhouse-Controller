### Why it works without any design changes

The web GUI is a package of static files (HTML, CSS, JS) served from LittleFS by T11. It communicates with the firmware exclusively through five REST endpoints — `GET /api/status`, `GET /api/config`, `POST /api/config`, `POST /api/command`, `GET /api/log` — plus a `POST /login` for session cookies. The GUI itself has no dependency on ESP-IDF, FreeRTOS, or any firmware-specific runtime. It's a standard browser application.

A mock server on the PC simply needs to:
1. Serve the same static files from a local directory
2. Answer the same REST endpoints with dummy JSON

The browser never knows the difference.

---

### Architecture of the mock server

```
Browser (localhost:8080)
        │
        │  HTTP — same origin, no CORS issue
        ▼
  Mock server (Python / Node.js)
        ├── GET  /           → serve index.html from web_assets/
        ├── GET  /assets/*   → serve CSS / JS / images
        ├── POST /login      → always accept any PIN, set session cookie
        ├── GET  /api/status → in-memory dummy state (JSON)
        ├── GET  /api/config → in-memory dummy config (JSON)
        ├── POST /api/config → mutate in-memory config, return 200
        ├── POST /api/command→ mutate window states, return 200
        └── GET  /api/log    → dummy log entries (JSON, paginated)
```

Because the static files and the API both come from the same origin (`localhost:8080`), CORS is not a concern.

---

### What the in-memory dummy state needs to hold

Mirroring what `GET /api/status` would return from a live ESP32:

```json
{
  "mode": "AUTOMATIC",
  "t_avg": 24,
  "rh_avg": 61,
  "wind_speed_avg": 3.2,
  "wind_dir_avg": 215,
  "windows": { "M1": "OPEN", "M2": "CLOSED", "M3": "CLOSED" },
  "is_daytime": true,
  "sensor_fault_t": false,
  "sensor_fault_w": false,
  "wind_override": false,
  "manual_override": false
}
```

The mock server updates these values on a timer (e.g. every 5 seconds) with a small random walk to simulate live sensor drift. `POST /api/command` transitions window states after a short artificial delay to simulate travel time.

---

### Scenario simulation

The most valuable feature of a mock server is being able to trigger edge cases that are difficult to provoke with hardware:

| Scenario | How to trigger |
|----------|---------------|
| Wind override | HTTP `POST /mock/scenario?name=wind_event` → sets `wind_override: true`, all windows `CLOSED` |
| Sensor fault | `POST /mock/scenario?name=sensor_fault` → sets `sensor_fault_t: true` |
| Night mode | Flip `is_daytime: false` → GUI should switch to night setpoints display |
| Log overflow | Inject 500 dummy log entries rapidly |
| Manual override | `POST /mock/scenario?name=manual_override` |

These `/mock/` endpoints are internal test controls — never exposed by the real firmware.

---

### Practical implementation

**Python + Flask** is the lowest-friction option — it's likely already available on the development machine, needs no build step, and is ~150 lines for a complete mock:

```python
# mock_server.py
from flask import Flask, jsonify, request, send_from_directory, make_response
import threading, time, random, copy

app = Flask(__name__, static_folder='web_assets')
state = { "mode": "AUTOMATIC", "t_avg": 22, "rh_avg": 60, ... }
config = { "t_max_day": 28, "rh_max_day": 80, ... }

def drift():
    while True:
        state["t_avg"] += random.uniform(-0.5, 0.5)
        time.sleep(5)

threading.Thread(target=drift, daemon=True).start()

@app.route('/')
def index():
    return send_from_directory('web_assets', 'index.html')

@app.route('/api/status')
def status():
    return jsonify(state)

@app.route('/api/config', methods=['GET', 'POST'])
def handle_config():
    if request.method == 'POST':
        config.update(request.json)
        return jsonify({"ok": True})
    return jsonify(config)

@app.route('/login', methods=['POST'])
def login():
    resp = make_response(jsonify({"ok": True}))
    resp.set_cookie('session', 'mock_session_token')
    return resp
```

**Node.js + Express** is equally straightforward if the GUI development toolchain is already Node-based — and has the advantage that the same `package.json` can run both the mock server and a live-reload watcher for the GUI source files.

---

### Development workflow fit

```
Edit GUI source files
        │
        ▼
python mock_server.py        ← or: npm run mock
        │
        ▼
Open http://localhost:8080    ← full GUI, live sensor drift, all scenarios
        │
  happy with GUI?
        │
        ▼
Package files → upload zip via OTA to ESP32
        │
        ▼
Verify against real hardware
```

---

### One thing to confirm when building the GUI

The mock server works cleanly as long as the GUI uses **relative URLs** for API calls (`fetch('/api/status')` rather than `fetch('http://192.168.4.1/api/status')`). Relative URLs resolve correctly against whichever origin is serving the page — the mock server in development, the ESP32 in production. If the GUI currently hardcodes the ESP32 IP, that should be changed to relative paths.
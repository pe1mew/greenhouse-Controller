# logparser — Greenhouse Controller Log Parser

**File:** `log/logparser.py`
**Document version:** 1.6 (matches firmware 2.0.0-a.6.35.7 — parser unchanged since a.6.35.6; a.6.35.7 was a DOM-only GUI reorder)
**Requires:** Python 3.10+, standard library only (no pip dependencies)

**What's new in 1.6** (matches firmware a.6.35.6):
- **Coredump retrieval audit events.** Three new SYSTEM subtypes (`value_a=18`
  detected-at-boot, `value_a=19` downloaded, `value_a=20` erased). The
  controller now writes the coredump to a dedicated 64 KB partition on every
  panic and exposes it via the GUI Log → Diagnostics panel (admin-only,
  rate-limited 1 op/10 s, audit-logged on every access). The parser renders
  each event with the approximate size where applicable.

**What was new in 1.5** (matches firmware a.6.35.5):
- **Every setting change in either GUI is now audit-logged.** Pre-a.6.35.5 the
  web GUI was silent on `/api/config` numeric writes, `/api/config` string
  writes (tz_str), `/api/wifi`, `/api/pin`, and `/api/web` — eight distinct
  silent paths. All eight now produce `SETPT` rows attributed to `Web UI`.
- **15 new SETPT parameter IDs** (23-37) covering tz_str, WiFi credentials, PIN
  changes, the status-website cfg fields, and `wind_prot_en`. See the SETPT
  table below for the full list and value semantics.
- **Sensitive-value sentinel**: param IDs 23-30 (tz_str, WiFi creds, PINs,
  status URL/secret) use `value_a=1` as a "set/changed" marker. The actual
  value is **never** in the CSV — operators see *who* changed *what kind of
  field* and *when* without the SD card becoming a credential exfil surface.
  Parser renders these rows as `<field> (set)` / `<field> (changed)`.
- New SETPT `value_a=14/15/16/17` OTA-stage events recognised (see SYSTEM
  table below — actually under SYSTEM, included here for completeness).

**What was new in 1.4** (matches firmware a.6.35.3):
- CSV row timestamps are **local time** (used to be UTC). Output column heading
  changed from "Timestamp (UTC)" to "Timestamp (local)". Older logs with UTC
  timestamps render identically — the parser doesn't interpret the timezone,
  just the column heading caveat changes.
- New `value_a=13` SYSTEM event recognised: T13 firmware-only fallback commit
  (added by firmware a.6.34). Rendered as "T13 firmware-only fallback commit".
- New documented producers of `value_a=1, value_b=0/1` (STA WiFi up/down) and
  `value_a=2, value_b=0/1` (NTP timeout/synced) — firmware a.6.35.3 now emits
  these edge-triggered. Older firmware did not emit them despite the spec.

---

## Purpose

Converts raw CSV log files downloaded from the greenhouse controller into
human-readable text.  Both log sources produce the same CSV format:

| Source | How to obtain | Typical filename |
|---|---|---|
| NVS ring buffer | Web GUI → Log tab → Download NVS | `nvs_log.csv` |
| SD card log file | Web GUI → Log tab → select SD file → Download | `20250607163022.csv` |

---

## Installation

No installation required.  The script uses only the Python standard library.

```
Python 3.10+
```

---

## Usage

### Parse a single file

```bash
python logparser.py <file.csv>
```

Creates `parsed_<stem>.txt` in the same directory as the input file.

**Examples:**

```bash
python logparser.py nvs_log.csv
# → parsed_nvs_log.txt

python logparser.py 20250607163022.csv
# → parsed_20250607163022.txt
```

---

### Parse all SD log files in the current directory

```bash
python logparser.py *
```

Scans the current working directory for all files matching the SD card naming
pattern (`YYYYMMDDHHMMSS.csv`).  Processes them in chronological order (the
filename is a local-time timestamp so lexicographic sort = time order).
All files are concatenated into a single output file named after the
**date of the earliest file**:

```
parsed_YYYYMMDD.txt
```

**Example:**

```
Directory contains:
  20250607120000.csv
  20250607180000.csv
  20250608063000.csv

python logparser.py *

Output:
  parsed_20250607.txt   (all three files concatenated)
```

---

## Output format

Each event is rendered as a single line:

```
Timestamp (local)    Type        Initiator       Description
--------------------------------------------------------------------
2025-06-07 14:30:22  [SENSOR ]   System          T=23 °C   RH=65 %
2025-06-07 14:30:52  [RELAY  ]   System          M1: → MOVING_OPEN
2025-06-07 14:31:10  [MODE   ]   System          Vent step → 1 (M1 open)  [T-demand: M1 open  RH-demand: neutral]
2025-06-07 14:35:00  [SETPT  ]   Admin (LCD)     t_max_day: 25 °C → 27 °C
2025-06-07 14:40:00  [SESSION]   Admin (LCD)     Session opened: Admin  [Admin (LCD)]
2025-06-07 14:45:00  [ALARM  ]   System          WIND OVERRIDE: SET — speed 8.5 m/s ≥ v_max 5.0 m/s
2025-06-07 14:50:00  [ALARM  ]   System          WIND OVERRIDE: CLEARED — speed 3.2 m/s, direction 180°
2025-06-07 15:00:00  [SYSTEM ]   System          System boot
```

The **combined file** (wildcard mode) prepends a summary header and appends a
total event count.

---

## Event type reference

### SENSOR
Periodic sensor snapshot posted by the Data Manager (T4) on every poll cycle.

| Field | Meaning |
|---|---|
| `value_a` | Average temperature (°C, integer) |
| `value_b` | Average relative humidity (%, integer) |
| `ch` | 0 (not motor-specific) |
| `param` | 0 (not a config event) |

**Example output:**
```
2025-06-07 14:30:22  [SENSOR ]   System          T=23 °C   RH=65 %
```

---

### RELAY
Motor relay state change.  Posted by the Relay Controller (T2) whenever a
channel transitions to a new state.

| Field | Meaning |
|---|---|
| `ch` | Motor channel: 1 = M1, 2 = M2, 3 = M3 |
| `value_a` | New channel state (see table below) |
| `value_b` | 0 |

**Channel states:**

| Code | Name | Meaning |
|---|---|---|
| 0 | UNKNOWN | Position not established |
| 1 | CLOSED | Fully closed |
| 2 | MOVING_OPEN | OPEN relay energised; travel timer running |
| 3 | OPEN | Fully open |
| 4 | MOVING_CLOSE | CLOSE relay energised; travel timer running |
| 5 | GAP_TO_OPEN | 2 s gap before opening |
| 6 | GAP_TO_CLOSE | 2 s gap before closing |

**Example output:**
```
2025-06-07 14:30:52  [RELAY  ]   System          M1: → MOVING_OPEN
2025-06-07 14:31:00  [RELAY  ]   System          M1: → OPEN
```

---

### MODE
Ventilation step change.  Posted by the Climate Controller (T6) each time it
recalculates the desired ventilation step.

| Field | Meaning |
|---|---|
| `value_a` | Resolved step (0–3) |
| `value_b` | Packed int16: high byte = T-demand step, low byte = RH-demand step (−1 = no demand) |

**Ventilation steps:**

| Step | Meaning |
|---|---|
| 0 | All windows closed |
| 1 | M1 open |
| 2 | M1 + M2 open |
| 3 | M1 + M2 + M3 open (ridge vent) |

**Example output:**
```
2025-06-07 14:31:10  [MODE   ]   System          Vent step → 1 (M1 open)  [T-demand: M1 open  RH-demand: neutral]
```

---

### SETPT
Configuration parameter changed.  Posted by:

- T8 (LCD UI) when a farmer/admin edits a setpoint via the LCD menu —
  `initiator` = `FARMER` or `ADMIN` from the active LCD session.
- T4 (Data Manager) after applying a Q4 message — picks up the `initiator`
  field the caller (T8 or T11) set on the Q4 message. T4 emits the audit row
  with the caller's attribution; this is the canonical "NVS task logs the
  change" path (since 2.0.0-a.6.35.5).
- T11 (Web Server) directly for the four paths that bypass Q4: `/api/config`
  with `str_value` (tz_str), `/api/wifi`, `/api/pin`, `/api/web`. All emit
  `initiator` = `WEB`.

| Field | Meaning |
|---|---|
| `param` | Parameter ID (see table below) |
| `value_a` | Old value (or sentinel `1` for "set/changed" on sensitive fields) |
| `value_b` | New value (or `0` for sensitive fields) |
| `ch` | Motor channel (only relevant for `dwell_open` / `dwell_close`) |

**Parameter IDs:**

| ID | Key | Unit | Type |
|---|---|---|---|
| 1 | t_min_day | °C | numeric, old → new |
| 2 | t_max_day | °C | numeric, old → new |
| 3 | t_min_ngt | °C | numeric, old → new |
| 4 | t_max_ngt | °C | numeric, old → new |
| 5 | rh_min_day | % | numeric, old → new |
| 6 | rh_max_day | % | numeric, old → new |
| 7 | rh_min_ngt | % | numeric, old → new |
| 8 | rh_max_ngt | % | numeric, old → new |
| 9 | hyst_t | °C | numeric, old → new |
| 10 | hyst_rh | % | numeric, old → new |
| 11 | rh_ctrl_en | (enabled/disabled) | boolean, old → new |
| 12 | cr_priority | | numeric, old → new |
| 13 | avg_win_t | samples | numeric, old → new |
| 14 | avg_win_rh | samples | numeric, old → new |
| 15 | v_max | m/s | numeric, old → new |
| 16 | dir_excl_low | ° | numeric, old → new |
| 17 | dir_excl_high | ° | numeric, old → new |
| 18 | dwell_open | s (per channel) | numeric, old → new |
| 19 | dwell_close | s (per channel) | numeric, old → new |
| 20 | poll_interval | s | numeric, old → new |
| 21 | lat/lon | | numeric, old → new (one row per lat_deg / lat_frac / lon_deg / lon_frac sub-field) |
| 22 | cr_applied | | numeric, old → new |
| 23 | tz_str | *(set)* | **sentinel** — TZ string was changed (string value not logged) |
| 24 | wifi_ssid | *(set)* | **sentinel** — WiFi SSID was changed (credential not logged) |
| 25 | wifi_psk | *(set)* | **sentinel** — WiFi STA passphrase was changed (credential not logged) |
| 26 | wifi_ap_psk | *(set)* | **sentinel** — WiFi AP passphrase was changed (credential not logged) |
| 27 | pin_farmer | *(changed)* | **sentinel** — Farmer PIN was changed (PIN not logged) |
| 28 | pin_admin | *(changed)* | **sentinel** — Admin PIN was changed (PIN not logged) |
| 29 | status_url | *(set)* | **sentinel** — Status-website URL was changed (URL not logged) |
| 30 | status_secret | *(set)* | **sentinel** — Status-website shared secret was changed (secret not logged) |
| 31 | status_intv_s | s | numeric, old → new |
| 32 | status_enable | (enabled/disabled) | boolean, old → new |
| 33 | status_expose | bitmask (hex) | numeric, old → new (parser renders as `0xNN`) |
| 34 | log_upload_h | h | numeric, old → new |
| 35 | log_upload_m | min | numeric, old → new |
| 36 | log_upload_rot | (enabled/disabled) | boolean, old → new |
| 37 | wind_prot_en | (enabled/disabled) | boolean, old → new |

**Sensitive-value policy (since 2.0.0-a.6.35.5).** Param IDs 23-30 cover
admin-sensitive settings — PIN rotations, WiFi credentials, the
status-website shared secret, the timezone string, and the status-website
URL. For these the firmware uses `value_a = 1` as a sentinel for "set" or
"changed" and `value_b = 0`. The actual value is **never** written to the
CSV. The audit row stamps *who* changed *what kind of field* and *when*
without making the SD card a credential exfil surface. The parser renders
these rows as `<field> (set)` or `<field> (changed)` rather than the
misleading `1 -> 0`.

**Example output:**
```
2025-06-07 14:35:00  [SETPT  ]   Admin (LCD)     t_max_day: 25 °C -> 27 °C
2025-06-07 14:36:00  [SETPT  ]   Web UI          poll_interval: 60 s -> 30 s
2025-06-07 14:37:12  [SETPT  ]   Web UI          wind_prot_en: enabled -> disabled
2025-06-07 14:38:01  [SETPT  ]   Web UI          status_intv_s: 120 s -> 180 s
2025-06-07 14:39:33  [SETPT  ]   Web UI          status_expose: 0x3F -> 0x0F
2025-06-07 14:42:18  [SETPT  ]   Web UI          pin_admin (changed)
2025-06-07 14:43:05  [SETPT  ]   Web UI          wifi_ssid (set)
2025-06-07 14:43:05  [SETPT  ]   Web UI          wifi_psk (set)
2025-06-07 14:43:10  [SETPT  ]   Web UI          tz_str (set)
```

**Audit attribution (since 2.0.0-a.6.35.5).** Before this release the LCD UI
emitted `SETPT` rows with correct `FARMER` / `ADMIN` attribution, but the
web GUI was silent on every config change — `POST /api/config`,
`POST /api/wifi`, `POST /api/pin`, `POST /api/web` all updated NVS without
an audit row. A PIN rotation or a wholesale climate-setpoint walk through
the browser left zero rows in the SD log. The 2.0.0-a.6.35.5 architecture
puts the audit emission at the NVS-task layer (T4) for all Q4-routed
changes, with T11 emitting directly for the four direct-write paths. Every
setting change in either GUI now produces a `SETPT` row identifying the
operator role (LCD farmer/admin or Web UI) and a `before -> after` (or
sentinel) value pair.

---

### SESSION
User session opened or closed.  Posted by the UI task (T8).

| Field | Meaning |
|---|---|
| `value_a` | Session level: 0=closed, 1=farmer, 2=admin |
| `initiator` | FARMER or ADMIN |

**Example output:**
```
2025-06-07 14:40:00  [SESSION]   Admin (LCD)     Session opened: Admin  [Admin (LCD)]
2025-06-07 14:55:00  [SESSION]   Admin (LCD)     Session closed  [Admin (LCD)]
```

---

### ALARM
Wind override and motor alarm events.

#### Motor alarm (T2 Relay Controller)

| `value_a` | `value_b` | Meaning |
|---|---|---|
| 1 | 0 | Motor alarm onset — all relays de-energised |
| 0 | 0 | Motor alarm cleared |

#### Wind override (T3 Safety Monitor)

| `value_a` | `value_b` | Meaning |
|---|---|---|
| −1 | 0 | SET — wind sensor fault safe-fail |
| speed×10 | v_max×10 | SET — measured speed exceeded limit |
| direction° | excl_low° | SET — direction inside exclusion zone |
| 0 | 0 | CLEARED — wind protection was disabled while active |
| speed×10 | direction° | CLEARED — conditions normalised |

> **Note:** `value_a=0, value_b=0` is ambiguous between motor alarm clearance
> and wind override clearance (disabled path).  The parser reports both
> possibilities.  Context from surrounding RELAY/MODE events will clarify which
> occurred.

#### Sensor read fault (T5 Sensor Poll, since 2.0.0-a.6.35.3)

T5 emits ALARM rows when an I²C / Modbus sensor stops responding (two
consecutive read failures) or recovers. The `channel` field carries the
sensor type so motor alarms and sensor-read alarms are distinguishable:

| `channel` | `value_a` | `value_b` | Meaning |
|---|---|---|---|
| 4 | 1 | 0 | T/RH sensor read fault TRIGGERED |
| 4 | 0 | 0 | T/RH sensor read fault CLEARED |
| 5 | 1 | 0 | Wind sensor read fault TRIGGERED |
| 5 | 0 | 0 | Wind sensor read fault CLEARED |

Channels 1/2/3 remain motor channels (RELAY events) and are not used by
sensor faults. Pre-a.6.35.3 firmware emitted T5 sensor faults with
`channel=0` and `value_a = ±1` / `±2`, which the parser misread as motor
alarms and wind-override events — operators looking at logs from that era
should treat ALARM rows around boot or OTA windows with skepticism.

**Example output:**
```
2025-06-07 14:45:00  [ALARM  ]   System          WIND OVERRIDE: SET — speed 8.5 m/s ≥ v_max 5.0 m/s
2025-06-07 14:50:00  [ALARM  ]   System          WIND OVERRIDE: CLEARED — speed 3.2 m/s, direction 180°
2025-06-07 16:00:00  [ALARM  ]   System          MOTOR ALARM: triggered — all relays de-energised
2025-06-07 16:01:05  [ALARM  ]   System          Motor alarm / wind override: CLEARED
```

---

### SYSTEM

Internal system events from various firmware tasks. `value_a` categorises the
subtype; `value_b` is the payload. The current encoding (firmware 1.18.2)
matches the LOG_SYSTEM table in `firmware/src/event_logger/event_logger.h`:

| `value_a` | `value_b` | Initiator | Producer | Meaning |
|---|---|---|---|---|
| **−1** | drop count | SYS | T9 (synthetic) | Q3 queue overflow — N events dropped |
| **0** | 0 | WEB | T14 status_post | Status POST failed (streak transition) |
| **0** | 1 | WEB | T14 status_post | Log upload failed |
| **0** | 2 | WEB | T14 status_post | Daily slot fired, no closed file on SD (1.17.27+) |
| **0** | 3 | WEB | T14 status_post | Daily slot fired, precondition blocked it (1.17.27+) |
| **1** | 0 | SYS | T10 net_manager | STA WiFi client disconnected |
| **1** | 1 | SYS | T10 net_manager | STA WiFi client connected |
| **1** | 0 | WEB | T14 status_post | Status POST success |
| **1** | 1 | WEB | T14 status_post | Log upload success |
| **2** | 0 | SYS | T10 net_manager | NTP timeout |
| **2** | 1 | SYS | T10 net_manager | NTP synced |
| **3** | 0 | SYS | T10 net_manager | WiFi AP stopped |
| **3** | 1 | SYS | T10 net_manager | WiFi AP started |
| **4** | 1 | SYS | T10 net_manager | Geolocation lookup success |
| **5** | 1–10 | SYS | T4 data_manager | Boot reason from `esp_reset_reason()` (1.17.27+, T4 since 1.17.31) |
| **6** | 0 | WEB | T14 → T9 | Force-rotate marker, last entry in rotated file (1.17.28+) |
| **7** | KB | SYS | T1 watchdog | Heap internal free (KB; every 60 s, 1.17.29+) |
| **8** | KB | SYS | T1 watchdog | Heap PSRAM free (KB; every 60 s, 1.17.29+) |
| **9** | 0 | SYS | T1 watchdog | Heap CORRUPTION detected by `heap_caps_check_integrity_all` (1.17.29+) |
| **10** | 0 | SYS | T2 relay_ctrl | T2 boot calibration skipped — NVS-recovered window state (1.17.36+, gh#18 Phase 3) |
| **11** | uid16 (int16-cast) | SYS | T4 boot + T9 SD-rotation | Unit ID — low 16 bits of WiFi-STA MAC, same format as AP SSID `Greenhouse-XXXX` (1.18.3+, gh#17) |
| **12** | KB | SYS | T1 watchdog | Heap internal largest contiguous block (KB; every 60 s, 1.18.2+, gh#20) |
| **13** | 0 | SYS | T13 ota_manager | Firmware-only fallback commit — verified firmware was committed because no paired web-asset upload arrived within the 120 s window (2.0.0-a.6.34+) |
| **14** | 0 | SYS | T13 ota_manager | OTA firmware POST started — bytes streaming to inactive bank (2.0.0-a.6.35.3+, was `post_log(0)` pre-renumbering) |
| **15** | 0 | SYS | T13 ota_manager | OTA firmware verified OK — awaiting web-asset upload (2.0.0-a.6.35.3+, was `post_log(1)`) |
| **16** | 0 | SYS | T13 ota_manager | OTA asset ZIP extracted OK — reboot scheduled (2.0.0-a.6.35.3+, was `post_log(2)`) |
| **17** | 0 | SYS | T13 ota_manager | OTA asset extraction FAILED — boot partition unchanged (2.0.0-a.6.35.3+, was `post_log(-1)`) |
| **18** | KB | SYS | T4 data_manager | Coredump from previous panic detected in flash at boot (2.0.0-a.6.35.6+) — download via GUI Log → Diagnostics |
| **19** | ≈ bytes/256 | WEB | T11 web_server | Admin downloaded the coredump via `GET /api/coredump/download` (2.0.0-a.6.35.6+) |
| **20** | 0 | WEB | T11 web_server | Admin erased the coredump partition via `POST /api/coredump/erase` (2.0.0-a.6.35.6+) |

**esp_reset_reason codes (value_a=5):**

| Code | Name |
|---|---|
| 1 | POWERON |
| 2 | EXT |
| 3 | SW — `esp_restart()` from software (planned reboot, OTA finalize, etc.) |
| 4 | PANIC — exception, watchdog re-triggered, etc. |
| 5 | INT_WDT — interrupt watchdog |
| 6 | TASK_WDT — task watchdog |
| 7 | WDT — other watchdog |
| 8 | DEEPSLEEP |
| 9 | BROWNOUT |
| 10 | SDIO |

**Example output (real 1.18.0 crash-loop excerpt):**
```
2026-05-14 08:02:33  [SYSTEM ]  System          Boot: esp_reset_reason = 6 (TASK_WDT)
2026-05-14 08:02:35  [SYSTEM ]  Web UI          T14 status POST: success
2026-05-14 08:02:38  [SYSTEM ]  System          STA WiFi client: connected
2026-05-14 08:02:39  [SYSTEM ]  System          NTP: synced
2026-05-14 08:02:41  [SYSTEM ]  System          Heap internal free: 271 KB
2026-05-14 08:02:41  [SYSTEM ]  System          Heap PSRAM free: 8189 KB
2026-05-14 08:02:41  [SYSTEM ]  System          Boot: esp_reset_reason = 3 (SW (esp_restart))
```

**Diagnosing reboot-loop signatures:** consecutive `value_a=5` rows within
seconds of each other indicate a crash loop; the `value_b` payload identifies
the killer. `value_b=6` (TASK_WDT) repeated three times followed by `value_b=3`
(SW) is the OTA-rollback signature — the bootloader marked the new bank
unhealthy and reverted. See `bin/build_release.ps1` notes and the 1.18.1
changelog entry for the field-observed case.

**Legacy/retired:** pre-1.17.31 firmware emitted a `value_a=0, value_b=0,
initiator=SYS` row as the boot marker. That code path was retired in 1.17.31
(boot reason now uses `value_a=5`). The parser still recognises old rows and
reports them as "Legacy boot marker (pre-1.17.31)".

---

## Initiator values

| CSV value | Displayed as |
|---|---|
| SYS | System |
| FARMER | Farmer (LCD) |
| ADMIN | Admin (LCD) |
| MQTT | MQTT |
| WEB | Web UI |

---

## Timestamps

Since firmware 2.0.0-a.6.35.3, timestamps in the CSV are **ISO 8601 local time**
(`YYYY-MM-DDTHH:MM:SS`), matching the SD card filename convention (which has
always been local time, e.g. `20260519163022.csv`). The local-time POSIX TZ is
taken from `cfg.tz_str` in NVS, set by the geolocation lookup or by the operator
via the LCD config menu. The parser displays them as `YYYY-MM-DD HH:MM:SS`.

Pre-a.6.35.3 logs have **UTC** row timestamps inside but **local-time** filenames,
which is why the filename date can differ from the row timestamps when the
controller is in a non-UTC timezone. The parser passes the string through
unchanged in both cases — only the column heading caveat differs. Operators
diffing old vs new logs across an upgrade should account for the local-vs-UTC
shift around the upgrade reboot.

---

## Known limitations

1. **ALARM ambiguity:** `value_a=0, value_b=0` cannot be unambiguously
   distinguished between a motor alarm clearance and a wind override clearance
   triggered by disabling wind protection while the override was active.

2. **MODE packed field:** The high/low byte decoding of `value_b` assumes the
   firmware stores `step_t` in the high byte and `step_rh` in the low byte as
   signed `int8` values.  If the firmware packing changes this must be updated.

3. **NVS log order:** NVS log entries are stored oldest-first.  The parser
   assumes the CSV rows are already in chronological order (as exported by the
   firmware).

4. **Wildcard mode** only picks up files named `YYYYMMDDHHMMSS.csv`.
   Files named `nvs_log.csv` or with other patterns must be passed explicitly.

5. **Heap-fragmentation interpretation:** since 1.18.2 the parser decodes
   `value_a=12` as "Heap internal largest contiguous block (KB)". A healthy
   build should show this value within ~10 % of `value_a=7` (free total).
   A widening gap — free-total stable, largest-block falling — is the
   fragmentation signature flagged in gh#20. There is currently no
   automated alert; visual inspection of the parsed output suffices.

# logparser — Greenhouse Controller Log Parser

**File:** `log/logparser.py`
**Document version:** 1.2 (matches firmware 1.18.2)
**Requires:** Python 3.10+, standard library only (no pip dependencies)

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
Timestamp (UTC)      Type        Initiator       Description
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
Configuration parameter changed.  Posted by the UI task (T8) when an operator
edits a setpoint, or by the Web Server (T11) via REST API.

| Field | Meaning |
|---|---|
| `param` | Parameter ID (see table below) |
| `value_a` | Old value |
| `value_b` | New value |
| `ch` | Motor channel (only relevant for `dwell_open` / `dwell_close`) |

**Parameter IDs:**

| ID | Key | Unit |
|---|---|---|
| 1 | t_min_day | °C |
| 2 | t_max_day | °C |
| 3 | t_min_ngt | °C |
| 4 | t_max_ngt | °C |
| 5 | rh_min_day | % |
| 6 | rh_max_day | % |
| 7 | rh_min_ngt | % |
| 8 | rh_max_ngt | % |
| 9 | hyst_t | °C |
| 10 | hyst_rh | % |
| 11 | rh_ctrl_en | (enabled/disabled) |
| 12 | cr_priority | |
| 13 | avg_win_t | samples |
| 14 | avg_win_rh | samples |
| 15 | v_max | m/s |
| 16 | dir_excl_low | ° |
| 17 | dir_excl_high | ° |
| 18 | dwell_open | s (per channel) |
| 19 | dwell_close | s (per channel) |
| 20 | poll_interval | s |
| 21 | lat/lon | |
| 22 | cr_applied | |

**Example output:**
```
2025-06-07 14:35:00  [SETPT  ]   Admin (LCD)     t_max_day: 25 °C → 27 °C
2025-06-07 14:36:00  [SETPT  ]   Web UI          poll_interval: 60 s → 30 s
```

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
| **12** | KB | SYS | T1 watchdog | Heap internal largest contiguous block (KB; every 60 s, 1.18.2+, gh#20) |

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

All timestamps in the CSV are **ISO 8601 UTC** (`YYYY-MM-DDTHH:MM:SS`).
The parser displays them as `YYYY-MM-DD HH:MM:SS` in the output.

SD card **filenames** use **local time** (e.g. `20250607163022.csv`),
which is why the filename date may differ from the UTC timestamps inside the file
when the controller is in a non-UTC timezone.

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

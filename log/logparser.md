# logparser — Greenhouse Controller Log Parser

**File:** `log/logparser.py`  
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
Internal system events from various firmware tasks.

| `value_a` | `value_b` | Initiator | Meaning |
|---|---|---|---|
| 0 | 0 | SYS | System boot |
| −1 | 0 | SYS | SD write failure — NVS-only mode |
| −2 | 0 | SYS | SD low space at floor — SD logging suspended |
| N > 0 | 0 | SYS | Q3 queue overflow: N events dropped |
| 0 or 1 | 0 | ADMIN | WiFi AP disabled / enabled |

**Example output:**
```
2025-06-07 12:00:00  [SYSTEM ]   System          System boot
2025-06-07 13:10:00  [SYSTEM ]   System          Q3 queue overflow: 3 event(s) dropped
2025-06-07 14:00:00  [SYSTEM ]   System          SD card write failure — falling back to NVS-only logging
```

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

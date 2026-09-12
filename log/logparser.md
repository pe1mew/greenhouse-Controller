# logparser — Greenhouse Controller Log Parser

**File:** `log/logparser.py`
**Document version:** 1.12 (matches firmware 2.7.0 + the `ropeSensor` window-sensor encodings)
**Requires:** Python 3.10+, standard library only (no pip dependencies)

**What's new in 1.12** (window-sensor encodings, `ropeSensor` branch):
- **`SENSOR_HR ch = 3` — M3 window position**, and **`ALARM ch = 6` params
  244—247** — the device events. Both have been emitted by T17 and decoded by
  `logparser.py` since Phase 3 (2026-09-10) but were **never documented here**, which
  is a standing-rule violation: a new encoding is supposed to be learned by the parser
  *and* written down in the same changeset. Paid off after the branch was rebased onto
  2.7.0. **Neither appears in a log from `main`**, which ships without the sensor.
- Also removed the `dwell_*_min` aliases from 2.7.0; they were an `/api/config` field,
  not a log encoding, so nothing here changes for them.

**What's new in 1.11** (matches firmware 2.6.0 — gh#54 + gh#59):
- **`MODE` rows now carry a discriminator.** `LOG_MODE_CHANGE` has two emitters:
  T6's vent-step decision (`param = 0`) and `dm_set_standby_ex()`'s STANDBY
  enter/leave (`param = 47`). Until 2.6.0 both carried `param = 0`, so every
  STANDBY transition was rendered as a ventilation decision that never happened,
  complete with a fabricated T/RH demand pair unpacked from a reserved zero.
  **Rows written by firmware before 2.6.0 cannot be separated** — if an old log
  shows a vent step at the moment an operator toggled STANDBY, that is why.
  `plot_daily.py` and `vent_step_replay.py` now skip `param = 47` rows.
- **Six new `SYSTEM` subtypes, 25-30** (gh#59): `is_daytime` flip, factory reset,
  Q1 command discarded on motor alarm, DS1307 unreadable, T6 move deferred on
  dwell, and Q4 write rejected as an unknown key. See the SYSTEM table.
- **Subtypes 22-24 documented at last.** They have been ROTA check / download /
  apply since firmware 2.2.0 and the parser has always decoded them, but
  `event_logger.h`'s table stopped at 21 — which is how gh#59 came to be filed
  claiming 22 was free.
- **A successful *web* login now writes a `SESSION` row** with initiator `WEB`.
  Before 2.6.0 only the LCD emitted `SESSION`, so the log recorded panel logins
  and nothing about network ones.

**What's new in 1.10** (matches firmware 2.5.0 — gh#58):
- **New event type `PIN_AUTH`.** Failed PIN entry, the lockout that follows five
  failures, and an attempt refused while locked out. Before 2.5.0 none of this was
  recorded anywhere — not on SD, and on the LCD not even on the serial console — so
  repeated PIN guessing left no trace. `SESSION` still records the successes, and
  `PIN_AUTH` deliberately carries **only** the failures, so the two together are the
  full picture. See the **PIN_AUTH** section below. Ordinal 9, appended after `SUN`.
- **`SENSOR` is now marked reserved.** It has had no emitter since rc.1.4.0
  (superseded by `SENSOR_HR`); the decoder stays for pre-rc.1.4.0 archives.

**What's new in 1.9** (matches firmware 2.2.0 — ROTA internet-pull OTA):
- **ROTA audit events (`value_a=22/23/24`).** The T16 internet-pull OTA client
  (`firmware/src/ota_client/ota_client.cpp`) writes a `LOG_SYSTEM` audit row at
  each stage of a remote update — `22` check, `23` download/verify, `24` apply —
  each carrying a `value_b` sub-code. A normal remote update reads
  `22.1 → 23.0 → 24.0 → BOOT`; a daytime find that waits for the night window
  reads `22.1 → 23.0 → 24.1` (then `24.0` when the window opens and the quiet
  gate clears). See the new **ROTA audit sub-codes** table under SYSTEM.
- **6 new SETPT parameter IDs (39-44)** for the ROTA config keys — `ota_enable`,
  `ota_check_h`, `ota_url`, `ota_secret`, `ota_win_lo`, `ota_win_hi`. `ota_url`
  (41) and `ota_secret` (42) follow the sensitive-value sentinel policy
  (`value_a=1` = "set"; the URL/secret is never written to the CSV).
- **Backfilled two decoders that shipped in the parser but were never documented
  here:** SYSTEM `value_a=21` (RTC-vs-NTP divergence, T4, since 2.1.3/gh#37) and
  SETPT ID `38` (`avg_win_wind`, since 2.1.0/gh#35).
- **Corrected the unit** on SETPT `avg_win_t` (13) / `avg_win_rh` (14) from
  *samples* to **min** — the parser has always rendered these config values in
  minutes; the table was stale.

**What's new in 1.8** (matches firmware 2.1.1, gh#34):
- **T14 log-upload failure now records the HTTP status code in the audit row.** Previously all upload failures wrote `value_a=0`; from 2.1.1 onward `value_a` holds the HTTP response code (e.g. `413`) when the server rejected the upload, or `0` when the failure was pre-HTTP (connection, write, or heap error). Parser renders the code: `T14 log upload: HTTP 413`. SYSTEM table updated.

**What's new in 1.7** (matches firmware 2.0.0-rc.1.2.1):
- **Legacy main.cpp heartbeat decoder added.** Pre-a.6.35.3 firmware emitted a
  synthetic `LOG_SYSTEM` row every 5 s with `value_a = uptime_seconds` and
  `value_b = free_internal_heap_kb` (retired in a.6.35.3 per the rationale
  comment in `firmware/src/main.cpp:heartbeat_task()`). The parser previously
  fell through to the opaque `System event: a=N b=M` generic for these rows.
  Now any SD log spanning the a.6.35.3 transition decodes them as
  `Legacy heartbeat (pre-a.6.35.3): uptime=N s, free internal heap=N KB`.
  The collision domain (uptime 5..18 s overlaps documented SYSTEM subtypes
  for SYS-initiator rows) is unavoidable — the parser correctly prefers the
  documented interpretation for those values; for uptime ≥ 19 s (where no
  documented SYS-initiator code exists) the legacy-heartbeat label is used.
  On a typical post-upgrade log this turns the largest source of "unknown
  System event" noise into a readable single-line per row.

**What was new in 1.6** (matches firmware a.6.35.6):
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
human-readable text. Log files are SD-card CSVs in the format described in
TSDS §5.3:

| Source | How to obtain | Typical filename |
|---|---|---|
| SD card log file | Web GUI → Log tab → select SD file → Download | `20250607163022.csv` |

> **rc.1.4.0 row-format upgrade.** From firmware 2.0.0-rc.1.4.0 onward, the periodic sensor snapshot is recorded as **three companion rows** per cycle (event type `SENSOR_HR`, sub-rows discriminated by `ch`: 0 = T+RH at 0.1 °C precision, 1 = wind, 2 = packed window-state bitmask) rather than the single `SENSOR` row used pre-rc.1.4.0. A new `SUN` event type records sunrise/sunset whenever the cached values change (~1 row/day in steady state). The parser decodes both row generations transparently — historical (pre-rc.1.4.0) files keep displaying their legacy `SENSOR` rows unchanged; rc.1.4.0+ files render the triplet + `SUN` rows; mixed files (an upgrade boundary) work too. See `model/logUpdatePlan.md` for the format-change rationale and the bitmask encoding.

> **NVS ring buffer note.** The NVS event-log ring buffer (`nvs_log.csv`) was retired in the 2.0.0-alpha.6.5 series — log persistence is now SD-card-only. The parser remains backward-compatible with the `nvs_log.csv` artefacts that may still be in your archive.

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
Timestamp (local)    Type           Initiator       Description
--------------------------------------------------------------------------------
2026-05-26 05:30:00  [SUN      ]    System          sunrise=05:30 local  sunset=21:36 local
2026-05-26 13:30:00  [SENSOR_HR]    System          T=23.4 degC   RH=65 %
2026-05-26 13:30:00  [SENSOR_HR]    System          wind=3.5 m/s  dir=158 deg
2026-05-26 13:30:00  [SENSOR_HR]    System          M1=OPEN  M2=OPEN  M3=OPEN  (0x002A)
2026-05-26 13:30:52  [RELAY    ]    System          M1: -> MOVING_OPEN
2026-05-26 13:31:10  [MODE     ]    System          Vent step -> 1 (M1 open)  [T-demand: M1 open  RH-demand: neutral]
2026-05-26 14:35:00  [SETPT    ]    Admin (LCD)     t_max_day: 25 degC -> 27 degC
2026-05-26 14:40:00  [SESSION  ]    Admin (LCD)     Session opened: Admin  [Admin (LCD)]
2026-05-26 14:45:00  [ALARM    ]    System          WIND OVERRIDE: SET — speed 8.5 m/s >= v_max 5.0 m/s
2026-05-26 14:50:00  [ALARM    ]    System          WIND OVERRIDE: CLEARED — speed 3.2 m/s, direction 180 deg
2026-05-26 15:00:00  [SYSTEM   ]    System          System boot
```

The output above mixes the rc.1.4.0+ event types (`SENSOR_HR`, `SUN`) with the carried-over types (`RELAY`, `MODE`, `SETPT`, `SESSION`, `ALARM`, `SYSTEM`). The legacy `SENSOR` row type is silently retained for pre-rc.1.4.0 files — see the Event type reference below.

The **combined file** (wildcard mode) prepends a summary header and appends a
total event count.

---

## Event type reference

### SENSOR (legacy, pre-rc.1.4.0 files only)
Periodic sensor snapshot — **sunset in rc.1.4.0**, no longer emitted by current firmware. Retained here because historical log files (rc.1.3.x and earlier) still carry these rows. The parser continues to recognise them so older files remain readable.

| Field | Meaning |
|---|---|
| `value_a` | Average temperature (°C, integer) |
| `value_b` | Average relative humidity (%, integer) |
| `ch` | 0 (not motor-specific) |
| `param` | 0 (not a config event) |

**Example output:**
```
2025-06-07 14:30:22  [SENSOR   ]  System          T=23 degC   RH=65 %
```

---

### SENSOR_HR (rc.1.4.0+)

Periodic sensor snapshot posted by the Data Manager (T4) on every poll cycle. Replaces the single-row `SENSOR` format with **three companion rows** sharing the same timestamp, discriminated by `ch`. See `model/logUpdatePlan.md` §2 for the full specification.

| `ch` | Subject | `value_a` | `value_b` |
|--:|---|---|---|
| 0 | Temperature + humidity | `t_c10` — temperature × 10 (0.1 °C precision) | `rh` — relative humidity, 0..100 % |
| 1 | Wind | `wind_dms` — wind speed × 10 (deci-m/s) | `wind_dir_deg` — wind direction, 0..359 ° |
| 2 | Window-state bitmask | 16-bit packed state + safety flags (see encoding below) | 0 (reserved) |

The bitmask (ch=2, `value_a`) packs all three window-channel states + three EG1 safety flags:

```
bits  1..0  = M1 state    (0=CLOSED, 1=MOVING_OPEN, 2=OPEN, 3=MOVING_CLOSE)
bits  3..2  = M2 state    (same encoding)
bits  5..4  = M3 state    (same encoding)
bits 11..6  = reserved (0)
bit  12     = EG1_BIT_WIND_OVERRIDE   ("WIND" flag in parser output)
bit  13     = EG1_BIT_MOTOR_ALARM     ("ALARM" flag)
bit  14     = EG1_BIT_CALIBRATING     ("CAL" flag)
bit  15     = reserved (0)
```

T2 internally uses an extended state enum with GAP states for direction reversals; those collapse to the matching MOVING state in the public `window_state_t` returned by the bitmask packer. The ~2 s GAP intervals are not visible in the log.

**Example output:**
```
2026-05-26 13:30:00  [SENSOR_HR]  System          T=23.4 degC   RH=65 %
2026-05-26 13:30:00  [SENSOR_HR]  System          wind=3.5 m/s  dir=158 deg
2026-05-26 13:30:00  [SENSOR_HR]  System          M1=OPEN  M2=OPEN  M3=OPEN  (0x002A)
2026-05-26 13:31:00  [SENSOR_HR]  System          M1=OPEN  M2=OPEN  M3=OPEN  [WIND]  (0x152A)
2026-05-26 14:00:00  [SENSOR_HR]  System          M1=CLOS  M2=CLOS  M3=CLOS  [CAL]  (0x4000)
```

The bitmask is also printed in hex (`(0x002A)`) at the end of every channel-2 line so the raw value can be cross-checked against the encoding above.


#### `ch = 3` — M3 window position (ropeSensor branch only)

Emitted by **T17** (`firmware/src/window_pos/window_pos_task.cpp`), the window
position task. Present only on the `ropeSensor` branch; `main` ships without it,
so a log from a production unit never contains this sub-row.

| Field | Meaning |
|---|---|
| `ch` | `3` |
| `param` | which window these samples describe — `3` = M3. One channel serves all three windows |
| `value_a` | opening in **0.1 mm** (device register 30001), or **`-1`** on sensor fault |
| `value_b` | rate, **SIGNED**, 0.1 mm/s (register 30012). Positive = opening, negative = closing, zero = at rest |

**Raw millimetres are logged, never a percentage.** Percent derives from register
40004 (the calibrated full travel), so a mis-measured travel corrupts a logged
percentage beyond recovery, while millimetres stay recomputable.

`value_b` is the **only** signed register in the device contract. Read unsigned,
a closing window at 157 mm/s appears as 6.4 m/s — that mistake is what the
signed decode was proven against on real hardware.

**Cadence.** T17 polls only while a channel is travelling, at
`travel_m3 / 150` ms, and otherwise emits one sample every 30 s so the log shows
the window sitting still rather than a gap.

**Example output:**
```
2026-09-12 16:00:01  [SENSOR_HR]  System   M3 position: 741.2 mm   rate -157.0 mm/s (closing)
2026-09-12 16:00:31  [SENSOR_HR]  System   M3 position: SENSOR FAULT
```

---

### SUN (rc.1.4.0+)

Sunrise/sunset record. Emitted by the Data Manager (T4) whenever its cached sun-time values change. In steady-state operation this fires once per local-midnight rollover (sunrise/sunset shifts 1–2 min/day in spring/autumn), once at boot, and once per operator coordinate edit via Q4. See `model/logUpdatePlan.md` §3 for the full specification.

| Field | Meaning |
|---|---|
| `value_a` | Sunrise — minutes from local midnight (0..1439) |
| `value_b` | Sunset — minutes from local midnight (0..1439) |
| `ch` | 0 (not motor-specific) |
| `param` | 0 (not a config event) |

The values are **local time**, matching the existing CSV timestamp convention. A historical log file is self-sufficient for per-day night-shading reconstruction — no live `/api/status` lookup needed.

**Example output:**
```
2026-05-26 05:30:00  [SUN      ]  System          sunrise=05:30 local  sunset=21:36 local
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
**`MODE` has TWO emitters, discriminated by `param` since firmware 2.6.0
(gh#54).** Check `param` before reading `value_a` / `value_b`.

| `param` | emitter | meaning |
|---|---|---|
| **0** | T6 `climate_control.cpp` | ventilation step decision — the table below |
| **47** | T4 `dm_set_standby_ex()` | STANDBY enter/leave — `value_a` 1 = entered, 0 = left; `value_b` reserved 0; `ch` = surface, 0 web / 1 LCD |

> **Old logs cannot be separated.** Emitter B has existed since rc.1.5.0 (gh#28)
> but carried `param = 0` until 2.6.0, so in any log written before 2.6.0 a
> STANDBY transition is rendered as a ventilation decision — including a
> fabricated "T-demand / RH-demand" pair unpacked from the reserved zero
> `value_b`. If an old log shows a vent step at the exact moment an operator
> toggled STANDBY, that is why. `plot_daily.py` and `vent_step_replay.py` skip
> `param = 47` rows; for pre-2.6.0 logs they cannot.

### Emitter A — ventilation step change (`param = 0`)

Posted by the Climate Controller (T6) each time it recalculates the desired
ventilation step.

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
| `ch` | Motor channel (only relevant for `travel` / `dwell_open` / `dwell_close`) |

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
| 13 | avg_win_t | min | numeric, old → new |
| 14 | avg_win_rh | min | numeric, old → new |
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
| 38 | avg_win_wind | min | numeric, old → new (2.1.0, gh#35 — independent wind averaging window) |
| 39 | ota_enable | (enabled/disabled) | boolean, old → new (2.2.0 ROTA) |
| 40 | ota_check_h | h | numeric, old → new — daily ROTA check hour, local time (2.2.0 ROTA) |
| 41 | ota_url | *(set)* | **sentinel** — ROTA manifest URL was changed (URL not logged; 2.2.0) |
| 42 | ota_secret | *(set)* | **sentinel** — ROTA per-unit HMAC secret was changed (secret not logged; 2.2.0) |
| 43 | ota_win_lo | h | numeric, old → new — apply-window start hour, local (lo==hi disables the window; 2.2.0) |
| 44 | ota_win_hi | h | numeric, old → new — apply-window end hour, local time (2.2.0) |
| 45 | wind_hyst | m/s | numeric, old → new — wind-speed release dead band (2.3.0, gh#46). **The firmware has emitted this since 2.3.0; the parser only learned it in 2.4.2**, so rows in logs from 2.3.0-2.4.1 render as `param#45` |
| 46 | travel | s (per channel) | numeric, old → new — motor full-travel time (2.4.2, gh#51). Never logged before 2.4.2, because until then it only took effect at reboot |

**Sensitive-value policy (since 2.0.0-a.6.35.5).** Param IDs 23-30 cover
admin-sensitive settings — PIN rotations, WiFi credentials, the
status-website shared secret, the timezone string, and the status-website
URL. For these the firmware uses `value_a = 1` as a sentinel for "set" or
"changed" and `value_b = 0`. The actual value is **never** written to the
CSV. The audit row stamps *who* changed *what kind of field* and *when*
without making the SD card a credential exfil surface. The parser renders
these rows as `<field> (set)` or `<field> (changed)` rather than the
misleading `1 -> 0`. Param IDs 41-42 (`ota_url`, `ota_secret`) join the same
sentinel set in 2.2.0 (ROTA).

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

### PIN_AUTH (2.5.0+)
A PIN authentication attempt that did **not** succeed. Posted by
`pin_auth_verify()` (`firmware/src/auth/pin_auth.cpp`), which is the single choke
point both the LCD keypad (T8) and `POST /api/login` (T11) go through.

A **successful** login emits no `PIN_AUTH` row — it is recorded as `SESSION`
instead (LCD only; see *Known limitations*). Read the two types together.

| Field | Meaning |
|---|---|
| `initiator` | The surface: `FARMER` / `ADMIN` = LCD keypad, `WEB` = `POST /api/login` |
| `ch` | The role attempted: 1 = farmer, 2 = admin |
| `value_a` | 0 = attempt failed, 1 = lockout armed, 2 = refused while locked out |
| `value_b` | Running failure count (0), lockout duration in s (1), seconds remaining (2) |

The entered digits are never logged.

**Not logged, by design:** an attempt whose PIN is the wrong *length*. It cannot
move the failure counter and so can never reach the lockout or succeed; logging it
would give an unauthenticated caller an unbounded way to flood the audit log. Those
reach the serial console only (`ESP_LOGW`).

**Example output** — five failures arming a 300 s lockout, then an attempt refused
during it, then one wrong admin PIN on its own counter (real rows from FDA4,
2026-09-12):
```
2026-09-12 11:39:53  [PIN_AUTH ]  Web UI          PIN failed: farmer, failure #1  [Web UI]
2026-09-12 11:39:55  [PIN_AUTH ]  Web UI          PIN failed: farmer, failure #2  [Web UI]
2026-09-12 11:39:57  [PIN_AUTH ]  Web UI          PIN failed: farmer, failure #3  [Web UI]
2026-09-12 11:39:58  [PIN_AUTH ]  Web UI          PIN failed: farmer, failure #4  [Web UI]
2026-09-12 11:39:59  [PIN_AUTH ]  Web UI          PIN LOCKOUT ARMED: farmer locked for 300s  [Web UI]
2026-09-12 11:40:01  [PIN_AUTH ]  Web UI          PIN refused, farmer locked out, 298s remaining  [Web UI]
2026-09-12 11:40:06  [PIN_AUTH ]  Web UI          PIN failed: admin, failure #1  [Web UI]
```

Counters and lockouts are **per role**: a farmer lockout does not affect admin.

---

### ALARM
Wind override and motor alarm events.

#### Motor alarm (T2 Relay Controller)

| `value_a` | `value_b` | Meaning |
|---|---|---|
| 1 | 0 | Motor alarm onset — all relays de-energised |
| 0 | 0 | Motor alarm cleared |

#### Wind override (T3 Safety Monitor)

**Since firmware 2.3.0 (gh#45)** every wind row stamps its subtype into the
`param` column, from a reserved discriminator band far above the config
parameter space. The parser decodes these exactly — no heuristics:

| `param` | `value_a` | `value_b` | Meaning |
|---|---|---|---|
| 240 | speed×10 | v_max×10 | SET — averaged speed reached v_max |
| 241 | direction° | excl_low° | SET — direction inside exclusion zone |
| 242 | speed×10 (or 0) | direction° (or 0) | CLEARED (0,0 = no wind reading, or protection disabled) |
| 243 | −1 | 0 | SET — wind sensor fault safe-fail |

Motor-alarm (T2) rows keep `param=0`, so on 2.3.0+ data a `(0,0)` ALARM row
is unambiguous: `param=242` = wind clear, `param=0` = motor alarm clear.

**Pre-2.3.0 logs** carry `param=0` on wind rows too and are decoded by a
value-magnitude heuristic:

| `value_a` | `value_b` | Meaning |
|---|---|---|
| −1 | 0 | SET — wind sensor fault safe-fail |
| speed×10 | v_max×10 | SET — measured speed met or exceeded limit |
| direction° | excl_low° | SET — direction inside exclusion zone |
| 0 | 0 | CLEARED — wind protection was disabled while active |
| speed×10 | direction° | CLEARED — conditions normalised |

> **Legacy caveats (param=0 rows only):** `value_a=0, value_b=0` is ambiguous
> between motor alarm clearance and wind override clearance (disabled path) —
> the parser reports both possibilities; use surrounding RELAY/MODE events.
> The speed-SET vs direction-SET split is a best-effort magnitude heuristic
> (gh#45): a direction row with `direction > excl_low ≤ 200` decodes as a
> speed row. Cross-check any legacy "direction" event against the SENSOR_HR
> wind samples at the same timestamp before trusting it.

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


#### Window position sensor events (T17, channel 6, ropeSensor branch only)

Emitted by **T17**. The channel-based dispatch follows T5's 4/5 convention: T2
and T3 keep `ch = 0`, and each later producer owns a channel of its own. `param`
then names the event within that channel, from the reserved band **244—247**.

| `param` | event | `value_a` | `value_b` |
|---|---|---|---|
| **244** | sensor fault set/cleared | `1` = fault set, `0` = cleared | device status bits (low byte), `0` when not carried |
| **245** | teach-mode transition | `0` aborted, `1` armed, `2` COMMITTED, `3` REFUSED | 0 |
| **246** | device status bitfield | the raw register-30006 bitfield | 0 |
| **247** | device restarted | new register-30008 uptime in seconds (masked to 15 bits) | 0 |
| **248** | **M3 control mode changed** | `0` = TIMED (travel timer), `1` = POSITION (opening distance) | gate reason, below |

**`param = 245`, `value_a = 3` (REFUSED) is decoded but never emitted.** A refused
teach leaves status bit 5 set with register 40007 still `1`, which is
indistinguishable from *still armed* at the next poll, so the firmware does not
claim it. The decode exists for a future device build that reports it.

**`param = 246` status bits**, as decoded, matching `WINDOWPOS_ST_*` in
`drivers/windowPos/src/window_pos.h`:

| bit | name | meaning |
|---|---|---|
| `0x01` | `startup:window` | no measurement window completed yet |
| `0x02` | `startup:avg` | averaging accumulator not filled |
| `0x04` | `WIPER OPEN` | wiper open (FR-E07) |
| `0x08` | `end sensor` | an end sensor is active |
| `0x10` | `BOTH ends` | both active — distrust bit 3 (FR-E16) |
| `0x20` | `teach armed` | teach armed |
| `0x40` | `implausible` | raw code outside the calibrated band |
| `0x80` | `NOT FOLLOWING` | switches saw movement, position did not |

**`param = 248` is the row that says which control law M3 was under.** The
sensor-presence gate publishes it, edge-triggered — one row per transition, not
per poll. `value_b` carries the reason:

| `value_b` | meaning |
|---|---|
| `0` | sensor present and trusted |
| `1` | probing, no verdict yet — **never appears in a log row** (see below) |
| `2` | no sensor answering at address 40 |
| `3` | bench build refused, contract 9 — **permanent**, never re-probed |
| `4` | sensor present but reporting its own fault — specifically the **wiper-open** bit or the `65535` sentinel, not any status bit |

**TIMED is the fallback and the failure direction**, and it is what `main`
ships, so a log full of `TIMED` rows is a unit behaving exactly as it always
has. **Demotion to TIMED is immediate; promotion to POSITION appears only at a
stroke boundary**, so a `POSITION` row always sits at the start of a movement
and never inside one.

**A healthy boot logs exactly ONE row**, `TIMED [sensor present and trusted]`,
which then becomes `POSITION` at the first stroke. `value_b = 1` (probing) is
**structurally unloggable**: the publisher has three call sites and none can pass
it — it is the initial value, visible only through `GET /api/diag/windowpos` in
the sub-second window before the first probe returns. Verified on FDA4
2026-09-12: two OTA reboots produced one `TIMED [ok]` row each, and the stroke
produced one `POSITION` row — three rows against 162 polls, which is what
edge-triggered is supposed to look like.

A unit with no sensor logs one `TIMED [no sensor answering]` about 30 s after
boot — two consecutive failed probes, matching T5's convention — and then
nothing further. **Not yet observed on hardware** (it needs the encoder's bus
cable pulled); the healthy path above is measured.

**`param = 247` matters more than it looks.** The device restarting silently
discards an armed teach, so a restart row sitting between an *armed* and an
expected *committed* row explains a calibration that appears to have been lost.

**Example output:**
```
2026-09-12 16:01:00  [ALARM    ]  System   position sensor FAULT set
2026-09-12 16:01:30  [ALARM    ]  System   position sensor fault cleared
2026-09-12 16:02:00  [ALARM    ]  System   teach COMMITTED
2026-09-12 16:02:30  [ALARM    ]  System   device status 0x0C [WIPER OPEN, end sensor]
2026-09-12 16:03:00  [ALARM    ]  System   device RESTARTED (uptime now 25 s -- any armed teach is lost)
2026-09-12 17:18:41  [ALARM    ]  System   M3 CONTROL MODE -> TIMED (travel timer)  [sensor present and trusted]
2026-09-12 17:20:47  [ALARM    ]  System   M3 CONTROL MODE -> POSITION (opening distance)  [sensor present and trusted]
2026-09-12 17:20:47  [ALARM    ]  System   Wind sensor fault: triggered (two consecutive read failures)
```

---

### SYSTEM

Internal system events from various firmware tasks. `value_a` categorises the
subtype; `value_b` is the payload. The current encoding (firmware 2.2.0)
matches the LOG_SYSTEM table in `firmware/src/event_logger/event_logger.h`:

| `value_a` | `value_b` | Initiator | Producer | Meaning |
|---|---|---|---|---|
| **−1** | drop count | SYS | T9 (synthetic) | Q3 queue overflow — N events dropped |
| **0** | 0 | WEB | T14 status_post | Status POST failed (streak transition) |
| **0** | 1 | WEB | T14 status_post | Log upload failed — pre-HTTP (connection/write/alloc error; no server response) |
| **HTTP 100–599** | 1 | WEB | T14 status_post | Log upload rejected by server — `value_a` is the HTTP status code (e.g. 413); 2.1.1+ (gh#34) |
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
| **21** | ± s (int16) | SYS | T4 data_manager | RTC (DS1307) vs NTP-synced clock divergence exceeds ±10 s — DS1307 ignored; check RTC/battery (2.1.3+, gh#37; rate-limited ~1/h) |
| **22** | sub-code | SYS | T16 ota_client | ROTA update **check** outcome — see ROTA sub-code table below (2.2.0+) |
| **23** | sub-code | SYS | T16 ota_client | ROTA **download/verify** outcome — see ROTA sub-code table below (2.2.0+) |
| **24** | sub-code | SYS | T16 ota_client | ROTA **apply** outcome — see ROTA sub-code table below (2.2.0+) |
| **25** | 0 night / 1 day | SYS | T4 `update_sun_times()` | **`is_daytime` FLIPPED** — the one-bit input that selects the day or night setpoint set. Fires at boot (on a −1 sentinel) and at each dawn/dusk crossing, so a log can always answer *which thresholds was the controller using?* (2.6.0+, gh#59) |
| **26** | IO0 level 1/2/3 | ADMIN | T8 ui_display | **FACTORY RESET executed.** 1 = PINs only, 2 = all settings no reboot, 3 = all settings + reboot. Written **synchronously** to SD because level 3 reboots immediately (2.6.0+, gh#59) |
| **27** | packed: hi byte = action, lo byte = source | SYS | T2 relay_controller | **Q1 command DISCARDED** because `EG1_BIT_MOTOR_ALARM` is set (FR-MA03). `ch` = requested channel, 0 = all. Actions 0 OPEN · 1 CLOSE · 2 CLOSE_ALL · 3 RESUME · 4 RECALIBRATE. Sources 0 T3 wind-safety · 1 T6 climate · 2 OPERATOR MANUAL (2.6.0+, gh#59) |
| **28** | `rtc_status_t` 1/2/3 | SYS | T4 data_manager | **DS1307 UNREADABLE** — 1 NO_DEVICE (no I2C ACK) · 2 COMM (I2C error) · 3 INVALID (out-of-range registers). Distinct from **21**, which is a chip that reads but diverges. This is the case that took the clock down in gh#55. Rate-limited ~1/h (2.6.0+, gh#59) |
| **29** | seconds remaining, **signed** | SYS | T2 relay_controller | **T6 move DEFERRED on the dwell timer.** `ch` = motor 1/2/3. **Sign carries direction: positive = OPEN deferred, negative = CLOSE deferred.** Latched to one row per deferral episode, so an M3 dwell of up to 25 min produces one row, not hundreds (2.6.0+, gh#59) |
| **30** | 0 | producer | T4 `apply_config_update()` | **Q4 config write REJECTED, unknown ns/key.** `initiator` identifies which producer tried; the key name is on the serial console only, because the 12-byte row cannot carry it. `/api/config` returns 400 before reaching Q4, so this fires only for the LCD/T10 producers or a future one (2.6.0+, gh#59) |

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

**ROTA audit sub-codes (`value_a=22/23/24`; `value_b` = sub-code):**

| `value_a` | Stage | `value_b` sub-codes |
|---|---|---|
| **22** | check | 0 up to date · 1 update found · 2 server unreachable / HTTP error · 3 skipped (clock not ready / OTA busy) · 4 auth rejected by server |
| **23** | download/verify | 0 OK · 1 TLS / pinned-cert failure · 2 SHA-256 / size mismatch · 3 downgrade / seq rejected · 4 min_version refusal |
| **24** | apply | 0 committed — reboot scheduled · 1 deferred (night-window / quiet gate) · 2 apply failed |

A normal remote update logs `22.1 → 23.0 → 24.0 → BOOT`. A daytime update that
waits for the configured night window logs `22.1 → 23.0 → 24.1`, then `24.0`
once the window opens and the quiet gate is clear (no window motion, no active
web/LCD session; see `design/rota_tds.md` §4.4).

**Example output (ROTA remote update, deferred to the night window):**
```
2026-07-13 14:02:04  [SYSTEM ]  System          ROTA check: update found
2026-07-13 14:02:11  [SYSTEM ]  System          ROTA download/verify: OK
2026-07-13 14:02:11  [SYSTEM ]  System          ROTA apply: deferred (night-window / quiet gate)
2026-07-13 02:00:07  [SYSTEM ]  System          ROTA apply: committed — reboot scheduled
```

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

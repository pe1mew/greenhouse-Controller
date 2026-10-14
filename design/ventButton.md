# The ventilation button: one press opens all windows for a set time (gh#96)

**Status:** designed 2026-10-14, not implemented. Version **2.18.0** (a feature, two settings, a log type, a payload
change). **Requirement:** [gh#96](https://github.com/pe1mew/greenhouse-Controller/issues/96), the crop team through
the operator, 2026-10-14. **Decisions:** all eleven taken the same day; the record is the comment on gh#96. This
document is the design those decisions fix; the issue keeps the use case and the implications.

## 1. What the crop team gets

A push button next to the controller, on the terminal J10 that was the motor-alarm input. Anyone can press it; no
PIN. The state is **VENT** on the LCD, "Ventilation button" in the web GUI, *ventilatieknop* in the manuals.

| Situation | What happens |
|---|---|
| **A press while the controller is AUTOMATIC** (no override, no calibration, no Standby of any kind) | M1, M2 and M3 open. The period is `btn_open_min` (D, default 30 min). The controller is in VENT: Standby held by the button, climate control paused. The LCD counts down |
| **Press 2, 3 and 4 during the period** | Each moves the end D later: one continuous period of at most 4·D. The LCD shows the new time left |
| **Press 5 or more** | Refused: `Max 4 presses` / `mm:ss left` |
| **The period ends** | The standard Standby exit (FR-MD03): T2 drops the dwell debt, **all windows close** (the CLOSE_ALL recalibration, up to 176 s), and climate control resumes from step 0, opening again as the climate demands. The lockout starts |
| **A press during the lockout** (60 min after the end) | Refused: `Button locked` / `mm:ss to go` |
| **A wind override begins during the period** | T3 closes all windows and takes the drives, as today. The clock runs on. When the override clears with time left, the windows open again |
| **A press during a wind override or a calibration** | Refused: `WIND: refused` or `Calibrating`. The period does not start, so no time is lost |
| **A press while the operator holds the windows** (an explicit Standby from the web or the LCD; an admin in the LCD manual-motor menu; a web teach or characterisation) | Refused: `Standby set` or `Admin session`. The operator's state wins |
| **An explicit mode choice during the period** (web `/api/mode`, LCD screen 3) | Ends the period at once; the lockout follows |
| **An admin enters the LCD manual-motor menu during the period** | Both hold Standby. The admin's commands win: the button posts nothing while another holder exists. At the period's end the button's hold goes; Standby stays until the admin session ends, and the recalibration comes then |
| **A reboot** | Ends the period and the lockout (RAM only, like every hold). The boot CLOSE_ALL closes the windows |

**The button does not:** override wind safety; start or extend anything while the operator holds the windows; end a
period early (a mode choice does); survive a reboot; appear as a control on the web GUI.

**Where it sits in the priority chain:** MOTOR_ALARM > WIND_OVERRIDE > calibration > **VENT (a held Standby)** >
AUTOMATIC. VENT breaks exactly one Standby rule on purpose, FR-MD02 (windows left where they are), and FR-MD05
(persistence) does not apply to a hold.

## 2. Design

The feature reuses the controller's existing "manual" mechanism. There is no manual mode in the code: `op_mode_t`
is AUTOMATIC, STANDBY, WIND_OVERRIDE and MOTOR_ALARM, derived from EG1 bits, and "manual" means **Standby held by
a session** (`dm_standby_hold()`: the admin's LCD menu holds `DM_STANDBY_HOLD_LCD`, the web teach
`DM_STANDBY_HOLD_TEACH`). The button becomes the third holder, **`DM_STANDBY_HOLD_BUTTON` 0x04.** That buys the
T6 inhibit (STANDBY is in its mask), the hold bookkeeping, the operator's override (`dm_set_standby_ex()` drops
every holder on an explicit choice), and the exit (`dm_standby_release()`: `T2_NOTIFY_CLEAR_DWELL`, the bit, the
MODE row, `CMD_RECALIBRATE`), all without new code in T4 beyond the constant and one accessor.

### 2.1 J10's two operations (T2, `j10_mode`)

J10 is a two-pin terminal: pin 1 is GPIO 42 (`PIN_OPTO_INPUT`), pin 2 is GND; a closed contact reads LOW. Today
T2 owns the pin: an any-edge ISR, a 75 ms debounce in its 20 ms loop, and four raw reads (at boot, at calibration
entry, every 400 ms during a calibration, every 5 s during the alarm's 60 s guard). The administrator now chooses
what the terminal is for:

| `j10_mode` | J10 is | T2 |
|---|---|---|
| **0** (factory default) | the RRK-3 motor-alarm input, today's behaviour | as today, unchanged |
| **1** | the crop team's ventilation button | installs no ISR, makes none of the four raw reads, never sets `EG1_BIT_MOTOR_ALARM`. The boot check that skips the CLOSE_ALL when the pin is LOW is skipped with it |

- **Read once, at boot.** T2 reads the key in its init, before the ISR would be installed. A change needs a
  restart; the GUI shows the configured and the active operation, and says "restart to apply" when they differ.
  The active operation is `t2_j10_mode()`, what T2 read at boot.
- **With 1 the motor-alarm feature is off on that unit.** Nothing that reads `EG1_BIT_MOTOR_ALARM` changes (T6, T3,
  T8, T1, T4, the status JSON, the GUI, the ROTA quiet gate, T17's refusals): the bit just never appears. FR-MA01–08
  are marked "only while J10 is the alarm input". The bench route `motor_alarm` injection keeps working with 0 and
  answers an error with 1.
- **Electrically the button is the RRK-3 contact's twin:** a normally-open dry contact across J10, closed = LOW.
  The 1 kΩ pull-up to 5 V and the 1.8 kΩ shunt stay. The switch sits next to the controller and is the
  administrator's (operator, 2026-10-14); no cable guidance.
- **Commissioning:** connect the button, select the button operation in the GUI, restart, one test press. A press
  made while J10 is still the alarm input acts as a motor alarm (relays off, windows UNKNOWN, the 60 s guard, a
  recalibration), so select first or press last.

### 2.2 The rule: `drivers/ventButton`

The timing and the refusals are a pure library, host-tested like `drivers/m3Hold` and `drivers/m3Char`: no
FreeRTOS, no ESP-IDF, milliseconds that may wrap. T18 feeds it a debounced press, a tick, and what the world
allows; it answers with at most one action per call and keeps the view the LCD and the GUI show.

```c
/* What the world allows, gathered by T18 before each call. */
typedef struct {
    bool override;      /* EG1 wind override, or the pin's own refusal: no OPEN may be posted */
    bool calibrating;   /* EG1_BIT_CALIBRATING */
    bool other_holder;  /* another Standby holder (LCD menu, teach), or the operator's explicit Standby */
    bool hold_lost;     /* the button's own holder bit is gone: an explicit mode choice ended it */
    bool any_not_open;  /* a window is neither OPEN nor opening (T2 states) */
} vb_env_t;

typedef enum {
    VB_NOTHING = 0,
    VB_START,        /* take the hold, open all three; the period begins */
    VB_EXTEND,       /* the end moves D later */
    VB_REFUSE,       /* out.reason says why; nothing moves */
    VB_REOPEN,       /* open every window that is not open: an override cleared with time left */
    VB_END,          /* release the hold; the lockout begins. out.reason: 0 lapsed, 1 operator */
    VB_LOCKOUT_END,  /* the button is available again */
} vb_action_t;

typedef enum { VB_R_NONE, VB_R_MAX_PRESSES, VB_R_LOCKOUT, VB_R_WIND, VB_R_CALIBRATING, VB_R_OPERATOR } vb_reason_t;

typedef struct { vb_action_t action; vb_reason_t reason; uint8_t presses; uint32_t left_ms; } vb_out_t;
typedef struct { uint8_t state; uint8_t presses; uint32_t left_ms; uint32_t lockout_left_ms; } vb_view_t;

void     vb_reset(vb_t *s, uint32_t d_ms);                    /* IDLE; D from btn_open_min */
void     vb_set_duration(vb_t *s, uint32_t d_ms);             /* a config change; applies to the next press */
vb_out_t vb_press(vb_t *s, uint32_t now, const vb_env_t *env);
vb_out_t vb_tick (vb_t *s, uint32_t now, const vb_env_t *env);
vb_view_t vb_view(const vb_t *s, uint32_t now);
```

| State | A press | A tick |
|---|---|---|
| **IDLE** | `override` → REFUSE wind; `calibrating` → REFUSE calibrating; `other_holder` → REFUSE operator; else **START**, end = now + D, presses = 1 | nothing |
| **ACTIVE** | presses < 4 → **EXTEND**, end += D; else REFUSE max presses | `hold_lost` → **END** (operator), lockout from now; now ≥ end → **END** (lapsed), lockout from now; else `any_not_open && !override && !calibrating && !other_holder` and 2 s since the last open → **REOPEN** |
| **LOCKOUT** | REFUSE lockout | now ≥ lockout end → **LOCKOUT_END**, IDLE |

Constants: `VB_MAX_PRESSES 4`, `VB_LOCKOUT_MS 3 600 000`, `VB_DEBOUNCE_MS 50` (T18's, applied before `vb_press`),
`VB_REOPEN_GAP_MS 2000`. A press during ACTIVE is an extension whatever the environment: the override closes the
windows and the clock runs on, so counting the press is right; it changes the end, not the windows.

**The REOPEN rule is why the library exists.** T3 closes once, at the onset of an override, and T2 never checks
the override before an OPEN (`EG1_BIT_WIND_OVERRIDE` appears in T2 only in the log bitmask). Every operator path
gates itself, and so must this one: an OPEN is posted only when nothing forbids it, for a window that is not open,
at most once per 2 s. After the override clears, the first tick that sees a closed window re-opens it; while the
admin holds the windows, nothing is posted at all.

### 2.3 The task: T18 `vent_button/`

A new task, priority 4 (as T17), 4 KB stack, created in `main.cpp` after T2, **only when `t2_j10_mode()` is 1**;
with 0 it is not created and the feature is absent from that boot. Its loop, every 20 ms:

1. **Read the pin** (`gpio_read(PIN_OPTO_INPUT) == GPIO_LOW`), by polling, never an interrupt. A press is the
   closing edge after `VB_DEBOUNCE_MS` of a steady LOW; a held or stuck contact is one press; the release is
   nothing.
2. **Gather `vb_env_t`:** the EG1 bits; `dm_standby_holders()` (new: the holder mask) and `dm_get_standby()` for
   `other_holder` (a holder that is not the button's, or STANDBY set with no holders at all = explicit) and
   `hold_lost` (ACTIVE but the button's bit is gone); `t2_get_window_states()` for `any_not_open`.
3. **Call the library** (`vb_press` on an edge, `vb_tick` otherwise) and carry out the action:
   - **START:** `dm_standby_hold(DM_STANDBY_HOLD_BUTTON, LOG_BY_BUTTON, 2)`; if it returns false (an explicit
     Standby was set in the meantime) the press is refused as operator; else three `CMD_OPEN` with `SRC_BUTTON`.
   - **EXTEND / REFUSE / LOCKOUT_END:** a log row, nothing else.
   - **REOPEN:** `CMD_OPEN` for each window that is CLOSED, MOVING_CLOSE or PART_OPEN.
   - **END (lapsed):** `dm_standby_release(DM_STANDBY_HOLD_BUTTON, LOG_BY_BUTTON, 2)`: the dwell clear, the bit,
     the MODE row and the recalibration come from T4. **END (operator):** the holder is already gone; only the
     row.
4. **Publish the view** (`vent_button_get(vb_public_t *)`, a copy under a spinlock) for T4's snapshot and T8.

Q1 is 8 deep and T2 does not drain it during a sweep (gh#79): the task posts only when not calibrating, three
commands at a start, one per window at a re-open, never more often than every 2 s. `xQueueSend` with a 100 ms
wait, as `post_manual()` does; a refused send is logged and retried by the next tick's REOPEN.

**Time** is `xTaskGetTickCount()` in ms, never `dm_get_unix_time()` (0 while the clock is unsynced, gh#55).

### 2.4 T2: `SRC_BUTTON`

`cmd_source_t` gains **`SRC_BUTTON`, appended.** T2 treats it as it treats `SRC_OPERATOR_MANUAL`: the dwell
timers are bypassed (they fire for `SRC_T6` only), a closing window is reversed after the 2 s gap, the taken
counter increments, `drive_src` records it. Every comparison on the source in T2 is a `== SRC_T6` or `== SRC_T3`
check today; the change audits each one and adds the source's name to `src_name()`. A full `CMD_OPEN` is a timed
end drive in both M3 modes, so nothing else in T2 changes, and T17 judges a button drive of M3 like any other (it
never reads the source). `CMD_STOP` stays the LCD's: a button never stops anything.

### 2.5 The log

| Row | type | initiator | ch | param | value_a | value_b |
|---|---|---|---|---|---|---|
| Press accepted (start or extension) | **`LOG_VENT_BUTTON`** (appended, the 11th `log_type_t`; CSV `VBTN`) | **`LOG_BY_BUTTON`** (appended; CSV `BUTTON`) | 0 | 1 | press n (1–4) | seconds left after it |
| Press refused | `LOG_VENT_BUTTON` | `LOG_BY_BUTTON` | 0 | 2 | reason: 1 max, 2 lockout, 3 wind, 4 calibrating, 5 operator | seconds left of the period or the lockout |
| Period ended | `LOG_VENT_BUTTON` | `LOG_BY_BUTTON` | 0 | 3 | 0 lapsed, 1 operator | the period's length, s |
| Re-opened after a clearance | `LOG_VENT_BUTTON` | `LOG_BY_BUTTON` | window 1–3 | 4 | 0 | seconds left |
| Lockout ended | `LOG_VENT_BUTTON` | `LOG_BY_BUTTON` | 0 | 5 | 0 | 0 |
| The hold's enter and leave | `MODE` (existing, param 47) | `LOG_BY_BUTTON` | **2** (0 = web, 1 = LCD today) | 47 | 1 enter / 0 leave | 1 (a hold) |
| Each drive | `RELAY` (existing) | SYS | window | — | — | — |
| The 30 s state sample | `SENSOR_HR` ch 2 (existing) | | | | **bit 15 = STANDBY** (EG1, any), **bit 11 = a VENT period** (T18's flag) | |

The two bitmask bits are reserved today (bits 6–11 and 15); bit 15 lets any Standby be seen in the HR rows at
last, bit 11 tells a period from the operator's Standby. `log/logparser.py` gets a `_decode_vbtn` branch, the
BUTTON initiator, `ch` 2 in `_decode_mode`'s param 47 branch and the two bits; `log/logparser.md` follows in the
same commit (the gh#54 rule). `bin/sd_soak.py` and `bin/law_conformance.py` treat a period as a Standby episode.
Nothing goes into `LOG_SYSTEM` (its code 19 is already emitted by two tasks, and the parser reads any SYS code
≥ 19 as a legacy heartbeat).

### 2.6 The LCD (T8)

- **Status page 2**, the mode page, while a period runs:

  ```
  Mode: VENT 25:13
  Press 2 of 4
  ```

  The mode line keeps FR-M06. Row 1 replaces the session line for the period; the session shows again when it
  ends. The page redraws every second while the view changes, the way `s_m3_redraw` redraws the M3 hold screen
  (today the status page redraws only on rotation or a key). The backlight stays blue; the RGB LED stays green.
- **A press shows a 3 s message** on whatever page is up: `Windows open` / `55:00 left` on a start or an
  extension; the refusal pair of §1 otherwise. A message discards keys while it is up, as every `show_msg()` does.
- **No new page.** Adding one shifts the hard-coded page indexes (`#` handlers, `ui_display.h`'s "5 = the windows
  page", `bin/at_lcd_m3_hold.py`). The manual-motor menu and the other pages are unchanged.
- T8 reads the view through `vent_button_get()`; a null view (the task not created) shows today's screens.

### 2.7 Status, web GUI, mock

- **`/api/status`:** `mode.flags` gains `vent_button` while a period runs; a block
  `"vent_button": {"active": bool, "left_s": n, "presses": n, "max": 4, "lockout_s": n, "j10": 0|1}` sits after
  `mode` (about 90 bytes; T14's 2048-byte buffer carries about 1.3 KB today). `mode.current` stays `STANDBY`
  during a period: no `op_mode_t` change, so every consumer that does not know the block keeps working. The
  public dashboard's flag table (outside this repo) gets the flag.
- **The Mode tile** reads "Ventilation button: 25 min left (press 2 of 4)" with an amber badge `VENT`. The Mode
  select stays usable, and a line under it says that choosing a mode ends the button period (the GUI rule: a
  control the operator can use is not greyed; one that cannot be used says why).
- **The Motors tab** gets the two settings (§2.8): the J10 selector next to the motor-alarm material, and the
  minutes per press below it, greyed with the reason "J10 is the alarm input" while `j10_mode` is 0.
- **`webUiMock/mock_server.py`:** the block and the flag, the two keys in `cfg`, `NVS_MAP` and `CONFIG_LIMITS`,
  and a `/api/mode` route (it has none, so the GUI's Mode Apply fails against the mock today).

### 2.8 Configuration

Two rows in `firmware/config/cfg_desc.inc`, namespace `motor`, both `CFG_KEY_SHADOW`, `CFG_F_PUB | CFG_P_Q4`,
channel 0, `int16_t` shadow fields in the motor block; `bin/check_cfg_desc.py` verifies both.

| Key | Meaning | Bounds (`cfg_limits.h`) | Default (`cfg_defaults.h`) | Audit id | Read by |
|---|---|---|---|---|---|
| `motor/j10_mode` | 0 = RRK-3 motor-alarm input, 1 = ventilation button | 0 – 1 | 0 | `LOG_PARAM_J10_MODE` = 58 | T2 at boot (`t2_j10_mode()`) |
| `motor/btn_open_min` | minutes one press adds | 5 – 120 (`CFG_MIN_BTN_OPEN_MIN`, `CFG_MAX_BTN_OPEN_MIN`) | 30 | `LOG_PARAM_BTN_OPEN_MIN` = 59 | T18, from `dm_cfg_snapshot()` at each press (`vb_set_duration`) |

Admin, web GUI only (FR-CF05's style). `4`, `60 min` and `50 ms` are the library's constants, not settings. A
motor-namespace write wakes T2's `load_motor_timings()`, which re-reads travel and dwell only: harmless. The
beheerder manual's "restart required" table gets `j10_mode`.

### 2.9 Interactions

| With | Rule |
|---|---|
| **An explicit mode choice** (web, LCD screen 3) | `dm_set_standby_ex()` drops every holder; T18 sees `hold_lost` on its next tick and ends the period (row 3, operator). The lockout follows (decision 4) |
| **The admin's LCD manual menu, a web teach or characterisation** during a period | They add their holder; `other_holder` stops T18's re-opens; the admin's commands win. At the period's end the button's release does nothing while another holder remains (`dm_standby_release` releases only the last) |
| **A press while they hold** | Refused, operator |
| **A teach or characterisation started during a period** | Refused by `commission.cpp` with a new reason, greyed in the GUI with "ventilation button active" |
| **The ROTA quiet gate** (`quiet_gate_except()`) | Also false while a period runs, or the apply's reboot would close the windows on the workers. A period is as rare at night as a teach |
| **T17** | Judges each button drive of M3 as any other (param 251); no target is involved, T6 is inhibited |
| **A reboot** | The boot CLOSE_ALL; the task starts IDLE. A LOW at boot is neither an alarm (§2.1) nor a press (the edge has to be seen) |
| **The motor alarm** | Cannot occur with `j10_mode` 1; with 0 there is no button |

### 2.10 Testing on the rig: `GET/POST /api/diag/button` (bench builds only)

`POST {"ms": 200}` holds the pin's software twin LOW for 200 ms: T18's own poll and debounce see a press exactly
as from the contact (the `s_alarm_inject` pattern of the motor alarm). `GET` reports the view: state, presses,
seconds left, lockout left, the J10 operation, and T18's last action with its reason. A harness,
`bin/at_vent_button.py`, can then run every case without a hand on the button, and read the LCD through
`/api/diag/key`'s `lcd` field. Fail-firsts (compile-time, bench only): `VB_FAILFIRST_NOCAP` (a fifth press
extends), `VB_FAILFIRST_NOGATE` (an OPEN is posted under the override), `VB_FAILFIRST_J10` (T2 keeps its ISR with
`j10_mode` 1).

## 3. Requirements (for FRS §5.3e, "Ventilation button")

| ID | Requirement | MoSCoW |
|---|---|---|
| FR-VB01 | The administrator **shall** assign the operation of terminal J10 from the web GUI: the RRK-3 motor-alarm input (default), or the crop team's ventilation button. The choice **shall** apply at the next restart and **shall** be persisted. While J10 is the button, §5.3a (Motor Emergency Alarm) **shall** be inactive. | Must |
| FR-VB02 | A press of the ventilation button while the controller is in Automatic, with no wind override, no calibration and no Standby, **shall** open all three windows and begin a period of `btn_open_min` minutes (administrator, web GUI only, 5–120, default 30) during which climate control issues no commands. | Must |
| FR-VB03 | Each further press during the period **shall** extend it by `btn_open_min`, up to four presses in all, as one continuous period. A fifth press **shall** be refused, and the LCD **shall** say so. | Must |
| FR-VB04 | When the period ends, the controller **shall** leave it as it leaves Standby (FR-MD03): the dwell debt dropped, a CLOSE_ALL recalibration, then Automatic. | Must |
| FR-VB05 | For 60 minutes after a period ends, a press **shall** be refused, and the LCD **shall** show the time until the button is available. | Must |
| FR-VB06 | Wind safety (§5.5) **shall** keep its authority during a period: an override closes all windows; the period's clock runs on; when the override clears with time left, the windows **shall** open again. A press during a wind override or a calibration **shall** be refused, and the period **shall not** start. | Must |
| FR-VB07 | The operator's states **shall** take precedence: a press **shall** be refused while Standby is set by the operator or held by an administrator session; an explicit mode choice during a period **shall** end it; an administrator's manual commands during a period **shall** not be countered by the button. | Must |
| FR-VB08 | During a period the LCD **shall** show the state, the time left and the presses used on the mode page, and **shall** confirm each press with the new time left; every refusal **shall** name its reason. | Must |
| FR-VB09 | The web GUI and the status payload **shall** show the state, the time left and the presses used. | Should |
| FR-VB10 | Every press (accepted or refused, with the reason), every start, extension and end of a period, every re-opening after a wind clearance and every lockout end **shall** be logged with a timestamp and "BUTTON" as the initiator. | Must |
| FR-VB11 | A press **shall** be the closing edge of a normally-open contact across J10, debounced; a held contact **shall** count once. The input **shall** be read by polling. | Must |
| FR-VB12 | Neither a period nor a lockout **shall** survive a restart. | Must |

Also touched: FR-M01 (the chain gains VENT), FR-MA01–08 (conditional on FR-VB01), FR-LG02 (the new events), §4.4
and C8 (J10's two operations).

## 4. Verification

| Check | Pass |
|---|---|
| Host: `drivers/ventButton` | A press from IDLE; the refusals from IDLE (wind, calibrating, operator); extensions 2–4 and the fifth refused; the end by lapse and by `hold_lost`, each starting the lockout; the lockout's refusal and its end; a re-open after an override clears with time left, none without time, none under another holder, none within 2 s of the last; a duration change applying to the next press; wrap-around; a view at every state. Each mutated rule (the cap, the lockout, the gate, the gap, the end) fails at least one test (`tools/mutation_check.py`-style) |
| Rig, `j10_mode` 0 | The bench motor-alarm injection still drops the relays, and the clearance still recalibrates after the guard; T18 is not created |
| Rig, `j10_mode` 1 | A LOW at boot neither raises the alarm nor skips the boot CLOSE_ALL; `t2_j10_mode()` reads 1; the injection route answers an error |
| Rig, bench build, `bin/at_vent_button.py` | One press: M1, M2 and M3 open (26 s, 26 s, 176 s), `mode.flags` has `vent_button`, the LCD shows `Mode: VENT`; a second press extends and the LCD shows it; the fifth is refused with the reason; the end: dwell cleared, CLOSE_ALL, T6 resumes and its first decisions conform; the lockout refuses with the time and ends; an explicit web mode choice ends a period (row 3 reason 1); an admin LCD session during a period: the admin's Open is not countered and the recalibration waits for the session; a reboot clears it |
| Rig, wind | Wind injected on the emulator (its Manual mode, with the operator's go): an override during a period closes all three, the clock runs on, the clearance re-opens them; a press during the override is refused |
| Rig, physical | A switch across J10 starts a period; a held switch counts once |
| Fail-first | `VB_FAILFIRST_NOCAP`: the fifth press extends, the harness fails; `VB_FAILFIRST_NOGATE`: a window opens under the override, the harness fails; `VB_FAILFIRST_J10`: the alarm fires on a press, the harness fails |
| Soak | ≥ 12 h on the bench build with a button period every other hour between the usual strokes: `at_wp_soak.py` counters 0, `sd_soak.py` 0 FAIL, the LCD sessions pass, every period's rows present, every decision after a period conforms |
| Release image | Byte-identical to the image tested, no bench routes; `fw_ver` and `asset_version` 2.18.0 on 2344 after the ROTA pull |

## 5. Documents

- **FRS:** §5.3e (§3 above), the touched requirements.
- **TSDS:** T18 in §4.2 and §4.3; the Q1 producers in §4.5 (every producer enumerated: T3, T6, T8, T4's
  recalibrate, T17's teach and characterisation, the bench routes, T18); §4.6.4 (the holders); §5.2's chain;
  §5.3 (the type, the initiator, the bits); §5.5; §5.8; §5.10 (the keys).
- **THDS and `realisation/installation.md`:** J10's two operations and the button's two wires.
- **`model/logUpdatePlan.md` §2.2:** bits 11 and 15.
- **Manuals:** the boer manual gets a section for the crop team (the button is farmer-visible) and a note in
  §12.6 that the motor alarm is off while J10 is the button; the beheerder manual gets the two settings, the
  commissioning steps, the log codes, a diagnosis entry and the restart-required row; both get a version row.
- **`memory/architecture.md`** (T18; its subsystem map still credits `safety_monitor/` with the motor alarm,
  which is T2's), CLAUDE.md's driver list, the changelog, `bin/2.18.0/release-notes.md` with the producer and
  consumer enumerations for Q1, EG1 and the holders.

## 6. Not in this version

- A virtual button on the web GUI, for the farmer.
- Ending a period from the button (a long press). A mode choice ends it.
- Keeping the windows open at the end instead of the recalibration: it needs the law to resume at the step that
  matches open windows, which `ventModelContract.md` does not offer.

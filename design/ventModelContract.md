# Ventilation control model — interface contract

| Field | Value |
|---|---|
| Document | Interface contract for the T6 ventilation control model |
| Date | 2026-09-17 |
| Revised | 2026-09-19: **interface 2**. Two gaps found by the model work while building its simulator against the header: the minimum interval between M3 moves was 16-bit milliseconds, at most 65.5 s, for a setting that stands in for 10 and 25 minute dwells (and against this document's own units table), and there was no state for a window at rest part-open. Now `uint32_t m3_min_interval_ms` and `VENT_WIN_PART_OPEN` |
| Status | **In force since 2026-09-20: T6 calls the library, and mode 1's law exists only here.** `drivers/ventModel/` holds `src/vent_model.h` and `src/vent_model_stepped.cpp` — mode 1's law, 23 host tests — and `src/vent_model_graded.cpp`, a first candidate for mode 2, with 15 more ([`model/closedloop/gradedCandidate.md`](../model/closedloop/gradedCandidate.md)); `pio test -e native`. The firmware compiles both through `firmware/components/ventModel`, and T6's inline copy was deleted in 2.12.0 (plan §5c), so the hand-kept duplication that ran from 2026-09-17 is over. **`graded` is selected whenever mode 2 is in force** (T6's model table, indexed by the effective mode), so enabling `motor/ctrl_mode_m3` also chooses that law — the operator allowed it for mode 2 TESTING on 2026-09-20, not as a shipped choice. The refactor's evidence: the byte-identical row, 96.8 % of 378 on the replay, 97.1 % of 381 in the closed loop (plan §5c), seven rig stages of the target path, the disarm rule demonstrated failing in isolation, and a 12 h mode-1 soak running from 2026-09-20 15:22. **Mode 2 has not been soaked** |
| Audience | Whoever writes or tunes a control model — a separate session, a separate agent, or a person. **This document is meant to be read on its own** |
| Scope decisions | [`integrateWindowPositionSensor.md`](integrateWindowPositionSensor.md) §5b (the two control modes, the position path) and §5c (the rules around this contract) |
| Requirements | [`functionalRequirementsSpecification.md`](functionalRequirementsSpecification.md), and [`windowPositionSensorRequirements.MD`](windowPositionSensorRequirements.MD) FR-WP04/05/17/18 |
| Evidence for tuning | [`../model/campaignResults_summer2026.md`](../model/campaignResults_summer2026.md), [`../model/thermalProfileCampaign.md`](../model/thermalProfileCampaign.md) §9.12-9.13, [`../model/closedloop/README.md`](../model/closedloop/README.md), SD logs in `../model/campaign-summer-2026/` — **§6 revised 2026-09-18** on correctly timed data, and for the sensor delay and the north-wind effect (NS-10) |

---

## 1. What you are writing

A **pure function** that decides, every time the controller has fresh measurements, what each of the
three greenhouse windows should do. You do not touch queues, hardware, storage, logging or safety.
You receive one input struct, you keep your own state in a struct the caller owns, and you fill one
output struct.

**The greenhouse.** One span at Herenboeren Willemshoeve (Soest), 40 × 16 m and about 2 400 m³, with three
ventilation openings:

| Window | What it is | Actuation |
|---|---|---|
| **M1** | roof vents | timed, binary: fully open or fully closed |
| **M2** | roof vents | timed, binary |
| **M3** | a 40 m hanging flap on ropes in the north wall, about 80 m² | binary, **or linear** when its wire position sensor is fitted and trusted |

**Two operator-selectable modes**, and your model implements one of them:

| Mode | M1, M2 | M3 | Model |
|---|---|---|---|
| **1** | timed, 3 steps of `max(hyst_t / 3, 1)` °C above the threshold | timed, binary | `stepped` — today's behaviour, re-expressed behind this interface |
| **2** | timed, **two fixed steps** | **linear**, a commanded aperture | `graded` — new, and the reason this contract exists |

Mode 2 **falls back to mode 1** whenever M3's position sensor is absent, faulted or not fitted. The
fallback is not your concern: the caller switches models and resets your state.

---

## 2. The interface

Normative. `drivers/ventModel/src/vent_model.h`:

```c
/* vent_model.h — the ventilation control model interface.
 *
 * HOST-COMPILABLE BY RULE: this header and every model implementation must
 * compile with a plain C/C++ compiler, with no ESP-IDF, FreeRTOS, Arduino or
 * project headers. The same sources run in the firmware, in the host unit
 * tests and in the offline replay. */
#ifndef VENT_MODEL_H
#define VENT_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define VENT_MODEL_API 2            /* 2 since 2026-09-19: see Revised, above */
#define VENT_WINDOWS   3            /* index 0 = M1, 1 = M2, 2 = M3 */
#define VENT_STEPS_MAX 3            /* steps run 0..3 (the firmware's NUM_VENT_STEPS) */
#define VENT_STEP_NONE (-1)         /* "no demand from this source" / "no step notion" */

typedef enum { VENT_CAP_DIGITAL = 0, VENT_CAP_LINEAR = 1 } vent_cap_t;

typedef enum {
    VENT_WIN_UNKNOWN = 0,           /* position not established (before calibration) */
    VENT_WIN_CLOSED,
    VENT_WIN_MOVING_OPEN,
    VENT_WIN_OPEN,
    VENT_WIN_MOVING_CLOSE,
    VENT_WIN_PART_OPEN,             /* at rest between the ends; LINEAR only, pos_x10 says where */
} vent_win_state_t;

typedef enum {
    VENT_ACT_HOLD = 0,              /* leave this window alone */
    VENT_ACT_CLOSE,                 /* drive fully closed */
    VENT_ACT_OPEN,                  /* drive fully open */
    VENT_ACT_TARGET,                /* drive to target_x10 — LINEAR windows only */
} vent_action_t;

typedef enum {
    VENT_RES_NONE = 0,              /* nothing commanded yet, or still moving */
    VENT_RES_DONE,                  /* arrived, confirmed */
    VENT_RES_FAIL_TIMEOUT,          /* the travel timer ran out without arrival */
    VENT_RES_FAIL_FAULT,            /* the position source failed during the move */
    VENT_RES_ABORTED,               /* safety or the operator took the window */
} vent_result_t;

typedef struct {
    vent_win_state_t state;         /* what the actuator believes right now */
    vent_cap_t       cap;           /* DIGITAL or LINEAR. M1, M2 are always DIGITAL */
    int16_t          pos_x10;       /* aperture, 0..1000 = 0..100.0 %. -1 = unknown.
                                     * Only ever >= 0 for a LINEAR window */
    uint32_t         pos_age_ms;    /* age of pos_x10 at the moment of this call */
    int16_t          last_target_x10;  /* last target the caller commanded, -1 = none */
    vent_result_t    last_result;   /* how that command ended */
    uint32_t         ms_since_move; /* since this window's last drive ended */
} vent_win_in_t;

typedef struct {
    /* --- time ------------------------------------------------------------ */
    uint32_t now_ms;                /* monotonic, wraps at 2^32. Use wrap-safe
                                     * differences only: (uint32_t)(a - b) */
    uint32_t unix_time;             /* wall clock, for your own logging needs only */
    bool     daytime;               /* the caller resolved the sun times */

    /* --- measurements ----------------------------------------------------- */
    int16_t  t_c10,  t_avg_c10;     /* air temperature, 0.1 °C */
    int16_t  t_avg_c;               /* the same average in WHOLE °C, as the
                                     * sensor layer rounded it. A law that
                                     * compares whole degrees must use this and
                                     * not divide t_avg_c10, or it rounds twice
                                     * and disagrees with the firmware in a
                                     * narrow band around each half degree */
    uint8_t  rh_pct, rh_avg_pct;    /* relative humidity, whole % */
    uint16_t wind_ms10, wind_avg_ms10;          /* wind speed, 0.1 m/s */
    uint16_t wind_dir_deg;          /* 0..359, meteorological */
    uint16_t wind_dir_avg_deg;
    uint16_t wind_dir_var_deg;      /* width of the arc covering the window's samples */
    bool     t_valid, rh_valid, wind_valid;     /* false = do not use that group */

    /* --- setpoints and tuning, day/night already resolved ----------------- */
    int16_t  t_max_c10;             /* ventilation threshold */
    uint8_t  rh_max_pct, rh_min_pct;
    uint8_t  hyst_t_c;              /* whole °C */
    uint8_t  hyst_rh_pct;
    uint8_t  cr_priority;           /* 0 = temperature first, 1 = humidity first,
                                     * 2 = the larger demand wins */
    bool     rh_ctrl_en;            /* humidity control master switch. Off, or
                                     * rh_valid false, means humidity casts no
                                     * vote at all */

    /* --- actuator limits the caller will enforce anyway ------------------- */
    uint16_t m3_deadzone_x10;       /* smallest aperture change worth a move, 0.1 % */
    uint32_t m3_min_interval_ms;    /* shortest time from the end of one M3 drive to the
                                     * start of the next (the linear dwell); 0 = none */

    /* --- the windows ------------------------------------------------------ */
    vent_win_in_t win[VENT_WINDOWS];
} vent_in_t;

typedef struct {
    vent_action_t action;
    int16_t       target_x10;       /* 0..1000, only with VENT_ACT_TARGET */
} vent_win_out_t;

typedef struct {
    /* The desired end state per window. NOT a sequence — the caller orders the
     * commands (see §3, "Ordering"). */
    vent_win_out_t win[VENT_WINDOWS];

    /* --- what the caller logs -------------------------------------------- */
    uint8_t  reason;                /* your code for "why", 0..255, logged verbatim */

    /* Mode 1's row, kept byte-identical: a stepped model reports the step it
     * resolved and the per-source steps behind it. -1 = "no such notion",
     * which is what a model without steps leaves them at. */
    int8_t   step;                  /* 0..VENT_STEPS_MAX, or VENT_STEP_NONE */
    int8_t   step_t;                /* temperature's own step, or -1 */
    int8_t   step_rh;               /* humidity's own step, -1 = no demand */

    /* Mode 2's row: a continuous demand in units the model documents. */
    int16_t  demand_t_x10;
    int16_t  demand_rh_x10;
} vent_out_t;

typedef struct { int32_t v[8]; } vent_state_t;   /* your memory. The caller owns it */

typedef struct {
    const char *name;               /* "stepped", "graded", … — logged and published */
    uint16_t    version;            /* bump on any behaviour change */
    void      (*reset)(vent_state_t *st);
    void      (*step)(const vent_in_t *in, vent_state_t *st, vent_out_t *out);
} vent_model_t;

const vent_model_t *vent_model_stepped(void);
const vent_model_t *vent_model_graded(void);

#endif /* VENT_MODEL_H */
```

### Units, once

| Quantity | Unit | Type |
|---|---|---|
| temperature | 0.1 °C | `int16_t` |
| humidity | whole % | `uint8_t` |
| wind speed | 0.1 m/s | `uint16_t` |
| wind direction | degree | `uint16_t` |
| aperture, target | 0.1 % of the taught span, 0..1000 | `int16_t`, −1 = unknown |
| time | millisecond, monotonic | `uint32_t` |
| hysteresis, setpoint margins | whole °C / whole % | `uint8_t` |

**Aperture is a percentage, not millimetres, on purpose.** The rig's M3 traverses 1.5 m in 13 s and
production's 40 m flap takes 176 s over a different span; a percentage is the only figure that means
the same thing on both, and the sensor reports a percentage natively.

---

## 3. The call contract

- **Cadence.** `step()` is called once per fresh measurement set: every `poll_interval_s`, default
  **30 s**, configurable 15–120 s. It is **not** called on a fixed timer, and it is **not** called
  while the controller is inhibited (see below), so gaps of minutes or hours are normal and your
  state may be reset between calls.
- **Context.** You run inside the climate task at priority 5, sharing its stack, between a sensor
  read and a queue write. **Return in well under a millisecond**, keep your own frame under about
  1 kB, and never block, sleep, allocate or take a lock.
- **Inhibits.** While the wind override, the motor alarm, a temperature-sensor fault, standby or a
  calibration sweep is active, the caller **does not call you at all** and discards any earlier
  output. Safety belongs to other tasks. Do not model it, do not try to override it.
- **State.** The caller calls `reset()` at boot, on a mode change, and when an inhibit begins.
  Anything you keep in `vent_state_t` — an integrator, a filter, a last-decision timestamp — must
  tolerate being zeroed at any of those points.
- **Validity.** `t_valid`, `rh_valid` and `wind_valid` may be false, and `pos_x10` may be −1 or
  stale (`pos_age_ms`). A missing input is not an error to report; decide conservatively and say so
  through `reason`.
- **Determinism.** Same input plus same state must give the same output, on the device and on a
  host. Integer arithmetic is strongly preferred; if you use floating point, avoid `libm` beyond the
  basic operators. No randomness, no reading the clock yourself — `now_ms` is the only time.

### What the caller does with your output

| Your output | What happens |
|---|---|
| `VENT_ACT_HOLD` | nothing is commanded for that window |
| `VENT_ACT_OPEN` / `VENT_ACT_CLOSE` | a full timed traverse is commanded, exactly as today |
| `VENT_ACT_TARGET` on a **LINEAR** window | clamped to 0..1000; dropped when within `m3_deadzone_x10` of the current position; deferred while `win[2].ms_since_move` is below `m3_min_interval_ms`; otherwise commanded, with the travel timer as the ceiling |
| `VENT_ACT_TARGET` on a **DIGITAL** window | **a model error.** Logged as such and treated as `HOLD` |
| `reason`, `demand_t_x10`, `demand_rh_x10` | written to the SD log with the decision, so it can be reconstructed afterwards |

**A part-open window is at neither end.** `VENT_WIN_PART_OPEN` never satisfies `VENT_ACT_OPEN` or
`VENT_ACT_CLOSE`, so the caller commands either one in full; a `TARGET` from it is an ordinary
move. Only a LINEAR window can be part-open, and `pos_x10` says where. A model that wants an end
must ask for it: the stepped law sends a part-open M3 to whichever end its step wants, never
HOLDs it, because holding would leave M3 part-open while the step says OPEN. T2 gained the state in 2.12.0 (`CH_PART_OPEN`, ordinal 7) and reports it: the two
enums share their ordinals, pinned with `static_assert`s in `climate_control.cpp`, so a
renumbering on either side fails the build rather than the greenhouse.

You therefore do **not** implement the deadband, the minimum interval, the clamping or the
"is this window even capable" check. You may read `m3_deadzone_x10` and `m3_min_interval_ms` to avoid
asking for moves that will be dropped — a dropped move is not an error, but it is noise in the log.

### Ordering, and why you must not sequence moves yourself

**Your output is a desired end state, not a sequence.** The caller decides the order, and it is
fixed:

1. **Narrowing first:** every `CLOSE`, and every `TARGET` that reduces an aperture.
2. **Then widening:** every `OPEN`, and every `TARGET` that increases one.

The reason is transient safety: the total open area stays monotone-decreasing while a decision is
being applied, so a cycle interrupted part-way — a wind override firing between two commands —
never leaves the greenhouse more open than both the old and the new decision intended. Today's
implementation documents the same rule at `reconcile_to_step()`.

Two consequences for your model:

- **At most one command per window per call**, and repeating the same output every call is the
  intended style. The actuator treats a command for the direction it is already going as a no-op,
  which is what makes a command lost to a dwell get re-issued once the dwell expires. Returning a
  steady desired state is therefore correct and cheap; do not go edge-triggered.
- **Do not use `HOLD` to mean "wait your turn".** If you want a window somewhere, say so on every
  call until it is there. Sequencing across calls is the caller's and the actuator's business, and a
  model that tries to choreograph it will fight the dwell timers and the reversal gap.

### Logging: which fields end up in the SD log

The caller writes the row, but the content comes from you, and the two modes use **different rows**
because a second meaning on one row is a defect this project already paid for (gh#54: every standby
row parsed as a ventilation decision for months).

| Mode | Row | Content |
|---|---|---|
| 1 | `LOG_MODE_CHANGE`, `param_id` 0 — **unchanged** | `value_a` = your `step`; `value_b` packs `step_t` (high byte) and `step_rh` (low byte), each as an int8 with −1 allowed |
| 2 | `LOG_MODE_CHANGE` with **its own `param_id`** | your `reason`, `demand_t_x10`, `demand_rh_x10`, and M3's commanded target |

- **A stepped model must fill `step`, `step_t` and `step_rh`**, or the existing row changes shape
  and `logparser.py`, `plot_daily.py` and `vent_step_replay.py` all misread history. That is the
  one hard compatibility requirement on mode 1's model.
- **A model with no step notion leaves all three at −1** and expresses itself through `reason` and
  the demands.
- The caller logs **on change**, not every call, so these fields must be a function of the current
  decision — never a counter or an accumulator.
- Mode 2's row, its `param_id` and the parser branch that decodes it are one change, shipped
  together. Adding the row without the parser is the gh#54 mistake repeated.

---

## 3a. Where the numbers come from

Every climate setting is entered by the operator, in the web GUI or on the LCD. **The model never
reads a setting** — it receives resolved values. The chain is:

1. **The GUI (T11) or the LCD (T8)** posts the change onto the configuration queue.
2. **T4** validates and clamps it against its single descriptor row in `firmware/config/cfg_desc.inc`,
   writes NVS, updates the in-memory shadow under a mutex, and emits the audit row.
3. **T6** takes a consistent copy of that shadow at the top of each cycle.
4. **T6 resolves** it for you: day or night, the hysteresis floors, the unit conversions, and the
   deadband in percent.

### Which field comes from where

| `vent_in_t` field | Source | What the caller already did |
|---|---|---|
| `t_max_c10` | `t_max_day` / `t_max_ngt` | picked one by `is_daytime`, converted whole °C to 0.1 °C |
| `rh_max_pct`, `rh_min_pct` | `rh_max_day/ngt`, `rh_min_day/ngt` | picked one by `is_daytime` |
| `hyst_t_c`, `hyst_rh_pct` | `hyst_t`, `hyst_rh` | floored at 1, so a law can divide by them |
| `cr_priority` | `cr_priority` | — |
| `rh_ctrl_en` | `rh_ctrl_en` | combined with `rh_valid`: either one false means humidity casts no vote |
| `daytime` | `is_daytime` | **not a setting** — T4 computes it from the sun times and the site coordinates |
| `t_avg_c10`, `t_avg_c`, `rh_avg_pct`, `wind_*_avg_*` | T5's reading, averaged | averaged over `avg_win_t`, `avg_win_rh`, `avg_win_wind` — **those settings are not passed to you**; you receive the result |
| `t_valid`, `rh_valid`, `wind_valid` | the measurement snapshot and the sensor-fault flags | — |
| `m3_deadzone_x10` | `deadzone_m3_mm` | converted from mm using M3's taught span, which is why you get a percentage |
| `m3_min_interval_ms` | the linear-dwell setting, converted to ms | **the key does not exist yet** (plan §10, decision 10); until it does, expect 0. Not the *minimum move* of §7, which is the shortest pulse that moves the leaf |
| `win[].state` | T2 | reversal gaps folded into the moving states |
| `win[].cap`, `pos_x10`, `pos_age_ms` | T17, through T4's pass-through | capability already reflects fitted, gate open and fault-free |
| `win[].last_target_x10`, `last_result`, `ms_since_move` | the caller's own record of what it commanded | — |
| `now_ms`, `unix_time` | the caller | — |

**Deliberately absent:** the heating setpoints (`t_min_*`, no heating in this installation) and every
wind-safety setting (`v_max`, `wind_hyst`, `dir_excl_low`/`dir_excl_high`). Wind safety belongs to
T3, which acts on its own and suspends the model; a direction-aware law gets the *measured* wind,
never the safety thresholds.

**Testing a law under any settings.** The closed-loop simulator plays the caller's side of this
table for every controller key, in the same places as the firmware: the fields above, T5's windows,
T3, T2's travel and dwell, T4's site and the poll interval. It takes the values from 5C88's logged
history, a unit's GET /api/config, or `--set KEY=VALUE` (`model/closedloop/settings.py`; `closed_loop.py
settings` lists where each key acts). With `--set wpos_fitted_m3=1` it also plays M3's side of mode 2:
`cap`, `pos_x10` with its age, the last target and how it ended, the deadband, the minimum interval,
and T2's stop rule (`model/closedloop/README.md`, "A linear M3").

### Three consequences to design around

- **A setting arrives whole, and late.** A GUI write is queued, so it reaches you on the first cycle
  after T4 applies it — up to one poll interval, about 30 s. Because the caller copies the whole
  shadow at once, you never see half of a multi-field change.
- **Bounds are enforced before you.** The descriptor clamps on the write path, so ranges can be
  trusted. Defend against division by zero anyway, as the stepped model does, so the model stays
  correct when a test calls it directly.
- **The operator can change a setting mid-run.** Nothing is latched for you: a threshold can move
  between two calls. Keep your state in `vent_state_t` interpretable when it does.

### Adding a tunable of your own

A gain you invent is not free — it becomes an operator setting, and in this repo that has a fixed
shape:

1. **One row in `cfg_desc.inc`**, naming a `K_*` key, a shadow field, `CFG_MIN/MAX_*`, a `DEF_*`
   default and a `LOG_PARAM_*` audit id. Verified by `bin/check_cfg_desc.py`, which the pre-commit
   hook runs.
2. **The caller passes it** into a new `vent_in_t` field — and the field is added to this contract.
3. **The surfaces:** the GUI control, the LCD screen if it belongs there, `webUiMock`'s limits, and
   `logparser.py`'s param table, so the audit row decodes.
4. **A control the operator cannot use right now is greyed out with the reason beside it, never
   hidden** — a mode 2 tunable in mode 1 is exactly that case.

**Until the key exists, keep the value a constant in your own file.** A slider that changes nothing
is this project's most-repeated defect: an affirmative signal for something that did not happen.

## 4. Where the code lives

Yes — the model is confined to its own header and implementation files, in its own library, with no
firmware dependencies. It follows the pattern every driver in this repo already uses: a
host-testable library under `drivers/`, wrapped by a thin ESP-IDF component.

```
drivers/ventModel/                    EXISTS
  library.json
  platformio.ini                      [env:native] — unity tests, no hardware
  set_compiler.py                     points at the MinGW toolchain, as the other drivers do
  src/vent_model.h                    the interface in §2
  src/vent_model_stepped.cpp          mode 1: today's step table, copied from T6
  src/vent_model_graded.cpp           mode 2: a first candidate (model/closedloop/gradedCandidate.md)
  test/test_vent_model/               host unit tests, stepped: 23 passing
  test/test_vent_model_graded/        host unit tests, graded: 15 passing
firmware/components/ventModel/        created in 2.12.0; no REQUIRES (the library sees no IDF)
  CMakeLists.txt                      idf_component_register over the sources above
```

The component proxy and the `REQUIRES` entry in `firmware/src/CMakeLists.txt` are deliberately
absent: an IDF component is compiled into every firmware build, and nothing in the firmware calls
the model yet. Adding them belongs with the T6 refactor, not before it.

The component needs **no** `REQUIRES` and no `-I firmware/config`: the model reads nothing but its
input struct. That is what keeps it host-compilable, and it is the one rule that must not be bent —
the moment a model includes an ESP-IDF or project header, the replay and the unit tests stop being
able to build it.

**Adding a model** is three things: the new `.cpp`, its accessor in `vent_model.h`, and one row in
the caller's model table. Nothing else in the firmware changes.

**A class instead?** The interface is deliberately a struct of function pointers rather than a C++
abstract base: no vtable, no heap, no RTTI, trivially usable from the C-style host harness, and it
matches the rest of this codebase. You may of course implement your model as a class internally and
expose the four members.

### Building and running the tests

```bash
cd drivers/ventModel && pio test -e native      # host unit tests, no hardware
```

---

## 5. What a new or tuned model must prove

1. **Host unit tests.** Cover at least: below threshold (everything closed), a rising ramp, a
   falling ramp with hysteresis, humidity-only demand, a temperature/humidity conflict at each
   `cr_priority`, invalid inputs, an unknown position, a linear window that reports `DIGITAL`, and a
   `FAIL_TIMEOUT` on the last command.
2. **Replay against real weather.** `model/vent_step_replay.py` already reconstructs the decision
   inputs from SD logs, runs `stepped` from this library on them (through
   `model/closedloop/ventmodel.py` since 2026-09-19, replacing a Python port whose output it
   reproduced exactly), and refuses to project unless it first reproduces the logged demands. **The mode 2 harness now exists**: `model/closedloop/closed_loop.py
   reproduce --plant2 <fit> --model <name> --set wpos_fitted_m3=1 --set ctrl_mode_m3=1` runs the
   compiled law against real weather with a linear M3, and prints exactly the list below. Report,
   for each candidate:
   - M3 openings per day, and motor starts per day per window;
   - M3 open time;
   - the temperature the greenhouse would have seen, against the logged temperature;
   - how often a commanded move was dropped by the deadband or deferred by the minimum interval.
3. **The reference check, before anything else.** `stepped` behind this interface must reproduce
   the logged decisions at least as well as today's replay does: **96.8 % of 378 decisions**
   (`campaignResults_summer2026.md` F8). Its `step`, `step_t` and `step_rh` must also reproduce the
   existing log row exactly, so history stays readable by the same parsers. **And
   `closed_loop.py gate-control`**, which runs the same library inside an emulated T5/T4/T6 chain
   rather than a reconstruction of it: **97.1 % of 381** as of 2026-09-20. Until all of these pass,
   a difference in `graded` cannot be attributed to the law rather than to the refactor.
4. **A soak on the dev rig** before production: at least 12 h, at least 10 judged strokes, all fault
   counters at 0, and the motor-start count per hour reported.
5. **The thermal claim can only be proven on the production greenhouse over a summer.** The rig can
   prove that commanded apertures are reached accurately and repeatably; it cannot prove that doing
   so damps the limit cycle. Say which of the two your evidence supports.

---

## 6. What the evidence says, and does not

Read `campaignResults_summer2026.md` before choosing a law. The short version:

*Revised 2026-09-18.* The first campaign analysis joined outdoor data that were two hours late (the
LoRa database stamps in UTC), and its claims about M3 are withdrawn: "a factor of 30–100", "about
0.05 /h with south-west wind", "M3 barely ventilates". What holds on correctly timed data:

- **M3 is the house's largest ventilator.** Every plant fitted on correctly timed data puts it at
  3–8× one roof window, 60 % or more of the window ventilation with everything open. The absolute
  air-change rates are not pinned down; only this ranking is.
- **In north wind the controller's reading drops about twice as far after M3 opens.** Across 236
  openings in normal operation, the drop at 25 min is -3.0 °C with wind from 315–45° against -1.3
  to -2.05 °C from other directions. It holds within a month, at matched wind speed and with the
  doors shut.
  - **Whether the whole house cools that much faster is open.** Indoor sensors at 1/4 and 3/4 of the
    length barely show it, and long all-open periods show no north-wind advantage. A law reading
    the controller's sensor may see a stronger effect of M3 in north wind than the house gets.
  - **§9 item 2** asks what your law does with wind direction. Do not build it on a direction
    effect of a particular size.
- **There is no measurement of a part-open M3.** Every test was fully open or fully closed, so the
  aperture-to-airflow curve is unknown. Step 2 to step 3 multiplies the open area about sixfold,
  and in the fitted plants it raises the greenhouse air's heat loss to outside by 1.7–1.8×.
- **The limit cycle that motivates linear control:** about a 42 min period and a 4.9 °C swing on
  2026-07-20, with M3 opening 3–4 times a day. Its amplitude is roughly the cooling rate times the
  25 min dwell. 7 of that day's 8 M3 openings came with wind from 321–354° at about 3 m/s.
- **M3 cycles in every wind.** Of 370 M3 openings across the campaign, 151 came with north wind
  (315–45°), 91 with wind from 200–290°, and 128 from other directions.
- **There is a closed-loop simulator now, with one known shortfall.** `model/closedloop/` compiles
  `drivers/ventModel/` into a host library and runs a law through an emulated T5/T4/T6 chain
  against a fitted plant, on logged weather. Fed the logged sensor rows, `stepped` reproduces 97.1 %
  of 381 logged temperature demands.
  - **The adopted plants** are
    `model/campaign-summer-2026/plant2/plant2_summer2026_Ca2.9_tau120_tau90_ev5_dir.json` and
    `…_tau240_tau90_ev5_dir.json`. They model the sensor's delay and a north-wind term. Against
    them `stepped` reproduces 5C88's M3 openings, its ~45 min cycle, its hours above 31 °C and its
    swing on days without north wind, on days the fit never saw.
  - **On north-wind days they under-state the swing** (3.0–3.2 against 3.9 °C), which is exactly
    what a linear law sets out to damp.
  - **So:** run a candidate against both plants, and treat a verdict that differs between them as
    unsettled. Treat a simulated reduction of the swing, above all in north wind, as a lead to
    confirm on the greenhouse. The point-by-point accuracy targets (AC-9, AC-10) still fail.
- **Tuning the existing law is a bad trade, which is why linear control was proposed:** raising
  `hyst_t` cuts cycling by about 80 % but costs about 55 % of M3's open time, and an extra dead band
  on stepping down was rejected after it merely made each opening longer.

---

## 7. Physical and mechanical constraints

| Constraint | Value | Consequence for a model |
|---|---|---|
| M3 traverse | **176 s** production, **13 s** on the dev rig | A full move is long against a 30 s decision cadence: dead time, not a delay |
| M3 leaf speed | ≈0.57 %/s production, ≈8.8 %/s rig | A target is reached slowly; commanding a new one mid-move is a reversal |
| M1, M2 traverse | ≈26 s | — |
| Position freshness | one sample per `travel/150`: ≈1.17 s production, 100 ms rig | Overshoot ≈ age × speed, ≈0.67 % production. The requirement is 1 %, so there is little room |
| Temperature reading | follows the air 3.5–5.5 min late: after an M3 move the logged temperature barely changes for 3–4 min (2026-09-18, NS-10) | Do not read an unchanged temperature in the first minutes after a move as a move that did nothing; a law that answers faster than the reading does will overshoot |
| Minimum move | **not yet measured** — the shortest pulse that actually moves the leaf | Until it is, do not rely on moves below a few percent |
| Dwell today | M3 25 min after opening, 10 min after closing; M1, M2 5 min after opening, none after closing | **As built in 2.12.0, mode 2 replaces BOTH of M3's dwells** with `min_intv_m3`, the interval between any two moves — not the open dwell alone, because a continuous law narrows as often as it widens. T2 arms it (`ch_dwell_ms()`) and T6 refuses a target inside it, both from the one key, so the actuator's dwell and the caller's interval cannot drift apart. **Its default is 600 s** (2026-09-20): free under a law that already holds ten minutes, and the floor that protects against one that does not. 0 means no dwell on M3 at all and is a test setting; **never above 900**, where mode 2 stops paying for itself |
| Reversal | the actuator inserts a 2 s gap, then drives the other way | A reversal is two drives and two judged movements; frequent reversals are motor wear |
| End switches | cut the motor at each physical end, invisible to the firmware | Over-driving is harmless; a target of 0 or 1000 is safe |
| Motor wear | no measured limit | Report starts per day. The stepped law produces 3–8 M3 openings a day; a continuous law must not multiply that |

---

## 8. Out of scope for a model

- **Safety.** Wind override, motor alarm, the boot close-all sweep, standby. Handled elsewhere, and
  a model is not called while any of them is active.
- **Mode selection and the fallback** between mode 2 and mode 1.
- **The actuator.** Relay timing, travel timers, dwell, reversal gaps, persisted state.
- **Logging and the queue.** The caller writes the row and posts the command, including your
  `reason` and demands.
- **Configuration.** You receive resolved values; you do not read storage or the key names.
- **The position sensor.** Its presence, its faults, its calibration and its trust level reach you
  only as `cap`, `pos_x10` and `pos_age_ms`.

---

## 9. Declare these when you submit a model

1. The law in one paragraph, and why it suits a slow actuator with a 30 s sample time.
2. Whether it needs wind direction, and what it does when the wind reading is invalid.
3. Its tunables: name, unit, range, default, and what each one trades against.
4. How it handles the temperature/humidity conflict, for each `cr_priority`.
5. What it does with an unknown or stale position, and with `FAIL_TIMEOUT` on its last command.
6. The expected motor starts per day, against the stepped law's 3–8 M3 openings.
7. Its `name`, its `version`, and the meaning of every `reason` code — they end up in the SD log and
   in the operator-facing status.
8. Whether it reports steps (`step`, `step_t`, `step_rh`, which keeps mode 1's log row) or uses the
   mode 2 row, and in what unit its `demand_*` values are expressed.
9. The replay output from §5, on at least two contrasting weeks: one with north wind (M3's side),
   one with wind from another quarter.

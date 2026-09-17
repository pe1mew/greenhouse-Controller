# Ventilation control model — interface contract

| Field | Value |
|---|---|
| Document | Interface contract for the T6 ventilation control model |
| Date | 2026-09-17 |
| Status | **The interface and the mode 1 reference exist as a library; the firmware does not use them yet.** `drivers/ventModel/` holds `src/vent_model.h` and `src/vent_model_stepped.cpp` — a faithful copy of the 2.9.1 stepped law — with 21 host tests passing (`pio test -e native`). The firmware in `main` still runs its own inline copy in T6 and is untouched; the two are kept in step by hand until T6 is refactored onto this interface in **2.11.0** |
| Audience | Whoever writes or tunes a control model — a separate session, a separate agent, or a person. **This document is meant to be read on its own** |
| Scope decisions | [`integrateWindowPositionSensor.md`](integrateWindowPositionSensor.md) §5b (the two control modes, the position path) and §5c (the rules around this contract) |
| Requirements | [`functionalRequirementsSpecification.md`](functionalRequirementsSpecification.md), and [`windowPositionSensorRequirements.MD`](windowPositionSensorRequirements.MD) FR-WP04/05/17/18 |
| Evidence for tuning | [`../model/campaignResults_summer2026.md`](../model/campaignResults_summer2026.md), [`../model/thermalProfileCampaign.md`](../model/thermalProfileCampaign.md), SD logs in `../model/campaign-summer-2026/` |

---

## 1. What you are writing

A **pure function** that decides, every time the controller has fresh measurements, what each of the
three greenhouse windows should do. You do not touch queues, hardware, storage, logging or safety.
You receive one input struct, you keep your own state in a struct the caller owns, and you fill one
output struct.

**The greenhouse.** One span at Herenboeren Willemshoeve (Soest), about 2 900 m³, with three
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

#define VENT_MODEL_API 1
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
    uint16_t m3_min_move_ms;        /* shortest interval between two M3 moves */

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
| `VENT_ACT_TARGET` on a **LINEAR** window | clamped to 0..1000; dropped when within `m3_deadzone_x10` of the current position; deferred when within `m3_min_move_ms` of the last move; otherwise commanded, with the travel timer as the ceiling |
| `VENT_ACT_TARGET` on a **DIGITAL** window | **a model error.** Logged as such and treated as `HOLD` |
| `reason`, `demand_t_x10`, `demand_rh_x10` | written to the SD log with the decision, so it can be reconstructed afterwards |

You therefore do **not** implement the deadband, the minimum interval, the clamping or the
"is this window even capable" check. You may read `m3_deadzone_x10` and `m3_min_move_ms` to avoid
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
  src/vent_model_graded.cpp           mode 2: the new law  ← your file, NOT YET WRITTEN
  test/test_vent_model/               host unit tests: 21 passing
firmware/components/ventModel/        NOT YET CREATED — the 2.11.0 wiring step
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
   inputs from SD logs and refuses to project unless it first reproduces the logged demands. Mode 2
   needs the model compiled into a host harness fed the same rows. Report, for each candidate:
   - M3 openings per day, and motor starts per day per window;
   - M3 open time;
   - the temperature the greenhouse would have seen, against the logged temperature;
   - how often a commanded move was dropped by the deadband or deferred by the minimum interval.
3. **The reference check, before anything else.** `stepped` behind this interface must reproduce
   the logged decisions at least as well as today's replay does: **96.8 % of 378 decisions**
   (`campaignResults_summer2026.md` F8). Its `step`, `step_t` and `step_rh` must also reproduce the
   existing log row exactly, so history stays readable by the same parsers. Until both pass, a
   difference in `graded` cannot be attributed to the law rather than to the refactor.
4. **A soak on the dev rig** before production: at least 12 h, at least 10 judged strokes, all fault
   counters at 0, and the motor-start count per hour reported.
5. **The thermal claim can only be proven on the production greenhouse over a summer.** The rig can
   prove that commanded apertures are reached accurately and repeatably; it cannot prove that doing
   so damps the limit cycle. Say which of the two your evidence supports.

---

## 6. What the evidence says, and does not

Read `campaignResults_summer2026.md` before choosing a law. The short version:

- **M3's effect depends on wind direction by a factor of 30–100.** About 0.05 /h air changes with
  south-west wind (the wall is leeward), roughly 3–10 /h with north wind (windward). A single fixed
  aperture-to-effect assumption will be wrong in one of the two regimes.
- **There is no measurement of a part-open M3.** Every test was fully open or fully closed, so the
  aperture-to-airflow curve is unknown. Step 2 to step 3 multiplies the open area about sixfold.
- **The limit cycle that motivates linear control:** about a 42 min period and a 4.9 °C swing on
  2026-07-20, with M3 opening 3–4 times a day. Its amplitude is roughly the cooling rate times the
  25 min dwell. **That day was windward:** 7 of its 8 M3 openings came with wind from 321–354° at
  about 3 m/s (log analysis, 2026-09-17).
- **But M3 also cycles when it barely ventilates.** Of 370 M3 openings across the campaign, 151 came
  with north wind, 91 with south-west wind, and 128 from sectors the campaign never characterised.
- **The plant model is not a simulator you can trust yet.** Its temperature and humidity accuracy
  targets both fail, and the closed-loop simulation has never been run. Tune against logged weather
  and real behaviour, not against the model's predictions.
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
| Minimum move | **not yet measured** — the shortest pulse that actually moves the leaf | Until it is, do not rely on moves below a few percent |
| Dwell today | M3 25 min after opening, 10 min after closing; M1, M2 5 min after opening, none after closing | Mode 2 replaces the open dwell with a minimum interval between moves; the caller enforces it |
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
9. The replay output from §5, on at least two contrasting weeks: one windward, one leeward.

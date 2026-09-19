/**
 * @file vent_model.h
 * @brief The ventilation control model interface — T6's decision law, isolated.
 *
 * Contract: `design/ventModelContract.md`. Read it before writing a model; it
 * carries the units, the call contract, the log fields and what a new model
 * must prove.
 *
 * ## The one rule that must not be bent
 *
 * **This header and every model implementation compile with a plain C/C++
 * compiler: no ESP-IDF, FreeRTOS, Arduino or project headers, no `firmware/`
 * include paths.** The same sources then run in the firmware, in the host unit
 * tests (`pio test -e native`) and in the offline replay. The moment a model
 * includes a project header, the tests and the replay stop being able to build
 * it.
 *
 * ## What a model is
 *
 * A pure function. It receives measurements, setpoints and the state of the
 * three windows, and it returns the state it wants each window to be in. It
 * does not touch queues, hardware, storage, logging or safety; the caller (T6)
 * owns all of that, applies the actuator limits and decides the ordering.
 *
 * ## Status
 *
 * **Not yet wired into the firmware.** `vent_model_stepped` is a faithful copy
 * of the 2.9.1 stepped logic in `firmware/src/climate_control/climate_control.cpp`,
 * built here so it can be tested on the host and serve as the reference for
 * `vent_model_graded` (mode 2). The firmware still runs its own inline copy;
 * the two are kept in step by hand until T6 is refactored onto this interface
 * (planned for 2.11.0 — plan §5b/§5c).
 *
 * @author  Greenhouse Controller project
 */

#ifndef VENT_MODEL_H
#define VENT_MODEL_H

#include <stdbool.h>
#include <stdint.h>

/** Interface revision. Bump only on an incompatible change.
 *  2 (2026-09-19): `m3_min_move_ms` (16-bit, at most 65.5 s) became
 *  `m3_min_interval_ms` (32-bit), and `VENT_WIN_PART_OPEN` was added. Both
 *  gaps were found by the model work, building a simulator against this
 *  header. */
#define VENT_MODEL_API  2

/** Windows, in fixed order: index 0 = M1, 1 = M2, 2 = M3. */
#define VENT_WINDOWS    3

/** Highest ventilation step a stepped model may report (mirrors the
 *  firmware's `NUM_VENT_STEPS`). Steps run 0..VENT_STEPS_MAX. */
#define VENT_STEPS_MAX  3

/** "No demand from this source", and "this model has no step notion".
 *  Mirrors the firmware's `VENT_STEP_NEUTRAL`. */
#define VENT_STEP_NONE  (-1)

/** Is a window positionable, or only open/closed? M1 and M2 are always
 *  digital; M3 is linear while its position sensor is fitted and trusted. */
typedef enum {
    VENT_CAP_DIGITAL = 0,
    VENT_CAP_LINEAR  = 1,
} vent_cap_t;

/** What the actuator believes a window is doing. Same order as the firmware's
 *  `window_state_t`, which has no part-open state yet: T2 gains one in 2.11.0
 *  (plan §5b), and it must take VENT_WIN_PART_OPEN's value, appended after the
 *  others so no existing value moves. */
typedef enum {
    VENT_WIN_UNKNOWN = 0,      /**< position not established (before calibration) */
    VENT_WIN_CLOSED,
    VENT_WIN_MOVING_OPEN,
    VENT_WIN_OPEN,
    VENT_WIN_MOVING_CLOSE,
    VENT_WIN_PART_OPEN,        /**< at rest between the ends. LINEAR windows
                                *   only; pos_x10 says where. It is neither
                                *   OPEN nor CLOSED: a model that wants either
                                *   end must ask for it */
} vent_win_state_t;

/** What a model wants done with one window. A desired END STATE, not a step in
 *  a sequence: the caller orders the commands (narrowing before widening) and
 *  drops the ones that are already satisfied. */
typedef enum {
    VENT_ACT_HOLD = 0,         /**< leave this window alone */
    VENT_ACT_CLOSE,            /**< drive fully closed */
    VENT_ACT_OPEN,             /**< drive fully open */
    VENT_ACT_TARGET,           /**< drive to target_x10 — LINEAR windows only */
} vent_action_t;

/** How the caller's last command to a window ended. */
typedef enum {
    VENT_RES_NONE = 0,         /**< nothing commanded yet, or still moving */
    VENT_RES_DONE,             /**< arrived, confirmed */
    VENT_RES_FAIL_TIMEOUT,     /**< the travel timer ran out without arrival */
    VENT_RES_FAIL_FAULT,       /**< the position source failed during the move */
    VENT_RES_ABORTED,          /**< safety or the operator took the window */
} vent_result_t;

/** One window, as it actually is. */
typedef struct {
    vent_win_state_t state;
    vent_cap_t       cap;
    int16_t          pos_x10;          /**< aperture 0..1000 = 0..100.0 %;
                                        *   -1 = unknown. LINEAR windows only */
    uint32_t         pos_age_ms;       /**< age of pos_x10 at this call */
    int16_t          last_target_x10;  /**< last target commanded, -1 = none */
    vent_result_t    last_result;      /**< how that command ended */
    uint32_t         ms_since_move;    /**< since this window's last drive ended */
} vent_win_in_t;

/** Everything a model may read. The caller fills it; the model treats it as
 *  const and reads nothing else — no clock, no storage, no globals. */
typedef struct {
    /* ---- time ---------------------------------------------------------- */
    uint32_t now_ms;             /**< monotonic, wraps at 2^32. Wrap-safe
                                  *   differences only: (uint32_t)(a - b) */
    uint32_t unix_time;          /**< wall clock, for the model's own needs only */
    bool     daytime;            /**< the caller resolved the sun times */

    /* ---- measurements -------------------------------------------------- */
    int16_t  t_c10;              /**< air temperature, 0.1 °C */
    int16_t  t_avg_c10;          /**< its sliding average, 0.1 °C */
    int16_t  t_avg_c;            /**< the same average in WHOLE °C, rounded by
                                  *   the sensor layer. The stepped law compares
                                  *   whole degrees and must use this rather
                                  *   than dividing t_avg_c10, or it rounds
                                  *   twice and disagrees with the firmware in
                                  *   a narrow band around each half degree */
    uint8_t  rh_pct;             /**< relative humidity, whole % */
    uint8_t  rh_avg_pct;         /**< its sliding average, whole % */
    uint16_t wind_ms10;          /**< wind speed, 0.1 m/s */
    uint16_t wind_avg_ms10;
    uint16_t wind_dir_deg;       /**< 0..359, meteorological */
    uint16_t wind_dir_avg_deg;
    uint16_t wind_dir_var_deg;   /**< width of the arc covering the samples */
    bool     t_valid;            /**< false = do not use the temperature group */
    bool     rh_valid;
    bool     wind_valid;

    /* ---- setpoints and tuning, day/night already resolved -------------- */
    int16_t  t_max_c10;          /**< ventilation threshold, 0.1 °C */
    uint8_t  rh_max_pct;
    uint8_t  rh_min_pct;
    uint8_t  hyst_t_c;           /**< whole °C */
    uint8_t  hyst_rh_pct;        /**< whole % */
    uint8_t  cr_priority;        /**< 0 = temperature first, 1 = humidity first,
                                  *   2 = the larger demand wins */
    bool     rh_ctrl_en;         /**< humidity control master switch. False =
                                  *   humidity casts no vote at all */

    /* ---- actuator limits the caller enforces anyway -------------------- */
    uint16_t m3_deadzone_x10;    /**< smallest aperture change worth a move */
    uint32_t m3_min_interval_ms; /**< shortest time from the end of one M3
                                  *   drive to the start of the next, the
                                  *   linear dwell; compare win[2].ms_since_move.
                                  *   0 = no limit. 32-bit because it stands in
                                  *   for dwells of 10 and 25 minutes. Not the
                                  *   "minimum move", which is the shortest
                                  *   pulse that moves the leaf (contract §7) */

    /* ---- the windows --------------------------------------------------- */
    vent_win_in_t win[VENT_WINDOWS];
} vent_in_t;

/** What a model wants done with one window. */
typedef struct {
    vent_action_t action;
    int16_t       target_x10;    /**< 0..1000, only with VENT_ACT_TARGET */
} vent_win_out_t;

/** A model's whole answer: the desired state per window, plus the fields the
 *  caller writes to the SD log. */
typedef struct {
    vent_win_out_t win[VENT_WINDOWS];

    /* ---- what the caller logs ------------------------------------------ */
    uint8_t  reason;             /**< model-defined "why", logged verbatim */

    /* Mode 1's row, kept byte-identical: `LOG_MODE_CHANGE` param 0 carries
     * step in value_a and (step_t << 8 | step_rh) in value_b. A model with no
     * step notion leaves all three at VENT_STEP_NONE. */
    int8_t   step;               /**< resolved step 0..VENT_STEPS_MAX, or -1 */
    int8_t   step_t;             /**< temperature's own step, or -1 */
    int8_t   step_rh;            /**< humidity's own step, -1 = no demand */

    /* Mode 2's row: a continuous demand, in units the model documents. */
    int16_t  demand_t_x10;
    int16_t  demand_rh_x10;
} vent_out_t;

/** A model's memory, owned and reset by the caller. Zeroed by
 *  `reset()` at boot, on a mode change and when an inhibit begins, so a model
 *  must tolerate losing it at any of those points. */
typedef struct {
    int32_t v[8];
} vent_state_t;

/** One model. The caller holds a table of these and uses exactly one. */
typedef struct {
    const char *name;            /**< "stepped", "graded", … — logged and published */
    uint16_t    version;         /**< bump on any behaviour change */
    void      (*reset)(vent_state_t *st);
    void      (*step)(const vent_in_t *in, vent_state_t *st, vent_out_t *out);
} vent_model_t;

/**
 * @brief Mode 1 — the stepped law: three equal steps above the threshold.
 *
 * A faithful copy of the 2.9.1 firmware logic. See `vent_model_stepped.cpp`
 * for the provenance of each function and the one deliberate difference.
 */
const vent_model_t *vent_model_stepped(void);

/**
 * @brief Mode 2 — a first CANDIDATE for the graded law (plan decision 9 is open).
 *
 * M1 and M2 as the stepped law's first two steps; M3 proportional to the
 * temperature excess with a rate limit. See `vent_model_graded.cpp`.
 */
const vent_model_t *vent_model_graded(void);

#endif /* VENT_MODEL_H */

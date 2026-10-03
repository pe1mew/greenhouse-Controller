/**
 * @file characterise.h
 * @brief M3's characterisation run: the unit measures its own window (plan §5e).
 *
 * ## What it is for
 *
 * The positioning figures the plan records for M3 (§3.6: the speed, the slack
 * lost on a reversal, the shortest pulse that moves the leaf, AT-WP02's
 * repeatability) were measured on the development rig with bench-only hooks
 * and harnesses. 5C88 takes release builds by ROTA only, and its window has
 * different mass, relays and speed. So the unit measures itself: an admin
 * starts a run on the commissioning card, the run drives M3 through four
 * phases, and the deadband follows from what it measured (decided by the
 * operator, 2026-10-03).
 *
 * | Phase | Gives |
 * |---|---|
 * | 1. Speed: 20 -> 80 % and back | the speed per direction; T17's read interval |
 * | 2. Reversal loss: 16 pulses in pairs | the travel lost on each reversal |
 * | 3. Minimum move: blocks of 5 pulses, a halving search | floor 2 and the dead time |
 * | 4a. AT-WP02: 10 approaches to 50 % | spread, hysteresis, landing error |
 * | 4b. The band check: 8 corrections a round | the deadband |
 *
 * The arithmetic is drivers/m3Char's, host-tested against the rig's archived
 * runs. This module is the procedure around it: commanding M3, the rest
 * between starts, the guards and the record.
 *
 * ## How it runs
 *
 * **In T17, like the teach.** T17 calls characterise_tick() on every pass of
 * its loop and characterise_reading() with every reading it takes, so the run
 * reads the leaf when it needs to, with no HTTP round trip and no extra bus
 * traffic. Its commands go on Q1 with `SRC_OPERATOR_MANUAL`, as the teach's
 * legs do: CMD_TARGET for the moves, CMD_PULSE for the pulses.
 *
 * **Every step has the same cycle:** wait out the rest since the last drive
 * ended; command; T2 must start the drive within 3 s; wait for it to end;
 * wait for the settle (T17's own rule, SETTLE_BASE_MS plus one measurement
 * window); read the leaf twice and average.
 *
 * **It holds STANDBY exactly as the teach does** (commission.cpp): from the
 * start, kept while the run lasts whatever its admin session does, released
 * once the run is over and the session has ended.
 *
 * **It ends, and commands nothing more, on any of these:** the Abort button; the
 * wind override, a motor alarm or a recalibration; M3 moved by anything but
 * the run; STANDBY ended by an explicit mode choice; a sensor fault, a gate
 * that closes or both end sensors at once; an end sensor during a pulse
 * phase; a drive that does not start or does not end in time. A drive already
 * started ends by its own rule.
 *
 * **It leaves one record** (data_manager/m3char_record.h), written once at the
 * end, complete or not, and SD log rows (`ALARM` ch 6, param 253). A complete
 * run's band becomes the measured band, which is in force while
 * `deadzone_src_m3` is 1.
 */

#ifndef CHARACTERISE_H
#define CHARACTERISE_H

#include <stdbool.h>
#include <stdint.h>

#include "window_pos.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where a run is. */
typedef enum {
    CHAR_IDLE = 0,   /**< no run since boot */
    CHAR_RUNNING,
    CHAR_DONE,       /**< ended on its own: see the reason (NONE, or NO_BAND) */
    CHAR_FAILED,     /**< ended early, or was refused: see the reason */
    CHAR_STATE_COUNT_
} char_state_t;

/**
 * Why a run was refused or how it ended. **Stored as the record's `outcome`
 * and logged, so append only:** never renumber.
 */
typedef enum {
    CHAR_ERR_NONE = 0,       /**< complete, with a band */
    CHAR_ERR_OPERATOR,       /**< the Abort button */
    CHAR_ERR_M3_BUSY,        /**< M3 moving, or moved by anything but the run */
    CHAR_ERR_SENSOR,         /**< a fault, no answer, a closed gate, too few readings */
    CHAR_ERR_BOTH_ENDS,      /**< both end sensors read active */
    CHAR_ERR_WIND,           /**< the wind override */
    CHAR_ERR_MOTOR_ALARM,
    CHAR_ERR_CALIBRATING,    /**< a recalibration sweep */
    CHAR_ERR_HOLD_LOST,      /**< STANDBY ended by an explicit mode choice */
    CHAR_ERR_NO_START,       /**< T2 did not start a command within 3 s */
    CHAR_ERR_TIMEOUT,        /**< a drive outlasted its limit */
    CHAR_ERR_END_REACHED,    /**< an end sensor made during a pulse phase */
    CHAR_ERR_NO_MOVE,        /**< phase 3's first width did not move the leaf at all */
    CHAR_ERR_NOT_FITTED,     /**< refused: `wpos_fitted_m3` is 0 */
    CHAR_ERR_NOT_TAUGHT,     /**< refused: the calibration is not VALID */
    CHAR_ERR_TEACH_RUNNING,  /**< refused: a teach is running */
    CHAR_ERR_BAD_REST,       /**< refused: the rest is out of bounds */
    CHAR_ERR_NO_MEMORY,      /**< refused: the run's buffers could not be allocated */
    CHAR_ERR_NO_BAND,        /**< complete, but the check never passed (or b0 was past the bound) */
    CHAR_ERR_COUNT_          /**< not a reason: how many there are */
} char_err_t;

/** Phases, as the status and the log number them. 5 is the plan's 4b. */
#define CHAR_PHASE_SPEED     1u
#define CHAR_PHASE_REVERSAL  2u
#define CHAR_PHASE_MINMOVE   3u
#define CHAR_PHASE_WP02      4u
#define CHAR_PHASE_BAND      5u

/** The rest between motor starts, entered on the card per run (plan §5e,
 *  decision 3; the default and bounds are the design's, confirmed 2026-10-03). */
#define CHAR_REST_MIN_S      2u
#define CHAR_REST_MAX_S      300u
#define CHAR_REST_DEFAULT_S  30u

/** Everything the commissioning card renders about a run in progress. */
typedef struct {
    char_state_t state;
    char_err_t   reason;
    uint8_t      phase;       /**< CHAR_PHASE_*; 0 before the first */
    uint8_t      round;       /**< phase 4b's round, 1-based; 0 outside it */
    uint16_t     band_mm;     /**< the band phase 4b is checking; 0 outside it */
    uint16_t     rest_s;
    uint16_t     starts;      /**< motor starts made so far */
    uint16_t     starts_est;  /**< about how many a whole run makes */
    uint32_t     elapsed_s;
    uint32_t     eta_s;       /**< about how long is left; 0 when not running */
} characterise_status_t;

/** @brief Snapshot for the GUI. Safe from any task. */
void characterise_status(characterise_status_t *out);

/**
 * @brief What a run costs, for the card before Start: about how many motor
 *        starts, and about how long M3 runs in all, from `travel_m3`.
 *        The duration is then about starts x rest + running.
 */
void characterise_estimate(uint16_t *starts, uint32_t *run_s);

/**
 * @brief Start a run. **This moves the window**, for 1-3 h on a production
 *        window at the default rest.
 *
 * Refused unless the sensor is fitted, taught (calibration VALID), trusted
 * and answering, M3 is at rest, no teach is running, and neither the wind
 * override, a motor alarm nor a recalibration is active. Once the refusals
 * have passed it holds STANDBY as the teach does.
 *
 * @param owner_token the starting web session (the hold waits for it to end)
 * @param rest_s      the rest between motor starts, CHAR_REST_MIN_S..MAX_S
 * @param why         set to the refusal; may be NULL
 * @return true when the run started
 */
bool characterise_start(const char *owner_token, uint16_t rest_s, char_err_t *why);

/** @brief Ask the running run to end (reason OPERATOR). Safe from any task. */
void characterise_abort(void);

/** @brief True while a run is starting or running. Safe from any task. */
bool characterise_active(void);

/** @brief True while the run needs T17 to read at rest on every idle tick. */
bool characterise_wants_prompt_read(void);

/** @brief Every pass of T17's loop: the rest timer, T2's state, the guards. T17 only. */
void characterise_tick(uint32_t now_ms);

/**
 * @brief Every reading T17 takes, at rest or in a stroke. T17 only.
 * @param r NULL when T17 has no sensor to read (not fitted, or its gate is
 *          shut): a running run then ends with CHAR_ERR_SENSOR.
 */
void characterise_reading(const windowpos_reading_t *r, uint32_t now_ms);

/** @brief The reason's short name for the route and the log; never NULL. */
const char *characterise_err_name(char_err_t e);

#ifdef __cplusplus
}
#endif

#endif /* CHARACTERISE_H */

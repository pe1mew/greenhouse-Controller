/**
 * @file commission.h
 * @brief M3 commissioning: teach the wire sensor, and judge the result (§6.3 item 4).
 *
 * ## What the teach is for
 *
 * The wire sensor reads a potentiometer. On its own that is a raw ADC code with
 * no physical meaning. The **teach maps that code onto a known distance**: the
 * operator measures the gap between the two **end sensors** and writes it to
 * `40004` ("full travel between calibration points", 0.1 mm), then the device
 * captures the raw code at each end-sensor transition during a full traverse.
 * Code and millimetres are correlated from then on.
 *
 * So a completed teach is **self-consistent by construction** — the distance was
 * supplied, the endpoints were observed, the mapping follows. There is nothing
 * in it for an admin to have an opinion about, and this module deliberately does
 * **not** ask for one. What it does instead is publish a
 * @ref commission_status_t::verdict that says whether the calibration on the
 * device looks sound, so a re-teach happens when there is a reason rather than
 * on a schedule.
 *
 * *(An earlier draft of this file timed the traverse and asked the operator to
 * accept the measured seconds. That solved a problem this project does not have:
 * the teach is a calibration, not a measurement awaiting judgement.)*
 *
 * ## Two distances, and they are not the same distance
 *
 * | | what it spans | where it lives |
 * |---|---|---|
 * | **travel time** | **end switch to end switch** — the full motor run, *past* the end sensors and into the blind overlap | `motor/travel_m3`, what T2 drives |
 * | **window size** | **end sensor to end sensor** — the 0…100 % range | device register `40004` |
 *
 * Confusing them is easy and expensive: the motor deliberately overdrives past
 * the end sensors, so the travel time is always the larger of the two.
 *
 * ## Ordering the teach requires (device contract §6.2)
 *
 *  1. `40002` measurement window **first** — `30005` refreshes once per window,
 *     and a stale capture is ~86 counts of silent calibration error at rig speed;
 *  2. `40004` = the measured window size;
 *  3. arm `40007`;
 *  4. **a full traverse with movement** — the capture register is chosen by
 *     direction of travel, so a stationary teach is meaningless;
 *  5. read the captures, which **commits** them to `40005`/`40006` and clears
 *     the teach. That read is not an inspection; it is the commit.
 *
 * Dev builds only, alongside the rest of the bench commissioning surface.
 */
#ifndef COMMISSION_H
#define COMMISSION_H

#ifdef MODBUS_BENCH

#include <stdbool.h>
#include <stdint.h>

#include "window_pos.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Is the calibration on the device usable? */
typedef enum {
    CAL_UNKNOWN = 0,   /**< device not answering, so nothing can be said */
    CAL_VALID,         /**< every check below passed */
    CAL_INVALID,       /**< see @ref commission_status_t::cal_reason */
} cal_verdict_t;

/** @brief Why a calibration was judged unusable. */
typedef enum {
    CAL_ERR_NONE = 0,
    CAL_ERR_NO_DEVICE,     /**< the sensor did not answer */
    CAL_ERR_NO_WINDOW_SIZE,/**< `40004` is zero — nothing was ever taught */
    CAL_ERR_NOT_TAUGHT,    /**< the two captures are equal: no traverse was seen */
    CAL_ERR_SPAN_NARROW,   /**< captures too close together to be a real traverse */
    CAL_ERR_TEACH_ARMED,   /**< bit 5 still set — a teach is in progress, not finished */
    CAL_ERR_WIPER_OPEN,    /**< bit 2 — the wiper circuit is open */
    CAL_ERR_IMPLAUSIBLE,   /**< bit 6 — raw code outside the calibrated band */
    CAL_ERR_NOT_FOLLOWING, /**< bit 7 — switches saw movement, position did not */
    /**
     * A teach has JUST committed and the sensor has not yet finished storing it.
     * Appended, never inserted: the web server maps these to strings by index.
     *
     * Reading the captures is the commit, but the sensor clears bit 5 and
     * persists 40005/40006 a moment AFTER that read. Measured on FDA4
     * 2026-09-16: a fresh read immediately after the commit still showed bit 5
     * set and the PREVIOUS capture (848 where the teach had taken 850). Judging
     * at that instant is therefore always wrong, and it was: every teach ended
     * "teach complete" beside "INVALID: teach still armed". So the commit shows
     * this instead, and the real verdict follows on the first reading with bit
     * 5 clear -- or at VERIFY_TIMEOUT_MS, which is what a REFUSED teach (bit 5
     * stays set) needs in order to surface as INVALID rather than wait forever.
     */
    CAL_ERR_VERIFYING,
} cal_err_t;

/** @brief Where a teach run is. */
typedef enum {
    TEACH_IDLE = 0,
    TEACH_ARMING,      /**< writing 40002 / 40004 / 40007 */
    TEACH_TRAVERSING,  /**< M3 commanded; waiting for both end sensors */
    TEACH_COMMITTING,  /**< reading the captures, which persists them */
    TEACH_DONE,
    TEACH_FAILED,
} teach_state_t;

/** @brief Why a teach run aborted. */
typedef enum {
    TEACH_ERR_NONE = 0,
    TEACH_ERR_NOT_AT_END,   /**< refused: M3 was not parked on an end sensor */
    TEACH_ERR_BOTH_ENDS,    /**< bit 4: bit 3 cannot say which end M3 is at */
    TEACH_ERR_SENSOR,       /**< the sensor faulted or stopped answering */
    TEACH_ERR_WIND,         /**< wind override intervened */
    TEACH_ERR_MOTOR_ALARM,  /**< RRK-3 motor alarm */
    TEACH_ERR_TIMEOUT,      /**< no end sensor within twice the configured travel */
    TEACH_ERR_DEVICE_WRITE, /**< a register write was refused */
} teach_err_t;

/** @brief Everything the commissioning screen renders. */
typedef struct {
    /* --- calibration, read from the device --- */
    cal_verdict_t verdict;
    cal_err_t     cal_reason;
    uint16_t      window_mm;      /**< `40004` converted to whole mm */
    uint16_t      taught_closed;  /**< `40005` raw code at the closed end */
    uint16_t      taught_open;    /**< `40006` raw code at the open end */
    uint16_t      span;           /**< |open − closed|, raw counts */
    uint8_t       span_pct;       /**< span as a percentage of the 1023 range */
    bool          teach_armed;    /**< bit 5, straight from the device */

    /* --- a teach run in progress --- */
    teach_state_t state;
    teach_err_t   run_reason;
    bool          dir_is_open;    /**< direction currently commanded */
} commission_status_t;

/** @brief Snapshot for the GUI. Safe from any task. */
void commission_status(commission_status_t *out);

/**
 * @brief Write the measured window size to the device (`40004`).
 *
 * This is the **end-sensor to end-sensor** distance, not the travel time.
 * Must be set before a teach: it is the known quantity the raw codes are
 * correlated against.
 *
 * @param mm whole millimetres, 100…5000.
 * @return false if out of range or the write failed.
 */
bool commission_set_window_mm(uint16_t mm);

/**
 * @brief Start a teach: arm the device, then drive M3 through a full traverse.
 *
 * **This moves the window.** Refused unless M3 is parked on an end sensor with
 * bit 4 clear — the capture register is chosen by direction of travel, so a
 * teach must start from a known end and cross to the other.
 *
 * @return false if refused; the reason is in the status.
 */
bool commission_teach_start(void);

/** @brief Abandon a teach run and abort the device's teach. */
void commission_teach_abort(void);

/** @brief Re-read the calibration from the device and re-judge it. */
void commission_refresh(void);

/**
 * @brief True while a just-committed teach is waiting for the sensor to finish
 *        storing it (@ref CAL_ERR_VERIFYING).
 *
 * T17 samples at rest only every 30 s, so without this the "verifying" state
 * would sit on screen for up to half a minute after the sensor had already
 * finished. While this is true T17 samples on every idle tick instead, which
 * resolves the verdict within about half a second of bit 5 clearing. It is true
 * only for those few seconds, so the extra bus reads are negligible.
 */
bool commission_wants_prompt_read(void);

/**
 * @brief Feed one sensor reading to the teach runner. Called from T17 only.
 *
 * @param r      the reading just taken, or NULL if the read failed.
 * @param now_ms monotonic milliseconds.
 */
void commission_tick(const windowpos_reading_t *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_BENCH */
#endif /* COMMISSION_H */

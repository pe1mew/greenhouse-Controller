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
 * captures the raw code as each end sensor makes while M3 is driven to both ends.
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
 * ## How a teach runs (device contract §6.2, §6.3)
 *
 * The device captures the raw code **at the moment an end sensor makes** -- its
 * inactive-to-active transition -- and only while armed. Arming discards any
 * earlier capture. It commits once BOTH ends are captured and both capture
 * registers have been read since, and it shows the commit by **clearing bit 5**.
 *
 *  1. `40002` measurement window **first** -- `30005` refreshes once per window,
 *     and a stale capture is ~86 counts of silent calibration error at rig speed;
 *  2. arm `40007`, then wait for a reading with bit 5 set: the window does not
 *     move until the device has confirmed it is listening;
 *  3. **drive M3 to both end sensors, one after the other, from wherever it
 *     starts.** Each leg goes the opposite way to where T2 believes M3 is,
 *     because T2 ignores a command to go where it thinks M3 already is. The
 *     run needs two end sensors to MAKE, and a leaf parked on an end cannot
 *     make that end's sensor without leaving it first -- so a teach from an end
 *     is always two traverses, and three when T2's belief is wrong (the first
 *     leg then runs into the end switch and moves nothing);
 *  4. nothing extra to commit. T17's ordinary poll is the full 15-register map,
 *     which includes `30013`/`30014`, so it satisfies the read handshake on its
 *     own (§6.3: "so does the 15-register full-map read").
 *
 * **Where M3 starts, and which way it goes first, do not affect the result.**
 *
 * *History, 2026-09-16.* The first version drove ONE traverse from a parked end
 * and then read `30013`/`30014`, believing that read was the commit. It could
 * only ever capture the far end, so the device never committed on its own
 * account. From OPEN it appeared to work, because T6 reopened the window
 * afterwards and made the open sensor by chance; from CLOSED, T6 was held off
 * by dwell and the teach was disarmed before T6 moved. Success depended on T6,
 * not on the teach. The "persist race" diagnosed the same day, and the
 * VERIFYING state added for it, were built on that misreading and are gone.
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
    CAL_ERR_COUNT_         /**< not a reason: how many there are. The web server's
                            *   string table is checked against it at compile time. */
} cal_err_t;

/** @brief Where a teach run is. */
typedef enum {
    TEACH_IDLE = 0,
    TEACH_ARMING,      /**< 40007 written; waiting for a reading with bit 5 set */
    TEACH_TRAVERSING,  /**< driving legs; fewer than two end sensors have made */
    TEACH_COMMITTING,  /**< both ends captured; waiting for the device to clear bit 5 */
    TEACH_DONE,
    TEACH_FAILED,
    TEACH_STATE_COUNT_ /**< not a state: how many there are */
} teach_state_t;

/** @brief Why a teach run aborted. */
typedef enum {
    TEACH_ERR_NONE = 0,
    TEACH_ERR_M3_BUSY,      /**< M3 was moving at the start, or something else moved it mid-teach */
    TEACH_ERR_BOTH_ENDS,    /**< bit 4: the end-sensor loop is faulted, bit 3 cannot be believed */
    TEACH_ERR_SENSOR,       /**< the sensor faulted or stopped answering */
    TEACH_ERR_WIND,         /**< wind override intervened */
    TEACH_ERR_MOTOR_ALARM,  /**< RRK-3 motor alarm */
    TEACH_ERR_TIMEOUT,      /**< a leg outlasted twice T2's own stroke time */
    TEACH_ERR_DEVICE_WRITE, /**< a register write failed, or bit 5 never appeared after arming */
    TEACH_ERR_NO_START,     /**< T2 did not start the commanded stroke */
    TEACH_ERR_NO_MOVE,      /**< M3 did not leave its end sensor in EITHER direction */
    TEACH_ERR_END_MISSED,   /**< a stroke ended between the end sensors -- travel_m3 too short? */
    TEACH_ERR_REFUSED,      /**< both ends made but bit 5 stayed set: captures < 64 counts apart */
    TEACH_ERR_DROPPED,      /**< bit 5 cleared before both ends had made -- sensor restarted? */
    TEACH_ERR_COUNT_        /**< not a reason: how many there are */
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
    bool          dir_is_open;    /**< direction of the leg being driven */
    uint8_t       leg;            /**< 1-based leg being driven; 0 before the first */
    uint8_t       ends_made;      /**< end sensors that have made since arming, 0..2 */

    /* --- automatic control --- */
    bool          standby_held;   /**< a teach holds STANDBY until its admin session ends */
} commission_status_t;

/** Legs a teach may drive: two, plus one for a T2 belief that turns out wrong. */
#define COMMISSION_TEACH_MAX_LEGS  3u

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
 * @brief Start a teach: arm the device; T17 then drives M3 to both end sensors.
 *
 * **This moves the window** -- two full traverses, three when T2's idea of
 * where M3 is turns out to be wrong. M3 may start anywhere: open, closed or
 * part-way. Refused while M3 is moving, while bit 4 is set, or when a fresh
 * reading fails or reports a fault.
 *
 * **Automatic control pauses** (operator decision, 2026-09-16). Once the
 * refusals have passed, the teach HOLDS STANDBY (dm_standby_hold()) until the
 * admin session that started it has ended -- logout, idle timeout, eviction or
 * reboot -- and no teach is running any more. That is the rule the LCD applies
 * to manual window control, except that a hold is never persisted, so a reboot
 * cannot leave the unit paused (gh#65). A STANDBY the operator had already set
 * is theirs: the teach does not hold it and never releases it.
 *
 * @param owner_token  the starting web session's token; NULL or empty means no
 *                     session to wait for, so the hold ends with the teach.
 * @return false if refused; the reason is in the status.
 */
bool commission_teach_start(const char *owner_token);

/**
 * @brief A web session has been logged out (T11). If it owns the teach's
 *        STANDBY hold, release it now -- or when the running teach ends.
 */
void commission_session_ended(const char *token);

/** @brief Abandon a teach run and abort the device's teach. */
void commission_teach_abort(void);

/** @brief Re-read the calibration from the device and re-judge it. */
void commission_refresh(void);

/**
 * @brief True while an armed teach on the device is this module's doing.
 *
 * That is: a run is active, OR the arm write is in flight before the run is
 * published, OR we disarmed within the last few seconds and bit 5 may not have
 * cleared yet. T17 uses it to tell our teach from an ORPHAN -- a device armed
 * with nothing here behind it, which T17 aborts (window_pos_task.cpp).
 */
bool commission_owns_teach(void);

/**
 * @brief True while a teach is running, and for a few readings after it ends.
 *
 * T17 samples at rest only every 30 s. A teach needs readings at rest too: to
 * see bit 5 appear before the first leg, to start the next leg when a stroke
 * ends, and to see bit 5 clear. While this is true T17 samples on every idle
 * tick (500 ms) instead. The few readings after the end let the stored verdict
 * catch up with the device's bit 5, which clears a moment after a disarm.
 */
bool commission_wants_prompt_read(void);

/**
 * @brief Feed one sensor reading to the teach runner. Called from T17 only.
 *
 * This is where the legs are commanded, so T17 is a Q1 producer on bench
 * builds while a teach runs. It is also where a STANDBY hold is released once
 * its session has ended -- so at rest that happens within T17's 30 s cadence.
 *
 * @param r      the reading just taken, or NULL when T17 has no sensor to read
 *               (its presence gate is shut) -- a running teach then fails.
 * @param now_ms monotonic milliseconds, T17's clock.
 */
void commission_tick(const windowpos_reading_t *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_BENCH */
#endif /* COMMISSION_H */

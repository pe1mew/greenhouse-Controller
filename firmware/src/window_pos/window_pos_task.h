/**
 * @file window_pos_task.h
 * @brief T17 — window position task: polls the wire encoder while M3 travels.
 *
 * Phase 2 of `design/integrateWindowPositionSensor.md`. Owns the **derived**
 * configuration from §3: nothing here is a hardcoded rate constant, because
 * the rig runs ~13× faster than production and a fixed number would be silently
 * production-only.
 *
 * ## Why a task of its own (§4)
 *
 * T5 keeps its exact 30 s rhythm. Slicing T5's sleep to poll at ~100 ms would
 * corrupt the one task that must hold a fixed cadence, and its averaging
 * windows are sample-counted. A second bus caller is **only safe because 2.4.1
 * shipped the Modbus mutex** (gh#49) — before that it was a data race.
 *
 * ## Priority: 4, deliberately below T2 and T3 (both 6)
 *
 * The mutex buys correctness, not permission. A Modbus transaction can hold the
 * bus ~215 ms, and that must never sit in front of a wind override. Priority 4
 * also puts it below T4/T5/T6 (5): in phases 2–3 this task is a **read-only
 * observer**, so it must never delay climate or safety work. Revisit only when
 * it becomes a control input (Phase 5, out of scope).
 *
 * ## Runs only while a channel is travelling
 *
 * Idle otherwise — no bus cost when nothing moves. That matters: at ~100 ms this
 * task would otherwise be the heaviest user of a bus shared with the T/RH and
 * wind sensors.
 *
 * ## What this task does NOT do
 *
 * It still changes no control directly. T2 stops on its timer; T6 steps on
 * temperature. This task observes, publishes a snapshot, and **publishes which
 * control law is admissible** (@ref windowpos_task_ctrl_mode). Acting on that
 * is Phase 4 (travel-complete) and Phase 5 (proportional); Phase 5 is out of
 * scope for this cycle.
 *
 * ## The sensor-presence gate (Phase 4)
 *
 * Before the gate, all three startup branches logged and then fell through into
 * the polling loop: "T17 idle" did not idle and "REFUSING" did not refuse. On a
 * unit with no sensor at addr 40 that cost a failed ~215 ms transaction per
 * poll, and because the derived poll floor is 100 ms, on the 13 s dev rig the
 * period is **shorter than the timeout** — so the bus was held continuously for
 * the whole stroke, against T5 whose receive loop never yields.
 *
 * The gate follows T5's house pattern deliberately (`sensor_poll.cpp`):
 * **two consecutive failures** flip the state, recovery is on the **first**
 * success, and the transition is **edge-logged**. What differs is the
 * consequence: T5 reports a fault and keeps polling, because a missing T/RH or
 * wind sensor is a fault the operator must see and because T3 safe-fails on
 * `EG1_BIT_SENSOR_FAULT_W` — the fault bit *is* the feature. Position is
 * optional, so here the consequence is to stop touching the bus and fall back.
 *
 * **In a release build the audit log is the only window onto this.** Every
 * T17 HTTP field, these counters included, lives under @c #ifdef MODBUS_BENCH,
 * so on a production unit the LOG_PARAM_WPOS_MODE row is the whole story --
 * which is why it is logged on every transition and why its `value_b` carries
 * the reason rather than just the fact.
 *
 * A shut gate re-probes every @c PROBE_RETRY_MS so a sensor connected
 * mid-session recovers by itself. That is one failed transaction per 30 s,
 * which is exactly what the idle path already spent. The bench build is the one
 * permanent latch: it cannot change without reflashing the device.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "window_pos.h"

/**
 * @brief Which control law M3 is under.
 *
 * Phase 4. The sensor-presence gate exists to answer this, not merely to save
 * bus time: with a trustworthy position the window can be driven to an opening
 * **distance**; without one the controller must fall back to T2's **travel
 * timer**, which is what it has always done and remains the proven behaviour.
 *
 * **TIMED is the safe default and the failure direction.** It is what ships on
 * `main`, so falling back costs nothing that was ever guaranteed. Anything that
 * makes position untrustworthy — absent sensor, comms gone, a device-reported
 * fault, a bench build — selects TIMED.
 */
typedef enum {
    WPOS_CTRL_TIMED    = 0, /**< Fallback: T2's travel timer. Today's behaviour. */
    WPOS_CTRL_POSITION = 1, /**< Position available and trusted; drive to distance. */
} windowpos_ctrl_mode_t;

/**
 * @brief Why the gate is in the state it is in.
 *
 * Carried in `value_b` of the LOG_PARAM_WPOS_MODE audit row, so a log answers
 * *why* M3 fell back and not just *that* it did.
 */
typedef enum {
    WPOS_GATE_OK           = 0, /**< Sensor present, identified, reading. */
    WPOS_GATE_PROBING      = 1, /**< Boot: no verdict yet. */
    WPOS_GATE_NO_SENSOR    = 2, /**< Ident/read failed PROBE_FAIL_LIMIT times running. */
    WPOS_GATE_BENCH_BUILD  = 3, /**< Contract 9 refusal. Latched; never re-probed. */
    WPOS_GATE_DEVICE_FAULT = 4, /**< Present and talking, but reporting a fault. */
} windowpos_gate_reason_t;

/**
 * @brief The control law currently in force for M3, and why.
 *
 * Safe to call from any task; takes no locks the caller can observe.
 *
 * @param out_reason  May be NULL. Receives the current gate reason.
 * @return @ref WPOS_CTRL_TIMED or @ref WPOS_CTRL_POSITION.
 *
 * @note **Demotion to TIMED is immediate; promotion to POSITION happens only at
 *       a stroke boundary.** The asymmetry is deliberate: dropping to the timer
 *       mid-stroke is safe because the timer is what would have run anyway,
 *       whereas switching *to* position control underneath a consumer that has
 *       already committed to a timed stroke is not. A caller therefore never
 *       sees the mode gain authority in the middle of a movement.
 */
windowpos_ctrl_mode_t windowpos_task_ctrl_mode(windowpos_gate_reason_t *out_reason);

/**
 * @brief Derived polling configuration, recomputed at the start of every stroke.
 *
 * Recomputing per stroke (rather than at boot) means a `travel_m3` change is
 * picked up without any notification wiring, and costs one integer divide per
 * stroke.
 */
typedef struct {
    uint32_t travel_ms;        /**< `travel_m3` × 1000, the value everything derives from. */
    uint32_t poll_ms;          /**< Poll interval. */
    uint16_t window_ms;        /**< Pushed to the device as `40002`. */
    uint16_t nominal_rate_x10; /**< Full travel / travel time, 0.1 mm/s. */
    uint16_t rate_limit_x10;   /**< FR-WP20 reject threshold: 2 × nominal. */
} windowpos_derived_t;

/**
 * @brief Soak counters (AT-WP05).
 *
 * `busy` is the one that matters: this task shares the bus with T5, and the
 * acceptance test asks whether a second caller costs the first anything.
 */
typedef struct {
    uint32_t reads_ok;      /**< Successful reads. */
    uint32_t err_busy;      /**< Bus lock not acquired -- lock contention with T5. */
    uint32_t err_comm;      /**< Timeout / CRC / exception. */
    uint32_t rejected_rate; /**< Samples dropped by the FR-WP20 plausibility check. */
    uint32_t strokes;       /**< Strokes observed since boot. */
    uint32_t probe_fail;    /**< Consecutive-failure probes that closed the gate. */
    uint32_t mode_changes;  /**< TIMED/POSITION transitions since boot. */
    uint32_t gated_polls;   /**< Ticks with a stroke in progress in which the
                             *   shut gate suppressed a poll. Not a count of
                             *   idle samples, which are negligible. */
} windowpos_counters_t;

/** @brief Copy the soak counters. @param out Destination, must not be NULL. */
void windowpos_task_counters(windowpos_counters_t *out);

/**
 * @brief Latest reading plus its age.
 *
 * @param out     Destination reading. May be NULL if only @p out_age_ms is wanted.
 * @param out_age_ms  Milliseconds since the reading was taken. May be NULL.
 * @return false if no successful read has happened yet — in which case @p out is
 *         untouched and must not be used. **A stale-but-present reading returns
 *         true**, so callers that care about freshness must check the age; the
 *         task stops polling entirely while the window is at rest, so ages of
 *         many minutes are normal and are not a fault.
 */
bool windowpos_task_snapshot(windowpos_reading_t *out, uint32_t *out_age_ms);

/**
 * @brief The derived configuration currently in force.
 * @param out Destination. Must not be NULL.
 * @return false before the first stroke has derived them.
 */
bool windowpos_task_derived(windowpos_derived_t *out);

/**
 * @brief T17 task entry point. Never returns.
 * @param pvParameters Unused; pass NULL.
 */
void task_window_pos(void *pvParameters);

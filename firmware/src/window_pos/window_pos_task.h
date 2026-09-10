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
 * ## What Phase 2 does NOT do
 *
 * Nothing here changes control. T2 still stops on its timer; T6 still steps on
 * temperature. This task observes and publishes a snapshot. Acting on position
 * is Phase 4 (travel-complete) and Phase 5 (proportional), and Phase 5 is out of
 * scope for this cycle.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "window_pos.h"

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

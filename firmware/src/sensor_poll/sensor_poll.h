/**
 * @file sensor_poll.h
 * @brief T5 — Sensor Poll task declaration.
 *
 * Modbus RTU master for the FG6485A (T/RH) and S200 (wind) sensors.
 * Polls at the configured interval (30–3600 s, default 60 s), maintains
 * sliding averages, posts sensor_reading_t to Q6, and posts LOG_SENSOR
 * events to Q3 on every poll (FR-LG09: snapshot = poll).
 *
 * ## Fault detection
 *  Each sensor is read with one immediate retry on failure.  After two
 *  consecutive failures the appropriate EG1 bit is set edge-triggered:
 *    - EG1_BIT_SENSOR_FAULT_T — FG6485A (temperature / relative humidity)
 *    - EG1_BIT_SENSOR_FAULT_W — S200 (wind speed / direction)
 *  The bit is cleared and a LOG_ALARM clearance event is posted on the
 *  first successful read following a fault.
 *
 * ## Sliding average
 *  Window size (samples) = avg_win_x_min × 60 / poll_interval_s,
 *  clamped [1, 360].  T and wind share the same window (avg_win_t);
 *  RH uses avg_win_rh.  If the window size changes (config update) the
 *  context is reset and re-warms over the next N poll cycles.
 *
 *  Wind direction is averaged using unit-vector decomposition (atan2) to
 *  handle the 0°/360° wrap correctly.
 *
 * ## Design references
 *  - firmwareImplementationPlan.md §Phase 3
 *  - design/tasks.md T5
 *  - design/technicalSoftwareDesignSpecification.md §5.x T5
 *  - FRS FR-SE01–FR-SE04, FR-LG09
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T5 — Sensor Poll task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_sensor_poll(void *pvParameters);

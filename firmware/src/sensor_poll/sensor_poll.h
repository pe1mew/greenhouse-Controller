/**
 * @file sensor_poll.h
 * @brief T5 — Sensor Poll task declaration.
 *
 * Modbus RTU master for the FG6485A (T/RH) and S200 (wind) sensors.
 * Polls at the configured interval (30–3600 s, default 60 s), maintains
 * sliding averages, posts sensor_reading_t to Q6, and posts LOG_SENSOR
 * events to Q3 on every poll (FR-LG09: snapshot = poll).
 *
 * ## Subsystem ownership
 *  - **Reads**: Modbus RTU bus (UART1, RS485 transceiver) — exclusive
 *    owner; no other task drives the bus.
 *  - **Writes**: Q6 (`xQueueOverwrite`, depth-1) consumed by T4 data
 *    manager; EG1 sensor-fault bits set/cleared edge-triggered; Q3 via
 *    `log_post()` for fault transitions only (LOG_SENSOR is emitted by
 *    T4 on Q6 receipt, not by T5 — avoids duplicate snapshots).
 *  - **Reads-only** config snapshot via `dm_cfg_snapshot()` each cycle.
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
 * @see  data_manager.h (Q6 consumer, dm_cfg_snapshot, dm_get_poll_interval_s)
 * @see  event_logger.h (log_post, LOG_ALARM encoding)
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T5 — Sensor Poll task entry point.
 *
 * Spawned once from main.cpp at boot. Loops forever: sleep
 * `cfg.poll_interval_s` seconds, refresh config, poll both Modbus
 * sensors with one retry each, update sliding averages, build a
 * `sensor_reading_t`, and `xQueueOverwrite(Q6, ...)`. Sensor faults are
 * reported via EG1 bits and LOG_ALARM events (one onset, one clearance);
 * intermediate failed polls are silent.
 *
 * @param pvParameters  Unused; pass NULL.
 * @note   Includes an 8 s boot-grace `vTaskDelay` so USB-CDC re-enumerates
 *         before the first log line; do not shorten without verifying
 *         early-boot logs are still visible on Windows hosts.
 * @warning Holds no mutex while polling — concurrent Modbus access from
 *          any other task would corrupt the bus. T5 is the sole driver.
 * @see    task_data_manager (Q6 consumer, emits LOG_SENSOR on receipt)
 */
void task_sensor_poll(void *pvParameters);

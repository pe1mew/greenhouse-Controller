/**
 * @file safety_monitor.h
 * @brief T3 — Safety Monitor task declaration.
 *
 * Evaluates wind speed and direction against configured thresholds on every
 * TN1 notification from T4 (new wind data available).
 *
 * ## Behaviour summary
 *  - Reads current wind measurement from T4 via dm_meas_snapshot() (MX2).
 *  - Reads configuration from T4 via dm_cfg_snapshot() (MX4).
 *  - If wind_prot_en is false: clears EG1.WIND_OVERRIDE if set and skips.
 *  - If EG1.SENSOR_FAULT_W is set: safe-fail — treats wind as threshold
 *    exceeded and issues CMD_CLOSE_ALL.
 *  - Speed check: wind_speed_avg_ms10 >= v_max × 10.
 *  - Direction check: wind_dir_avg_deg within [dir_excl_low, dir_excl_high]
 *    on the 0–359° circle (wraps through 0° when dir_excl_low > dir_excl_high).
 *  - On unsafe onset: sets EG1.WIND_OVERRIDE, posts CMD_CLOSE_ALL (SRC_T3)
 *    to Q1, posts LOG_ALARM to Q3.
 *  - On safe clearance: clears EG1.WIND_OVERRIDE, posts CMD_RESUME (SRC_T3)
 *    to Q1, posts LOG_ALARM to Q3.
 *  - MOTOR_ALARM interaction: T3 evaluates normally; T2 discards Q1 commands
 *    while EG1.MOTOR_ALARM is set.
 *
 * ## Log events posted to Q3
 *  | Condition                   | event_type | value_a                     | value_b                |
 *  |-----------------------------|------------|-----------------------------|------------------------|
 *  | Onset — speed exceeded (W1) | LOG_ALARM  | wind_speed_avg_ms10         | v_max × 10             |
 *  | Onset — dir excluded   (W2) | LOG_ALARM  | wind_dir_avg_deg            | dir_excl_low           |
 *  | Onset — sensor fault        | LOG_ALARM  | −1 (fault indicator)        | 0                      |
 *  | Clearance             (W3)  | LOG_ALARM  | wind_speed_avg_ms10 (or 0)  | wind_dir_avg_deg (or 0)|
 *  | Disabled-while-active       | LOG_ALARM  | 0                           | 0                      |
 *
 * ## Design references
 *  - firmwareImplementationPlan.md §Phase 4
 *  - design/tasks.md T3
 *  - design/technicalSoftwareDesignSpecification.md §T3
 *  - FRS §5.4 FR-WS01–FR-WS11
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T3 — Safety Monitor task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_safety_monitor(void *pvParameters);

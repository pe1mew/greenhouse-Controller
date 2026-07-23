/**
 * @file safety_monitor.h
 * @brief T3 — Safety Monitor task declaration.
 *
 * Evaluates wind speed and direction against configured thresholds on every
 * TN1 notification from T4 (new wind data available).
 *
 * ## Task-graph slot
 *  - TN1 (consumer)   — task-notification from T4 on every sensor update.
 *  - Q1 (producer)    — CMD_CLOSE_ALL / CMD_RESUME with source=SRC_T3.
 *  - Q3 (producer)    — LOG_ALARM via log_post() on every transition.
 *  - EG1 (set/clear)  — owns EG1_BIT_WIND_OVERRIDE; reads
 *                       EG1_BIT_SENSOR_FAULT_W and EG1_BIT_MOTOR_ALARM.
 *  - MX2 (read)       — measurement snapshot via dm_meas_snapshot().
 *  - MX4 (read)       — config snapshot via dm_cfg_snapshot().
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
 *
 * Since 2.3.0 (gh#45) every wind row stamps its subtype into `param`
 * (LOG_PARAM_ALARM_WIND_*, band 240–243) so consumers decode the type
 * directly. Pre-2.3.0 rows carry param = LOG_PARAM_NONE and must be
 * decoded by the legacy value-magnitude heuristic.
 *
 *  | Condition                   | event_type | param                       | value_a                     | value_b                |
 *  |-----------------------------|------------|-----------------------------|-----------------------------|------------------------|
 *  | Onset — speed exceeded (W1) | LOG_ALARM  | ALARM_WIND_SPEED (240)      | wind_speed_avg_ms10         | v_max × 10             |
 *  | Onset — dir excluded   (W2) | LOG_ALARM  | ALARM_WIND_DIR   (241)      | wind_dir_avg_deg            | dir_excl_low           |
 *  | Onset — sensor fault        | LOG_ALARM  | ALARM_WIND_FAULT (243)      | −1 (fault indicator)        | 0                      |
 *  | Clearance             (W3)  | LOG_ALARM  | ALARM_WIND_CLEAR (242)      | wind_speed_avg_ms10 (or 0)  | wind_dir_avg_deg (or 0)|
 *  | Disabled-while-active       | LOG_ALARM  | ALARM_WIND_CLEAR (242)      | 0                           | 0                      |
 *
 * (T2 motor-alarm rows keep param = LOG_PARAM_NONE, so on 2.3.0+ firmware a
 * (0,0) ALARM row is unambiguous: param 242 = wind clear, NONE = motor.)
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
 *
 * Subscribes to the FreeRTOS task watchdog and runs an event-driven loop:
 * blocks on TN1 (with a 2 s timeout that doubles as a WDT-kick heartbeat),
 * then evaluates wind conditions and drives the wind-override state machine
 * (see file header).
 *
 * @param  pvParameters  Unused; pass NULL.
 * @warning Safety-critical.  A T3 hang would silently disable wind
 *          protection — the 2 s timeout exists specifically so the task WDT
 *          can detect a stuck loop.
 * @see    task_relay_controller() (Q1 consumer), task_data_manager() (TN1
 *         source).
 */
void task_safety_monitor(void *pvParameters);

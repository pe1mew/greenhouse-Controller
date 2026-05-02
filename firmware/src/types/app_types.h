/**
 * @file app_types.h
 * @brief Shared compile-time constants, struct types, enum types, and
 *        FreeRTOS handle declarations used across all firmware tasks.
 *
 * This is the single header every firmware module includes for inter-task
 * communication types and system-wide constants.  It must compile cleanly
 * on both the ESP32-S3 target and a host-side PlatformIO test runner.
 *
 * ## Sections
 *  1. Factory-default hardware constants — NVS-backed at runtime; these
 *     macros are the factory defaults written on first boot.
 *  2. FreeRTOS RTOS handle externs       — declared here, defined in main.cpp.
 *  3. Queue / message struct types       — one struct per queue.
 *  4. Enumeration types                  — FSM states, modes, roles.
 *  5. Event group bit definitions        — EG1 system-state flags.
 *
 * Sections 2–5 are stubbed with TODO comments; they will be populated
 * during Phase 0 (project scaffold) once the full type system is finalised.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 * Section 1 — Factory-default hardware constants
 * ============================================================ */

/**
 * @defgroup motor_travel Motor full-travel time — factory defaults
 *
 * These macros define the **factory-default** motor full-travel times in
 * **seconds**.  They are written to NVS on first boot (or after factory
 * reset) and can subsequently be adjusted by a technician via the web GUI
 * (FR-CF05, admin access level).
 *
 * ### Runtime behaviour
 * T2 (Relay Controller) reads the *live* travel times from T4 (MX4) —
 * i.e. the values currently held in NVS `motor/travel_mN` — not these
 * macros.  T2 converts seconds to milliseconds for `vTaskDelay`:
 * @code
 *   uint32_t travel_ms = (uint32_t)travel_s * 1000UL;
 *   vTaskDelay(pdMS_TO_TICKS(travel_ms));
 * @endcode
 *
 * T6 reads the M3 travel time from T4 (MX4) to size the calibration wait
 * after a manual override is cleared:
 * @code
 *   uint32_t cal_wait_ms = (uint32_t)travel_m3_s * 1000UL + 10000UL;
 * @endcode
 *
 * ### NVS keys (namespace `motor`)
 * | Key          | Type     | Default                    | Range (s) |
 * |--------------|----------|----------------------------|-----------|
 * | `travel_m1`  | int16_t  | MOTOR_M1_TRAVEL_S_DEFAULT  | 5 – 600   |
 * | `travel_m2`  | int16_t  | MOTOR_M2_TRAVEL_S_DEFAULT  | 5 – 600   |
 * | `travel_m3`  | int16_t  | MOTOR_M3_TRAVEL_S_DEFAULT  | 5 – 600   |
 *
 * ### Relationship to dwell time
 * Dwell time is a *separate* configurable hold period (NVS keys
 * `dwell_open_mN` / `dwell_close_mN`, unit: **minutes**).  It is the
 * minimum time T2 must wait *after* travel completes before accepting the
 * next command on that channel (FR-A09–FR-A12).  The two parameters are
 * independent and must never be confused:
 *
 * | Parameter          | What it times                          | NVS key / source          | Unit |
 * |--------------------|----------------------------------------|---------------------------|------|
 * | `travel_mN`        | Relay energisation (window in motion)  | NVS `motor/travel_mN`     | s    |
 * | `dwell_open_mN`    | Min hold at OPEN before CLOSE accepted | NVS `motor/dwell_open_mN` | min  |
 * | `dwell_close_mN`   | Min hold at CLOSED before OPEN accepted| NVS `motor/dwell_close_mN`| min  |
 *
 * Source: FRS §4.3 motor run-time table; FR-CF05; FR-A09–FR-A12.
 * @{
 */

/** M1 (window 1) factory-default full-travel time: 21 s. */
#define MOTOR_M1_TRAVEL_S_DEFAULT    21

/** M2 (window 2) factory-default full-travel time: 21 s. */
#define MOTOR_M2_TRAVEL_S_DEFAULT    21

/**
 * M3 (ridge vent) factory-default full-travel time: 171 s.
 * T6 uses the *runtime* M3 travel time from T4 (MX4) for the
 * post-manual-override calibration wait (travel_m3_s * 1000 + 10 000 ms).
 */
#define MOTOR_M3_TRAVEL_S_DEFAULT   171

/** Minimum permitted travel time (seconds) — enforced by T4 on NVS write. */
#define MOTOR_TRAVEL_S_MIN            5

/** Maximum permitted travel time (seconds) — enforced by T4 on NVS write. */
#define MOTOR_TRAVEL_S_MAX          600

/** @} */

/* -----------------------------------------------------------------------
 * Graduated ventilation — number of opening steps (FR-C09, FR-C10)
 *
 * Each step adds one more channel to the open set (compile-time table in
 * climate_control.cpp):
 *   Step 1 — M1 only
 *   Step 2 — M1 + M2
 *   Step 3 — M1 + M2 + M3
 *
 * Changing this constant requires a matching update to VENT_STEP_TABLE[]
 * in climate_control.cpp.
 * ----------------------------------------------------------------------- */
#define NUM_VENT_STEPS   3

/* ============================================================
 * Section 2 — FreeRTOS RTOS handle externs
 *
 * All handles are defined (created) in main.cpp and declared extern here
 * so every task file can reference them without passing handles as
 * function arguments.
 *
 * TODO (Phase 0): uncomment after adding FreeRTOS includes.
 * ============================================================ */

/*
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

// Queues (defined in main.cpp)
extern QueueHandle_t Q1;   // window_cmd_t      — T3/T6/T8/T11/T12 → T2
extern QueueHandle_t Q2;   // key_event_t        — T7 → T8
extern QueueHandle_t Q3;   // log_event_t        — all tasks → T9
extern QueueHandle_t Q4;   // config_update_t    — T8/T10/T11 → T4
extern QueueHandle_t Q5;   // net_status_t       — T10 → T8
extern QueueHandle_t Q6;   // sensor_reading_t   — T5 → T4 (overwrite)

// Task handles (defined in main.cpp)
extern TaskHandle_t task_t1;   // Watchdog / Heartbeat
extern TaskHandle_t task_t2;   // Relay Controller
extern TaskHandle_t task_t3;   // Safety Monitor
extern TaskHandle_t task_t4;   // Data Manager
extern TaskHandle_t task_t5;   // Sensor Poll
extern TaskHandle_t task_t6;   // Climate Control
extern TaskHandle_t task_t7;   // Keypad Scan
extern TaskHandle_t task_t8;   // UI / Display
extern TaskHandle_t task_t9;   // Event Logger
extern TaskHandle_t task_t10;  // Network Manager
extern TaskHandle_t task_t11;  // Web Server
extern TaskHandle_t task_t12;  // MQTT Client
// task_t13 (OTA) is created on demand by T11; no permanent handle

// Event group
extern EventGroupHandle_t EG1; // System state flags (see Section 5)

// Mutexes
extern SemaphoreHandle_t MX1;  // I2C bus (T4 RTC + T8 LCD)
extern SemaphoreHandle_t MX2;  // Current measurement data
extern SemaphoreHandle_t MX3;  // Measurement ring buffers
extern SemaphoreHandle_t MX4;  // Configuration settings (NVS shadow)
extern SemaphoreHandle_t MX5;  // LittleFS active partition
*/

/* ============================================================
 * Section 3 — Queue / message struct types
 *
 * TODO (Phase 0): define the following structs.
 * ============================================================ */

/*
typedef struct {
    // Q1 — actuation command
    // cmd_action_t action;   CMD_OPEN / CMD_CLOSE / CMD_CLOSE_ALL / CMD_RESUME
    // uint8_t      channel;  0 = all channels; 1 = M1; 2 = M2; 3 = M3
    // cmd_source_t source;   SRC_T3 / SRC_T6 / SRC_T8 / SRC_T11 / SRC_T12
} window_cmd_t;

typedef struct {
    // Q2 — keypad key event
    // char     key;          KP_NO_KEY or ASCII character from keypad_scan()
    // bool     repeated;     true if key-repeat generated this event
} key_event_t;

typedef struct {
    // Q3 — event log entry (see TSDS §5.3 for field descriptions)
    // uint32_t        timestamp;
    // log_type_t      event_type;
    // log_initiator_t initiator;
    // uint8_t         channel;
    // int16_t         value_a;
    // int16_t         value_b;
    // uint8_t         reserved[2];
} log_event_t;

typedef struct {
    // Q4 — NVS configuration update request
    // char     ns[16];       NVS namespace (e.g. "climate")
    // char     key[16];      NVS key (e.g. "t_max_day")
    // int32_t  value;        new value (cast to the appropriate NVS type by T4)
} config_update_t;

typedef struct {
    // Q5 — network status from T10 to T8
    // bool     client_connected;
    // bool     ap_active;
    // char     ip_str[16];   dotted-decimal IPv4 string
} net_status_t;

typedef struct {
    // Q6 — sensor reading snapshot from T5 to T4
    // int16_t  temperature_c;        raw (rounded to nearest integer)
    // uint8_t  humidity_pct;         raw
    // uint16_t wind_speed_ms10;      wind speed × 10 (e.g. 35 = 3.5 m/s)
    // uint16_t wind_dir_deg;         0–359
    // int16_t  t_avg_c;              sliding average (rounded)
    // uint8_t  rh_avg_pct;           sliding average
    // uint16_t wind_speed_avg_ms10;  sliding average × 10
    // uint16_t wind_dir_avg_deg;     sliding average
    // uint32_t timestamp;
} sensor_reading_t;
*/

/* ============================================================
 * Section 4 — Enumeration types
 *
 * TODO (Phase 0): define the following enums.
 * ============================================================ */

/*
typedef enum {
    WIN_UNKNOWN,
    WIN_CLOSED,
    WIN_MOVING_OPEN,
    WIN_OPEN,
    WIN_MOVING_CLOSE,
} window_state_t;

typedef enum {
    MODE_AUTOMATIC,
    MODE_STANDBY,
    MODE_WIND_OVERRIDE,
    MODE_MANUAL_OVERRIDE,
} op_mode_t;

typedef enum {
    SESSION_NONE,
    SESSION_FARMER,
    SESSION_ADMIN,
} session_t;

typedef enum {
    LOG_SENSOR, LOG_RELAY, LOG_MODE_CHANGE,
    LOG_SETPOINT, LOG_SESSION, LOG_ALARM, LOG_SYSTEM,
} log_type_t;

typedef enum {
    LOG_BY_SYSTEM, LOG_BY_FARMER, LOG_BY_ADMIN, LOG_BY_MQTT, LOG_BY_WEB,
} log_initiator_t;

typedef enum {
    SRC_T3, SRC_T6, SRC_T8, SRC_T11, SRC_T12,
} cmd_source_t;

typedef enum {
    CMD_OPEN, CMD_CLOSE, CMD_CLOSE_ALL, CMD_RESUME,
} cmd_action_t;
*/

/* ============================================================
 * Section 5 — Event group bit definitions (EG1)
 *
 * TODO (Phase 0): uncomment after adding FreeRTOS includes.
 * ============================================================ */

/*
#define EG1_BIT_WIND_OVERRIDE    (1 << 0)  // Set/cleared by T3
#define EG1_BIT_MANUAL_OVERRIDE  (1 << 1)  // Set by T2; cleared by T6
#define EG1_BIT_SENSOR_FAULT_T   (1 << 2)  // Set/cleared by T5
#define EG1_BIT_SENSOR_FAULT_W   (1 << 3)  // Set/cleared by T5
#define EG1_BIT_OTA_IN_PROGRESS  (1 << 4)  // Set/cleared by T13
*/

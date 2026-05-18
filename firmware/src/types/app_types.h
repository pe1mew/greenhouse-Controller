/**
 * @file app_types.h
 * @brief Shared compile-time constants, struct types, enum types, and
 *        FreeRTOS handle declarations used across all firmware tasks.
 *
 * This is the single header every firmware module includes for inter-task
 * communication types and system-wide constants.
 *
 * ## Sections
 *  1. Factory-default hardware constants — NVS-backed at runtime; these
 *     macros are the factory defaults written on first boot.
 *  2. FreeRTOS includes + RTOS handle externs — declared here, defined in main.cpp.
 *  3. Enumeration types — FSM states, modes, roles, log types.
 *  4. Queue / message struct types — one struct per queue.
 *  5. Event group bit definitions — EG1 system-state flags.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 * Section 1 — Factory-default hardware constants
 *
 * NVS factory defaults (incl. MOTOR_M*_TRAVEL_S_DEFAULT and
 * MOTOR_TRAVEL_MARGIN_S_DEFAULT) live in config/cfg_defaults.h.
 * Min/max validation bounds live in config/cfg_limits.h.
 * Files that need either set should include those headers directly.
 * ============================================================ */

/** Graduated ventilation steps (Step 1=M1, Step 2=M1+M2, Step 3=M1+M2+M3). */
#define NUM_VENT_STEPS  3

/* ============================================================
 * Section 2 — FreeRTOS includes + RTOS handle externs
 *
 * All primitives are created in main.cpp and declared extern here so
 * every task module can reference them without handle-passing boilerplate.
 * ============================================================ */

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

/* Queues (defined in main.cpp) */
extern QueueHandle_t Q1;   /**< window_cmd_t   — T3/T6 → T2 only (C9: manual window commands out of scope) */
extern QueueHandle_t Q2;   /**< key_event_t    — T7 → T8 */
extern QueueHandle_t Q3;   /**< log_event_t    — all tasks → T9 (use log_post(), never xQueueSend directly) */
extern QueueHandle_t Q4;   /**< config_update_t — T8/T10/T11 → T4 */
extern QueueHandle_t Q5;   /**< net_status_t   — T10 → T8 (depth 1, xQueueOverwrite) */
extern QueueHandle_t Q6;   /**< sensor_reading_t — T5 → T4 (depth 1, xQueueOverwrite) */

/* Task handles (defined in main.cpp) */
extern TaskHandle_t task_t1;   /**< Watchdog / Heartbeat */
extern TaskHandle_t task_t2;   /**< Relay Controller */
extern TaskHandle_t task_t3;   /**< Safety Monitor */
extern TaskHandle_t task_t4;   /**< Data Manager */
extern TaskHandle_t task_t5;   /**< Sensor Poll */
extern TaskHandle_t task_t6;   /**< Climate Control */
extern TaskHandle_t task_t7;   /**< Keypad Scan */
extern TaskHandle_t task_t8;   /**< UI / Display */
extern TaskHandle_t task_t9;   /**< Event Logger */
extern TaskHandle_t task_t10;  /**< Network Manager */
extern TaskHandle_t task_t11;  /**< Web Server */
extern TaskHandle_t task_t12;  /**< MQTT Client */
/* task_t13 (OTA) is created on demand by T11; no permanent handle */
extern TaskHandle_t task_t14;  /**< Status website POST */
extern TaskHandle_t task_t15;  /**< Status-POST supervisor (gh#18 Phase 4) */

/* Event group (defined in main.cpp) */
extern EventGroupHandle_t EG1; /**< System state flags — see Section 5 */

/* Mutexes (defined in main.cpp) */
extern SemaphoreHandle_t MX1;  /**< I2C bus (T4 RTC + T8 LCD) */
extern SemaphoreHandle_t MX2;  /**< Current measurement data */
extern SemaphoreHandle_t MX3;  /**< Measurement ring buffers */
extern SemaphoreHandle_t MX4;  /**< Configuration settings (NVS shadow) */
extern SemaphoreHandle_t MX5;  /**< LittleFS active partition */

/* ============================================================
 * Section 3 — Enumeration types
 *
 * Enums are declared before the queue structs that reference them.
 * ============================================================ */

/** Per-channel window position state (T2 state machine). */
typedef enum {
    WIN_UNKNOWN,        /**< Position not yet established (before CLOSE_ALL calibration) */
    WIN_CLOSED,         /**< Window fully closed (at close end-switch) */
    WIN_MOVING_OPEN,    /**< Relay energised in OPEN direction; travel timer running */
    WIN_OPEN,           /**< Window fully open (travel timer expired) */
    WIN_MOVING_CLOSE,   /**< Relay energised in CLOSE direction; travel timer running */
} window_state_t;

/** System operating mode (highest-priority active state wins). */
typedef enum {
    MODE_AUTOMATIC,      /**< Normal climate control active */
    MODE_STANDBY,        /**< Climate control paused by operator */
    MODE_WIND_OVERRIDE,  /**< Wind safety has forced all windows closed */
    MODE_MOTOR_ALARM,    /**< RRK-3 emergency stop active; all control suspended */
} op_mode_t;

/** Authenticated session level for LCD and web UI. */
typedef enum {
    SESSION_NONE,    /**< No active session */
    SESSION_FARMER,  /**< Farmer-level access (setpoints, mode) */
    SESSION_ADMIN,   /**< Admin-level access (all settings) */
} session_t;

/** Event log entry type — stored as `event_type` field in log_entry_t. */
typedef enum {
    LOG_SENSOR,       /**< Periodic sensor snapshot (T, RH, wind) */
    LOG_RELAY,        /**< Relay state change (window open/close/stop) */
    LOG_MODE_CHANGE,  /**< Operating mode transition */
    LOG_SETPOINT,     /**< Configuration parameter change */
    LOG_SESSION,      /**< User session open/close */
    LOG_ALARM,        /**< Alarm onset or clearance (motor alarm, sensor fault, wind) */
    LOG_SYSTEM,       /**< System event (boot, NVS migration, Q3 drop-overflow count) */
} log_type_t;

/** Log initiator — who or what triggered the event. */
typedef enum {
    LOG_BY_SYSTEM,  /**< Firmware-internal (T2, T3, T5, T6, etc.) */
    LOG_BY_FARMER,  /**< Farmer session via LCD */
    LOG_BY_ADMIN,   /**< Admin session via LCD */
    LOG_BY_MQTT,    /**< MQTT message (T12) */
    LOG_BY_WEB,     /**< Web session (T11) */
} log_initiator_t;

/**
 * @brief Log param_id — identifies which config parameter changed in a
 *        LOG_SETPOINT event (C1–C22 from logAnalysis.md).
 *
 * For C18/C19 the motor channel is identified by the `channel` field (1/2/3).
 * Non-CONFIG events use LOG_PARAM_NONE (0).
 */
typedef enum {
    LOG_PARAM_NONE         =  0,  /**< Non-CONFIG events */
    LOG_PARAM_T_MIN_DAY    =  1,  /**< C1  t_min_day */
    LOG_PARAM_T_MAX_DAY    =  2,  /**< C2  t_max_day */
    LOG_PARAM_T_MIN_NGT    =  3,  /**< C3  t_min_ngt */
    LOG_PARAM_T_MAX_NGT    =  4,  /**< C4  t_max_ngt */
    LOG_PARAM_RH_MIN_DAY   =  5,  /**< C5  rh_min_day */
    LOG_PARAM_RH_MAX_DAY   =  6,  /**< C6  rh_max_day */
    LOG_PARAM_RH_MIN_NGT   =  7,  /**< C7  rh_min_ngt */
    LOG_PARAM_RH_MAX_NGT   =  8,  /**< C8  rh_max_ngt */
    LOG_PARAM_HYST_T       =  9,  /**< C9  hyst_t */
    LOG_PARAM_HYST_RH      = 10,  /**< C10 hyst_rh */
    LOG_PARAM_RH_CTRL_EN   = 11,  /**< C11 rh_ctrl_en */
    LOG_PARAM_CR_PRIORITY  = 12,  /**< C12 cr_priority */
    LOG_PARAM_AVG_WIN_T    = 13,  /**< C13 avg_win_t */
    LOG_PARAM_AVG_WIN_RH   = 14,  /**< C14 avg_win_rh */
    LOG_PARAM_V_MAX        = 15,  /**< C15 v_max */
    LOG_PARAM_DIR_EXCL_LOW = 16,  /**< C16 dir_excl_low */
    LOG_PARAM_DIR_EXCL_HI  = 17,  /**< C17 dir_excl_high */
    LOG_PARAM_DWELL_OPEN   = 18,  /**< C18 dwell_open_mX  (channel = motor 1/2/3) */
    LOG_PARAM_DWELL_CLOSE  = 19,  /**< C19 dwell_close_mX (channel = motor 1/2/3) */
    LOG_PARAM_POLL_INTV    = 20,  /**< C20 poll_interval */
    LOG_PARAM_LAT_LON      = 21,  /**< C21 lat / lon */
    LOG_PARAM_CR_APPLIED   = 22,  /**< C22 automatic T vs RH conflict resolution */
} log_param_id_t;

/**
 * @brief Q1 actuation command source.
 * Only T3 (Safety Monitor) and T6 (Climate Control) post to Q1.
 * Manual window commands from LCD/web/MQTT are out of scope (C9).
 */
typedef enum {
    SRC_T3,  /**< Safety Monitor (wind safety, CLOSE_ALL) */
    SRC_T6,  /**< Climate Control (graduated ventilation) */
} cmd_source_t;

/** Q1 actuation command action. */
typedef enum {
    CMD_OPEN,       /**< Open the specified channel (full travel) */
    CMD_CLOSE,      /**< Close the specified channel (full travel) */
    CMD_CLOSE_ALL,  /**< Close all channels (calibration / safety) */
    CMD_RESUME,     /**< Resume automatic mode after wind override */
} cmd_action_t;

/* ============================================================
 * Section 4 — Queue / message struct types
 * ============================================================ */

/** Q1 — actuation command (T3/T6 → T2). */
typedef struct {
    cmd_action_t action;   /**< CMD_OPEN / CMD_CLOSE / CMD_CLOSE_ALL / CMD_RESUME */
    uint8_t      channel;  /**< 0 = all channels; 1 = M1; 2 = M2; 3 = M3 */
    cmd_source_t source;   /**< SRC_T3 or SRC_T6 */
} window_cmd_t;

/** Q2 — keypad key event (T7 → T8). */
typedef struct {
    char key;       /**< ASCII character from keypad_scan(), or '\0' for no key */
    bool repeated;  /**< true if key-repeat generated this event */
} key_event_t;

/**
 * @brief Q3 — fixed 12-byte log record (all tasks → T9 via log_post()).
 *
 * Layout is packed: 4+1+1+1+1+2+2 = 12 bytes with no compiler padding.
 * The binary layout is used for NVS blob storage and SD card CSV export.
 *
 * | Offset | Field      | Type    | Description |
 * |--------|------------|---------|-------------|
 * | 0      | timestamp  | uint32  | Unix epoch seconds |
 * | 4      | event_type | uint8   | log_type_t |
 * | 5      | initiator  | uint8   | log_initiator_t |
 * | 6      | channel    | uint8   | motor 1/2/3, or 0 for non-motor events |
 * | 7      | param_id   | uint8   | log_param_id_t; 0 for non-CONFIG events |
 * | 8      | value_a    | int16   | first payload (sensor value, old setting, reason code) |
 * | 10     | value_b    | int16   | second payload (new setting, threshold, etc.) |
 */
typedef struct {
    uint32_t timestamp;   /**< Unix epoch seconds */
    uint8_t  event_type;  /**< log_type_t */
    uint8_t  initiator;   /**< log_initiator_t */
    uint8_t  channel;     /**< motor 1/2/3, or 0 for non-motor events */
    uint8_t  param_id;    /**< log_param_id_t; 0 for non-CONFIG events */
    int16_t  value_a;     /**< first payload */
    int16_t  value_b;     /**< second payload */
} log_entry_t;

typedef log_entry_t log_event_t;  /**< Alias used by Q3 producers */

/** Q4 — NVS configuration update request (T8/T10/T11 → T4). */
typedef struct {
    char    ns[16];   /**< NVS namespace (e.g. "climate") */
    char    key[16];  /**< NVS key (e.g. "t_max_day") */
    int32_t value;    /**< New value (cast to appropriate NVS type by T4) */
} config_update_t;

/** Q5 — network status (T10 → T8; depth 1, xQueueOverwrite). */
typedef struct {
    bool client_connected;  /**< true if connected to a WiFi AP as client */
    bool ap_active;         /**< true if AP mode is currently running */
    bool ntp_synced;        /**< true if NTP has successfully synced this session */
    char ip_str[16];        /**< dotted-decimal IPv4 address string */
} net_status_t;

/** Q6 — sensor reading snapshot (T5 → T4; depth 1, xQueueOverwrite). */
typedef struct {
    int16_t  temperature_c;         /**< Raw temperature, integer °C */
    uint8_t  humidity_pct;          /**< Raw relative humidity, 0–100 % */
    uint8_t  _reserved;             /**< Alignment padding */
    uint16_t wind_speed_ms10;       /**< Wind speed × 10 (e.g. 35 = 3.5 m/s) */
    uint16_t wind_dir_deg;          /**< Wind direction, 0–359 ° */
    int16_t  t_avg_c;               /**< Sliding-average temperature, integer °C */
    uint8_t  rh_avg_pct;            /**< Sliding-average humidity, 0–100 % */
    uint8_t  _reserved2;            /**< Alignment padding */
    uint16_t wind_speed_avg_ms10;   /**< Sliding-average wind speed × 10 */
    uint16_t wind_dir_avg_deg;      /**< Sliding-average wind direction, 0–359 ° */
    uint16_t wind_dir_variation_deg;/**< Width of the smallest arc containing
                                      *  every direction sample in the current
                                      *  sliding window (0–359). E.g. wind
                                      *  oscillating 100° ↔ 160° gives 60. */
    uint32_t timestamp;             /**< Unix epoch seconds of this reading */
} sensor_reading_t;

/**
 * @brief Aggregated controller status snapshot (read-mostly).
 *
 * Filled by dm_status_snapshot(). Consumed by build_canonical_status_json()
 * for both the local web UI (/api/status, WebSocket) and the remote status
 * website POST. Single source of truth for "what is the controller doing
 * right now".
 *
 * Empty / pre-NTP / pre-sensor states are rendered as zeros so the JSON
 * builder never has to special-case missing data; consumers gate display on
 * presence of the tile object.
 */
typedef struct {
    /* Climate */
    int16_t  t_c10;          /**< Latest temperature × 10 (e.g. 234 = 23.4 °C) */
    int16_t  t_avg_c10;      /**< Sliding-average temperature × 10 */
    uint8_t  rh_pct;         /**< Latest RH (0–100 %) */
    uint8_t  rh_avg_pct;     /**< Sliding-average RH */

    /* Wind */
    uint16_t w_ms10;         /**< Latest wind speed × 10 */
    uint16_t w_avg_ms10;     /**< Sliding-average wind speed × 10 */
    uint16_t w_dir_deg;      /**< Latest wind direction (0–359 °) */
    uint16_t w_avg_dir_deg;  /**< Sliding-average wind direction */
    uint16_t w_dir_variation_deg; /**< Arc width spanning every direction sample
                                    * in the current sliding window (0–359). */

    /* Active climate setpoints — the day-or-night value currently in force
     * (selected from cfg by is_daytime). Surfaced on the local web GUI Status
     * tiles and the canonical status JSON so operators see at a glance which
     * threshold the controller is regulating against right now. */
    int16_t t_max_active;    /**< Active max temperature setpoint (°C) */
    uint8_t rh_max_active;   /**< Active max humidity setpoint (%) */
    uint8_t rh_min_active;   /**< Active min humidity setpoint (%) */
    bool    rh_ctrl_enabled; /**< Live state of the Humidity-control switch;
                              *   when false the RH setpoints are configured
                              *   but inert. Drives a dim-style on the local
                              *   web GUI and field-omission in the T14
                              *   status-website POST. */

    /* Windows */
    window_state_t win[3];   /**< M1 = win[0], M2 = win[1], M3 = win[2] */

    /* Mode + raw EG1 bits (for local-UI badges; harmless on the public dashboard) */
    op_mode_t mode;
    uint32_t  eg1_bits;

    /* Sun — minutes from local midnight (already DST-adjusted). The cfg
     * shadow stores UTC minutes; dm_status_snapshot adds the current TZ
     * offset so consumers can render verbatim as HH:MM. */
    bool     is_daytime;
    int32_t  sunrise_mins_local;
    int32_t  sunset_mins_local;

    /* System */
    uint32_t ts_unix;        /**< Last known Unix UTC timestamp */
    char     time_iso[20];   /**< Local-time ISO-8601 ("YYYY-MM-DDTHH:MM:SS") */
    bool     ntp_synced;
    char     ip[16];         /**< Dotted-decimal STA IPv4 ("" if not connected) */
    int16_t  rssi;           /**< STA RSSI in dBm (0 if not connected) */
    char     fw[24];         /**< Firmware version string (compiled into firmware).
                              *   alpha.6.17.1: bumped 16→24. The 1.20.3-era 6-char
                              *   "1.20.3" string fit in 16 bytes but the 2.0.0
                              *   alpha tags ("2.0.0-alpha.6.17" = 16 chars + NUL
                              *   = 17 bytes) overflowed by one — alpha.6.17 was
                              *   caught with fw="2.0.0-alpha.6.1" (truncated). 24
                              *   bytes gives comfortable headroom through 2.0.0-rc.N
                              *   and 2.x.x.x patterns. */
    char     assets[24];     /**< Asset version string from manifest.json on the
                              *   active LittleFS partition. Differs from `fw`
                              *   when an OTA bank flip didn't bring the matching
                              *   web assets along — surfaces silent firmware/
                              *   assets mismatches. Bumped 16→24 in lockstep with
                              *   `fw` above (same alpha-tag-length concern). */
    uint32_t uptime_s;       /**< Seconds since boot */

    /* Top-level — always emitted regardless of expose mask */
    uint16_t update_interval_s;  /**< Cycle the controller advertises to the dashboard */
} status_snapshot_t;

/** Bit positions in cfg_shadow_t::status_expose. */
#define STATUS_EXPOSE_CLIMATE   (1u << 0)
#define STATUS_EXPOSE_WIND      (1u << 1)
#define STATUS_EXPOSE_WINDOWS   (1u << 2)
#define STATUS_EXPOSE_MODE      (1u << 3)
#define STATUS_EXPOSE_SUN       (1u << 4)
#define STATUS_EXPOSE_SYSTEM    (1u << 5)
#define STATUS_EXPOSE_ALL       0x3Fu

/* ============================================================
 * Section 5 — Event group bit definitions (EG1)
 * ============================================================ */

#define EG1_BIT_WIND_OVERRIDE    (1 << 0)  /**< Set/cleared by T3 — wind safety active */
/* bit 1 reserved — was MANUAL_OVERRIDE; removed (hardware does not support manual op detection) */
#define EG1_BIT_SENSOR_FAULT_T   (1 << 2)  /**< Set/cleared by T5 — T/RH sensor fault */
#define EG1_BIT_SENSOR_FAULT_W   (1 << 3)  /**< Set/cleared by T5 — wind sensor fault */
#define EG1_BIT_OTA_IN_PROGRESS  (1 << 4)  /**< Set/cleared by T13 — OTA update in progress */
#define EG1_BIT_MOTOR_ALARM      (1 << 5)  /**< Set/cleared by T2 — RRK-3 emergency stop active */
#define EG1_BIT_CALIBRATING      (1 << 6)  /**< Set/cleared by T2 — CLOSE_ALL calibration in progress */

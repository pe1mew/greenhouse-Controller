/**
 * @file app_types.h
 * @brief Shared compile-time constants, struct types, enum types, and
 *        FreeRTOS handle declarations used across all firmware tasks.
 *
 * This is the single header every firmware module includes for inter-task
 * communication types and system-wide constants. It is the project's
 * reference card — if you need to know which task produces a queue, what
 * the payload layout looks like, or which EG1 bit signals which condition,
 * it's documented here.
 *
 * ## Sections
 *  1. Factory-default hardware constants — NVS-backed at runtime; these
 *     macros are the factory defaults written on first boot.
 *  2. FreeRTOS includes + RTOS handle externs — declared here, defined in
 *     `system_globals.cpp` (the Phase-6.1 bootstrap unit).
 *  3. Enumeration types — FSM states, modes, roles, log types.
 *  4. Queue / message struct types — one struct per queue.
 *  5. Event group bit definitions — EG1 system-state flags.
 *
 * ## Producer / consumer summary
 *  - Q1 carries `window_cmd_t` from T3 (Safety Monitor) and T6 (Climate
 *    Control) to T2 (Relay Controller). Demand reconciliation between T6's
 *    graduated ventilation and T3's wind-safety overrides happens via Q1
 *    windows — both tasks post; T2 arbitrates by source.
 *  - Q2 carries `key_event_t` from T7 (Keypad Scan) to T8 (UI / Display)
 *    only.
 *  - Q3 carries `log_event_t` (== `log_entry_t`) from every producer to
 *    T9 (Event Logger). Producers MUST call `log_post()`, never
 *    `xQueueSend` directly — log_post handles drop-counting on overflow
 *    so the volume of dropped events is itself recoverable as a
 *    LOG_SYSTEM row.
 *  - Q4 carries `config_update_t` from T8 (LCD), T10 (Network Manager
 *    for SNTP/geo updates) and T11 (web server) to T4 (Data Manager).
 *  - Q5 carries `net_status_t` from T10 to T8 (depth 1, overwrite).
 *  - Q6 carries `sensor_reading_t` from T5 (Sensor Poll) to T4 (depth 1,
 *    overwrite).
 *
 * @see  system_globals.cpp for queue depths and mutex semantics.
 * @see  design/tasks.md for the full task / queue / mutex matrix.
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

/**
 * @brief Number of graduated ventilation steps T6 selects between.
 *
 * Step 1 opens M1 only; Step 2 opens M1+M2; Step 3 opens M1+M2+M3. Mapping
 * lives in `climate_control.cpp`. Value drives loop bounds in the climate
 * FSM and the wind-direction-exclusion check that gates the high-step
 * windows. Changing this requires rewiring the step→channel map.
 */
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

/* Queues (defined in system_globals.cpp) */
extern QueueHandle_t Q1;   /**< window_cmd_t     — T3/T6 → T2 only, depth 8 (C9: manual window commands out of scope) */
extern QueueHandle_t Q2;   /**< key_event_t      — T7 → T8, depth 8 */
extern QueueHandle_t Q3;   /**< log_event_t      — all tasks → T9, depth 32 (use log_post(), never xQueueSend directly) */
extern QueueHandle_t Q4;   /**< config_update_t  — T8/T10/T11 → T4, depth 16 */
extern QueueHandle_t Q5;   /**< net_status_t     — T10 → T8, depth 1 (xQueueOverwrite — latest wins) */
extern QueueHandle_t Q6;   /**< sensor_reading_t — T5 → T4,  depth 1 (xQueueOverwrite — latest wins) */

/* Task handles (defined in system_globals.cpp, set by xTaskCreatePinnedToCore call in main.cpp). */
extern TaskHandle_t task_t1;   /**< T1  — Watchdog / Heartbeat: TWDT kick, NeoPixel, heap rows, OTA mark-healthy. */
extern TaskHandle_t task_t2;   /**< T2  — Relay Controller: per-channel window FSM, motor-alarm RRK-3 monitor. */
extern TaskHandle_t task_t3;   /**< T3  — Safety Monitor: wind override, CLOSE_ALL on safety conditions. */
extern TaskHandle_t task_t4;   /**< T4  — Data Manager: NVS shadow, sensor ring buffer, applies Q4 config updates. */
extern TaskHandle_t task_t5;   /**< T5  — Sensor Poll: SHT31 T/RH + WS-3000 wind, publishes Q6. */
extern TaskHandle_t task_t6;   /**< T6  — Climate Control: graduated ventilation FSM, posts Q1 window commands. */
extern TaskHandle_t task_t7;   /**< T7  — Keypad Scan: 4×4 matrix debounce + autorepeat, publishes Q2. */
extern TaskHandle_t task_t8;   /**< T8  — UI / Display: LCD render + keypad menu FSM + session manager. */
extern TaskHandle_t task_t9;   /**< T9  — Event Logger: drains Q3 to SD CSV with ring-buffer overflow handling. */
extern TaskHandle_t task_t10;  /**< T10 — Network Manager: WiFi STA/AP, SNTP, geo lookup, publishes Q5. */
extern TaskHandle_t task_t11;  /**< T11 — Web Server: esp_http_server with 25+ routes + /ws WebSocket. */
extern TaskHandle_t task_t12;  /**< T12 — MQTT Client (optional; may be NULL if disabled). */
/* task_t13 (OTA) is created on demand by T11 — no permanent handle. */
extern TaskHandle_t task_t14;  /**< T14 — Status website POST: periodic JSON upload to remote dashboard. */
extern TaskHandle_t task_t15;  /**< T15 — Status-POST supervisor (gh#18 Phase 4): circuit-breaker for T14 backoff. */

/* Event group (defined in system_globals.cpp). */
extern EventGroupHandle_t EG1; /**< System state flags — see Section 5 for bit definitions. */

/* Mutexes (defined in system_globals.cpp). */
extern SemaphoreHandle_t MX1;  /**< I2C bus (T4 RTC + T8 LCD on shared LIB-2). */
extern SemaphoreHandle_t MX2;  /**< Current measurement data (sensor_reading_t snapshot). */
extern SemaphoreHandle_t MX3;  /**< Measurement ring buffers (sensor history, served by /api/history). */
extern SemaphoreHandle_t MX4;  /**< Configuration settings (NVS-backed cfg_shadow_t). */
extern SemaphoreHandle_t MX5;  /**< LittleFS active partition (T11 read vs T13 OTA cross-bank write). */

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

    /* a.6.35.5 — admin / web-only setting changes that previously had no
     * audit trail. Sensitive fields (PIN, WiFi credentials, secrets) log
     * a "changed" marker without exposing the value:
     *   value_a = 1, value_b = 0 → field was set/changed
     * Non-sensitive integer fields log old → new exactly the way the C1..C22
     * setpoints do. Strings without sensitive content (tz_str) get the same
     * "set" marker — the parser includes the field-name; the value isn't
     * loggable as int16 anyway. */
    LOG_PARAM_TZ_STR         = 23,  /**< system/tz_str   — value_a=1=set */
    LOG_PARAM_WIFI_SSID      = 24,  /**< wifi/ssid       — value_a=1=set */
    LOG_PARAM_WIFI_PSK       = 25,  /**< wifi/psk        — value_a=1=set */
    LOG_PARAM_WIFI_AP_PSK    = 26,  /**< wifi/ap_psk     — value_a=1=set */
    LOG_PARAM_PIN_FARMER     = 27,  /**< pin role=farmer — value_a=1=changed */
    LOG_PARAM_PIN_ADMIN      = 28,  /**< pin role=admin  — value_a=1=changed */
    LOG_PARAM_STATUS_URL     = 29,  /**< system/status_url    — value_a=1=set */
    LOG_PARAM_STATUS_SECRET  = 30,  /**< system/status_secret — value_a=1=set */
    LOG_PARAM_STATUS_INTV    = 31,  /**< system/status_intv_s — old → new */
    LOG_PARAM_STATUS_ENABLE  = 32,  /**< system/status_enable — old → new */
    LOG_PARAM_STATUS_EXPOSE  = 33,  /**< system/status_expose — old → new (bitmask) */
    LOG_PARAM_LOG_UPLOAD_H   = 34,  /**< system/log_upload_h   — old → new */
    LOG_PARAM_LOG_UPLOAD_M   = 35,  /**< system/log_upload_m   — old → new */
    LOG_PARAM_LOG_UPLOAD_ROT = 36,  /**< system/log_upload_rot — old → new */

    /* Wind subsystem boolean (since 1.20.x; not previously enumerated). */
    LOG_PARAM_WIND_PROT_EN   = 37,  /**< wind/wind_prot_en — old → new */
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
    uint8_t initiator;/**< log_initiator_t — carried through to the audit row
                       *   T4 emits after applying the change. LCD-UI senders
                       *   set LOG_BY_FARMER or LOG_BY_ADMIN from the active
                       *   session; the web server sets LOG_BY_WEB. Since
                       *   2.0.0-a.6.35.5; older callers that left it zero
                       *   produce LOG_BY_SYSTEM rows which surface as a
                       *   missing-attribution audit signal. */
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
    int16_t  temperature_c;         /**< Raw temperature, integer °C
                                      *  (kept for climate_control setpoint
                                      *  comparison + LCD render + LOG_SENSOR
                                      *  value_a; the operator-visible
                                      *  precision lives in `temperature_c10`
                                      *  below — rc.1.3.1) */
    uint8_t  humidity_pct;          /**< Raw relative humidity, 0–100 % */
    uint8_t  _reserved;             /**< Alignment padding */
    uint16_t wind_speed_ms10;       /**< Wind speed × 10 (e.g. 35 = 3.5 m/s) */
    uint16_t wind_dir_deg;          /**< Wind direction, 0–359 ° */
    int16_t  t_avg_c;               /**< Sliding-average temperature, integer °C
                                      *  (same role as `temperature_c` for
                                      *  whole-°C consumers; tenths in
                                      *  `t_avg_c10`) */
    uint8_t  rh_avg_pct;            /**< Sliding-average humidity, 0–100 % */
    uint8_t  _reserved2;            /**< Alignment padding */
    uint16_t wind_speed_avg_ms10;   /**< Sliding-average wind speed × 10 */
    uint16_t wind_dir_avg_deg;      /**< Sliding-average wind direction, 0–359 ° */
    uint16_t wind_dir_variation_deg;/**< Width of the smallest arc containing
                                      *  every direction sample in the current
                                      *  sliding window (0–359). E.g. wind
                                      *  oscillating 100° ↔ 160° gives 60. */
    int16_t  temperature_c10;       /**< Raw temperature × 10 (e.g. 234 = 23.4 °C);
                                      *  added rc.1.3.1 to preserve the
                                      *  FG6485A's native 0.1 °C resolution
                                      *  end-to-end into the canonical status
                                      *  JSON + /api/history. Same source
                                      *  float; only the storage precision
                                      *  differs from `temperature_c`. */
    int16_t  t_avg_c10;             /**< Sliding-average temperature × 10
                                      *  (same precision policy as
                                      *  `temperature_c10`). */
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
                              *   status-website POST. Also emitted as the
                              *   `humidity_ctrl_off` mode-flag in the
                              *   canonical JSON when false (a.6.35.4+) so
                              *   both surfaces show a "Hum off" badge. */
    bool    wind_protect_enabled; /**< Live state of the wind-protection
                              *   subsystem. True when `cfg.v_max > 0`; false
                              *   when the operator set v_max ≤ 0 to disable
                              *   wind-driven window closing entirely. When
                              *   false, the canonical JSON emits the
                              *   `wind_protect_off` mode-flag and the GUI
                              *   surfaces a "Wind off" badge in the Alarms
                              *   card so the operator and public dashboard
                              *   can see at a glance that the wind safety
                              *   net is currently inactive. (a.6.35.4+) */
    bool    coredump_available; /**< True iff a valid coredump from a previous
                              *   panic is stored in the coredump partition.
                              *   Cached at boot by T4 from
                              *   esp_core_dump_image_check(). Drives the
                              *   `coredump_available` mode-flag in the
                              *   canonical JSON; the local GUI Alarms card
                              *   shows a blue "Coredump available" badge and
                              *   the Log tab adds a download/erase panel.
                              *   Cleared by dm_coredump_clear() after the
                              *   operator wipes the partition via
                              *   POST /api/coredump/erase. (a.6.35.6+) */

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

/* ============================================================
 * Status-expose bitmask
 *
 * Bit positions in `cfg_shadow_t::status_expose`. The admin selects which
 * tile groups are pushed to the public dashboard via the /api/web "expose"
 * field. The local web GUI always renders everything (STATUS_EXPOSE_ALL +
 * include_disabled_setpoints=true via build_canonical_status_json).
 * ============================================================ */

/** @brief Expose climate tile (T, RH, setpoints) to the public dashboard. */
#define STATUS_EXPOSE_CLIMATE   (1u << 0)
/** @brief Expose wind tile (speed, direction, variation) to the public dashboard. */
#define STATUS_EXPOSE_WIND      (1u << 1)
/** @brief Expose windows tile (M1/M2/M3 states) to the public dashboard. */
#define STATUS_EXPOSE_WINDOWS   (1u << 2)
/** @brief Expose operating mode + alarm badges to the public dashboard. */
#define STATUS_EXPOSE_MODE      (1u << 3)
/** @brief Expose sunrise/sunset minutes and is_daytime flag to the public dashboard. */
#define STATUS_EXPOSE_SUN       (1u << 4)
/** @brief Expose system tile (uptime, IP, RSSI, fw_ver, assets_ver) to the public dashboard. */
#define STATUS_EXPOSE_SYSTEM    (1u << 5)
/** @brief Convenience: all six expose bits set (mask `0x3F`). Used by the local UI / WebSocket push. */
#define STATUS_EXPOSE_ALL       0x3Fu

/* ============================================================
 * Section 5 — Event group bit definitions (EG1)
 *
 * EG1 is a single shared event group; bits are write-side owned by the
 * task noted in each definition's comment and read by any task that
 * needs to react. Readers should use xEventGroupGetBits() (non-blocking)
 * unless a true block-until-condition wait is required.
 * ============================================================ */

/** @brief Wind safety active — T3 forced all windows closed; T6 should not open. Write: T3. */
#define EG1_BIT_WIND_OVERRIDE    (1 << 0)
/* bit 1 reserved — was MANUAL_OVERRIDE; removed (hardware does not support manual op detection) */
/** @brief T/RH sensor fault detected by T5 — temperature/humidity readings invalid. Write: T5. */
#define EG1_BIT_SENSOR_FAULT_T   (1 << 2)
/** @brief Wind sensor fault detected by T5 — wind speed/direction readings invalid. Write: T5. */
#define EG1_BIT_SENSOR_FAULT_W   (1 << 3)
/** @brief OTA update in progress — T13 is writing the inactive bank. Write: T13. */
#define EG1_BIT_OTA_IN_PROGRESS  (1 << 4)
/** @brief RRK-3 motor alarm — emergency stop active; T2 has suspended all relay outputs. Write: T2. */
#define EG1_BIT_MOTOR_ALARM      (1 << 5)
/** @brief CLOSE_ALL calibration in progress — T2 is performing the boot-time calibration sweep. Write: T2. */
#define EG1_BIT_CALIBRATING      (1 << 6)

/**
 * @file system_globals.cpp
 * @brief Phase-6.1 FreeRTOS-infrastructure bootstrap — definitions + init.
 *
 * Owns the storage for every extern symbol declared in `types/app_types.h`
 * (queues Q1..Q6, mutexes MX1..MX5, event group EG1 and task handles
 * task_t1..task_t15) and provides `system_globals_init()` to create the
 * underlying RTOS objects at boot.
 *
 * ## Queue sizing rationale
 * Depths come from the inter-task design (firmwareImplementationPlan.md
 * §Phase 1 + design/tasks.md):
 *   - Q1 (window_cmd_t,    T3/T6 → T2):       8 — peak load is several cmds
 *                                                 back-to-back during CLOSE_ALL
 *   - Q2 (key_event_t,     T7    → T8):       8 — debouncing in T7 keeps this thin
 *   - Q3 (log_event_t,     *     → T9):      32 — gh#22 ring-handover sizing;
 *                                                 logs burst on mode change
 *   - Q4 (config_update_t, T8/T10/T11 → T4): 16 — peak during web-save of many fields
 *   - Q5 (net_status_t,    T10   → T8):       1 — overwrite semantics (latest-wins)
 *   - Q6 (sensor_reading_t,T5    → T4):       1 — overwrite semantics (latest-wins)
 *
 * ## Mutex semantics
 *   - MX1 — I2C bus serialisation (RTC + LCD on shared LIB-2)
 *   - MX2 — current sensor_reading_t (publisher T5, readers T6/T8/T11/T14)
 *   - MX3 — sensor history ring buffer (writer T4, reader T11 /api/history)
 *   - MX4 — cfg_shadow_t (NVS-backed config; writer T4, readers everywhere)
 *   - MX5 — LittleFS active partition guard (T11 vs T13 cross-bank-write)
 *
 * @note All mutexes are non-recursive; deadlock prevention is design-level
 *       (consistent lock ordering across tasks, kept short).
 * @see  types/app_types.h for the matching extern declarations + per-handle
 *       producer/consumer mapping.
 *
 * @author Greenhouse Controller project
 */

#include "system_globals.h"

#include "types/app_types.h"

#include "esp_log.h"

static const char *TAG = "T-GLOBALS";

/* ============================================================
 * Symbol definitions
 *
 * Every name below has a matching `extern` declaration in app_types.h.
 * Definitions are placed here (the C standard requires one and only one
 * non-extern definition per externally-linked variable).
 * ============================================================ */

/* Queues */
QueueHandle_t Q1 = NULL;   /* window_cmd_t   — T3/T6 → T2 */
QueueHandle_t Q2 = NULL;   /* key_event_t    — T7 → T8 */
QueueHandle_t Q3 = NULL;   /* log_event_t    — all → T9 */
QueueHandle_t Q4 = NULL;   /* config_update_t — T8/T10/T11 → T4 */
QueueHandle_t Q5 = NULL;   /* net_status_t   — T10 → T8 */
QueueHandle_t Q6 = NULL;   /* sensor_reading_t — T5 → T4 */

/* Task handles. NULL until the corresponding Phase-6.N alpha spawns the task. */
TaskHandle_t task_t1  = NULL;
TaskHandle_t task_t2  = NULL;
TaskHandle_t task_t3  = NULL;
TaskHandle_t task_t4  = NULL;
TaskHandle_t task_t5  = NULL;
TaskHandle_t task_t6  = NULL;
TaskHandle_t task_t7  = NULL;
TaskHandle_t task_t8  = NULL;
TaskHandle_t task_t9  = NULL;
TaskHandle_t task_t10 = NULL;
TaskHandle_t task_t11 = NULL;
TaskHandle_t task_t12 = NULL;
TaskHandle_t task_t14 = NULL;
TaskHandle_t task_t15 = NULL;
TaskHandle_t task_t16 = NULL;

/* Event group */
EventGroupHandle_t EG1 = NULL;

/* Mutexes */
SemaphoreHandle_t MX1 = NULL;
SemaphoreHandle_t MX2 = NULL;
SemaphoreHandle_t MX3 = NULL;
SemaphoreHandle_t MX4 = NULL;
SemaphoreHandle_t MX5 = NULL;
SemaphoreHandle_t MX_TLS = NULL;

/* ============================================================
 * system_globals_init
 * ============================================================ */

extern "C" int system_globals_init(void)
{
    /* Idempotence guard — if Q1 is already set, assume the rest is too. */
    if (Q1 != NULL) {
        ESP_LOGW(TAG, "system_globals_init: already initialised (no-op)");
        return 0;
    }

    /* ---- Queues ---- */
    Q1 = xQueueCreate(8,  sizeof(window_cmd_t));
    Q2 = xQueueCreate(8,  sizeof(key_event_t));
    Q3 = xQueueCreate(32, sizeof(log_event_t));
    Q4 = xQueueCreate(16, sizeof(config_update_t));
    Q5 = xQueueCreate(1,  sizeof(net_status_t));
    Q6 = xQueueCreate(1,  sizeof(sensor_reading_t));

    if (Q1 == NULL || Q2 == NULL || Q3 == NULL ||
        Q4 == NULL || Q5 == NULL || Q6 == NULL) {
        ESP_LOGE(TAG, "queue creation FAILED — out of heap?");
        return -1;
    }

    /* ---- Mutexes (all recursive=false; deadlock-prevention is design-level) ---- */
    MX1 = xSemaphoreCreateMutex();
    MX2 = xSemaphoreCreateMutex();
    MX3 = xSemaphoreCreateMutex();
    MX4 = xSemaphoreCreateMutex();
    MX5 = xSemaphoreCreateMutex();
    MX_TLS = xSemaphoreCreateMutex();   /* 2.2.0 (ROTA) — T14/T16 TLS serialisation */

    if (MX1 == NULL || MX2 == NULL || MX3 == NULL ||
        MX4 == NULL || MX5 == NULL || MX_TLS == NULL) {
        ESP_LOGE(TAG, "mutex creation FAILED — out of heap?");
        return -2;
    }

    /* ---- Event group ---- */
    EG1 = xEventGroupCreate();
    if (EG1 == NULL) {
        ESP_LOGE(TAG, "event group creation FAILED — out of heap?");
        return -3;
    }

    ESP_LOGI(TAG, "system_globals_init OK: "
                  "queues=Q1..Q6 (%u/%u/%u/%u/%u/%u depths), "
                  "mutexes=MX1..MX5, EG1 (no bits set)",
             8u, 8u, 32u, 16u, 1u, 1u);
    return 0;
}

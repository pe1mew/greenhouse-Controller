/**
 * @file event_logger.cpp
 * @brief Q3 drop-oldest log helper and T9 task stub.
 *
 * @author  Greenhouse Controller project
 */

#include "event_logger.h"
#include "../types/app_types.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/portmacro.h>

/* Q3 is defined (created) in main.cpp and declared extern in app_types.h
 * once Phase 0 uncomments the handle externs.  Until then the reference is
 * satisfied at link time. */
extern QueueHandle_t Q3;

/* -----------------------------------------------------------------------
 * Drop counter — tracks events lost due to Q3 overflow
 *
 * Protected by a FreeRTOS spinlock so that concurrent callers from
 * different tasks increment it safely without blocking.  On ESP32-S3 the
 * spinlock is a 32-bit CAS instruction; the critical section is sub-
 * microsecond.
 * ----------------------------------------------------------------------- */
static portMUX_TYPE    g_drop_mux     = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_q3_dropped = 0;

/* -----------------------------------------------------------------------
 * log_post() — the single entry point for all Q3 producers
 * ----------------------------------------------------------------------- */

void log_post(const log_event_t *evt)
{
    /* --- Common path: queue has space --- */
    if (xQueueSend(Q3, evt, 0) == pdPASS) {
        return;
    }

    /* --- Queue is full: evict the oldest entry to make room --- */
    log_event_t discard;
    xQueueReceive(Q3, &discard, 0);   /* removes oldest; result ignored */

    /* Count the evicted entry as a dropped event. */
    portENTER_CRITICAL(&g_drop_mux);
    g_q3_dropped++;
    portEXIT_CRITICAL(&g_drop_mux);

    /* --- Retry send: may fail if a concurrent sender took the freed slot ---
     *
     * The gap between xQueueReceive and the retry xQueueSend is not atomic.
     * A concurrent caller can win the freed slot, leaving this retry without
     * space.  This is an acknowledged race: extremely rare given Q3's depth
     * (32) and T9's drain rate.  A mutex around the whole sequence would
     * eliminate it but adds latency in T3 and T6; deferred until profiling
     * demonstrates it is needed.
     */
    if (xQueueSend(Q3, evt, 0) != pdPASS) {
        /* New event also lost — count it. */
        portENTER_CRITICAL(&g_drop_mux);
        g_q3_dropped++;
        portEXIT_CRITICAL(&g_drop_mux);
    }
}

/* -----------------------------------------------------------------------
 * log_take_dropped_count() — read and reset the drop counter
 *
 * Called by T9 only.  The portENTER_CRITICAL ensures that no in-flight
 * log_post() increment races with the reset: on exit the counter is 0
 * and `count` holds the pre-reset value.
 * ----------------------------------------------------------------------- */

uint32_t log_take_dropped_count(void)
{
    portENTER_CRITICAL(&g_drop_mux);
    uint32_t count = g_q3_dropped;
    g_q3_dropped   = 0;
    portEXIT_CRITICAL(&g_drop_mux);
    return count;
}

/* -----------------------------------------------------------------------
 * T9 task — stub (Phase 5 implementation)
 *
 * Full implementation: Phase 5 of firmwareImplementationPlan.md.
 *
 * The stub holds the task in a blocked state.  Phase 5 replaces
 * vTaskDelay(portMAX_DELAY) with:
 *
 *   for (;;) {
 *       log_event_t evt;
 *       xQueueReceive(Q3, &evt, portMAX_DELAY);
 *       // ... write to NVS / SD ...
 *       // After drain pass, check and surface drop counter:
 *       uint32_t dropped = log_take_dropped_count();
 *       if (dropped > 0) {
 *           log_event_t sys_evt = { .event_type = LOG_SYSTEM,
 *                                   .value_a    = (int16_t)dropped };
 *           // Use xQueueSend directly (not log_post) to avoid re-entrant eviction:
 *           xQueueSend(Q3, &sys_evt, 0);
 *       }
 *   }
 *
 * ----------------------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

void task_event_logger(void *pvParameters)
{
    (void)pvParameters;
    /* TODO Phase 5: consume Q3; write NVS ring buffer; write SD CSV;
     * rotate log files; surface drop counter as LOG_SYSTEM event. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

#ifdef __cplusplus
}
#endif

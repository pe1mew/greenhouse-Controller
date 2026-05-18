/**
 * @file watchdog.cpp
 * @brief T1 — Watchdog / Heartbeat task (minimal — Phase 6.N.1).
 *
 * See watchdog.h for the design rationale.
 *
 * @author  Greenhouse Controller project
 */

#include "watchdog.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include "../ota_manager/ota_manager.h"   /* OTA_HEALTHY_MS, ota_mark_healthy */
#include "gpio_util.h"                    /* alpha.6.24 — PIN_HB_LED toggle (1 Hz blink) */

static const char *TAG = "T1";

void task_watchdog(void *pvParameters)
{
    (void)pvParameters;

    /* Subscribe to the IDF TWDT. The default panic-on-timeout behaviour is
     * retained — if T1 ever stops kicking, the panic handler dumps a
     * coredump to the dedicated partition so the post-mortem can identify
     * which task locked up. */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "[T1] watchdog task alive — tick=%ums, ota_healthy_at=%ums",
             (unsigned)T1_TICK_MS, (unsigned)OTA_HEALTHY_MS);

    /* Tick counter — wraps after ~24 days at 500 ms. Used only for log
     * cadence and the one-shot ota_mark_healthy gate; wrap-around is
     * benign (the boolean below is the actual gate). */
    uint32_t tick_count          = 0;
    const uint32_t OTA_HEALTHY_TICKS = OTA_HEALTHY_MS / T1_TICK_MS;
    bool ota_healthy_marked      = false;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* Kick the WDT first — highest priority concern. */
        esp_task_wdt_reset();

        /* Toggle the heartbeat LED (PIN_HB_LED = GPIO41, amber on Unit 2).
         * 1 Hz blink rate matches 1.20.3 exactly — the operator-recognisable
         * "T1 is alive" indicator. The integration heartbeat_task in main.cpp
         * runs at 5 s cadence (10× slower) and is NOT what should drive this
         * signal; alpha.6.24 moved the toggle here. */
        gpio_toggle(PIN_HB_LED);

        /* Heartbeat log every 10 ticks (= 5 s). Quiet enough not to flood
         * the serial buffer; frequent enough that an operator watching the
         * monitor can confirm T1 is alive. */
        if (tick_count % 10 == 0) {
            uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
            ESP_LOGI(TAG, "[T1] tick=%lu  uptime=%lus",
                     (unsigned long)tick_count, (unsigned long)uptime_s);
        }

        /* After OTA_HEALTHY_MS of stable uptime, reset the OTA fail
         * counter. Idempotent in principle but rate-limited to one NVS
         * write per boot via the local boolean. The check fires every
         * tick after the gate opens so a missed call (e.g. early NVS
         * busy) would self-heal on the next tick — but ota_mark_healthy
         * itself takes the cheap path when the counter is already 0. */
        if (!ota_healthy_marked && tick_count >= OTA_HEALTHY_TICKS) {
            ota_mark_healthy();
            ota_healthy_marked = true;
            ESP_LOGI(TAG, "[T1] ota_mark_healthy() called at tick=%lu "
                          "(boot survived %ums)",
                     (unsigned long)tick_count, (unsigned)OTA_HEALTHY_MS);
        }

        tick_count++;

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(T1_TICK_MS));
    }
}

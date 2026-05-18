/**
 * @file network_manager.cpp
 * @brief T10 — Network Manager task (Phase 6.N.1, minimal).
 *
 * **alpha.6.14 minimal-T10 status** (2026-05-18):
 *
 * The original 1.20.3 file (720 lines, Arduino-WiFi.h + Arduino-HTTPClient
 * based) is archived alongside this file as `network_manager_1.20.3_original
 * .cpp.archived`. Per the migration plan, the 1.20.3 state machine
 * (`WiFi.begin / WL_CONNECTED / configTime / WiFi.softAP / HTTPClient geo
 * lookup`) was always destined for a from-scratch IDF rewrite — the
 * arduino-WiFi APIs don't map 1:1 to esp_wifi/esp_netif/esp_event.
 *
 * This minimal T10 implements the **operationally essential** subset:
 *  1. Posts `net_status_t` to Q5 (consumed by T8's LCD WiFi status page).
 *  2. Sends `DM_NOTIFY_NTP_SYNCED` (TN4) to T4 after the initial NTP sync,
 *     so T4 writes the post-SNTP system time back to the DS1307 RTC.
 *  3. Polls esp_netif/esp_wifi state every NET_POLL_MS, re-posts Q5 on
 *     any change.
 *
 * **Deferred to alpha.6.14.X (or 2.1.x)** — the 1.20.3 features that did
 * not survive into this minimal version, listed here so they are not lost:
 *  - **AP fallback** (soft-AP if STA can't connect). 1.20.3's AP-on-fail
 *    behaviour is replaced for now by ESP-IDF's auto-reconnect (which
 *    retries STA indefinitely without the operator's intervention). The
 *    AP-mode captive portal will land in a follow-up patch.
 *  - **Exponential backoff state machine** (2 → 4 → 8 → 16 → 32 → 60 s).
 *    esp_wifi's internal reconnect retries at a fixed cadence; sufficient
 *    for the soak.
 *  - **HTTPClient geo/timezone lookup** (`http://ip-api.com/...` JSON
 *    fetch). Timezone is hard-coded by T4 via NVS (`tz=CET-1CEST,M3.5.0,
 *    M10.5.0/3`); no need for the per-boot HTTP probe.
 *  - **Periodic 24 h NTP resync.** The DS1307 RTC is precise enough for
 *    multi-day operation; the SD-card daily logs and T4 RTC-readback every
 *    minute provide enough monitoring. Add periodic resync if Phase 7
 *    soak shows drift.
 *
 * **Dependencies in place**:
 *  - `wifi_tickle_run()` (alpha.3.2) called from app_main_stub.cpp BEFORE
 *    T10 spawns, so esp_wifi is already initialised + connected + SNTP
 *    synced by the time T10 starts its main loop.
 *  - `Q5` queue (depth 1, xQueueOverwrite) created by `system_globals_init`.
 *  - `task_t4` handle populated by alpha.6.7 spawn.
 *  - `task_t10` handle declared in `system_globals.cpp`, populated by
 *    app_main_stub.cpp's spawn block.
 *
 * @author  Greenhouse Controller project
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

#include "network_manager.h"
#include "../types/app_types.h"           /* Q5, net_status_t, task_t4 */
#include "../data_manager/data_manager.h" /* DM_NOTIFY_NTP_SYNCED */

static const char *TAG = "T10_NET";

/** Main-loop tick (ms). Mirrors 1.20.3's NET_POLL_MS for behavioural parity. */
#define NET_POLL_MS  5000u

/** Plausibility threshold for "NTP-synced wall clock" (2023-11-14). */
#define NTP_MIN_EPOCH  ((time_t)1700000000)

/**
 * @brief Build a `net_status_t` snapshot from the current esp_wifi/esp_netif state.
 *
 * client_connected = `esp_wifi_sta_get_ap_info` succeeds.
 * ap_active        = always false in this minimal T10 (AP support deferred).
 * ntp_synced       = `time(NULL) > NTP_MIN_EPOCH` (wall clock is plausible).
 * ip_str           = STA netif IP as dotted-decimal, or "" if not up.
 */
static void snapshot_state(net_status_t *out)
{
    memset(out, 0, sizeof(*out));

    /* STA connection state. */
    wifi_ap_record_t ap = {};
    out->client_connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    /* AP state — deferred. Always false in alpha.6.14. */
    out->ap_active = false;

    /* NTP sync state. */
    out->ntp_synced = (time(NULL) > NTP_MIN_EPOCH);

    /* IP address — query the default STA netif. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip = {};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            snprintf(out->ip_str, sizeof(out->ip_str), IPSTR, IP2STR(&ip.ip));
        }
    }
}

/**
 * @brief Compare two net_status_t snapshots for material equality.
 *
 * "Material" excludes flag-bit jitter that doesn't matter to T8's LCD.
 * In practice the entire struct is compared since every field drives
 * something visible.
 */
static bool snapshots_equal(const net_status_t *a, const net_status_t *b)
{
    return a->client_connected == b->client_connected
        && a->ap_active        == b->ap_active
        && a->ntp_synced       == b->ntp_synced
        && (strncmp(a->ip_str, b->ip_str, sizeof(a->ip_str)) == 0);
}

void task_network_manager(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T10] task alive (minimal T10 — see file header for deferred features)");

    /* Brief settling delay so wifi_tickle's last log lines flush. The
     * esp_wifi + esp_netif state is queryable immediately, but logging
     * order is nicer if T10's first Q5 post comes after wifi_tickle's
     * "SNTP synced after N ms" line. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Initial snapshot + Q5 post. */
    net_status_t prev = {};
    snapshot_state(&prev);
    if (xQueueOverwrite(Q5, &prev) == pdTRUE) {
        ESP_LOGI(TAG, "[T10] initial Q5 post: client=%d ap=%d ntp=%d ip=\"%s\"",
                 (int)prev.client_connected, (int)prev.ap_active,
                 (int)prev.ntp_synced, prev.ip_str);
    } else {
        ESP_LOGW(TAG, "[T10] initial Q5 overwrite failed");
    }

    /* If NTP synced at boot (wifi_tickle's job), notify T4 so it can write
     * the post-SNTP system time back to the DS1307 via the TN4 path. T4's
     * dm_get_unix_time accessor is called immediately by event_logger and
     * sensor_poll; we want T4's RTC writeback to happen ASAP. */
    if (prev.ntp_synced && task_t4 != NULL) {
        xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
        ESP_LOGI(TAG, "[T10] TN4 sent to T4 (DM_NOTIFY_NTP_SYNCED)");
    }

    /* Main loop — periodic state polling. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NET_POLL_MS));

        net_status_t cur = {};
        snapshot_state(&cur);

        if (!snapshots_equal(&prev, &cur)) {
            ESP_LOGI(TAG, "[T10] state changed: client=%d ap=%d ntp=%d ip=\"%s\"",
                     (int)cur.client_connected, (int)cur.ap_active,
                     (int)cur.ntp_synced, cur.ip_str);
            (void)xQueueOverwrite(Q5, &cur);

            /* If NTP just transitioned to synced (e.g. after a reconnect
             * that included a fresh SNTP run), notify T4 again. The TN4
             * path in T4 is idempotent — multiple notifies in the same
             * boot just refresh the RTC. */
            if (cur.ntp_synced && !prev.ntp_synced && task_t4 != NULL) {
                xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
                ESP_LOGI(TAG, "[T10] TN4 re-sent on NTP-synced transition");
            }

            prev = cur;
        }
    }
}

/**
 * @file wifi_tickle.cpp
 * @brief Phase-3 ESP-IDF WiFi tickle implementation.
 *
 * See wifi_tickle.h for the rationale and scope. This file deliberately uses
 * direct esp_wifi/esp_event/esp_netif/esp_sntp calls — NO Arduino-ESP32
 * WiFi.h, NO HTTPClient.h, NO configTime() wrapper.
 *
 * Event-driven design (per migration plan, structurally fixes gh#21):
 *   1. wifi_tickle_run() registers handlers on WIFI_EVENT + IP_EVENT.
 *   2. esp_wifi_start() fires WIFI_EVENT_STA_START → handler calls esp_wifi_connect().
 *   3. WIFI_EVENT_STA_DISCONNECTED → handler sets a FAIL bit on the event group.
 *   4. IP_EVENT_STA_GOT_IP → handler logs the IP, sets a SUCCESS bit.
 *   5. The blocking xEventGroupWaitBits() in wifi_tickle_run() returns as
 *      soon as either bit fires (or timeout expires).
 *
 * This is the canonical IDF v5 WiFi-STA pattern, modelled on
 * examples/wifi/getting_started/station/main/station_example_main.c
 * (ESP-IDF 5.5 docs).
 *
 * @author Greenhouse Controller project
 */

#include "wifi_tickle.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

/* LIB-7 wrapper from drivers/nvs (migrated alpha.2.3) — used to read
 * the WiFi credentials persisted by the arduino-era 1.20.3 firmware. */
#include "nvs_config.h"
#include "freertos/timers.h"   /* alpha.6.31 — STA reconnect back-off timer */

static const char *TAG = "T-WIFI";

/* Event-group bits driven by the WiFi/IP event handlers. */
#define BIT_GOT_IP        (1u << 0)
#define BIT_DISCONNECTED  (1u << 1)

static EventGroupHandle_t  s_evt_group = NULL;
static esp_netif_t        *s_sta_netif = NULL;
static int                 s_retry_count = 0;   /* still used for the boot-time tickle's 3-shot fast path */
static const int           kMaxRetries  = 3;
static char                s_last_ip[16] = {0};   /* "xxx.xxx.xxx.xxx" + NUL */

/* alpha.6.31 — STA reconnect with infinite retry + exponential back-off.
 * The wifi_tickle event handler kicks a one-shot timer on each disconnect;
 * the timer expires and calls esp_wifi_connect(), retry budget never runs
 * out. Back-off ladder mirrors the 1.20.3 design (2 → 4 → 8 → 16 → 32 → 60 s
 * cap) so a permanently-down AP doesn't hammer the radio at full rate.
 *
 * Reset to BACKOFF_INIT_MS on every successful STA_GOT_IP. */
#define BACKOFF_INIT_MS    2000u
#define BACKOFF_MAX_MS    60000u

static TimerHandle_t s_reconnect_timer    = NULL;
static uint32_t      s_reconnect_delay_ms = BACKOFF_INIT_MS;

static void reconnect_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGI(TAG, "Reconnect attempt (back-off was %lu ms)",
             (unsigned long)s_reconnect_delay_ms);
    esp_wifi_connect();
    /* Bump the back-off for the *next* drop. Reset on STA_GOT_IP. */
    s_reconnect_delay_ms <<= 1;
    if (s_reconnect_delay_ms > BACKOFF_MAX_MS) {
        s_reconnect_delay_ms = BACKOFF_MAX_MS;
    }
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        s_reconnect_timer = xTimerCreate("wifi_reconnect",
                                         pdMS_TO_TICKS(s_reconnect_delay_ms),
                                         pdFALSE, NULL, reconnect_timer_cb);
        if (s_reconnect_timer == NULL) {
            ESP_LOGE(TAG, "xTimerCreate(wifi_reconnect) failed — "
                          "falling back to immediate retry");
            esp_wifi_connect();
            return;
        }
    }
    /* xTimerChangePeriod auto-starts the timer. Period of 1 tick triggers
     * almost immediately for the first retry; subsequent retries use
     * s_reconnect_delay_ms. */
    xTimerChangePeriod(s_reconnect_timer,
                       pdMS_TO_TICKS(s_reconnect_delay_ms), 0);
}

/**
 * Unified handler for WIFI_EVENT and IP_EVENT. Registered against the
 * default event loop. Runs in the event-loop task (priority 20 by default,
 * stack 2304 bytes — sufficient for ESP_LOGI + xEventGroupSetBits).
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                /* esp_wifi_start() completed — initiate the connect attempt.
                 * Doing this from the event handler (rather than polling
                 * WiFi.status() == WL_DISCONNECTED) is the structural
                 * fix for gh#21: the lwip stack is already initialised by
                 * the time STA_START fires, so esp_wifi_connect() can't
                 * race against the tcpip_adapter setup. */
                ESP_LOGI(TAG, "WIFI_EVENT_STA_START — calling esp_wifi_connect()");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                /* Boot-time tickle: kMaxRetries fast attempts so the boot
                 * gate doesn't block forever on a missing AP. After that
                 * we hand off to the back-off timer for indefinite retry.
                 *
                 * alpha.6.31 — no terminal "give up" any more. The 1.20.3
                 * behaviour was infinite-retry with exponential back-off;
                 * the 3-strike kill from earlier alphas would leave the
                 * unit permanently offline after a transient AP drop. */
                wifi_event_sta_disconnected_t *disc =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED reason=%d retry=%d/%d",
                         (int)disc->reason, s_retry_count, kMaxRetries);

                if (s_retry_count < kMaxRetries) {
                    s_retry_count++;
                    esp_wifi_connect();
                    /* Don't signal BIT_DISCONNECTED yet — let the fast
                     * retries play out within the tickle's boot budget. */
                } else {
                    /* Hand off to the back-off timer. xEventGroupSetBits
                     * still fires so the initial wifi_tickle_run() returns
                     * — but the back-off keeps retrying in the background. */
                    xEventGroupSetBits(s_evt_group, BIT_DISCONNECTED);
                    schedule_reconnect();
                }
                break;
            }

            default:
                /* Other WiFi events (SCAN_DONE, BEACON_TIMEOUT, etc.) — log at
                 * DEBUG, ignore in the tickle. */
                ESP_LOGD(TAG, "WIFI_EVENT id=%ld", (long)event_id);
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
                snprintf(s_last_ip, sizeof(s_last_ip), IPSTR, IP2STR(&ev->ip_info.ip));
                ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP ip=%s gw=" IPSTR " netmask=" IPSTR,
                         s_last_ip,
                         IP2STR(&ev->ip_info.gw),
                         IP2STR(&ev->ip_info.netmask));
                s_retry_count        = 0;   /* clear fast-retry budget for next drop */
                s_reconnect_delay_ms = BACKOFF_INIT_MS;   /* alpha.6.31 — reset back-off */
                xEventGroupSetBits(s_evt_group, BIT_GOT_IP);
                break;
            }

            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "IP_EVENT_STA_LOST_IP");
                break;

            default:
                ESP_LOGD(TAG, "IP_EVENT id=%ld", (long)event_id);
                break;
        }
    }
}

/**
 * Best-effort SNTP run. Returns true if we got a plausible epoch within
 * the budget (3 s of polling), false on timeout.
 *
 * Uses the new esp_netif_sntp_* API (IDF v5+). The older lwip-direct
 * sntp_* calls still work but the netif-aware variant is the recommended
 * path forward — it handles SNTP server failover and IPv4/IPv6 cleanly.
 */
static bool sntp_quick_sync(void)
{
    ESP_LOGI(TAG, "Starting SNTP (pool.ntp.org)");
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Plausibility threshold: 2023-11-14. Matches the network_manager.cpp
     * arduino-era logic so the bar for "synced" is the same.
     *
     * Budget was 3 s in alpha.3.1 — hit dead-on with no time to spare.
     * Extended to 10 s in alpha.3.2 because the first SNTP poll on a
     * fresh boot has to do DNS resolution (1-2 s on many residential
     * gateways) PLUS the UDP/123 round-trip PLUS multiple SNTP packets
     * to converge — 3 s is unrealistic. The arduino-era network_manager
     * used `NTP_WAIT_STEPS = 30` × 1-second sleeps = 30 s, so 10 s here
     * is still aggressive but reasonable. */
    const time_t MIN_EPOCH = 1700000000;
    const int    NTP_POLL_ITERS = 100;   /* 100 × 100 ms = 10 s */
    bool synced = false;
    for (int i = 0; i < NTP_POLL_ITERS; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        time_t now = time(NULL);
        if (now > MIN_EPOCH) {
            ESP_LOGI(TAG, "SNTP synced after %d ms — epoch=%ld", i * 100, (long)now);
            synced = true;
            break;
        }
    }

    if (!synced) {
        ESP_LOGW(TAG, "SNTP did not reach a plausible epoch in budget");
    }

    /* Always deinit so the next run starts clean (and we don't leak the
     * SNTP timer task). */
    esp_netif_sntp_deinit();
    return synced;
}

wifi_tickle_status_t wifi_tickle_run(uint32_t connect_timeout_ms)
{
    /* Step 1: read credentials from NVS. */
    char ssid[64] = {0};
    char psk[64]  = {0};
    nvs_cfg_get_str(NVS_NS_WIFI, "ssid", ssid, sizeof(ssid));
    nvs_cfg_get_str(NVS_NS_WIFI, "psk",  psk,  sizeof(psk));

    const bool have_sta_creds = (ssid[0] != '\0');
    if (have_sta_creds) {
        ESP_LOGI(TAG, "NVS credentials: ssid='%s' psk=%s",
                 ssid, psk[0] ? "***(set)" : "(empty)");
    } else {
        /* alpha.6.30 — no SSID in NVS does NOT short-circuit the stack init
         * any more. T10's AP mode (Greenhouse-XXYY recovery AP) needs
         * esp_wifi_init/_start to have happened, even when no STA credentials
         * are configured yet (that's exactly the recovery-flow case: fresh
         * unit, operator joins the AP, sets the real SSID via the GUI).
         *
         * The old code short-circuited here with WIFI_TICKLE_NO_SSID, which
         * meant esp_netif_create_default_wifi_ap() later aborted with
         * ESP_ERR_INVALID_STATE because the underlying esp_wifi peripheral
         * had never been initialised. Now we init the stack regardless,
         * skip only the STA-connect step. */
        ESP_LOGI(TAG, "no SSID in NVS — WiFi stack will init but skip STA-connect");
    }

    /* Step 2: event-group for waiting on STA_GOT_IP vs STA_DISCONNECTED. */
    s_evt_group   = xEventGroupCreate();
    s_retry_count = 0;
    s_last_ip[0]  = '\0';
    if (s_evt_group == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return WIFI_TICKLE_INIT_FAILED;
    }

    /* Step 3: netif + event loop + default STA netif.
     * Each of these is idempotent on the IDF side: esp_netif_init() and
     * esp_event_loop_create_default() return ESP_ERR_INVALID_STATE if
     * already done. Tolerate that. */
    esp_err_t err;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return WIFI_TICKLE_INIT_FAILED;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return WIFI_TICKLE_INIT_FAILED;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta returned NULL");
            return WIFI_TICKLE_INIT_FAILED;
        }
    }

    /* Step 4: init the WiFi driver. ESP_ERR_INVALID_STATE is the IDF v5
     * "already initialised" return for esp_wifi_init — tolerate it so a
     * re-run of the tickle in the same boot doesn't fail. */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return WIFI_TICKLE_INIT_FAILED;
    }

    /* Step 5: register handlers for WIFI_EVENT + IP_EVENT. The
     * `instance_register` form returns instance handles we could later
     * use to unregister cleanly; for a one-shot tickle we don't need
     * them so the out-params are NULL. */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler: %s", esp_err_to_name(err));
        return WIFI_TICKLE_INIT_FAILED;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler: %s", esp_err_to_name(err));
        return WIFI_TICKLE_INIT_FAILED;
    }

    /* Step 6: configure STA mode. Without credentials we still set STA mode
     * + esp_wifi_start so esp_netif_create_default_wifi_ap() can succeed in
     * T10's start_ap (the AP-handler-registration step inside that call
     * asserts the wifi peripheral is up). T10 promotes STA → APSTA when
     * the operator enables AP via NVS wifi/ap_enable. */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA): %s", esp_err_to_name(err));
        return WIFI_TICKLE_INIT_FAILED;
    }

    if (have_sta_creds) {
        wifi_config_t cfg = {};
        strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid)     - 1);
        strncpy((char *)cfg.sta.password, psk,  sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable    = true;
        cfg.sta.pmf_cfg.required   = false;
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
            return WIFI_TICKLE_INIT_FAILED;
        }
    }

    /* Step 7: kick off WIFI_EVENT_STA_START — handler will call
     * esp_wifi_connect from there IF we have credentials (it reads the
     * config we just set via esp_wifi_set_config). Without credentials,
     * esp_wifi_start still brings the radio up; STA just won't connect. */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return WIFI_TICKLE_INIT_FAILED;
    }

    if (!have_sta_creds) {
        ESP_LOGI(TAG, "WiFi stack up; STA-connect skipped (no SSID) — ready for AP mode");
        return WIFI_TICKLE_NO_SSID;
    }

    ESP_LOGI(TAG, "esp_wifi_start OK — waiting up to %lu ms for STA_GOT_IP",
             (unsigned long)connect_timeout_ms);

    /* Step 8: block until the event handlers signal one of the two bits,
     * or the budget expires. */
    EventBits_t bits = xEventGroupWaitBits(s_evt_group,
                                           BIT_GOT_IP | BIT_DISCONNECTED,
                                           pdTRUE,   /* clear on exit */
                                           pdFALSE,  /* OR semantics */
                                           pdMS_TO_TICKS(connect_timeout_ms));

    if (bits & BIT_GOT_IP) {
        ESP_LOGI(TAG, "WiFi tickle: STA up, IP=%s", s_last_ip);

        /* Step 9: SNTP sync (best-effort, ~3 s budget). */
        bool ntp_ok = sntp_quick_sync();
        return ntp_ok ? WIFI_TICKLE_OK : WIFI_TICKLE_OK_NO_NTP;
    }

    if (bits & BIT_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi tickle: gave up after %d retries (reason in logs above)",
                 kMaxRetries);
        return WIFI_TICKLE_DISCONNECTED;
    }

    ESP_LOGW(TAG, "WiFi tickle: STA_GOT_IP not received within %lu ms",
             (unsigned long)connect_timeout_ms);
    return WIFI_TICKLE_CONNECT_TIMEOUT;
}

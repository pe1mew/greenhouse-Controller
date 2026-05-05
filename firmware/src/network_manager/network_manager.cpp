/**
 * @file network_manager.cpp
 * @brief T10 — Network Manager task (Phase 8).
 *
 * Manages WiFi station (client) and soft-AP lifecycle, triggers NTP
 * synchronisation, updates the DS1307 RTC via TN4 after NTP sync, and
 * reports network status to T8 via Q5.
 *
 * ── WiFi credentials (NVS namespace "wifi") ────────────────────────────────
 *  Key         Type    Default   Meaning
 *  ssid        str     ""        Station SSID; empty disables client mode
 *  psk         str     ""        Station passphrase (empty = open network)
 *  ap_enable   i32     0         1 = start soft-AP; 0 = stop soft-AP
 *  ap_psk      str     ""        Soft-AP passphrase (empty = open AP)
 *
 * ── Client state machine ───────────────────────────────────────────────────
 *  NET_IDLE        No SSID configured; station disabled.
 *  NET_CONNECTING  WiFi.begin() called; polling for WL_CONNECTED.
 *                  Timeout 30 s → NET_BACKOFF.
 *  NET_CONNECTED   WL_CONNECTED; configTime() running.
 *  NET_RUNNING     Client up; NTP result resolved (success or timeout).
 *  NET_BACKOFF     Connection lost; waiting exponential backoff then retry.
 *                  Backoff sequence: 2 → 4 → 8 → 16 → 32 → 60 s (cap).
 *
 * ── AP mode ────────────────────────────────────────────────────────────────
 *  AP mode is independent of client state (WIFI_AP_STA).
 *  AP SSID: "Greenhouse-XXYY" (last two bytes of MAC address).
 *  AP auto-shutdown: cfg.ap_timeout_min (from MX4, 0 = never shutdown).
 *  T10 polls NVS "wifi/ap_enable" every NET_POLL_MS; starts or stops AP when
 *  the flag changes.  T8 or T11 enable AP by writing Q4 {ns="wifi",
 *  key="ap_enable", value=1}; T4 persists it to NVS; T10 reads it here.
 *
 * ── NTP synchronisation ────────────────────────────────────────────────────
 *  On WL_CONNECTED: configTime(0, 0, "pool.ntp.org").
 *  Poll time(NULL) up to NTP_WAIT_STEPS × 1 s for a plausible timestamp
 *  (> 1 700 000 000 = 2023-11-14, safely below any real current date).
 *  On success: xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits).
 *  T4 then calls rtc_set_time() under MX1 to update the DS1307.
 *
 * ── Q5 status posting ──────────────────────────────────────────────────────
 *  xQueueOverwrite(Q5, &status) on every state change.
 *  Fields: client_connected, ap_active, ip_str (STA IP or "" if not connected).
 *
 * @author  Greenhouse Controller project
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <Arduino.h>
#include <esp_log.h>
#include <time.h>
#include <string.h>

#include <WiFi.h>      /* Arduino-ESP32 WiFi */

#include "network_manager.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "nvs_config.h"

static const char *TAG = "T10_NET";

/* ============================================================
 * Compile-time constants
 * ============================================================ */
#define NET_POLL_MS          5000u   /**< Main loop tick (ms) */
#define STA_CONNECT_TIMEOUT_MS 30000u/**< Time to wait for WL_CONNECTED */
#define NTP_WAIT_STEPS          30   /**< Seconds to poll for NTP sync */
#define NTP_MIN_EPOCH  1700000000L   /**< Plausibility threshold (2023-11-14) */
#define BACKOFF_INIT_MS   2000u      /**< Initial reconnect backoff */
#define BACKOFF_MAX_MS   60000u      /**< Maximum reconnect backoff */

/** Default WPA2 passphrase for the soft-AP.
 *  Stored as plaintext in NVS (wifi/ap_psk); WPA2 requires the raw key.
 *  Configurable by the administrator via the web interface (T11).
 *  See TSDS §5.x WiFi AP mode. */
#define AP_PSK_DEFAULT  "0123456789"

/* ============================================================
 * Client FSM states
 * ============================================================ */
typedef enum {
    NET_IDLE,        /**< No SSID configured */
    NET_CONNECTING,  /**< WiFi.begin() called; awaiting WL_CONNECTED */
    NET_CONNECTED,   /**< WL_CONNECTED; running NTP sync inline */
    NET_RUNNING,     /**< Fully up; monitoring for drops */
    NET_BACKOFF,     /**< Lost connection; waiting before retry */
} net_client_state_t;

/* ============================================================
 * Module state
 * ============================================================ */
static net_client_state_t s_state        = NET_IDLE;
static bool               s_ap_active    = false;
static bool               s_ntp_synced   = false;
static uint32_t           s_backoff_ms   = BACKOFF_INIT_MS;
static TickType_t         s_conn_start   = 0;   /**< Tick when NET_CONNECTING entered */
static TickType_t         s_ap_started   = 0;   /**< Tick when AP was started */
static int32_t            s_ap_enabled_nvs = 0; /**< Last-known NVS ap_enable value */

/* WiFi credentials (read from NVS at startup) */
static char s_ssid[64]   = {0};
static char s_psk[64]    = {0};
static char s_ap_psk[64] = {0};
static char s_ap_ssid[24] = {0};  /**< "Greenhouse-XXYY", built from MAC */

/* ============================================================
 * Helpers
 * ============================================================ */

static void post_q5(bool client_conn, bool ap_active, const char *ip)
{
    net_status_t st;
    st.client_connected = client_conn;
    st.ap_active        = ap_active;
    snprintf(st.ip_str, sizeof(st.ip_str), "%s", ip ? ip : "");
    xQueueOverwrite(Q5, &st);
}

static void log_sys(int16_t value_a, int16_t value_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = LOG_SYSTEM;
    ev.initiator  = LOG_BY_SYSTEM;
    ev.value_a    = value_a;
    ev.value_b    = value_b;
    log_post(&ev);
}

/* ============================================================
 * AP management
 * ============================================================ */

static void start_ap(void)
{
    if (s_ap_active) return;

    /* Use NVS password if set; fall back to factory default (never open AP). */
    const char *psk = (s_ap_psk[0] != '\0') ? s_ap_psk : AP_PSK_DEFAULT;
    bool ok = WiFi.softAP(s_ap_ssid, psk, /*channel=*/1,
                          /*hidden=*/0, /*max_conn=*/4);
    if (ok) {
        s_ap_active  = true;
        s_ap_started = xTaskGetTickCount();
        ESP_LOGI(TAG, "AP started: SSID='%s' IP=%s",
                 s_ap_ssid, WiFi.softAPIP().toString().c_str());
        log_sys(3, 1);   /* value_a=3: AP event; value_b=1: started */
        post_q5(s_state == NET_RUNNING, true,
                s_state == NET_RUNNING ? WiFi.localIP().toString().c_str() : "");
    } else {
        ESP_LOGE(TAG, "WiFi.softAP() failed");
    }
}

static void stop_ap(void)
{
    if (!s_ap_active) return;
    WiFi.softAPdisconnect(false);  /* false = leave radio up for STA */
    s_ap_active = false;
    ESP_LOGI(TAG, "AP stopped");
    log_sys(3, 0);   /* value_a=3: AP event; value_b=0: stopped */
    post_q5(s_state == NET_RUNNING, false,
            s_state == NET_RUNNING ? WiFi.localIP().toString().c_str() : "");
}

/**
 * @brief Check NVS "wifi/ap_enable" and start/stop AP accordingly.
 *        Also enforces AP auto-shutdown timer.
 */
static void poll_ap(void)
{
    /* ---- ap_enable flag ---- */
    int32_t ap_en = 0;
    nvs_cfg_get_i32(NVS_NS_WIFI, "ap_enable", &ap_en);

    if (ap_en != s_ap_enabled_nvs) {
        s_ap_enabled_nvs = ap_en;
        if (ap_en) {
            start_ap();
        } else {
            stop_ap();
        }
    }

    /* ---- Auto-shutdown ---- */
    if (s_ap_active) {
        cfg_shadow_t cfg;
        dm_cfg_snapshot(&cfg);
        if (cfg.ap_timeout_min > 0) {
            uint32_t elapsed_ms = (uint32_t)(
                (xTaskGetTickCount() - s_ap_started) * portTICK_PERIOD_MS);
            uint32_t limit_ms = (uint32_t)cfg.ap_timeout_min * 60u * 1000u;
            if (elapsed_ms >= limit_ms) {
                ESP_LOGI(TAG, "AP auto-shutdown after %ld min",
                         (long)cfg.ap_timeout_min);
                /* Clear the NVS flag so it doesn't restart on next poll */
                nvs_cfg_set_i32(NVS_NS_WIFI, "ap_enable", 0);
                s_ap_enabled_nvs = 0;
                stop_ap();
            }
        }
    }
}

/* ============================================================
 * NTP sync (called once when WL_CONNECTED fires)
 * ============================================================ */

static void run_ntp_sync(void)
{
    ESP_LOGI(TAG, "Starting NTP sync (pool.ntp.org)");
    configTime(0, 0, "pool.ntp.org");

    for (int i = 0; i < NTP_WAIT_STEPS; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (time(NULL) > NTP_MIN_EPOCH) {
            break;
        }
    }

    if (time(NULL) > NTP_MIN_EPOCH) {
        s_ntp_synced = true;
        ESP_LOGI(TAG, "NTP sync OK — unix=%lu", (unsigned long)time(NULL));
        /* Notify T4 to write the new time to the DS1307 */
        xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
        log_sys(2, 1);   /* value_a=2: NTP event; value_b=1: synced */
    } else {
        ESP_LOGW(TAG, "NTP sync timeout after %d s", NTP_WAIT_STEPS);
        log_sys(2, 0);   /* value_a=2: NTP event; value_b=0: timeout */
    }
}

/* ============================================================
 * Client state-machine step — called every NET_POLL_MS
 * Returns the delay to apply before the next call (ms).
 * ============================================================ */

static uint32_t step_client(void)
{
    wl_status_t wifi_st = WiFi.status();

    switch (s_state) {

        /* ── NET_IDLE ─────────────────────────────────────────── */
        case NET_IDLE:
            /* Nothing to do; re-check SSID periodically in case it was
             * written to NVS by the web server after first boot. */
            nvs_cfg_get_str(NVS_NS_WIFI, "ssid", s_ssid, sizeof(s_ssid));
            if (s_ssid[0] != '\0') {
                nvs_cfg_get_str(NVS_NS_WIFI, "psk", s_psk, sizeof(s_psk));
                ESP_LOGI(TAG, "SSID now configured — connecting to '%s'", s_ssid);
                WiFi.begin(s_ssid, s_psk[0] ? s_psk : nullptr);
                s_conn_start = xTaskGetTickCount();
                s_state      = NET_CONNECTING;
            }
            return NET_POLL_MS;

        /* ── NET_CONNECTING ───────────────────────────────────── */
        case NET_CONNECTING:
            if (wifi_st == WL_CONNECTED) {
                ESP_LOGI(TAG, "WiFi connected: IP=%s RSSI=%d dBm",
                         WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
                log_sys(1, 1);   /* value_a=1: STA event; value_b=1: connected */
                s_state = NET_CONNECTED;
                /* Post Q5 immediately so T8 can show IP before NTP wait */
                post_q5(true, s_ap_active, WiFi.localIP().toString().c_str());
                /* Run NTP sync inline (blocks up to NTP_WAIT_STEPS s) */
                run_ntp_sync();
                s_state      = NET_RUNNING;
                s_backoff_ms = BACKOFF_INIT_MS;   /* Reset backoff on success */
                post_q5(true, s_ap_active, WiFi.localIP().toString().c_str());

            } else {
                uint32_t elapsed_ms = (uint32_t)(
                    (xTaskGetTickCount() - s_conn_start) * portTICK_PERIOD_MS);
                if (elapsed_ms >= STA_CONNECT_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "WiFi connect timeout — backoff %lu ms",
                             (unsigned long)s_backoff_ms);
                    s_state = NET_BACKOFF;
                }
            }
            return NET_POLL_MS;

        /* ── NET_CONNECTED ────────────────────────────────────── */
        case NET_CONNECTED:
            /* Transient state — handled inline in NET_CONNECTING branch above.
             * Should not normally be entered from the loop directly. */
            s_state = NET_RUNNING;
            return NET_POLL_MS;

        /* ── NET_RUNNING ──────────────────────────────────────── */
        case NET_RUNNING:
            if (wifi_st != WL_CONNECTED) {
                ESP_LOGW(TAG, "WiFi lost (status=%d) — backoff %lu ms",
                         (int)wifi_st, (unsigned long)s_backoff_ms);
                log_sys(1, 0);   /* value_a=1: STA event; value_b=0: disconnected */
                s_ntp_synced = false;
                s_state      = NET_BACKOFF;
                post_q5(false, s_ap_active, "");
            }
            return NET_POLL_MS;

        /* ── NET_BACKOFF ──────────────────────────────────────── */
        case NET_BACKOFF: {
            uint32_t delay = s_backoff_ms;
            /* Double backoff for next time (capped) */
            s_backoff_ms = (s_backoff_ms < BACKOFF_MAX_MS)
                           ? (s_backoff_ms * 2u) : BACKOFF_MAX_MS;

            /* Re-read credentials in case they were updated via web server */
            nvs_cfg_get_str(NVS_NS_WIFI, "ssid", s_ssid, sizeof(s_ssid));
            nvs_cfg_get_str(NVS_NS_WIFI, "psk",  s_psk,  sizeof(s_psk));

            if (s_ssid[0] == '\0') {
                ESP_LOGI(TAG, "SSID cleared — back to IDLE");
                WiFi.disconnect(false);
                s_state = NET_IDLE;
                return NET_POLL_MS;
            }

            ESP_LOGI(TAG, "Reconnecting to '%s' in %lu ms", s_ssid,
                     (unsigned long)delay);
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(delay));
            WiFi.begin(s_ssid, s_psk[0] ? s_psk : nullptr);
            s_conn_start = xTaskGetTickCount();
            s_state      = NET_CONNECTING;
            return NET_POLL_MS;
        }
    }
    return NET_POLL_MS;
}

/* ============================================================
 * T10 task entry point
 * ============================================================ */

void task_network_manager(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "T10 task alive");

    /* ── Read WiFi credentials from NVS ── */
    nvs_cfg_get_str_or_default(NVS_NS_WIFI, "ssid",   "", s_ssid,   sizeof(s_ssid));
    nvs_cfg_get_str_or_default(NVS_NS_WIFI, "psk",    "", s_psk,    sizeof(s_psk));
    nvs_cfg_get_str_or_default(NVS_NS_WIFI, "ap_psk", AP_PSK_DEFAULT, s_ap_psk, sizeof(s_ap_psk));

    ESP_LOGI(TAG, "NVS WiFi: ssid='%s' psk=%s",
             s_ssid[0] ? s_ssid : "(empty)",
             s_psk[0]  ? "***"  : "(empty)");

    /* ── Set WiFi mode (AP+STA allows both simultaneously) ── */
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);  /* T10 manages reconnection itself */

    /* ── Build AP SSID from last 2 MAC bytes ── */
    {
        uint8_t mac[6] = {0};
        WiFi.macAddress(mac);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid),
                 "Greenhouse-%02X%02X", mac[4], mac[5]);
        ESP_LOGI(TAG, "AP SSID will be '%s'", s_ap_ssid);
    }

    /* ── Read initial ap_enable from NVS ── */
    nvs_cfg_get_i32_or_default(NVS_NS_WIFI, "ap_enable", 0, &s_ap_enabled_nvs);
    if (s_ap_enabled_nvs) {
        start_ap();
    }

    /* ── Start client if SSID is configured ── */
    if (s_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Connecting to SSID '%s'", s_ssid);
        WiFi.begin(s_ssid, s_psk[0] ? s_psk : nullptr);
        s_conn_start = xTaskGetTickCount();
        s_state      = NET_CONNECTING;
    } else {
        ESP_LOGI(TAG, "No SSID configured — station disabled");
        s_state = NET_IDLE;
    }

    /* Initial Q5 post */
    post_q5(false, s_ap_active, "");

    /* ── Main loop ── */
    for (;;) {
        /* 1. AP management (start/stop/timeout) */
        poll_ap();

        /* 2. Client state machine step */
        uint32_t delay_ms = step_client();

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @file wifi_tickle.h
 * @brief Phase-3 ESP-IDF WiFi tickle — IDF-native STA connect + SNTP sync.
 *
 * Self-contained validation module for the ESP-IDF migration's Phase 3
 * (2.0.0-alpha.3). Exercises:
 *   - esp_netif initialisation + default STA netif
 *   - esp_event default loop + WIFI_EVENT / IP_EVENT handler registration
 *   - esp_wifi STA mode bring-up
 *   - WIFI_EVENT_STA_START → esp_wifi_connect() driven by the event handler
 *     (NOT a polling loop — structurally precludes the gh#21 lwIP-init race
 *     that bit the arduino-era code)
 *   - IP_EVENT_STA_GOT_IP → log IP, kick off SNTP
 *   - esp_sntp (SNTP_OPMODE_POLL, pool.ntp.org) + wait for plausible epoch
 *
 * Replaces:
 *   - `WiFi.h` style: `WiFi.begin(ssid, psk)` + `WiFi.status()` polling
 *   - `configTime("pool.ntp.org")` Arduino wrapper
 *
 * Defers:
 *   - Soft-AP bring-up (Phase 6 — part of the full task_network_manager port)
 *   - Backoff state machine (Phase 6)
 *   - Geo/timezone HTTP fetch (Phase 4 — uses esp_http_client, same path
 *     as the gh#23 status-post fix)
 *   - Q5 net_status_t posting (Phase 6 — depends on data_manager being live)
 *   - Periodic 24-hour NTP resync (Phase 6 — part of long-running task loop)
 *
 * Credentials are read from NVS via LIB-7 (alpha.2.3) — keys
 * `wifi/ssid` and `wifi/psk`. These survive across firmware reflashes
 * because NVS lives on its own partition.
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return status from the WiFi tickle run. */
typedef enum {
    WIFI_TICKLE_OK              = 0, /**< Connected, got IP, SNTP synced */
    WIFI_TICKLE_OK_NO_NTP       = 1, /**< Connected, got IP, but SNTP timed out */
    WIFI_TICKLE_NO_SSID         = 2, /**< NVS wifi/ssid is empty — skipped */
    WIFI_TICKLE_INIT_FAILED     = 3, /**< esp_netif/esp_wifi init returned error */
    WIFI_TICKLE_CONNECT_TIMEOUT = 4, /**< STA_START fired but no STA_GOT_IP within budget */
    WIFI_TICKLE_DISCONNECTED    = 5, /**< STA_DISCONNECTED before STA_GOT_IP */
} wifi_tickle_status_t;

/**
 * @brief Run the Phase-3 WiFi acceptance tickle.
 *
 * Blocking. Reads SSID/PSK from NVS, brings up STA mode, waits for the
 * `IP_EVENT_STA_GOT_IP` event, then runs SNTP for up to ~5 s.
 *
 * Stays connected after returning (does NOT call esp_wifi_stop). The
 * heartbeat task that runs after this call can then use the network if it
 * wants (currently doesn't — Phase 4 will introduce status-post traffic).
 *
 * @param connect_timeout_ms  How long to wait for IP_EVENT_STA_GOT_IP.
 *                            Per the migration plan: < 5 s expected.
 *                            Recommend 10000 (10 s) as a defensive cap.
 * @return wifi_tickle_status_t outcome.
 */
wifi_tickle_status_t wifi_tickle_run(uint32_t connect_timeout_ms);

#ifdef __cplusplus
}
#endif

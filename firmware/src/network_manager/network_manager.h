/**
 * @file network_manager.h
 * @brief T10 — Network Manager task declaration + WiFi init helper.
 *
 * Manages WiFi AP and client lifecycle, triggers NTP sync, updates the
 * DS1307 RTC after NTP sync, and posts net_status_t to Q5.
 *
 * Also owns the blocking boot-time WiFi STA bring-up entry-point
 * `nm_wifi_init_blocking()`, folded in from the retired `wifi_tickle.cpp`
 * scaffold in 2.0.0-rc.1.3. Same logic, same boot ordering — the function
 * just lives in T10's home now.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return status from `nm_wifi_init_blocking()`. */
typedef enum {
    NM_WIFI_OK              = 0, /**< Connected, got IP, SNTP synced */
    NM_WIFI_OK_NO_NTP       = 1, /**< Connected, got IP, but SNTP timed out */
    NM_WIFI_NO_SSID         = 2, /**< NVS wifi/ssid is empty — stack up, STA-connect skipped */
    NM_WIFI_INIT_FAILED     = 3, /**< esp_netif/esp_wifi init returned error */
    NM_WIFI_CONNECT_TIMEOUT = 4, /**< STA_START fired but no STA_GOT_IP within budget */
    NM_WIFI_DISCONNECTED    = 5, /**< STA_DISCONNECTED before STA_GOT_IP */
} nm_wifi_status_t;

/**
 * @brief Blocking boot-time WiFi STA bring-up + SNTP sync.
 *
 * Folded in from the retired `wifi_tickle.cpp` (2.0.0-rc.1.3). Reads
 * SSID/PSK from NVS via LIB-7, brings up STA mode, registers the long-lived
 * WIFI_EVENT/IP_EVENT handler (the same handler stays alive after this
 * returns and drives subsequent reconnects via the exponential-backoff
 * timer added in alpha.6.31), waits for `IP_EVENT_STA_GOT_IP`, then runs
 * SNTP for up to ~10 s.
 *
 * Stays connected after returning. Called once from `app_main` BEFORE
 * `task_network_manager` is spawned — T10's monitoring loop assumes WiFi
 * init has already happened.
 *
 * @param connect_timeout_ms  How long to wait for IP_EVENT_STA_GOT_IP.
 *                            Recommend 10000 (10 s) — same value the old
 *                            `wifi_tickle_run()` used.
 * @return nm_wifi_status_t outcome.
 */
nm_wifi_status_t nm_wifi_init_blocking(uint32_t connect_timeout_ms);

/**
 * @brief T10 — Network Manager task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_network_manager(void *pvParameters);

#ifdef __cplusplus
}
#endif

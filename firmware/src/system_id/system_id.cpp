/**
 * @file system_id.cpp
 * @brief Unit identifier helper — implementation (gh#17, since 1.18.3).
 *
 * Reads the WiFi-STA factory MAC via `esp_read_mac()` (works before WiFi is
 * initialised, unlike `WiFi.macAddress()` which requires the stack to be up).
 * Caches the result in a static so subsequent calls are O(1).
 *
 * Thread safety: the cache is lazy-initialised on first call. Subsequent
 * loads of `s_cached` are aligned 16-bit reads, racy-but-safe — the same
 * value would be computed on a concurrent first-call race.
 *
 * @author Greenhouse Controller project
 */

#include "system_id.h"

#include <esp_mac.h>      /* esp_read_mac, ESP_MAC_WIFI_STA */
#include <stdio.h>

/** @brief Cached 16-bit unit ID — set once by `load_unit_id()` then immutable. */
static uint16_t s_cached  = 0u;
/** @brief First-call gate for `load_unit_id()` so the eFuse read happens once. */
static bool     s_inited  = false;

/**
 * @brief Lazy-load the unit ID from the WiFi-STA factory MAC.
 *
 * No-op after the first successful call. Reads bytes 4 and 5 of the
 * eFuse-burned MAC; high byte is `mac[4]`, low byte is `mac[5]`.
 *
 * @note ESP_MAC_WIFI_STA matches what `WiFi.macAddress()` returns later —
 *       keeping the two values aligned means the on-AP SSID
 *       (`Greenhouse-XXXX`) and the on-log unit ID are always the same
 *       four hex chars.
 */
static void load_unit_id(void)
{
    if (s_inited) {
        return;
    }
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_cached = ((uint16_t)mac[4] << 8) | (uint16_t)mac[5];
    s_inited = true;
}

uint16_t system_unit_id_u16(void)
{
    load_unit_id();
    return s_cached;
}

void system_unit_id_str(char *buf, size_t cap)
{
    load_unit_id();
    if (buf == NULL || cap < 5u) {
        return;
    }
    /* snprintf with %04X always writes exactly 4 hex chars + NUL for any
     * uint16 value. cap >= 5 is enforced above. */
    (void)snprintf(buf, cap, "%04X", (unsigned)s_cached);
}

void system_mac_str(char *buf, size_t cap)
{
    if (buf == NULL || cap < 13u) {
        return;
    }
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);   /* same source as the unit id */
    (void)snprintf(buf, cap, "%02x%02x%02x%02x%02x%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @file system_id.cpp
 * @brief Unit identifier helper — implementation (gh#17, since 1.18.3).
 *
 * Reads the WiFi-STA factory MAC via `esp_read_mac()` (works before WiFi is
 * initialised, unlike `WiFi.macAddress()` which requires the stack to be up).
 * Caches the result in a static so subsequent calls are O(1).
 */

#include "system_id.h"

#include <esp_mac.h>      /* esp_read_mac, ESP_MAC_WIFI_STA */
#include <stdio.h>

static uint16_t s_cached  = 0u;
static bool     s_inited  = false;

static void load_unit_id(void)
{
    if (s_inited) {
        return;
    }
    uint8_t mac[6] = {0};
    /* ESP_MAC_WIFI_STA matches what WiFi.macAddress() returns later — keeping
     * the two values aligned means the on-AP SSID and the on-log unit ID are
     * always the same four hex chars. */
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

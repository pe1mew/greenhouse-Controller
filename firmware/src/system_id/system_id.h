/**
 * @file system_id.h
 * @brief Unit identifier derived from the factory-burned WiFi MAC (gh#17).
 *
 * Surfaces a per-unit ID that:
 *  - survives factory reset (it's burned into the chip's eFuse — no NVS dependency)
 *  - matches the existing AP-SSID convention `Greenhouse-XXXX` (last 2 MAC bytes)
 *  - is available before WiFi is initialised (uses `esp_read_mac()` directly)
 *
 * For the project's expected fleet size (≤ tens of units) the 16-bit ID has
 * negligible collision probability — see `firmware/issues.md` and the
 * evaluation note in the gh#17 thread for the math. If the fleet ever exceeds
 * ~50 units, widen this to a 24-bit ID (last 3 MAC bytes) — the call sites
 * use the public functions below so the upgrade is localised here.
 *
 * Thread-safety: the cached value is computed lazily on first call and never
 * mutates afterwards, so all subsequent reads are racy-but-safe (single
 * aligned 16-bit load).
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the unit ID as a `uint16_t` formed from MAC bytes 4 and 5.
 *
 * The high byte is `mac[4]`, the low byte is `mac[5]`. Same numeric ordering
 * as the AP-SSID format `Greenhouse-%02X%02X` produced by the network
 * manager.
 */
uint16_t system_unit_id_u16(void);

/**
 * @brief Write the unit ID as a 4-character hex string into @p buf.
 *
 * Format: `"AABB"` (uppercase, fixed width, no separator). The caller-supplied
 * buffer must be at least 5 bytes (4 chars + NUL). On error (NULL buf or
 * cap < 5) the function is a no-op.
 */
void system_unit_id_str(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

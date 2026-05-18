/**
 * @file time_compat.h
 * @brief Arduino → ESP-IDF time-source compatibility shim.
 *
 * Provides `millis_idf()` and `micros_idf()` as drop-in replacements for the
 * Arduino-era `millis()` and `micros()` calls used throughout the firmware.
 *
 * Why a shim and not a per-file s/millis()/(uint32_t)(esp_timer_get_time()/1000)/?
 *   - Readability: the call sites carry the same shape as before, only the
 *     identifier changes from `millis()` to `millis_idf()`. Cuts noise out
 *     of the diffs that activate each subsystem.
 *   - Consistency: a single point of truth for the conversion (esp_timer_get_time
 *     returns int64 microseconds; the shim picks 32-bit ms because that's
 *     what every existing caller expects).
 *   - Hardening: the shim can grow to add wraparound-safe arithmetic helpers
 *     later (e.g. `time_elapsed_ms(uint32_t since)`) without re-touching
 *     every callsite.
 *
 * The shim adds zero runtime overhead — `static inline` + a single 64-bit
 * divide that GCC optimises into a multiply-by-magic-number on Xtensa.
 *
 * Use:
 *     #include "util/time_compat.h"
 *     uint32_t t0 = millis_idf();
 *     // ... work ...
 *     uint32_t elapsed = millis_idf() - t0;
 *
 * Wraparound: the 32-bit ms counter wraps every ~49.7 days. All existing
 * call sites use the canonical `(now - then) >= timeout` form which is
 * wraparound-safe per the C standard's unsigned-subtraction semantics.
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdint.h>
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Milliseconds since boot, wrapping at 2^32 (~49.7 days).
 *
 * Drop-in replacement for arduino-era `millis()`. Resolution: 1 ms.
 *
 * @return Free-running millisecond counter.
 */
static inline uint32_t millis_idf(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

/**
 * @brief Microseconds since boot, wrapping at 2^32 (~71.6 minutes).
 *
 * Drop-in replacement for arduino-era `micros()`. Resolution: 1 µs.
 *
 * NOTE: the underlying `esp_timer_get_time()` returns int64 µs and never
 * wraps in practical timeframes; the cast to uint32 here exists to match
 * arduino's return type. Callers that need long durations should use the
 * 64-bit `esp_timer_get_time()` directly.
 *
 * @return Free-running microsecond counter (truncated to 32 bits).
 */
static inline uint32_t micros_idf(void)
{
    return (uint32_t)esp_timer_get_time();
}

#ifdef __cplusplus
}
#endif

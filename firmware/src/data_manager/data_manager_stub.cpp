/**
 * @file data_manager_stub.cpp
 * @brief Phase-6.6 stub layer for T4 functions called by other tasks before
 *        T4 itself is activated.
 *
 * Phase-6 brings task subsystems online incrementally. T9 (event_logger,
 * Phase 6.6) needs `dm_get_unix_time()` for log timestamps. T4 (data_manager)
 * activates later (Phase 6.7+) because it has its own dependencies on
 * Q4 / MX1 / MX2 / MX3 / RTC sync that require more plumbing. To break the
 * circular dep (T4 calls log_post → Q3 → T9; T9 calls dm_get_unix_time
 * which lives in T4), this stub provides the minimum API the active build
 * needs, backed by stdlib `time()`.
 *
 * `time(NULL)` is fed by:
 *   - SNTP (Phase 3 wifi_tickle's `esp_netif_sntp_*` call sets the system
 *     clock when a successful sync arrives)
 *   - The DS1307 RTC's stored value (1.20.3 production firmware on Unit 2
 *     has been keeping wall-clock alive in the battery-backed chip; once
 *     T4 activates it will set the libc clock from the RTC at boot)
 *
 * Until T4 fully activates, `time(NULL)` returns:
 *   - boot uptime in seconds (no SNTP yet → log timestamps are useless but
 *     non-crashing — graceful failure)
 *   - real Unix epoch after SNTP sync (most boots so far)
 *
 * **REMOVE THIS FILE WHEN THE REAL `data_manager.cpp` ACTIVATES.** The full
 * T4 port will provide its own dm_get_unix_time() via the cfg shadow's
 * current_unix_ts field under MX4. The linker will refuse to take two
 * definitions, so the stub removal is forcing.
 *
 * @author Greenhouse Controller project
 */

#include <time.h>
#include <stdint.h>

#include "data_manager.h"

/* C++ linkage — matches the declaration in data_manager.h (which lives at
 * file scope outside any `extern "C"` block, so the declared name is
 * mangled with the C++ ABI). When the real T4 ports in Phase 6.7+, its
 * definition must also use C++ linkage; the linker will then refuse to
 * coexist with this stub, forcing its removal. */
uint32_t dm_get_unix_time(void)
{
    return (uint32_t)time(NULL);
}

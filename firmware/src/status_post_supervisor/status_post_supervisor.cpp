/**
 * @file status_post_supervisor.cpp
 * @brief T15 — Status-POST supervisor implementation (gh#18 Phase 4).
 *
 * See the header for design overview. Implementation notes:
 *
 *  - Tick interval: 30 s. Long enough that the supervisor itself imposes
 *    negligible CPU cost; short enough that a 60-s wedge window is
 *    detected within two ticks worst-case.
 *
 *  - Respawn budget: 1 per 5 min, 10 per hour. Both are NVS-persisted
 *    (`t15_respawn_h` rolls at HH:00 boundary). Exhausting either budget
 *    escalates to a planned reboot.
 *
 *  - Heap-drop budget: 64 KB cumulative since boot. T14 samples the delta
 *    around every HTTPS call; the supervisor only reads the accumulator
 *    via `status_post_heap_drop_bytes()`.
 *
 *  - Planned reboot: persists `t15_planreboot = 1`, then calls
 *    `esp_restart()`. Phase 3's window-state recovery means the next boot
 *    is ~2 s instead of ~171 s. The boot-reason recorded in the next boot
 *    is `ESP_RST_SW` (= 3), distinguishable from `ESP_RST_PANIC` (= 4) /
 *    `ESP_RST_INT_WDT` (= 5).
 *
 *  - Force-respawn: calls `status_post_force_teardown()` first (idempotent
 *    close of the persistent TLS session — Phase 1's static
 *    `WiFiClientSecure` survives `vTaskDelete` and would otherwise leak),
 *    then `vTaskDelete(task_t14)`, a 100 ms delay so FreeRTOS reclaims the
 *    stack, then `xTaskCreatePinnedToCore()` with the same arguments
 *    main.cpp used at first boot. Stores the new handle into `task_t14`.
 *
 * @warning This translation unit is currently **excluded from the build**
 *          (not listed in `firmware/src/CMakeLists.txt` SRCS). The source
 *          and documentation reflect the intended-future-state interface;
 *          re-enable by adding the file back to SRCS when T14 leak/wedge
 *          telemetry warrants resuming supervision.
 *
 * @see status_post_supervisor.h
 *
 * @author Greenhouse Controller project
 */

#include "status_post_supervisor.h"

#include "../types/app_types.h"
#include "../status_post/status_post.h"
#include "../event_logger/event_logger.h"     /* gh#26: SD unmount before reset */
#include "../../../drivers/nvs/src/nvs_config.h"

#include <Arduino.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_system.h>     /* esp_restart() */
#include <time.h>

static const char *TAG = "T15";

/* ============================================================
 * Public symbol — defined here to keep the supervisor self-contained.
 * (Avoiding a definition in main.cpp also means a future build that omits
 * T15 from the link doesn't drag an unused TaskHandle_t along.)
 * ============================================================ */

/** @brief Public handle for the supervisor task (extern in app_types.h). */
TaskHandle_t task_t15 = NULL;

/* ============================================================
 * Compile-time tunables
 * ============================================================ */

/** @brief Polling cadence in ms — supervisor evaluates T14 health every 30 s. */
#define T15_TICK_MS              30000u

/** @brief Sleep chunk between WDT kicks (ms).
 *
 * Must be strictly less than the task-WDT timeout — ESP-IDF default is
 * 5 s; 1 s gives 5× safety margin. gh#19 fix; pre-fix the 30 s vTaskDelay
 * between kicks caused TASK_WDT resets every ~5 s of uptime and triple-
 * boot OTA rollback on Unit 1 (1.18.0).
 */
#define T15_WDT_KICK_CHUNK_MS     1000u

/** @brief Heartbeat must advance within this window or T14 is considered wedged (ms). */
#define T15_WEDGE_TIMEOUT_MS     60000u

/** @brief Cumulative heap-drop limit (bytes) before T15 escalates to a planned reboot. */
#define T15_HEAP_DROP_LIMIT      (64u * 1024u)

/** @brief Minimum elapsed time between respawns (ms). Less → respawn storm → planned reboot. */
#define T15_MIN_RESPAWN_GAP_MS  (5u * 60u * 1000u)

/** @brief Maximum respawns per local-time hour. Exhaustion → planned reboot. */
#define T15_HOURLY_RESPAWN_LIMIT 10u

/* Match main.cpp's spawn arguments for T14. Re-declared here rather than
 * shared in a header so a typo in main.cpp doesn't silently desync the
 * respawn parameters. If the canonical values change, both call sites need
 * to be updated. */

/** @brief T14 task stack in bytes — must match main.cpp's xTaskCreate value. */
#define T14_STACK_BYTES          12288u

/** @brief T14 task priority — TASK_PRIO_LOW in main.cpp. */
#define T14_PRIORITY             3u

/** @brief T14 task core affinity (0 = Core 0). */
#define T14_CORE                 0u

/* NVS keys (namespace NVS_NS_SYSTEM). */

/** @brief NVS key: respawn count within the current local-time hour. */
static const char K_RESPAWN_H[]   = "t15_respawn_h";

/** @brief NVS key: local-time hour-of-day (0..23) at which the count was reset. */
static const char K_RESPAWN_HR[]  = "t15_resp_hr";

/** @brief NVS key: planned-reboot indicator (1 = next boot resumes from planned reboot). */
static const char K_PLAN_REBOOT[] = "t15_planreboot";

/* ============================================================
 * Module-private state
 * ============================================================ */

/** @brief Last observed T14 heartbeat value (set when it advances). */
static uint32_t s_last_heartbeat       = 0u;

/** @brief Tick (ms) at which `s_last_heartbeat` was set — wedge timer reference. */
static uint32_t s_last_heartbeat_tick  = 0u;

/** @brief Tick (ms) of the last respawn — minimum-gap budget reference. */
static uint32_t s_last_respawn_tick    = 0u;

/** @brief Respawn count within the current local-time hour. */
static uint32_t s_respawn_this_hour    = 0u;

/** @brief Local-time hour 0..23 the hourly counter belongs to (-1 = pre-NTP). */
static int      s_respawn_hour_marker  = -1;

/** @brief True if this boot resumed from a planned reboot — cleared after T14 recovers. */
static bool     s_was_planned_reboot   = false;

/* Forward declaration of the T14 entry — we re-spawn it here. */
extern "C" void task_status_post(void *pvParameters);

bool supervisor_was_planned_reboot(void)
{
    return s_was_planned_reboot;
}

/**
 * @brief Persist the planned-reboot flag, unmount SD, and `esp_restart()`.
 *
 * Sets `NVS_NS_SYSTEM/t15_planreboot=1` so the next boot can surface the
 * "planned reboot recovery" indicator in the status JSON. Unmounts the SD
 * card (gh#26) so FatFs flushes its write-back cache before reset —
 * pre-fix, planned reboots left phantom directory entries and zero-byte
 * ghost CSVs (Unit 1 1.20.0 forensic window). A 250 ms delay lets the
 * NVS commit and UART log lines flush.
 *
 * @param reason  Free-form human-readable cause; emitted at ERROR level.
 * @note  Does not return — calls `esp_restart()`.
 */
static void planned_reboot(const char *reason)
{
    ESP_LOGE(TAG, "PLANNED REBOOT — %s", reason);
    (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, K_PLAN_REBOOT, 1);

    /* gh#26 (1.20.2): unmount the SD card cleanly before esp_restart() so
     * the Arduino-ESP32 SD library / FatFs flushes its directory cache and
     * write-back queue to physical media. Pre-fix, a planned reboot
     * discarded whatever was pending in the cache: phantom directory
     * entries, partially-rotated files, zero-byte ghost files. Observed on
     * Unit 1 (id=12F0) in the 1.20.0 forensic window — three planned
     * reboots and three CSVs (20260516025038, 20260516031506, 20260516041646)
     * that the controller logged creating but never landed on the card.
     * Pulls in the unmount call before the UART drain so the resulting
     * "[T9] SD unmounted via web request" log line also makes it out.
     * Tolerates already-unmounted state — event_logger_sd_unmount() is
     * idempotent. */
    event_logger_sd_unmount();

    /* Brief delay so the NVS commit lands and the ESP_LOGE / SD-unmount log
     * lines make it to the UART output buffer before reset. */
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

/**
 * @brief Roll the hourly respawn counter on the HH:00 boundary.
 *
 * Compares the current local-time hour against `s_respawn_hour_marker`;
 * if different, resets the hourly counter to zero and persists both values
 * to NVS. No-op before NTP sync (epoch < 2023-11-15) — we never want to
 * roll based on a stale boot-time clock.
 */
static void maybe_roll_hour_counter(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) { return; }   /* pre-NTP — leave the marker alone */
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_hour != s_respawn_hour_marker) {
        s_respawn_hour_marker = lt.tm_hour;
        s_respawn_this_hour   = 0u;
        (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, K_RESPAWN_H,  0);
        (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, K_RESPAWN_HR, (int32_t)lt.tm_hour);
    }
}

/**
 * @brief Force-respawn T14 after enforcing minimum-gap and hourly budgets.
 *
 * Order: budget checks (escalate to `planned_reboot()` if exceeded);
 * `status_post_force_teardown()` (idempotent TLS close);
 * `vTaskDelete(task_t14)`; 100 ms delay so FreeRTOS reaps the TCB and
 * stack; `xTaskCreatePinnedToCore()` with the same args main.cpp used;
 * persist the new hourly count; force-reset heartbeat tracking so the
 * wedge timer doesn't immediately re-fire before the new incarnation
 * gets a tick in.
 *
 * @param reason  Free-form human-readable cause; emitted at WARN level.
 * @note  May not return — escalation paths call `planned_reboot()` which
 *        in turn calls `esp_restart()`.
 */
static void respawn_t14(const char *reason)
{
    uint32_t now_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    /* Minimum-gap budget. */
    if (s_last_respawn_tick != 0u &&
        (now_tick - s_last_respawn_tick) < T15_MIN_RESPAWN_GAP_MS) {
        planned_reboot("T14 respawn rate exceeded (< 5 min since last)");
        return;
    }

    /* Hourly budget. */
    maybe_roll_hour_counter();
    if (s_respawn_this_hour >= T15_HOURLY_RESPAWN_LIMIT) {
        planned_reboot("T14 hourly respawn budget exhausted");
        return;
    }

    ESP_LOGW(TAG, "RESPAWNING T14 — %s", reason);

    /* Close the persistent TLS session so the next incarnation starts clean.
     * status_post_force_teardown() is idempotent. */
    status_post_force_teardown();

    /* Delete the task. The static module-private state in status_post.cpp
     * (breaker structs, heap-drop accumulator, heartbeat counter, secure
     * client) all survive the delete — only the task's stack and TCB are
     * reclaimed. The recreated task picks up exactly where the old one
     * left off in terms of breaker state. */
    if (task_t14 != NULL) {
        vTaskDelete(task_t14);
        task_t14 = NULL;
    }

    /* 100 ms is empirically enough for FreeRTOS's idle task to reap the
     * deleted task's resources on this Arduino-ESP32 config. */
    vTaskDelay(pdMS_TO_TICKS(100));

    BaseType_t rc = xTaskCreatePinnedToCore(
        task_status_post, "T14_WEB", T14_STACK_BYTES, NULL,
        T14_PRIORITY, &task_t14, T14_CORE);
    if (rc != pdPASS) {
        planned_reboot("xTaskCreate(T14) failed during respawn");
        return;
    }

    s_last_respawn_tick = now_tick;
    s_respawn_this_hour++;
    (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, K_RESPAWN_H,
                          (int32_t)s_respawn_this_hour);

    /* Force a heartbeat refresh so the wedge timer doesn't immediately
     * re-trigger before the new task gets its first iteration in. */
    s_last_heartbeat      = 0u;
    s_last_heartbeat_tick = now_tick;
}

/**
 * @brief Restore NVS-persisted supervisor state at task entry.
 *
 * Loads the hourly respawn counter (clamped to [0, T15_HOURLY_RESPAWN_LIMIT]),
 * the hour-of-day marker, and the planned-reboot flag. The planned-reboot
 * flag is held in RAM until cleared after T14 demonstrates one successful
 * POST (heartbeat advance + breaker closed).
 */
static void load_persisted_state(void)
{
    int32_t v = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_RESPAWN_H,  0, &v);
    if (v < 0)                              { v = 0; }
    if (v > (int32_t)T15_HOURLY_RESPAWN_LIMIT) { v = (int32_t)T15_HOURLY_RESPAWN_LIMIT; }
    s_respawn_this_hour = (uint32_t)v;

    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_RESPAWN_HR, -1, &v);
    s_respawn_hour_marker = (v >= 0 && v <= 23) ? (int)v : -1;

    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, K_PLAN_REBOOT, 0, &v);
    s_was_planned_reboot = (v != 0);
    if (s_was_planned_reboot) {
        ESP_LOGW(TAG, "boot resumed from PLANNED REBOOT — clearing flag "
                      "once T14 makes one successful POST");
    }
}

/* ============================================================
 * Task entry
 * ============================================================ */

/**
 * @brief T15 supervisor task body (see header for overview).
 *
 * Subscribes to the task WDT, restores NVS state, then loops forever with
 * a 30 s tick (broken into 1 s WDT-kick chunks per gh#19). Per tick:
 *  - (a) Wedge detector — compare `status_post_heartbeat()`; if unchanged
 *        for `T15_WEDGE_TIMEOUT_MS`, call `respawn_t14()`.
 *  - (b) Leak detector — if `status_post_heap_drop_bytes()` ≥ 64 KB,
 *        escalate to `planned_reboot()` (does not return).
 *  - (c) Planned-reboot housekeeping — clear the flag once T14 has
 *        demonstrated recovery (heartbeat advanced past 5 + breaker closed).
 *  - Roll the hourly respawn counter on local-time HH:00 boundary.
 *
 * @param pvParameters  Unused.
 */
void task_status_post_supervisor(void *pvParameters)
{
    (void)pvParameters;

    /* Watchdog subscription — same pattern as T1, T2 since 1.17.29. */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "T15 started — supervising T14 every %lu ms",
             (unsigned long)T15_TICK_MS);

    load_persisted_state();

    /* Seed the heartbeat tracking so we don't false-trigger on the first
     * tick if T14 hasn't been scheduled yet. */
    s_last_heartbeat      = status_post_heartbeat();
    s_last_heartbeat_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    for (;;) {
        /* gh#19 fix: break the 30-s polling delay into ≤ 1-s chunks so the
         * task watchdog (default 5-s timeout, kicked once per chunk) is
         * never starved. Pre-fix the 30-s vTaskDelay between WDT kicks
         * caused TASK_WDT resets every ~5 s of uptime, then OTA rollback
         * after three consecutive bad boots. Confirmed in the
         * Unit-1 1.18.0 NVS log (2026-05-14 08:02:18→08:02:41:
         * SW → TASK_WDT → TASK_WDT → SW). */
        uint32_t slept_ms = 0u;
        while (slept_ms < T15_TICK_MS) {
            esp_task_wdt_reset();
            uint32_t chunk = (T15_TICK_MS - slept_ms);
            if (chunk > T15_WDT_KICK_CHUNK_MS) { chunk = T15_WDT_KICK_CHUNK_MS; }
            vTaskDelay(pdMS_TO_TICKS(chunk));
            slept_ms += chunk;
        }
        esp_task_wdt_reset();   /* one final kick before the work block */

        uint32_t now_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* (a) Wedge detector. */
        uint32_t hb_now = status_post_heartbeat();
        if (hb_now != s_last_heartbeat) {
            s_last_heartbeat      = hb_now;
            s_last_heartbeat_tick = now_tick;
        } else if ((now_tick - s_last_heartbeat_tick) >= T15_WEDGE_TIMEOUT_MS) {
            respawn_t14("heartbeat stuck for > 60 s");
            continue;   /* skip leak / planned-reboot checks this tick */
        }

        /* (b) Heap-leak detector. */
        uint32_t drop = status_post_heap_drop_bytes();
        if (drop >= T15_HEAP_DROP_LIMIT) {
            planned_reboot("T14 cumulative heap drop crossed 64 KB");
            /* unreachable */
        }

        /* (c) Successful-post check — clear the planned-reboot flag once
         * T14 demonstrates it can complete one POST. status_post_backoff_active()
         * returning false AND a heartbeat that advanced past the boot value
         * is sufficient evidence. */
        if (s_was_planned_reboot && !status_post_backoff_active() && hb_now > 5u) {
            s_was_planned_reboot = false;
            (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, K_PLAN_REBOOT, 0);
            ESP_LOGI(TAG, "planned-reboot flag cleared (T14 healthy)");
        }

        maybe_roll_hour_counter();
    }
}

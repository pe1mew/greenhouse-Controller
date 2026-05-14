/**
 * @file status_post_supervisor.h
 * @brief T15 — Status-POST supervisor (gh#18 Phase 4, since 1.18.0).
 *
 * Watches T14 (the status-POST task) for three failure modes:
 *
 *  1. **Wedge** — T14's heartbeat counter has not advanced for 60 s.
 *     Triggers a force-respawn: clean TLS teardown, vTaskDelete, brief
 *     pause for FreeRTOS to reclaim the stack, re-create the task.
 *
 *  2. **Leak** — T14's cumulative heap drop has crossed a 64 KB threshold.
 *     Triggers a planned reboot (`esp_restart()`). NVS-persisted state
 *     ensures the next boot resumes in ~2 s rather than ~171 s, because
 *     Phase 3 wrote each window channel's last terminal state to NVS.
 *
 *  3. **Respawn storm** — more than 10 respawns within one hour, or more
 *     than 1 respawn within 5 minutes. Indicates T14 cannot recover via
 *     vTaskDelete + recreate; escalates to planned reboot.
 *
 * T15 is itself watchdog-subscribed (same pattern as T2 since 1.17.29).
 * If T15 wedges, the hardware WDT resets the chip cleanly within the WDT
 * timeout window — at the cost of `ESP_RST_TASK_WDT` boot reason in the
 * next session.
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief T15 task entry. Spawned by main.cpp on Core 0 at priority 4.
 *        Spawned BEFORE T14 so the supervisor handle is valid by the
 *        time T14 first runs.
 *
 * @param pvParameters Unused; pass NULL.
 */
void task_status_post_supervisor(void *pvParameters);

/**
 * @brief Returns true if this boot's `system/t15_planreboot` flag was 1.
 *
 * Cleared by the supervisor itself after T14 makes one successful POST.
 * Surfaced via the status JSON so a planned-reboot recovery is visible
 * to the operator in the web GUI (distinguishes it from a panic-class
 * reset that happened to reboot with `ESP_RST_SW`).
 */
bool supervisor_was_planned_reboot(void);

#ifdef __cplusplus
}
#endif

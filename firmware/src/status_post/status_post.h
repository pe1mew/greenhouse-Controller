/**
 * @file status_post.h
 * @brief T14 — Status website POST task (Phase 9).
 *
 * Pushes the controller's runtime status to a configurable REST endpoint on
 * a fixed cycle (60–300 s), and uploads the most recently closed CSV log
 * file to the same endpoint on rotation and as a daily fallback.
 *
 * Implements the controller side of design/technical-spec-statusWebsite.md:
 *  - POST /api.php                  → status payload (this task)
 *  - POST /api.php?action=log       → log file upload (Phase E)
 *
 * All configuration lives in NVS_NS_SYSTEM and is exposed via the
 * cfg_shadow_t (see data_manager.h §Status website / web-tab settings).
 *
 * The task is on Core 0 alongside T10/T11 with priority TASK_PRIO_LOW; it
 * sleeps 1 s between iterations and only opens an HTTP connection when a
 * cycle is due. HTTPS endpoints are supported via WiFiClientSecure with
 * certificate validation disabled (setInsecure) — see the impact analysis
 * for the trade-off.
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
 * @brief T14 task entry. Spawned by main.cpp on Core 0 at TASK_PRIO_LOW.
 * @param pvParameters Unused; pass NULL.
 */
void task_status_post(void *pvParameters);

/**
 * @brief Format the last-attempt outcome for display on the Web tab.
 *
 * Writes a short human-readable string into @p buf, e.g.
 *   "OK 2026-05-10 14:30:22"   (last cycle succeeded)
 *   "FAIL 2026-05-10 14:30:22" (last cycle failed)
 *   ""                          (no attempt yet this boot)
 *
 * @param buf  Destination buffer.
 * @param cap  Capacity of @p buf in bytes.
 */
void status_post_last_str(char *buf, size_t cap);

/**
 * @brief Format the last log-upload outcome for display on the Web tab.
 *
 * Same shape as status_post_last_str(), but for the periodic log upload
 * (Phase E). Returns an empty string until Phase E is wired up.
 */
void status_post_last_log_str(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

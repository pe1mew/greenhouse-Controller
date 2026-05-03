/**
 * @file network_manager.h
 * @brief T10 — Network Manager task declaration.
 *
 * Manages WiFi AP and client lifecycle, triggers NTP sync, updates the
 * DS1307 RTC after NTP sync, and posts net_status_t to Q5.
 *
 * Full implementation: Phase 8 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T10 — Network Manager task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_network_manager(void *pvParameters);

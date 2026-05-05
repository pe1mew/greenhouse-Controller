/**
 * @file ota_manager.h
 * @brief T13 — OTA Manager task declaration.
 *
 * Handles dual-bank firmware and LittleFS OTA updates.  Created on demand
 * by T11 (no permanent task handle).  Implements 3-consecutive-fail rollback.
 * Sets/clears EG1.OTA_IN_PROGRESS.
 *
 * Full implementation: Phase 10 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T13 — OTA Manager task entry point.
 *
 * Created on demand by T11 via xTaskCreate().  Deletes itself on completion
 * or failure.
 *
 * @param pvParameters  Unused; pass NULL.
 */
void task_ota_manager(void *pvParameters);

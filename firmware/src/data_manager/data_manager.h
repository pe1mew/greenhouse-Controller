/**
 * @file data_manager.h
 * @brief T4 — Data Manager task declaration.
 *
 * Central data store for all configuration settings and live measurement data.
 * Loads NVS on boot, maintains MX4-protected configuration shadow, maintains
 * MX2/MX3-protected measurement ring buffers, handles RTC read/write, and
 * computes sunrise/sunset times.
 *
 * Full implementation: Phase 1 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T4 — Data Manager task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_data_manager(void *pvParameters);

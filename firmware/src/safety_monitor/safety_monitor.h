/**
 * @file safety_monitor.h
 * @brief T3 — Safety Monitor task declaration.
 *
 * Evaluates wind speed and direction against configured thresholds.
 * Issues CMD_CLOSE_ALL to Q1 when wind safety conditions are met;
 * issues CMD_RESUME when conditions clear.  Sets/clears EG1.WIND_OVERRIDE.
 *
 * Full implementation: Phase 3 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T3 — Safety Monitor task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_safety_monitor(void *pvParameters);

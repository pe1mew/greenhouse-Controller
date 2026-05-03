/**
 * @file keypad_scan.h
 * @brief T7 — Keypad Scan task declaration.
 *
 * Scans the 4×4 membrane keypad every 20 ms, debounces key presses,
 * generates key-repeat events, and posts key_event_t to Q2.
 *
 * Full implementation: Phase 4 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T7 — Keypad Scan task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_keypad_scan(void *pvParameters);

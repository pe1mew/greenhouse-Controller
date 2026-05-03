/**
 * @file sensor_poll.h
 * @brief T5 — Sensor Poll task declaration.
 *
 * Modbus RTU master for the FG6485A (T/RH) and S200 (wind) sensors.
 * Polls at the configured interval (30–3600 s, default 60 s), maintains
 * sliding averages, posts sensor_reading_t to Q6, and posts LOG_SENSOR
 * events to Q3 on every poll (FR-LG09: snapshot = poll).
 *
 * Full implementation: Phase 3 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T5 — Sensor Poll task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_sensor_poll(void *pvParameters);

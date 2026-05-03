/**
 * @file mqtt_client.h
 * @brief T12 — MQTT Client task declaration.
 *
 * Publishes sensor data and system status to the configured broker.
 * Subscribes to command topics; posts config_update_t to Q4 for
 * recognised setpoint/mode topics.  Does NOT post to Q1 — window
 * commands via MQTT are out of scope (C9).
 *
 * Full implementation: Phase 9 of firmwareImplementationPlan.md.
 *
 * @author  Greenhouse Controller project
 */

#pragma once

/**
 * @brief T12 — MQTT Client task entry point.
 * @param pvParameters  Unused; pass NULL.
 */
void task_mqtt_client(void *pvParameters);

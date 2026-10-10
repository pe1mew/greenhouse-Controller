/**
 * @file freertos/task.h
 * @brief Host-build stand-in for ESP-IDF's FreeRTOS task.h (LIB-4 native tests).
 *
 * vTaskDelay() is implemented in test/mock_freertos.cpp against the
 * simulated clock in test/mock_i2c_bus.h. Never on a target include path.
 */

#pragma once

#include "freertos/FreeRTOS.h"

/**
 * @brief Block for @p xTicksToDelay ticks.
 *
 * FreeRTOS wakes the task when the tick count reaches (count at the call +
 * xTicksToDelay). A call made just before a tick interrupt is therefore
 * released (xTicksToDelay - 1) tick periods later, plus an instant; that
 * least guaranteed time is what the simulation advances the clock by.
 */
void vTaskDelay(const TickType_t xTicksToDelay);

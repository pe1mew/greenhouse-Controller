/**
 * @file mock_freertos.cpp
 * @brief vTaskDelay() on the simulated clock (LIB-4 native tests).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mock_i2c_bus.h"

int mock_task_delay_calls = 0;

void vTaskDelay(const TickType_t xTicksToDelay)
{
    mock_task_delay_calls++;
    if (xTicksToDelay == 0) {
        return;                                   /* only a yield */
    }
    /* tasks.c: xTimeToWake = xTickCount + xTicksToDelay. The tick that is
     * about to fire counts as the first, so the least time that passes is
     * (n - 1) whole periods and an instant. The driver can only rely on that. */
    const uint64_t tick_ns = 1000000000ULL / configTICK_RATE_HZ;
    mock_advance_ns((uint64_t)(xTicksToDelay - 1u) * tick_ns + MOCK_TICK_INSTANT_NS);
}

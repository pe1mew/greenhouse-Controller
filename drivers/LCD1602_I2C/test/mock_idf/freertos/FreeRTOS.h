/**
 * @file freertos/FreeRTOS.h
 * @brief Host-build stand-in for ESP-IDF's FreeRTOS.h (LIB-4 native tests).
 *
 * The native build compiles the ESP-IDF branch of lcd1602.cpp (see
 * test/include_lib.cpp), whose delays are vTaskDelay(pdMS_TO_TICKS(ms)).
 * Only the tick arithmetic is provided, as ESP-IDF 5.5.0 defines it
 * (FreeRTOS-Kernel projdefs.h, xtensa portmacro.h). Never on a target
 * include path.
 */

#pragma once

#include <stdint.h>

/* The firmware's tick rate: CONFIG_FREERTOS_HZ=1000 in
 * firmware/sdkconfig.defaults. A different rate changes what every
 * pdMS_TO_TICKS() in the driver really waits, so keep this in step. */
#define configTICK_RATE_HZ  1000

typedef uint32_t TickType_t;

#define portTICK_PERIOD_MS  ((TickType_t)1000 / configTICK_RATE_HZ)

#define pdMS_TO_TICKS(xTimeInMs) \
    ((TickType_t)(((TickType_t)(xTimeInMs) * (TickType_t)configTICK_RATE_HZ) / (TickType_t)1000U))

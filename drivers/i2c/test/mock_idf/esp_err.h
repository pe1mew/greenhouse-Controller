/**
 * @file esp_err.h
 * @brief Host-build stand-in for ESP-IDF's esp_err.h (LIB-2 native tests).
 *
 * Found through `-I test/mock_idf` in the native env, ahead of anything
 * else, because the native build compiles the ESP-IDF branch of i2c_bus.cpp.
 * Never on a target include path.
 *
 * Values copied from ESP-IDF 5.5.0 (components/esp_common/include/esp_err.h),
 * the framework the firmware builds against (espressif32@6.12.0), so a code
 * printed by a failing assertion reads the same as on the board.
 */

#pragma once

typedef int esp_err_t;

#define ESP_OK                     0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM             0x101
#define ESP_ERR_INVALID_ARG        0x102
#define ESP_ERR_INVALID_STATE      0x103
#define ESP_ERR_NOT_FOUND          0x105
#define ESP_ERR_TIMEOUT            0x107
#define ESP_ERR_INVALID_RESPONSE   0x108

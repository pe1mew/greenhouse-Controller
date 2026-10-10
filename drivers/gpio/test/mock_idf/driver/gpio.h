/**
 * @file driver/gpio.h
 * @brief Host-build stand-in for ESP-IDF's driver/gpio.h (LIB-1 native tests).
 *
 * gpio_util.cpp includes "driver/gpio.h" with no UNIT_TEST switch, so the
 * native env puts test/mock_idf/ on the include path and this file takes the
 * real header's place. Never on a target include path.
 *
 * Declarations mirror ESP-IDF 5.5.0 for the ESP32-S3 — hal/gpio_types.h,
 * driver/gpio.h, soc/esp32s3 gpio_num.h and soc_caps.h. Only what
 * gpio_util.cpp uses is declared. The behaviour behind the three functions
 * is simulated in mock_gpio.cpp; see mock_gpio.h for what is modelled.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* ---------------------------------------------------------------------------
 * soc/esp32s3/include/soc/gpio_num.h, soc_caps.h
 *
 * The S3 has GPIO 0-21 and 26-48; 22-25 do not exist. gpio_util.cpp only
 * casts integers to gpio_num_t, so the enumerator list is abbreviated.
 * --------------------------------------------------------------------------- */
typedef enum {
    GPIO_NUM_NC  = -1,
    GPIO_NUM_0   = 0,
    GPIO_NUM_48  = 48,
    GPIO_NUM_MAX = 49,
} gpio_num_t;

#define SOC_GPIO_PIN_COUNT               49
#define SOC_GPIO_VALID_GPIO_MASK         (0x1FFFFFFFFFFFFULL & ~(0ULL | (1ULL << 22) | (1ULL << 23) \
                                                                     | (1ULL << 24) | (1ULL << 25)))
#define SOC_GPIO_VALID_OUTPUT_GPIO_MASK  (SOC_GPIO_VALID_GPIO_MASK)

#define GPIO_IS_VALID_GPIO(gpio_num)        (((gpio_num) >= 0) && \
                                             (((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0))
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio_num) (((gpio_num) >= 0) && \
                                             (((1ULL << (gpio_num)) & SOC_GPIO_VALID_OUTPUT_GPIO_MASK) != 0))

/* ---------------------------------------------------------------------------
 * hal/gpio_types.h
 * --------------------------------------------------------------------------- */
#define GPIO_MODE_DEF_DISABLE  (0)
#define GPIO_MODE_DEF_INPUT    (1UL << 0)
#define GPIO_MODE_DEF_OUTPUT   (1UL << 1)
#define GPIO_MODE_DEF_OD       (1UL << 2)

typedef enum {
    GPIO_MODE_DISABLE         = GPIO_MODE_DEF_DISABLE,
    GPIO_MODE_INPUT           = GPIO_MODE_DEF_INPUT,
    GPIO_MODE_OUTPUT          = GPIO_MODE_DEF_OUTPUT,
    GPIO_MODE_OUTPUT_OD       = ((GPIO_MODE_DEF_OUTPUT) | (GPIO_MODE_DEF_OD)),
    GPIO_MODE_INPUT_OUTPUT_OD = ((GPIO_MODE_DEF_INPUT) | (GPIO_MODE_DEF_OUTPUT) | (GPIO_MODE_DEF_OD)),
    GPIO_MODE_INPUT_OUTPUT    = ((GPIO_MODE_DEF_INPUT) | (GPIO_MODE_DEF_OUTPUT)),
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0x0,
    GPIO_PULLUP_ENABLE  = 0x1,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0x0,
    GPIO_PULLDOWN_ENABLE  = 0x1,
} gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE    = 0,
    GPIO_INTR_POSEDGE    = 1,
    GPIO_INTR_NEGEDGE    = 2,
    GPIO_INTR_ANYEDGE    = 3,
    GPIO_INTR_LOW_LEVEL  = 4,
    GPIO_INTR_HIGH_LEVEL = 5,
    GPIO_INTR_MAX,
} gpio_int_type_t;

/* ---------------------------------------------------------------------------
 * driver/gpio.h — the S3 lacks SOC_GPIO_SUPPORT_PIN_HYS_FILTER, so the real
 * struct has no hys_ctrl_mode member on this target either.
 * --------------------------------------------------------------------------- */
typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

/** @brief Configure every pad in pin_bit_mask. ESP_ERR_INVALID_ARG, and no
 *         pad touched, if the mask is 0 or names a pin the S3 lacks. */
esp_err_t gpio_config(const gpio_config_t *pGPIOConfig);

/** @brief Write the output latch. ESP_ERR_INVALID_ARG for a pin that cannot
 *         be an output. */
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);

/** @brief Read the input buffer. ESP-IDF: "If the pad is not configured for
 *         input (or input and output) the returned value is always 0." */
int gpio_get_level(gpio_num_t gpio_num);

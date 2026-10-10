/**
 * @file mock_gpio.cpp
 * @brief ESP-IDF GPIO functions answered from a simulated ESP32-S3 pad (LIB-1 native tests).
 *
 * See mock_gpio.h for the model. The three ESP-IDF functions below follow
 * components/esp_driver_gpio/src/gpio.c in ESP-IDF 5.5.0: gpio_config()
 * validates the whole mask before touching any pad, gpio_set_level()
 * validates the pin, gpio_get_level() does not validate at all.
 */

#include "mock_gpio.h"
#include <string.h>

int mock_gpio_config_calls;
int mock_gpio_config_rejected;
int mock_gpio_set_level_rejected;
int mock_gpio_floating_reads;
int mock_gpio_contention;

static mock_gpio_pad_t s_pad[MOCK_GPIO_PIN_COUNT];

/* True when the SoC drives the pad; *level receives what it drives. */
static bool soc_drives(const mock_gpio_pad_t *p, uint8_t *level)
{
    if (!p->output_en) {
        return false;
    }
    if (p->open_drain && p->out_latch != 0) {
        return false;                       /* open drain releases on 1 */
    }
    *level = p->out_latch;
    return true;
}

static void check_contention(const mock_gpio_pad_t *p)
{
    uint8_t soc = 0;
    if (p->external == MOCK_EXT_NONE || !soc_drives(p, &soc)) {
        return;
    }
    uint8_t ext = (p->external == MOCK_EXT_HIGH) ? 1u : 0u;
    if (soc != ext) {
        mock_gpio_contention++;
    }
}

/* ---------------------------------------------------------------------------
 * Control and inspection
 * --------------------------------------------------------------------------- */

void mock_gpio_reset(void)
{
    memset(s_pad, 0, sizeof(s_pad));        /* also MOCK_EXT_NONE, latch 0 */
    mock_gpio_config_calls       = 0;
    mock_gpio_config_rejected    = 0;
    mock_gpio_set_level_rejected = 0;
    mock_gpio_floating_reads     = 0;
    mock_gpio_contention         = 0;
}

const mock_gpio_pad_t *mock_gpio_pad(uint8_t pin)
{
    return (pin < MOCK_GPIO_PIN_COUNT) ? &s_pad[pin] : NULL;
}

int mock_gpio_pad_level(uint8_t pin)
{
    if (pin >= MOCK_GPIO_PIN_COUNT) {
        return -1;
    }
    const mock_gpio_pad_t *p = &s_pad[pin];
    uint8_t soc = 0;
    if (soc_drives(p, &soc)) {
        return soc;
    }
    if (p->external == MOCK_EXT_LOW)  return 0;
    if (p->external == MOCK_EXT_HIGH) return 1;
    if (p->pull_up && !p->pull_down)  return 1;
    if (p->pull_down && !p->pull_up)  return 0;
    return -1;                              /* floating, or both pulls on */
}

void mock_gpio_drive_external(uint8_t pin, mock_gpio_ext_t drive)
{
    if (pin >= MOCK_GPIO_PIN_COUNT) {
        return;
    }
    s_pad[pin].external = drive;
    check_contention(&s_pad[pin]);
}

/* ---------------------------------------------------------------------------
 * ESP-IDF API (driver/gpio.h)
 * --------------------------------------------------------------------------- */

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    mock_gpio_config_calls++;

    uint64_t mask = cfg->pin_bit_mask;
    if (mask == 0 || (mask & ~SOC_GPIO_VALID_GPIO_MASK) != 0) {
        mock_gpio_config_rejected++;        /* "GPIO_PIN mask error" */
        return ESP_ERR_INVALID_ARG;
    }
    if ((cfg->mode & GPIO_MODE_DEF_OUTPUT) != 0 &&
        (mask & ~SOC_GPIO_VALID_OUTPUT_GPIO_MASK) != 0) {
        mock_gpio_config_rejected++;        /* "GPIO can only be used as input mode" */
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t n = 0; n < MOCK_GPIO_PIN_COUNT; n++) {
        if (((mask >> n) & 1ULL) == 0) {
            continue;
        }
        mock_gpio_pad_t *p = &s_pad[n];
        p->configured  = true;
        p->last_config = *cfg;
        p->input_en    = (cfg->mode & GPIO_MODE_DEF_INPUT)  != 0;
        p->output_en   = (cfg->mode & GPIO_MODE_DEF_OUTPUT) != 0;
        p->open_drain  = (cfg->mode & GPIO_MODE_DEF_OD)     != 0;
        p->pull_up     = cfg->pull_up_en   != GPIO_PULLUP_DISABLE;
        p->pull_down   = cfg->pull_down_en != GPIO_PULLDOWN_DISABLE;
        /* The latch is left alone: an output enabled here drives whatever
         * the latch already holds, as the GPIO_OUT register does. */
        check_contention(p);
    }
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (gpio_num < 0 || gpio_num >= MOCK_GPIO_PIN_COUNT ||
        !GPIO_IS_VALID_OUTPUT_GPIO(gpio_num)) {
        mock_gpio_set_level_rejected++;
        return ESP_ERR_INVALID_ARG;
    }
    s_pad[gpio_num].out_latch = (level != 0) ? 1u : 0u;
    check_contention(&s_pad[gpio_num]);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    if (gpio_num < 0 || gpio_num >= MOCK_GPIO_PIN_COUNT) {
        return 0;
    }
    const mock_gpio_pad_t *p = &s_pad[gpio_num];
    if (!p->input_en) {
        return 0;                           /* input buffer off: always 0 */
    }
    int level = mock_gpio_pad_level((uint8_t)gpio_num);
    if (level < 0) {
        mock_gpio_floating_reads++;
        return 0;
    }
    return level;
}

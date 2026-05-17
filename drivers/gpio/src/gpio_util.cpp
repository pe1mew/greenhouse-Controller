/**
 * @file gpio_util.cpp
 * @brief GPIO utility driver — ESP-IDF implementation (LIB-1).
 *
 * Migrated from arduino-esp32 in 2.0.0-alpha.2 (Phase 2). The public API
 * declared in `gpio_util.h` is unchanged — callers don't need to know
 * which underlying API does the work.
 *
 * API mapping (arduino → ESP-IDF):
 *   pinMode(p, OUTPUT)        → gpio_config({ mode = GPIO_MODE_OUTPUT })
 *   pinMode(p, INPUT)         → gpio_config({ mode = GPIO_MODE_INPUT })
 *   pinMode(p, INPUT_PULLUP)  → gpio_config({ mode = GPIO_MODE_INPUT,
 *                                              pull_up_en = ENABLE })
 *   digitalWrite(p, HIGH/LOW) → gpio_set_level(p, 1/0)
 *   digitalRead(p)            → gpio_get_level(p)
 *
 * Behavioural equivalence to the arduino API:
 *   - Each call to gpio_set_pin_mode() reconfigures the pin completely,
 *     matching the arduino-era semantic of "pinMode replaces prior config".
 *   - Interrupt type is set to GPIO_INTR_DISABLE on every reconfig.
 *     This driver doesn't handle ISRs; callers wanting GPIO interrupts
 *     should use the ESP-IDF API directly with `gpio_install_isr_service()`
 *     + `gpio_isr_handler_add()`.
 *   - Pull-down is always disabled. The arduino INPUT_PULLDOWN mode is
 *     not exposed by this driver (was never used in the project).
 *   - GPIO_OUTPUT uses ESP-IDF's GPIO_MODE_INPUT_OUTPUT (not the
 *     output-only GPIO_MODE_OUTPUT). Reason: ESP-IDF's gpio_get_level()
 *     on a pin configured pure-output returns ALWAYS 0 regardless of
 *     the latched output value, so a read-modify-write pattern (used by
 *     gpio_toggle()) would never observe the current output state. The
 *     arduino-esp32 wrapper handled this implicitly by mapping
 *     pinMode(OUTPUT) onto an input+output configuration. We do the
 *     same explicitly to preserve gpio_toggle()'s observable behaviour
 *     (and gpio_read() on output pins more generally — e.g. relay state
 *     introspection that the relay_controller uses for self-checks).
 *     This matches arduino's digitalRead() semantics 1:1.
 */

#include "gpio_util.h"
#include "driver/gpio.h"

void gpio_set_pin_mode(uint8_t pin, gpio_util_mode_t mode)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << pin);
    cfg.intr_type    = GPIO_INTR_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;

    switch (mode) {
        case GPIO_INPUT:
            cfg.mode       = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_DISABLE;
            break;
        case GPIO_OUTPUT:
            /* INPUT_OUTPUT (not OUTPUT) so gpio_get_level() returns the
             * latched output value. Required for gpio_toggle() and for
             * any caller doing read-back of output pins. See header
             * comment above for the full rationale. */
            cfg.mode       = GPIO_MODE_INPUT_OUTPUT;
            cfg.pull_up_en = GPIO_PULLUP_DISABLE;
            break;
        case GPIO_INPUT_PULLUP:
            cfg.mode       = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            break;
    }

    gpio_config(&cfg);
}

void gpio_write(uint8_t pin, gpio_util_level_t level)
{
    gpio_set_level((gpio_num_t)pin, (level == GPIO_HIGH) ? 1 : 0);
}

gpio_util_level_t gpio_read(uint8_t pin)
{
    return (gpio_get_level((gpio_num_t)pin) != 0) ? GPIO_HIGH : GPIO_LOW;
}

void gpio_toggle(uint8_t pin)
{
    gpio_write(pin, (gpio_read(pin) == GPIO_HIGH) ? GPIO_LOW : GPIO_HIGH);
}

void gpio_rs485_init(void)
{
    gpio_set_pin_mode(PIN_RS485_DE_RE, GPIO_OUTPUT);
    gpio_write(PIN_RS485_DE_RE, GPIO_LOW);
}

void gpio_set_rs485_direction(bool transmit)
{
    gpio_write(PIN_RS485_DE_RE, transmit ? GPIO_HIGH : GPIO_LOW);
}

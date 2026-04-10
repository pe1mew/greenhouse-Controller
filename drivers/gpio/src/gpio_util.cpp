#ifndef UNIT_TEST
  #include <Arduino.h>
#else
  #include "../test/mock_gpio.h"
#endif

#include "gpio_util.h"

void gpio_set_pin_mode(uint8_t pin, gpio_util_mode_t mode)
{
    switch (mode) {
        case GPIO_INPUT:        pinMode(pin, INPUT);        break;
        case GPIO_OUTPUT:       pinMode(pin, OUTPUT);       break;
        case GPIO_INPUT_PULLUP: pinMode(pin, INPUT_PULLUP); break;
    }
}

void gpio_write(uint8_t pin, gpio_util_level_t level)
{
    digitalWrite(pin, (level == GPIO_HIGH) ? HIGH : LOW);
}

gpio_util_level_t gpio_read(uint8_t pin)
{
    return (digitalRead(pin) == HIGH) ? GPIO_HIGH : GPIO_LOW;
}

void gpio_toggle(uint8_t pin)
{
    gpio_write(pin, (gpio_read(pin) == GPIO_HIGH) ? GPIO_LOW : GPIO_HIGH);
}

void gpio_set_rs485_direction(bool transmit)
{
    gpio_write(PIN_RS485_DE_RE, transmit ? GPIO_HIGH : GPIO_LOW);
}

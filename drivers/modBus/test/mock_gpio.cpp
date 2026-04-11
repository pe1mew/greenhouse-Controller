#include "mock_gpio.h"

static bool s_direction = false;   /* false = RX (power-on default) */

void gpio_set_rs485_direction(bool transmit)
{
    s_direction = transmit;
    mock_log_event(transmit ? MOCK_EVT_GPIO_HIGH : MOCK_EVT_GPIO_LOW);
}

bool mock_gpio_get_direction(void)
{
    return s_direction;
}

void mock_gpio_reset(void)
{
    s_direction = false;
}

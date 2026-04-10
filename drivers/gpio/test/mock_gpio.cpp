#include "mock_gpio.h"
#include <string.h>

uint8_t pin_state[48];
uint8_t pin_mode_arr[48];

void pinMode(uint8_t pin, uint8_t mode)
{
    if (pin < 48) {
        pin_mode_arr[pin] = mode;
    }
}

void digitalWrite(uint8_t pin, uint8_t val)
{
    if (pin < 48) {
        pin_state[pin] = val;
    }
}

int digitalRead(uint8_t pin)
{
    if (pin < 48) {
        return pin_state[pin];
    }
    return LOW;
}

void mock_gpio_reset(void)
{
    memset(pin_state,    LOW,   sizeof(pin_state));
    memset(pin_mode_arr, INPUT, sizeof(pin_mode_arr));
}

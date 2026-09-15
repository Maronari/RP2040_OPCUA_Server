/**
 * @file
 * @brief Implementation of the host-test hardware/gpio.h stand-in (see that
 *        header for rationale).
 */
#include "hardware/gpio.h"

#include <string.h>

gpio_stub_pin_t g_gpio_stub_pins[NUM_BANK0_GPIOS];

void gpio_stub_reset(void)
{
    memset(g_gpio_stub_pins, 0, sizeof(g_gpio_stub_pins));
}

void gpio_init(unsigned int gpio)
{
    g_gpio_stub_pins[gpio] = (gpio_stub_pin_t){0};
}

void gpio_set_dir(unsigned int gpio, bool out)
{
    g_gpio_stub_pins[gpio].out_enabled = out;
}

void gpio_put(unsigned int gpio, bool value)
{
    g_gpio_stub_pins[gpio].driven_level = value;
}

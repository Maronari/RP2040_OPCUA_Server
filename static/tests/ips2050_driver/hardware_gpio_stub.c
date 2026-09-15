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
    unsigned int gpio;

    memset(g_gpio_stub_pins, 0, sizeof(g_gpio_stub_pins));
    for (gpio = 0u; gpio < NUM_BANK0_GPIOS; ++gpio)
    {
        g_gpio_stub_pins[gpio].external_level = true;
    }
}

void gpio_init(unsigned int gpio)
{
    bool external_level = g_gpio_stub_pins[gpio].external_level;

    g_gpio_stub_pins[gpio] = (gpio_stub_pin_t){0};
    g_gpio_stub_pins[gpio].external_level = external_level;
}

void gpio_set_dir(unsigned int gpio, bool out)
{
    g_gpio_stub_pins[gpio].out_enabled = out;
}

unsigned int gpio_get_dir(unsigned int gpio)
{
    return g_gpio_stub_pins[gpio].out_enabled ? GPIO_OUT : GPIO_IN;
}

void gpio_put(unsigned int gpio, bool value)
{
    g_gpio_stub_pins[gpio].driven_level = value;
}

bool gpio_get(unsigned int gpio)
{
    return g_gpio_stub_pins[gpio].out_enabled ? g_gpio_stub_pins[gpio].driven_level
                                               : g_gpio_stub_pins[gpio].external_level;
}

void gpio_disable_pulls(unsigned int gpio)
{
    g_gpio_stub_pins[gpio].pulls_disabled = true;
}

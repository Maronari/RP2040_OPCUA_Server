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

unsigned int gpio_get_dir(unsigned int gpio)
{
    return g_gpio_stub_pins[gpio].out_enabled ? GPIO_OUT : GPIO_IN;
}

void gpio_put(unsigned int gpio, bool value)
{
    g_gpio_stub_pins[gpio].level = value;
}

bool gpio_get(unsigned int gpio)
{
    /* An output-enabled pad reads back its own driven level; a
     * high-impedance (input) pad reads whatever the internal pull-up
     * (native_gpio_output_start()'s open-drain default) or -- on this
     * host stub, with no external circuit -- the pull-up state alone,
     * settles to. */
    return g_gpio_stub_pins[gpio].out_enabled ? g_gpio_stub_pins[gpio].level : g_gpio_stub_pins[gpio].pull_up;
}

void gpio_pull_up(unsigned int gpio)
{
    g_gpio_stub_pins[gpio].pull_up = true;
}

void gpio_disable_pulls(unsigned int gpio)
{
    g_gpio_stub_pins[gpio].pull_up = false;
}

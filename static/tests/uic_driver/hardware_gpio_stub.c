/**
 * @file
 * @brief Implementation of the host-test hardware/gpio.h stand-in (see that
 *        header for rationale).
 */
#include "hardware/gpio.h"

#include <string.h>

bool g_gpio_stub_external_level[NUM_BANK0_GPIOS];
bool g_gpio_stub_pulls_disabled[NUM_BANK0_GPIOS];

void gpio_stub_reset(void)
{
    memset(g_gpio_stub_external_level, 0, sizeof(g_gpio_stub_external_level));
    memset(g_gpio_stub_pulls_disabled, 0, sizeof(g_gpio_stub_pulls_disabled));
}

void gpio_init(unsigned int gpio)
{
    (void)gpio;
}

void gpio_set_dir(unsigned int gpio, bool out)
{
    (void)gpio;
    (void)out;
}

void gpio_disable_pulls(unsigned int gpio)
{
    g_gpio_stub_pulls_disabled[gpio] = true;
}

bool gpio_get(unsigned int gpio)
{
    return g_gpio_stub_external_level[gpio];
}

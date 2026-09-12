/**
 * @file
 * @brief Minimal host-test stand-in for the Pico SDK's hardware/gpio.h,
 *        covering only the calls drivers/src/uic_driver.c's digital-input
 *        mode makes (gpio_init/gpio_set_dir/gpio_disable_pulls/gpio_get).
 *        The test sets g_gpio_stub_external_level[] directly to simulate
 *        the TLV9034PWR comparator frontend driving a level onto the pin,
 *        since this driver never drives these pins itself.
 */
#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdbool.h>

#define NUM_BANK0_GPIOS 48u
#define GPIO_OUT 1
#define GPIO_IN 0

extern bool g_gpio_stub_external_level[NUM_BANK0_GPIOS];
extern bool g_gpio_stub_pulls_disabled[NUM_BANK0_GPIOS];

/** @brief Test-only helper (not part of the real Pico SDK): reset every pin (external levels default to false). */
void gpio_stub_reset(void);

void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool out);
void gpio_disable_pulls(unsigned int gpio);
bool gpio_get(unsigned int gpio);

#endif

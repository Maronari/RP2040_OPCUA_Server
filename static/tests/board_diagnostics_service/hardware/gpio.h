/**
 * @file
 * @brief Minimal host-test stand-in for the Pico SDK's hardware/gpio.h,
 *        covering only the calls services/src/board_diagnostics_service.c
 *        makes (gpio_init/gpio_set_dir/gpio_disable_pulls/gpio_get). All
 *        four pins are inputs only -- this driver never drives them -- so
 *        gpio_get always returns the test-settable external_level.
 */
#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdbool.h>

#define NUM_BANK0_GPIOS 48u
#define GPIO_OUT 1
#define GPIO_IN 0

extern bool g_gpio_stub_external_level[NUM_BANK0_GPIOS];
extern bool g_gpio_stub_pulls_disabled[NUM_BANK0_GPIOS];

/** @brief Test-only helper (not part of the real Pico SDK): reset every pin (external levels default to true, matching an unasserted active-low line's idle level). */
void gpio_stub_reset(void);

void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool out);
void gpio_disable_pulls(unsigned int gpio);
bool gpio_get(unsigned int gpio);

#endif

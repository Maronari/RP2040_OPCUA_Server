/**
 * @file
 * @brief Host-test stand-in for the Pico SDK's hardware/gpio.h, modeling
 *        real pad state (output-enable, driven level, and a separate
 *        "external_level" the test can poke to simulate a signal driven
 *        onto an input pin by outside circuitry -- the DOx_FAULT
 *        optoisolator/pull-up network drivers/src/ips2050_driver.c never
 *        drives itself).
 *
 * Shadows the real Pico SDK header via test-directory include-path ordering
 * (see static/tools/test_ips2050_driver.py), the same technique used by
 * static/tests/gpio_output_driver/. A separate copy from that directory's
 * stub (not shared) because this driver's self_test() genuinely reads an
 * externally-driven fault input, a case that stub's gpio_get() (which only
 * ever returns the internal pull state for a non-driven pin) cannot model.
 */
#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdbool.h>

#define NUM_BANK0_GPIOS 48u
#define GPIO_OUT 1
#define GPIO_IN 0

typedef struct
{
    bool out_enabled;
    bool driven_level;
    bool external_level;
    bool pulls_disabled;
} gpio_stub_pin_t;

extern gpio_stub_pin_t g_gpio_stub_pins[NUM_BANK0_GPIOS];

/** @brief Test-only helper (not part of the real Pico SDK): reset every pin to its power-on state (external_level defaults to true, matching an unasserted active-low fault line's idle level). */
void gpio_stub_reset(void);

void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool out);
unsigned int gpio_get_dir(unsigned int gpio);
void gpio_put(unsigned int gpio, bool value);
bool gpio_get(unsigned int gpio);
void gpio_disable_pulls(unsigned int gpio);

#endif

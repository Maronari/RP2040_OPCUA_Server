/**
 * @file
 * @brief Host-test stand-in for the Pico SDK's hardware/gpio.h, modeling
 *        real pad state (output-enable, driven level, pull-up) instead of
 *        the SDK's own no-op "host" platform stubs.
 *
 * Shadows the real Pico SDK header via test-directory include-path ordering
 * (see static/tools/test_gpio_output_driver.py), the same technique used by
 * static/tests/w5x00_transport/. Function signatures/macro values match
 * libraries/pico-sdk/src/rp2_common/hardware_gpio/include/hardware/gpio.h
 * exactly for the subset drivers/src/gpio_output_channel_driver.c calls, so
 * that driver source compiles completely unmodified against this stub.
 *
 * Unlike the Pico SDK's own host-platform hardware_gpio (all no-op stubs,
 * no state, no gpio_get_dir()), this stub tracks enough real state to prove
 * the driver's actual direction/level/pull sequencing -- open-drain assert/
 * release transitions, invert semantics, and self_test's pass/fail
 * comparison -- rather than merely proving the code compiles and links.
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
    bool level;
    bool pull_up;
} gpio_stub_pin_t;

extern gpio_stub_pin_t g_gpio_stub_pins[NUM_BANK0_GPIOS];

/** @brief Test-only helper (not part of the real Pico SDK): reset every pin to its power-on state. */
void gpio_stub_reset(void);

void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool out);
unsigned int gpio_get_dir(unsigned int gpio);
void gpio_put(unsigned int gpio, bool value);
bool gpio_get(unsigned int gpio);
void gpio_pull_up(unsigned int gpio);
void gpio_disable_pulls(unsigned int gpio);

#endif

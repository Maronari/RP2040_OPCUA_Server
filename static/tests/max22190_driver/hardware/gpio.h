/**
 * @file
 * @brief Host-test stand-in for the Pico SDK's hardware/gpio.h, modeling
 *        real pad state (output-enable, driven level, and a separate
 *        "external_level" the test can poke to simulate a signal driven
 *        onto an input pin by outside circuitry -- here the MAX22190's
 *        open-drain /READY output on DI_READY_N/GPIO24, which
 *        drivers/src/max22190_driver.c only ever reads).
 *
 * Shadows the real Pico SDK header via test-directory include-path ordering
 * (see static/tools/test_max22190_driver.py), the same technique used by
 * static/tests/ips2050_driver/ and static/tests/gpio_output_driver/.
 *
 * Note the external_level default: gpio_stub_reset() leaves every pin high,
 * which for the active-low DI_READY_N means "chip not ready". A test that
 * wants a healthy chip must pull it low explicitly, so no test can pass the
 * readiness gate by accident.
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

/**
 * @file
 * @brief Minimal host-test stand-in for the Pico SDK's hardware/gpio.h,
 *        covering only the calls drivers/src/tps3851_watchdog_driver.c
 *        makes (gpio_init/gpio_put/gpio_set_dir) -- both pins are outputs
 *        only, so no externally-driven input state needs modeling here
 *        (contrast static/tests/ips2050_driver/hardware/gpio.h).
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
} gpio_stub_pin_t;

extern gpio_stub_pin_t g_gpio_stub_pins[NUM_BANK0_GPIOS];

/** @brief Test-only helper (not part of the real Pico SDK): reset every pin to its power-on state. */
void gpio_stub_reset(void);

void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool out);
void gpio_put(unsigned int gpio, bool value);

#endif

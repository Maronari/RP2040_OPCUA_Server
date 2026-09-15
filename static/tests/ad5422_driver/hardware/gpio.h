/**
 * @file
 * @brief Host-test stand-in for the Pico SDK's hardware/gpio.h, for the
 *        one GPIO drivers/src/ad5422_driver.c drives itself:
 *        `AO_ENABLE_REQ` (`GPIO9`). Declarations only -- the test defines
 *        them, so pin changes land in the same event log as SPI frames and
 *        delays and their relative order can be asserted.
 *
 * Shadows the real Pico SDK header via test-directory include-path ordering
 * (see static/tools/test_ad5422_driver.py).
 */
#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdbool.h>

#define GPIO_OUT 1
#define GPIO_IN 0

void gpio_init(unsigned int gpio);
void gpio_set_dir(unsigned int gpio, bool out);
void gpio_put(unsigned int gpio, bool value);
void gpio_disable_pulls(unsigned int gpio);

#endif

#ifndef DRIVERS_LED_DRIVER_H
#define DRIVERS_LED_DRIVER_H

#include <stdbool.h>

#include "pico/types.h"

/**
 * @brief Configure a single GPIO pin as a push-pull output for the status LED.
 *
 * Drives the pin low (off) immediately after configuration. Must be called
 * once before led_driver_set()/led_driver_toggle() have any effect; calling
 * it again re-targets the driver at a different pin (the previous pin is
 * left in its last driven state, not restored to input).
 *
 * @param pin RP2350 GPIO number to own exclusively as the LED output.
 * @note Not synchronized; call from a single task/init context only.
 */
void led_driver_init(uint pin);

/**
 * @brief Drive the LED pin high or low.
 *
 * No-op (silently ignored) until led_driver_init() has been called.
 *
 * @param on true to drive the pin high, false to drive it low.
 */
void led_driver_set(bool on);

/**
 * @brief Invert the LED pin's current output level.
 *
 * No-op until led_driver_init() has been called.
 */
void led_driver_toggle(void);

/**
 * @brief Report whether led_driver_init() has been called.
 *
 * @return true once a pin has been configured.
 */
bool led_driver_is_initialized(void);

#endif

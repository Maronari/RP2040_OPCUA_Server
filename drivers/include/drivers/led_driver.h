#ifndef DRIVERS_LED_DRIVER_H
#define DRIVERS_LED_DRIVER_H

#include <stdbool.h>

#include "pico/types.h"

void led_driver_init(uint pin);
void led_driver_set(bool on);
void led_driver_toggle(void);
bool led_driver_is_initialized(void);

#endif

#ifndef DRIVERS_WATCHDOG_DRIVER_H
#define DRIVERS_WATCHDOG_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

void watchdog_driver_init(uint32_t timeout_ms);
void watchdog_driver_kick(void);
bool watchdog_driver_is_enabled(void);
uint32_t watchdog_driver_get_timeout_ms(void);

#endif

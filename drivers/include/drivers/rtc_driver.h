#ifndef DRIVERS_RTC_DRIVER_H
#define DRIVERS_RTC_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

bool rtc_driver_init(void);
bool rtc_driver_is_valid(void);
void rtc_driver_set_unix_time(uint32_t seconds);
uint32_t rtc_driver_get_unix_time(void);

#endif

#ifndef RTC_DRIVER_TEST_PICO_AON_TIMER_H
#define RTC_DRIVER_TEST_PICO_AON_TIMER_H

#include <stdbool.h>
#include <time.h>

/* Both pico-sdk flavors: rtc_driver.c uses the timespec API unless built
 * with PICO_RP2040=1, which selects the calendar API. */
bool aon_timer_is_running(void);
bool aon_timer_start(const struct timespec *ts);
bool aon_timer_set_time(const struct timespec *ts);
bool aon_timer_get_time(struct timespec *ts);
bool aon_timer_start_calendar(const struct tm *tm);
bool aon_timer_set_time_calendar(const struct tm *tm);
bool aon_timer_get_time_calendar(struct tm *tm);

#endif

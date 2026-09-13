#ifndef DRIVERS_RTC_DRIVER_H
#define DRIVERS_RTC_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the RTC driver's runtime state.
 *
 * Records whether the RP2350 Always-On timer (`pico/aon_timer.h`) is
 * already running (e.g. carried over a soft reset) as the initial validity
 * flag. Does not itself start the AON timer -- that only happens on the
 * first rtc_driver_set_unix_time() call.
 *
 * @return Always true; initialization of the driver's bookkeeping cannot
 *         fail even if the AON timer is not running.
 */
bool rtc_driver_init(void);

/**
 * @brief Report whether the RTC currently holds a time believed to be valid.
 *
 * True only if rtc_driver_init() has run, a prior rtc_driver_set_unix_time()
 * (or a pre-running AON timer at init) established a time, and the AON
 * timer is still running. rtc_driver_get_unix_time[_us]() both return 0
 * when this is false, so callers must check validity separately if 0 is a
 * meaningful timestamp in their context.
 *
 * @return true if the RTC has a valid, running time source.
 */
bool rtc_driver_is_valid(void);

/**
 * @brief Set the RTC to the given Unix time (UTC, seconds since epoch).
 *
 * Lazily calls rtc_driver_init() if it was not called yet. Starts the AON
 * timer if not already running, or updates it in place otherwise. On
 * RP2040 this converts to/from a `struct tm` via an internal Gregorian
 * calendar calculation; on other platforms it uses `struct timespec`
 * directly.
 *
 * @param seconds Unix timestamp (UTC) to program into the RTC.
 * @return true if the AON-timer start/set call succeeded; false otherwise,
 *         in which case the RTC is no longer considered valid.
 * @post rtc_driver_is_valid() is true only if this call returned true.
 * @note Hardware side effect: reprograms the RP2350 Always-On timer.
 */
bool rtc_driver_set_unix_time(uint32_t seconds);

/**
 * @brief Read the current Unix time (UTC, seconds) from the RTC.
 *
 * @return 0 if rtc_driver_is_valid() is false or the underlying read fails;
 *         otherwise the current time.
 */
uint32_t rtc_driver_get_unix_time(void);

/**
 * @brief Read the current Unix time (UTC, microseconds) from the RTC.
 *
 * On RP2040 this is derived from the whole-second value only (sub-second
 * resolution is not available there), i.e. always a multiple of 1,000,000;
 * on other platforms it uses the AON timer's nanosecond field.
 *
 * @return 0 if rtc_driver_is_valid() is false or the underlying read fails;
 *         otherwise the current time in microseconds since the Unix epoch.
 */
uint64_t rtc_driver_get_unix_time_us(void);

#endif

#ifndef DRIVERS_RTC_DRIVER_H
#define DRIVERS_RTC_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Concurrency contract (every function below): safe to call concurrently
 * from any FreeRTOS task, including the lwIP tcpip thread; not from an ISR.
 * One internal leaf mutex serializes every AON timer access together with
 * the driver's validity bookkeeping, so a reader can never observe the
 * window in which pico-sdk's aon_timer_set_time() has stopped the timer to
 * rewrite it. A caller that cannot take the mutex within its bounded wait
 * gets the "no valid time" result and changes nothing. See rtc_driver.c.
 */

/**
 * @brief Initialize the RTC driver's runtime state and its lock.
 *
 * Records whether the RP2350 Always-On timer (`pico/aon_timer.h`) is
 * already running (e.g. carried over a soft reset) as the initial validity
 * flag. Does not itself start the AON timer -- that only happens on the
 * first rtc_driver_set_unix_time() call.
 *
 * Must complete once before any concurrent use of this driver: creating the
 * lock is the one step that is not itself serialized. time_service_init()
 * satisfies this from InitTask, before TimeTask or SNTP start.
 *
 * @return true once the driver's bookkeeping is initialized, even if the AON
 *         timer is not running; false only if the lock could not be taken,
 *         which is a caller error (a concurrent first use).
 */
bool rtc_driver_init(void);

/**
 * @brief Report whether the RTC currently holds a time believed to be valid.
 *
 * True only if rtc_driver_init() has run, a prior rtc_driver_set_unix_time()
 * (or a pre-running AON timer at init) established a time, and the AON
 * timer is still running. rtc_driver_get_unix_time[_us]() both return 0
 * when this is false, so callers must check validity separately if 0 is a
 * meaningful timestamp in their context -- or use
 * rtc_driver_try_get_unix_time_us(), which reports both atomically.
 *
 * @return true if the RTC has a valid, running time source.
 */
bool rtc_driver_is_valid(void);

/**
 * @brief Set the RTC to the given Unix time (UTC, seconds since epoch).
 *
 * Lazily initializes the driver if rtc_driver_init() was not called yet.
 * Starts the AON timer if not already running, or updates it in place
 * otherwise. On RP2040 this converts to/from a `struct tm` via an internal
 * Gregorian calendar calculation; on other platforms it uses
 * `struct timespec` directly.
 *
 * @param seconds Unix timestamp (UTC) to program into the RTC.
 * @return true if the AON-timer start/set call succeeded. false if the
 *         hardware rejected the time, in which case the RTC is no longer
 *         considered valid; or if the driver lock could not be taken, in
 *         which case nothing was written and validity is unchanged.
 * @post A true return means rtc_driver_is_valid() is true.
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

/**
 * @brief Check validity and read the time (UTC, microseconds) as one atomic
 *        operation.
 *
 * Unlike calling rtc_driver_is_valid() and then
 * rtc_driver_get_unix_time_us(), no set can land between the check and the
 * read, and a valid reading of 0 is distinguishable from no reading. Same
 * resolution as rtc_driver_get_unix_time_us().
 *
 * @param out_unix_time_us Destination; must be non-NULL (NULL returns
 *        false). Written only on success.
 * @return true if the RTC was valid and the read succeeded.
 */
bool rtc_driver_try_get_unix_time_us(uint64_t *out_unix_time_us);

#endif

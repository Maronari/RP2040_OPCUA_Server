#ifndef DRIVERS_WATCHDOG_DRIVER_H
#define DRIVERS_WATCHDOG_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Enable the RP2350 hardware watchdog with the given timeout.
 *
 * Clamps @p timeout_ms to the hardware maximum of 16000 ms. Passing 0 is
 * treated as "do not enable" (no-op, watchdog stays disabled) rather than
 * as an immediate-reset request.
 *
 * @param timeout_ms Requested timeout in milliseconds; silently clamped to
 *        16000 ms; 0 is a no-op.
 * @post On success, an unfed watchdog will reset the MCU once the timeout
 *       elapses -- callers must arrange periodic watchdog_driver_kick()
 *       calls before enabling it.
 * @note Not reentrant/synchronized; call once during system init.
 */
void watchdog_driver_init(uint32_t timeout_ms);

/**
 * @brief Feed the hardware watchdog to defer the next reset.
 *
 * No-op if watchdog_driver_init() was never called or was called with a
 * timeout of 0 (watchdog left disabled).
 *
 * @note Must be called periodically, well within the configured timeout,
 *       by every task whose liveness the watchdog is meant to guard
 *       (see WatchdogTask in docs/code/freertos.md).
 */
void watchdog_driver_kick(void);

/**
 * @brief Report whether the hardware watchdog is currently enabled.
 *
 * @return true if watchdog_driver_init() successfully enabled the watchdog.
 */
bool watchdog_driver_is_enabled(void);

/**
 * @brief Return the clamped timeout, in milliseconds, passed to the hardware.
 *
 * @return The effective (post-clamp) timeout, or 0 if the watchdog was
 *         never enabled.
 */
uint32_t watchdog_driver_get_timeout_ms(void);

#endif

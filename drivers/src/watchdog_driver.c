/**
 * @file
 * @brief Thin wrapper around the RP2350 hardware watchdog (safety-critical).
 *
 * Responsibilities:
 * - Enable the hardware watchdog with a clamped, bounded timeout.
 * - Feed it (watchdog_driver_kick()) so callers control exactly when the
 *   MCU is allowed to reset for missed liveness.
 *
 * Ownership: g_watchdog_enabled/g_watchdog_timeout_ms are process-lifetime
 * globals mirroring hardware state; there is no way to disable the watchdog
 * once enabled (matches the RP2350 hardware, which cannot be soft-disabled).
 *
 * Concurrency: no internal locking. watchdog_driver_kick() is expected to be
 * called from WatchdogTask (see docs/code/freertos.md); reads
 * (is_enabled/get_timeout_ms) are safe from any task since they are simple
 * word-sized loads of values written once at init.
 */
#include "drivers/watchdog_driver.h"

#include "hardware/watchdog.h"

#define WATCHDOG_DRIVER_MAX_TIMEOUT_MS 16000u

static bool g_watchdog_enabled = false;
static uint32_t g_watchdog_timeout_ms = 0u;

void watchdog_driver_init(uint32_t timeout_ms)
{
    if (timeout_ms == 0u)
    {
        return;
    }

    g_watchdog_timeout_ms = timeout_ms > WATCHDOG_DRIVER_MAX_TIMEOUT_MS
                                ? WATCHDOG_DRIVER_MAX_TIMEOUT_MS
                                : timeout_ms;
    watchdog_enable(g_watchdog_timeout_ms, true);
    g_watchdog_enabled = true;
}

void watchdog_driver_kick(void)
{
    if (!g_watchdog_enabled)
    {
        return;
    }

    watchdog_update();
}

bool watchdog_driver_is_enabled(void)
{
    return g_watchdog_enabled;
}

uint32_t watchdog_driver_get_timeout_ms(void)
{
    return g_watchdog_timeout_ms;
}

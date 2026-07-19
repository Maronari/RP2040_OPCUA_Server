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

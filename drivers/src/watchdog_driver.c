#include "drivers/watchdog_driver.h"

#include "hardware/watchdog.h"

static bool g_watchdog_enabled = false;

void watchdog_driver_init(uint32_t timeout_ms)
{
    if (timeout_ms == 0u)
    {
        return;
    }

    watchdog_enable(timeout_ms, true);
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

/**
 * @file
 * @brief Implementation of the TPS3851 external-supervisor GPIO wrapper.
 *
 * See drivers/include/drivers/tps3851_watchdog_driver.h for the full
 * contract and the standing GPIO2/GPIO3-vs-W5500 pin-conflict caveat.
 */
#include "drivers/tps3851_watchdog_driver.h"

#include "hardware/gpio.h"

#define TPS3851_WD_KICK_GPIO 2u
#define TPS3851_WD_ENABLE_GPIO 3u

static bool g_enabled;
static bool g_kick_level;

void tps3851_watchdog_driver_init(void)
{
    gpio_init(TPS3851_WD_ENABLE_GPIO);
    gpio_put(TPS3851_WD_ENABLE_GPIO, false);
    gpio_set_dir(TPS3851_WD_ENABLE_GPIO, GPIO_OUT);
    gpio_init(TPS3851_WD_KICK_GPIO);
    gpio_put(TPS3851_WD_KICK_GPIO, false);
    gpio_set_dir(TPS3851_WD_KICK_GPIO, GPIO_OUT);
    g_enabled = false;
    g_kick_level = false;
}

void tps3851_watchdog_driver_enable(void)
{
    gpio_put(TPS3851_WD_ENABLE_GPIO, true);
    g_enabled = true;
}

void tps3851_watchdog_driver_kick(void)
{
    g_kick_level = !g_kick_level;
    gpio_put(TPS3851_WD_KICK_GPIO, g_kick_level);
}

bool tps3851_watchdog_driver_is_enabled(void)
{
    return g_enabled;
}

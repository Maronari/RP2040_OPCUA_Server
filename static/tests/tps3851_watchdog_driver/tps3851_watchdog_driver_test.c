/**
 * @file
 * @brief Host tests for the real drivers/src/tps3851_watchdog_driver.c
 *        against a stateful hardware/gpio.h stub -- proves the actual
 *        GPIO pin sequencing (safe-low-before-output-enable on init,
 *        WD_ENABLE only asserted on explicit enable(), WD_KICK toggling
 *        on each kick()), not just that calls dispatch.
 */
#include "drivers/tps3851_watchdog_driver.h"

#include "hardware/gpio.h"

#include <assert.h>

#define WD_KICK_GPIO 2u
#define WD_ENABLE_GPIO 3u

static void test_init_drives_safe_disabled_state(void)
{
    gpio_stub_reset();
    tps3851_watchdog_driver_init();
    assert(g_gpio_stub_pins[WD_ENABLE_GPIO].out_enabled && !g_gpio_stub_pins[WD_ENABLE_GPIO].driven_level);
    assert(g_gpio_stub_pins[WD_KICK_GPIO].out_enabled && !g_gpio_stub_pins[WD_KICK_GPIO].driven_level);
    assert(!tps3851_watchdog_driver_is_enabled());
}

static void test_enable_asserts_wd_enable_only(void)
{
    gpio_stub_reset();
    tps3851_watchdog_driver_init();
    tps3851_watchdog_driver_enable();
    assert(g_gpio_stub_pins[WD_ENABLE_GPIO].driven_level == true);
    assert(g_gpio_stub_pins[WD_KICK_GPIO].driven_level == false); /* kick is untouched by enable() */
    assert(tps3851_watchdog_driver_is_enabled());
}

static void test_kick_toggles_each_call(void)
{
    gpio_stub_reset();
    tps3851_watchdog_driver_init();
    tps3851_watchdog_driver_kick();
    assert(g_gpio_stub_pins[WD_KICK_GPIO].driven_level == true);
    tps3851_watchdog_driver_kick();
    assert(g_gpio_stub_pins[WD_KICK_GPIO].driven_level == false);
    tps3851_watchdog_driver_kick();
    assert(g_gpio_stub_pins[WD_KICK_GPIO].driven_level == true);
    /* WD_ENABLE is untouched by kicking. */
    assert(g_gpio_stub_pins[WD_ENABLE_GPIO].driven_level == false);
}

static void test_kick_works_before_enable(void)
{
    gpio_stub_reset();
    tps3851_watchdog_driver_init();
    assert(!tps3851_watchdog_driver_is_enabled());
    tps3851_watchdog_driver_kick(); /* must not require enable() first */
    assert(g_gpio_stub_pins[WD_KICK_GPIO].driven_level == true);
    assert(!tps3851_watchdog_driver_is_enabled());
}

int main(void)
{
    test_init_drives_safe_disabled_state();
    test_enable_asserts_wd_enable_only();
    test_kick_toggles_each_call();
    test_kick_works_before_enable();
    return 0;
}

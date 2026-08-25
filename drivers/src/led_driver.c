/**
 * @file
 * @brief Single-pin GPIO driver for the device status LED.
 *
 * Responsibilities:
 * - Own one GPIO pin as a push-pull output for the status LED.
 * - No debouncing, blink scheduling, or brightness control -- callers own
 *   any blink pattern by calling led_driver_set()/led_driver_toggle()
 *   repeatedly from their own timer/task.
 *
 * Ownership: g_led_initialized/g_led_pin are process-lifetime globals with
 * no accessor beyond led_driver_is_initialized().
 *
 * Concurrency: none -- no locking. Callers must not call led_driver_init()
 * concurrently with led_driver_set()/led_driver_toggle() from another task.
 */
#include "drivers/led_driver.h"

#include "hardware/gpio.h"

/** True once led_driver_init() has configured g_led_pin as an output. */
static bool g_led_initialized = false;
/** GPIO pin number owned by this driver; only meaningful when g_led_initialized. */
static uint g_led_pin = 0;

void led_driver_init(uint pin)
{
    g_led_pin = pin;
    gpio_init(g_led_pin);
    gpio_set_dir(g_led_pin, GPIO_OUT);
    gpio_put(g_led_pin, 0);
    g_led_initialized = true;
}

void led_driver_set(bool on)
{
    if (!g_led_initialized)
    {
        return;
    }

    gpio_put(g_led_pin, on ? 1 : 0);
}

void led_driver_toggle(void)
{
    if (!g_led_initialized)
    {
        return;
    }

    gpio_xor_mask(1u << g_led_pin);
}

bool led_driver_is_initialized(void)
{
    return g_led_initialized;
}

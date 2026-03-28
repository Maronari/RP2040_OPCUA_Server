#include "drivers/led_driver.h"

#include "hardware/gpio.h"

static bool g_led_initialized = false;
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

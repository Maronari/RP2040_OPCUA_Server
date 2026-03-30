#include <stdio.h>

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#ifdef PICO_DEFAULT_LED_PIN
#define USB_SMOKE_LED_PIN PICO_DEFAULT_LED_PIN
#else
#define USB_SMOKE_LED_PIN 25
#endif

int main(void)
{
    bool led_state = false;
    uint32_t uptime_ms = 0;

    gpio_init(USB_SMOKE_LED_PIN);
    gpio_set_dir(USB_SMOKE_LED_PIN, GPIO_OUT);
    gpio_put(USB_SMOKE_LED_PIN, 0);

    stdio_init_all();
    sleep_ms(200);

    printf("[USB SMOKE]\tboot\n");
    fflush(stdout);

    while (true)
    {
        led_state = !led_state;
        gpio_put(USB_SMOKE_LED_PIN, led_state ? 1 : 0);
        uptime_ms = to_ms_since_boot(get_absolute_time());

        printf("[USB SMOKE]\tuptime_ms=%lu led=%u\n",
               (unsigned long)uptime_ms,
               led_state ? 1u : 0u);
        fflush(stdout);

        sleep_ms(500);
    }
}

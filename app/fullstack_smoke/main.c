#include <stdio.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/led_driver.h"
#include "drivers/platform_clock.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#ifdef PICO_DEFAULT_LED_PIN
#define FULLSTACK_SMOKE_LED_PIN PICO_DEFAULT_LED_PIN
#else
#define FULLSTACK_SMOKE_LED_PIN 25
#endif

static void blink_task(void *argument)
{
    bool led_state = false;
    uint32_t uptime_ms = 0;
    (void)argument;

    while (true)
    {
        led_state = !led_state;
        led_driver_set(led_state);
        uptime_ms = to_ms_since_boot(get_absolute_time());

        printf("[FULLSTACK SMOKE]\tuptime_ms=%lu led=%u free_heap=%lu\n",
               (unsigned long)uptime_ms,
               led_state ? 1u : 0u,
               (unsigned long)xPortGetFreeHeapSize());
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_assert_handler(const char *file, unsigned line)
{
    (void)file;
    (void)line;

    while (true)
    {
        led_driver_toggle();
        sleep_ms(150);
    }
}

void vApplicationMallocFailedHook(void)
{
    while (true)
    {
        led_driver_toggle();
        sleep_ms(100);
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    (void)task_name;

    while (true)
    {
        led_driver_toggle();
        sleep_ms(250);
    }
}

int main(void)
{
    platform_clock_init();
    led_driver_init(FULLSTACK_SMOKE_LED_PIN);

    stdio_init_all();
    sleep_ms(200);

    printf("[FULLSTACK SMOKE]\tboot\n");
    fflush(stdout);

    xTaskCreate(blink_task, "Blink", 1024, NULL, 1, NULL);
    vTaskStartScheduler();

    while (true)
    {
        tight_loop_contents();
    }
}

#include <stdio.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/led_driver.h"
#include "drivers/platform_clock.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "services/config_service.h"
#include "services/diag_service.h"
#include "services/network_manager.h"

#ifdef PICO_DEFAULT_LED_PIN
#define NETWORK_SMOKE_LED_PIN PICO_DEFAULT_LED_PIN
#else
#define NETWORK_SMOKE_LED_PIN 25
#endif

static const device_profile_config_t g_network_smoke_profile = {
    .manufacturer = "RP2 OPC UA",
    .model = "NetworkSmoke",
    .serial_number = "",
    .hw_revision = "network-smoke",
    .fw_revision = "dev",
    .build_time = __DATE__ " " __TIME__,
    .git_hash = "dev",
    .device_name = "RP2 Network Smoke",
    .opcua_hostname = "rp2-network-smoke",
    .ntp_server = "pool.ntp.org",
    .enable_temperature_sensor = false,
    .enable_heap_diagnostics = true,
    .enable_task_diagnostics = true,
    .enable_alarm_ack_method = false,
    .opcua_port = 4840,
    .watchdog_timeout_ms = 0,
    .sampling_period_ms = 1000,
    .publish_period_ms = 1000,
    .temp_offset_c = 0.0f,
    .temp_scale = 1.0f,
    .high_temperature_threshold_c = 60.0f,
    .high_high_temperature_threshold_c = 75.0f,
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .network_mode = NETWORK_MODE_DHCP,
    .static_ip = {192, 168, 10, 42},
    .static_netmask = {255, 255, 255, 0},
    .static_gateway = {192, 168, 10, 1},
    .static_dns = {8, 8, 8, 8}};

static void monitor_task(void *argument)
{
    bool led_state = false;
    (void)argument;

    while (true)
    {
        network_manager_status_t status = {0};
        uint32_t uptime_ms = to_ms_since_boot(get_absolute_time());

        led_state = !led_state;
        led_driver_set(led_state);

        network_manager_get_status(&status);
        printf("[NETWORK SMOKE]\tuptime_ms=%lu led=%u link=%u ipv4=%u ip=%s dhcp=%s rx=%lu err=%lu inerr=%lu reconn=%lu\n",
               (unsigned long)uptime_ms,
               led_state ? 1u : 0u,
               status.link_up ? 1u : 0u,
               status.has_ipv4 ? 1u : 0u,
               status.ipv4_address,
               status.dhcp_state,
               (unsigned long)status.rx_packets,
               (unsigned long)status.rx_errors,
               (unsigned long)status.input_errors,
               (unsigned long)status.reconnect_count);
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
    led_driver_init(NETWORK_SMOKE_LED_PIN);

    stdio_init_all();
    sleep_ms(200);

    printf("[NETWORK SMOKE]\tboot\n");
    fflush(stdout);

    config_service_init(&g_network_smoke_profile);
    diag_service_init();

    printf("[NETWORK SMOKE]\tbefore network_manager_init\n");
    fflush(stdout);

    if (!network_manager_init())
    {
        printf("[NETWORK SMOKE]\tnetwork_manager_init failed\n");
        fflush(stdout);
        while (true)
        {
            led_driver_toggle();
            sleep_ms(100);
        }
    }

    printf("[NETWORK SMOKE]\tafter network_manager_init\n");
    fflush(stdout);

    xTaskCreate(network_manager_task, "NetworkTask", 2048, NULL, 2, NULL);
    xTaskCreate(monitor_task, "MonitorTask", 2048, NULL, 1, NULL);
    vTaskStartScheduler();

    while (true)
    {
        tight_loop_contents();
    }
}

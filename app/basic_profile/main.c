#include <stdio.h>

#include <FreeRTOS.h>
#include <task.h>

#include "hardware/structs/timer.h"
#include "pico/stdlib.h"

#include "drivers/led_driver.h"
#include "drivers/platform_clock.h"
#include "drivers/watchdog_driver.h"
#include "opcua_model/opcua_model.h"
#include "services/alarm_service.h"
#include "services/config_service.h"
#include "services/diag_service.h"
#include "services/network_manager.h"
#include "services/sensor_manager.h"
#include "services/time_service.h"

#define NETWORK_TASK_STACK_SIZE 2048
#define SENSOR_TASK_STACK_SIZE 1024
#define TIME_TASK_STACK_SIZE 1024
#define ALARM_TASK_STACK_SIZE 1024
#define WATCHDOG_TASK_STACK_SIZE 512
#define OPCUA_TASK_STACK_SIZE (15 * 1024)

#define NETWORK_TASK_PRIORITY 2
#define SENSOR_TASK_PRIORITY 3
#define TIME_TASK_PRIORITY 3
#define ALARM_TASK_PRIORITY 3
#define WATCHDOG_TASK_PRIORITY 3
#define OPCUA_TASK_PRIORITY 4

#ifndef DEVICE_PROFILE_FW_REVISION
#define DEVICE_PROFILE_FW_REVISION "dev"
#endif

#ifndef DEVICE_PROFILE_HW_REVISION
#define DEVICE_PROFILE_HW_REVISION "unknown"
#endif

#ifndef DEVICE_PROFILE_BUILD_TIME
#define DEVICE_PROFILE_BUILD_TIME __DATE__ " " __TIME__
#endif

#ifndef DEVICE_PROFILE_GIT_HASH
#define DEVICE_PROFILE_GIT_HASH "unknown"
#endif

#ifdef PICO_DEFAULT_LED_PIN
#define DEVICE_PROFILE_LED_PIN PICO_DEFAULT_LED_PIN
#else
#define DEVICE_PROFILE_LED_PIN 25
#endif

static const device_profile_config_t g_basic_profile = {
    .manufacturer = "RP2040 OPC UA",
    .model = "BasicFirmwareProfile",
    .serial_number = "",
    .hw_revision = DEVICE_PROFILE_HW_REVISION,
    .fw_revision = DEVICE_PROFILE_FW_REVISION,
    .build_time = DEVICE_PROFILE_BUILD_TIME,
    .git_hash = DEVICE_PROFILE_GIT_HASH,
    .device_name = "RP2040 Basic Device",
    .opcua_hostname = "rp2040-basic-profile",
    .ntp_server = "pool.ntp.org",
    .enable_temperature_sensor = true,
    .enable_heap_diagnostics = true,
    .enable_task_diagnostics = true,
    .enable_alarm_ack_method = true,
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

static bool create_task(TaskFunction_t entry_point,
                        const char *name,
                        configSTACK_DEPTH_TYPE stack_size,
                        UBaseType_t priority,
                        TaskHandle_t *out_handle)
{
    if (xTaskCreate(entry_point, name, stack_size, NULL, priority, out_handle) != pdPASS)
    {
        printf("[APP]\t\tFailed to create task %s\n", name);
        return false;
    }

    diag_service_register_task(name, *out_handle);
    return true;
}

static void watchdog_task(void *argument)
{
    const uint32_t timeout_ms = *(const uint32_t *)argument;
    const TickType_t delay_ticks = pdMS_TO_TICKS(timeout_ms > 1u ? timeout_ms / 2u : 1u);

    while (1)
    {
        watchdog_driver_kick();
        vTaskDelay(delay_ticks);
    }
}

int main(void)
{
    TaskHandle_t task_handle = NULL;
    TaskHandle_t watchdog_handle = NULL;
    opcua_model_config_t model_config;
    const device_profile_config_t *config = NULL;

    platform_clock_init();
    timer_hw->dbgpause = 0;
    stdio_init_all();
    sleep_ms(3000);

    led_driver_init(DEVICE_PROFILE_LED_PIN);
    led_driver_set(false);

    config_service_init(&g_basic_profile);
    config = config_service_get();
    diag_service_init();

    if (!network_manager_init())
    {
        printf("[APP]\t\tNetwork manager init failed\n");
    }
    if (!sensor_manager_init())
    {
        printf("[APP]\t\tSensor manager init failed\n");
    }
    if (!time_service_init())
    {
        printf("[APP]\t\tTime service init failed\n");
    }
    if (!alarm_service_init())
    {
        printf("[APP]\t\tAlarm service init failed\n");
    }

    if (config->watchdog_timeout_ms > 0)
    {
        watchdog_driver_init(config->watchdog_timeout_ms);
    }

    model_config.application_name = config->device_name;
    model_config.hostname = config->opcua_hostname;
    model_config.port = config->opcua_port;
    model_config.enable_alarm_methods = config->enable_alarm_ack_method;
    model_config.enable_heap_diagnostics = config->enable_heap_diagnostics;
    model_config.enable_task_diagnostics = config->enable_task_diagnostics;
    if (!opcua_model_init(&model_config))
    {
        printf("[APP]\t\tOPC UA model init failed\n");
    }

    create_task(network_manager_task, "NetworkTask", NETWORK_TASK_STACK_SIZE, NETWORK_TASK_PRIORITY, &task_handle);

    if (config->enable_temperature_sensor)
    {
        create_task(sensor_manager_task, "SensorTask", SENSOR_TASK_STACK_SIZE, SENSOR_TASK_PRIORITY, &task_handle);
    }

    create_task(time_service_task, "TimeTask", TIME_TASK_STACK_SIZE, TIME_TASK_PRIORITY, &task_handle);
    create_task(alarm_service_task, "AlarmTask", ALARM_TASK_STACK_SIZE, ALARM_TASK_PRIORITY, &task_handle);
    create_task(opcua_model_task, "OpcUaTask", OPCUA_TASK_STACK_SIZE, OPCUA_TASK_PRIORITY, &task_handle);

    if (config->watchdog_timeout_ms > 0)
    {
        if (xTaskCreate(watchdog_task,
                        "WatchdogTask",
                        WATCHDOG_TASK_STACK_SIZE,
                        (void *)&config->watchdog_timeout_ms,
                        WATCHDOG_TASK_PRIORITY,
                        &watchdog_handle) == pdPASS)
        {
            diag_service_register_task("WatchdogTask", watchdog_handle);
        }
        else
        {
            printf("[APP]\t\tFailed to create task WatchdogTask\n");
        }
    }

    vTaskStartScheduler();

    while (1)
    {
    }
}

void vApplicationMallocFailedHook(void)
{
    diag_service_set_last_fault_code(DIAG_FAULT_CODE_MALLOC_FAILED);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    diag_service_set_last_fault_code(DIAG_FAULT_CODE_STACK_OVERFLOW);
    printf("[FreeRTOS]\tStack overflow: %s\n", task_name);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

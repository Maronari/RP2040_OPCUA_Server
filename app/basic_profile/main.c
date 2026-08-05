#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "hardware/exception.h"
#include "hardware/structs/timer.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "drivers/led_driver.h"
#include "drivers/flash_operation_coordinator.h"
#include "drivers/platform_clock.h"
#include "drivers/watchdog_driver.h"
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
#include "opcua_model/opcua_model.h"
#endif
#include "ota/ota_flash.h"
#include "psram_allocator.h"
#include "services/alarm_service.h"
#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/config_import_service.h"
#include "services/diag_service.h"
#include "services/device_channels.h"
#include "services/firmware_update_service.h"
#include "services/history_service.h"
#include "services/network_manager.h"
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
#include "services/security_identity_import_service.h"
#include "services/security_service.h"
#include "services/security_trust_import_service.h"
#endif
#include "services/sensor_manager.h"
#include "services/time_service.h"
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
#include "device_identity_provisioning.h"
#endif

#define STACK_DEPTH_BYTES(byte_count) ((configSTACK_DEPTH_TYPE)(((byte_count) + sizeof(StackType_t) - 1u) / sizeof(StackType_t)))

#define NETWORK_TASK_STACK_SIZE STACK_DEPTH_BYTES(4u * 1024u)
#define SENSOR_TASK_STACK_SIZE STACK_DEPTH_BYTES(2u * 1024u)
#define TIME_TASK_STACK_SIZE STACK_DEPTH_BYTES(2u * 1024u)
#define ALARM_TASK_STACK_SIZE STACK_DEPTH_BYTES(2u * 1024u)
#define WATCHDOG_TASK_STACK_SIZE STACK_DEPTH_BYTES(1u * 1024u)
#define INIT_TASK_STACK_SIZE STACK_DEPTH_BYTES(6u * 1024u)
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
#define OPCUA_TASK_STACK_SIZE STACK_DEPTH_BYTES(64u * 1024u)
#endif
#define APP_DEFAULT_WATCHDOG_TIMEOUT_MS 80000u

#define fflush(_stream) stdio_flush()

#define INIT_TASK_PRIORITY 4
#define NETWORK_TASK_PRIORITY 2
#define SENSOR_TASK_PRIORITY 3
#define TIME_TASK_PRIORITY 3
#define ALARM_TASK_PRIORITY 3
#define WATCHDOG_TASK_PRIORITY 3
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
#define OPCUA_TASK_PRIORITY 1
#endif

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
    .manufacturer = "RP2 OPC UA",
    .model = "BasicFirmwareProfile",
    .serial_number = "",
    .hw_revision = DEVICE_PROFILE_HW_REVISION,
    .fw_revision = DEVICE_PROFILE_FW_REVISION,
    .build_time = DEVICE_PROFILE_BUILD_TIME,
    .git_hash = DEVICE_PROFILE_GIT_HASH,
    .device_name = "RP2 Basic Device",
    .opcua_hostname = "rp2-basic-profile",
    .ntp_server = "pool.ntp.org",
    .mqtt_broker_url = "opc.mqtt://127.0.0.1:1883",
    .mqtt_topic_root = "devices/rp2-basic-profile",
    .enable_heap_diagnostics = true,
    .enable_task_diagnostics = true,
    .enable_alarm_ack_method = true,
    .enable_mqtt_pubsub = false,
    .mqtt_pubsub_use_json = true,
    .opcua_port = 4840,
    .timezone_offset_minutes = 0,
    .watchdog_timeout_ms = APP_DEFAULT_WATCHDOG_TIMEOUT_MS,
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .network_mode = NETWORK_MODE_DHCP,
    .static_ip = {192, 168, 1, 42},
    .static_netmask = {255, 255, 255, 0},
    .static_gateway = {192, 168, 1, 0},
    .static_dns = {8, 8, 8, 8},
    .channels = {
        {
            .in_use = 1u,
            .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
            .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_FLOAT,
            .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
            .id = DEVICE_CHANNEL_ID_TEMPERATURE_1,
            .name = DEVICE_CHANNEL_NAME_TEMPERATURE_1,
            .description = "Internal temperature telemetry channel",
            .unit = "degC",
            .flags = TELEMETRY_CHANNEL_FLAG_ENABLED |
                     TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                     TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED,
            .settings = {
                .enabled = true,
                .sample_period_ms = 1000u,
                .publish_period_ms = 1000u,
                .history_interval_ms = 1000u,
                .scale = 1.0,
                .offset = 0.0,
                .warning_threshold = 60.0,
                .critical_threshold = 75.0,
            },
        },
    }};

static uint32_t g_watchdog_timeout_ms_resolved = 0u;
static volatile bool g_watchdog_kick_enabled = true;

static void app_stop_watchdog_kicks(const char *reason);

static void app_wait_for_usb_console(void)
{
    const absolute_time_t deadline = make_timeout_time_ms(15000);

    while (!stdio_usb_connected() && !time_reached(deadline))
    {
        sleep_ms(50);
    }

    if (stdio_usb_connected())
    {
        sleep_ms(500);
    }
}

static void app_init_psram_allocator(void)
{
    if (psram_allocator_init())
    {
        printf("[PSRAM]\t\tready total=%u free=%u largest=%u\n",
               (unsigned)psram_total_size(),
               (unsigned)psram_free_size(),
               (unsigned)psram_largest_free_block());
    }
    else
    {
        printf("[PSRAM]\t\tunavailable; open62541 allocator will use SRAM fallback\n");
    }
    fflush(stdout);
}

static void app_fault_loop(bool blink_led, uint32_t blink_period_ms)
{
    while (1)
    {
        if (blink_led && led_driver_is_initialized())
        {
            led_driver_toggle();
            sleep_ms(blink_period_ms);
        }
        else
        {
            tight_loop_contents();
        }
    }
}

static void app_fault_trap(const char *tag, uint32_t blink_period_ms)
{
    app_stop_watchdog_kicks(tag);
    printf("[%s]\t\tfatal trap\n", tag != NULL ? tag : "FAULT");
    fflush(stdout);
    app_fault_loop(true, blink_period_ms);
}

static void app_hardfault_handler(void)
{
    app_fault_trap("HardFault", 50u);
}

#if PICO_RP2350
static void app_memmanage_handler(void)
{
    app_fault_trap("MemManage", 75u);
}

static void app_busfault_handler(void)
{
    app_fault_trap("BusFault", 75u);
}

static void app_usagefault_handler(void)
{
    app_fault_trap("UsageFault", 75u);
}

static void app_securefault_handler(void)
{
    app_fault_trap("SecureFault", 75u);
}
#endif

static void app_install_exception_handlers(void)
{
#if PICO_RP2350
    /* On RP2350 NS builds, leaving the SDK-installed exception vectors in place
       avoids interfering with the USB bring-up path while debugging early boot. */
    return;
#else
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, app_hardfault_handler);
#endif
}

static void print_heap_usage(const char *stage)
{
    printf("[APP]\t\t%s, free heap=%u, min ever free=%u\n",
           stage,
           (unsigned)xPortGetFreeHeapSize(),
           (unsigned)xPortGetMinimumEverFreeHeapSize());
    fflush(stdout);
}

static void app_log_ota_state(void)
{
    ota_state_record_t record;

    if (!ota_flash_read_state(&record))
    {
        printf("[APP]\t\tOTA state: empty or invalid\n");
        fflush(stdout);
        return;
    }

    printf("[APP]\t\tOTA state=%s image_size=%lu crc32=0x%08lx version=%lu\n",
           ota_state_to_string(record.state),
           (unsigned long)record.image_size,
           (unsigned long)record.image_crc32,
           (unsigned long)record.version_number);
    printf("[APP]\t\tOTA staged_offset=0x%08lx staged_size=0x%08lx last_error=%s attempts=%lu\n",
           (unsigned long)record.staged_bank_offset,
           (unsigned long)record.staged_bank_size,
           ota_error_to_string(record.last_error),
           (unsigned long)record.boot_attempts);
    fflush(stdout);
}

static void app_acknowledge_ota_boot_if_needed(void)
{
    ota_state_record_t record;
    ota_flash_status_t status;

    if (!ota_flash_read_state(&record) || record.state != OTA_IMAGE_STATE_READY_TO_BOOT)
    {
        return;
    }

    status = ota_flash_reset_state();
    if (status != OTA_FLASH_STATUS_OK)
    {
        return;
    }
}

static bool create_task(TaskFunction_t entry_point,
                        const char *name,
                        configSTACK_DEPTH_TYPE stack_size,
                        UBaseType_t priority,
                        TaskHandle_t *out_handle)
{
    if (xTaskCreate(entry_point, name, stack_size, NULL, priority, out_handle) != pdPASS)
    {
        printf("[APP]\t\tFailed to create task %s, free heap=%u, min ever free=%u\n",
               name,
               (unsigned)xPortGetFreeHeapSize(),
               (unsigned)xPortGetMinimumEverFreeHeapSize());
        fflush(stdout);
        return false;
    }

    diag_service_register_task(name, *out_handle);
    printf("[APP]\t\tCreated task %s, stack=%u, free heap=%u\n",
           name,
           (unsigned)stack_size,
           (unsigned)xPortGetFreeHeapSize());
    fflush(stdout);
    return true;
}

static uint32_t app_resolve_watchdog_timeout_ms(const device_profile_config_t *config)
{
    if (config != NULL && config->watchdog_timeout_ms > 0u)
    {
        return config->watchdog_timeout_ms;
    }

    return APP_DEFAULT_WATCHDOG_TIMEOUT_MS;
}

static void app_stop_watchdog_kicks(const char *reason)
{
    g_watchdog_kick_enabled = false;

    if (watchdog_driver_is_enabled())
    {
        printf("[APP]\t\tWatchdog armed for recovery%s%s\n",
               reason != NULL ? ": " : "",
               reason != NULL ? reason : "");
        fflush(stdout);
    }
}

#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
static bool provision_device_identity_if_configured(void)
{
#if DEVICE_IDENTITY_PROVISIONING_ENABLED
    security_service_blob_view_t current_certificate = {0};
    security_service_blob_view_t current_private_key = {0};
    const bool have_same_certificate =
        security_service_get_device_certificate(&current_certificate) &&
        current_certificate.length == g_provisioned_device_certificate_length &&
        memcmp(current_certificate.data,
               g_provisioned_device_certificate,
               g_provisioned_device_certificate_length) == 0;
    const bool have_same_private_key =
        security_service_get_device_private_key(&current_private_key) &&
        current_private_key.length == g_provisioned_device_private_key_length &&
        memcmp(current_private_key.data,
               g_provisioned_device_private_key,
               g_provisioned_device_private_key_length) == 0;

    if (have_same_certificate && have_same_private_key)
    {
        printf("[APP]\t\tDevice identity already matches provisioned build assets\n");
        fflush(stdout);
        return true;
    }

    if (!security_service_set_device_identity(g_provisioned_device_certificate,
                                              g_provisioned_device_certificate_length,
                                              g_provisioned_device_private_key,
                                              g_provisioned_device_private_key_length))
    {
        printf("[APP]\t\tDevice identity provisioning failed\n");
        fflush(stdout);
        return false;
    }

    printf("[APP]\t\tProvisioned device identity from build assets\n");
    fflush(stdout);
#endif

    return true;
}

static bool provision_trust_list_if_configured(void)
{
#if DEVICE_TRUST_LIST_PROVISIONING_ENABLED
    security_service_blob_view_t current_trust_list = {0};
    const bool have_same_trust_list =
        security_service_get_trust_list(&current_trust_list) &&
        current_trust_list.length == g_provisioned_trust_list_length &&
        memcmp(current_trust_list.data,
               g_provisioned_trust_list,
               g_provisioned_trust_list_length) == 0;

    if (have_same_trust_list)
    {
        printf("[APP]\t\tTrust list already matches provisioned build assets\n");
        fflush(stdout);
        return true;
    }

    if (!security_service_set_trust_list(g_provisioned_trust_list,
                                         g_provisioned_trust_list_length))
    {
        printf("[APP]\t\tTrust list provisioning failed\n");
        fflush(stdout);
        return false;
    }

    printf("[APP]\t\tProvisioned trust list from build assets\n");
    fflush(stdout);
#endif

    return true;
}
#endif

static void watchdog_task(void *argument)
{
    (void)argument;
    const uint32_t timeout_ms = watchdog_driver_get_timeout_ms();
    const TickType_t delay_ticks = pdMS_TO_TICKS(timeout_ms > 2000u ? 1000u : timeout_ms / 2u);

    while (1)
    {
        if (g_watchdog_kick_enabled)
        {
            watchdog_driver_kick();
        }
        vTaskDelay(delay_ticks);
    }
}

static void app_init_task(void *argument)
{
    TaskHandle_t task_handle = NULL;
    TaskHandle_t watchdog_handle = NULL;
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
    opcua_model_config_t model_config;
#endif
    const device_profile_config_t *config = NULL;
    (void)argument;

    if (!flash_operation_coordinator_init())
    {
        printf("[APP]\t\tFlash operation coordinator init failed\n");
        fflush(stdout);
        app_fault_loop(true, 250u);
    }

    config_service_init(&g_basic_profile);
    config = config_service_get();

    diag_service_init();
    config_import_service_init();
    firmware_update_service_init();
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
    security_identity_import_service_init();
    security_trust_import_service_init();
#endif
    channel_manager_init();
    diag_service_register_task("InitTask", xTaskGetCurrentTaskHandle());
    print_heap_usage("After services init");

    if (!network_manager_init())
    {
        printf("[APP]\t\tNetwork manager init failed\n");
    }

    if (!sensor_manager_init())
    {
        printf("[APP]\t\tSensor manager init failed\n");
    }

    if (!history_service_init())
    {
        printf("[APP]\t\tHistory service init failed\n");
    }

#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
    if (!security_service_init())
    {
        printf("[APP]\t\tSecurity service init failed\n");
    }
    else if (!provision_device_identity_if_configured())
    {
        printf("[APP]\t\tProvisioned device identity could not be stored\n");
    }
    else if (!provision_trust_list_if_configured())
    {
        printf("[APP]\t\tProvisioned trust list could not be stored\n");
    }
#endif

    if (!time_service_init())
    {
        printf("[APP]\t\tTime service init failed\n");
    }

    if (!alarm_service_init())
    {
        printf("[APP]\t\tAlarm service init failed\n");
    }

    g_watchdog_timeout_ms_resolved = app_resolve_watchdog_timeout_ms(config);
    g_watchdog_kick_enabled = true;
    watchdog_driver_init(g_watchdog_timeout_ms_resolved);
    printf("[APP]\t\tRunning FW=%s Build=%s Git=%s\n",
           DEVICE_PROFILE_FW_REVISION,
           DEVICE_PROFILE_BUILD_TIME,
           DEVICE_PROFILE_GIT_HASH);
    printf("[APP]\t\tWatchdog enabled, requested=%u ms effective=%u ms%s\n",
           (unsigned)g_watchdog_timeout_ms_resolved,
           (unsigned)watchdog_driver_get_timeout_ms(),
           (config != NULL && config->watchdog_timeout_ms == 0u) ? " (fallback default)" : "");
    fflush(stdout);

#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
    model_config.application_name = config->device_name;
    model_config.hostname = config->opcua_hostname;
    model_config.port = config->opcua_port;
    model_config.enable_alarm_methods = config->enable_alarm_ack_method;
    model_config.enable_heap_diagnostics = config->enable_heap_diagnostics;
    model_config.enable_task_diagnostics = config->enable_task_diagnostics;
    model_config.enable_mqtt_pubsub = config->enable_mqtt_pubsub;
    model_config.mqtt_pubsub_use_json = config->mqtt_pubsub_use_json;
    model_config.mqtt_broker_url = config->mqtt_broker_url;
    model_config.mqtt_topic_root = config->mqtt_topic_root;
    if (!opcua_model_init(&model_config))
    {
        printf("[APP]\t\tOPC UA model init failed\n");
        fflush(stdout);
    }
#endif

    create_task(network_manager_task, "NetworkTask", NETWORK_TASK_STACK_SIZE, NETWORK_TASK_PRIORITY, &task_handle);

    if (channel_manager_count() > 0u)
    {
        create_task(sensor_manager_task, "SensorTask", SENSOR_TASK_STACK_SIZE, SENSOR_TASK_PRIORITY, &task_handle);
    }

    create_task(time_service_task, "TimeTask", TIME_TASK_STACK_SIZE, TIME_TASK_PRIORITY, &task_handle);
    create_task(alarm_service_task, "AlarmTask", ALARM_TASK_STACK_SIZE, ALARM_TASK_PRIORITY, &task_handle);
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
    create_task(opcua_model_task, "OpcUaTask", OPCUA_TASK_STACK_SIZE, OPCUA_TASK_PRIORITY, &task_handle);
#endif

    if (watchdog_driver_is_enabled())
    {
        if (xTaskCreate(watchdog_task,
                        "WatchdogTask",
                        WATCHDOG_TASK_STACK_SIZE,
                        (void *)&g_watchdog_timeout_ms_resolved,
                        WATCHDOG_TASK_PRIORITY,
                        &watchdog_handle) == pdPASS)
        {
            diag_service_register_task("WatchdogTask", watchdog_handle);
        }
        else
        {
            printf("[APP]\t\tFailed to create task WatchdogTask\n");
            fflush(stdout);
        }
    }

    print_heap_usage("Init task complete");
    app_acknowledge_ota_boot_if_needed();
    vTaskDelete(NULL);
}

int main(void)
{
    TaskHandle_t init_task_handle = NULL;

    platform_clock_init();
    led_driver_init(DEVICE_PROFILE_LED_PIN);
    stdio_init_all();
    app_wait_for_usb_console();
    app_init_psram_allocator();
    app_install_exception_handlers();

    printf("[APP]\t\tBooting %s FW=%s HW=%s Build=%s Git=%s\n",
           g_basic_profile.device_name,
           DEVICE_PROFILE_FW_REVISION,
           DEVICE_PROFILE_HW_REVISION,
           DEVICE_PROFILE_BUILD_TIME,
           DEVICE_PROFILE_GIT_HASH);
#ifdef W5X00_DIAGNOSTIC_NO_OPCUA
    printf("[APP] Network diagnostic build: OPC UA disabled\n");
#endif
    fflush(stdout);
    app_log_ota_state();

    if (xTaskCreate(app_init_task,
                    "InitTask",
                    INIT_TASK_STACK_SIZE,
                    NULL,
                    INIT_TASK_PRIORITY,
                    &init_task_handle) != pdPASS)
    {
        printf("[APP]\t\tFailed to create InitTask, free heap=%u, min ever free=%u\n",
               (unsigned)xPortGetFreeHeapSize(),
               (unsigned)xPortGetMinimumEverFreeHeapSize());
        fflush(stdout);
        app_fault_loop(true, 250u);
    }

    printf("[APP]\t\tStarting scheduler\n");
    fflush(stdout);
    vTaskStartScheduler();

    while (1)
    {
        tight_loop_contents();
    }
}

void app_assert_handler(const char *file, unsigned line)
{
    app_stop_watchdog_kicks("assert");
    printf("[ASSERT]\t\t%s:%u\n", file != NULL ? file : "unknown", line);
    fflush(stdout);
    app_fault_loop(true, 100u);
}

void vApplicationMallocFailedHook(void)
{
    HeapStats_t heap_stats = {0};

    vPortGetHeapStats(&heap_stats);
    diag_service_set_last_fault_code(DIAG_FAULT_CODE_MALLOC_FAILED);
    app_stop_watchdog_kicks("malloc failed");
    printf("[FreeRTOS]\tMalloc failed, req=%u, adj=%u, free heap=%u, min ever free=%u, largest block=%u, free blocks=%u\n",
           (unsigned)xPortGetLastMallocFailedRequestedSize(),
           (unsigned)xPortGetLastMallocFailedAdjustedSize(),
           (unsigned)xPortGetFreeHeapSize(),
           (unsigned)xPortGetMinimumEverFreeHeapSize(),
           (unsigned)heap_stats.xSizeOfLargestFreeBlockInBytes,
           (unsigned)heap_stats.xNumberOfFreeBlocks);
    fflush(stdout);

    app_fault_loop(true, 250u);
}

void vApplicationGetIdleTaskMemory(StaticTask_t **task_buffer,
                                   StackType_t **stack_buffer,
                                   configSTACK_DEPTH_TYPE *stack_size)
{
    static StaticTask_t idle_task;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
    *task_buffer = &idle_task;
    *stack_buffer = idle_stack;
    *stack_size = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **task_buffer,
                                    StackType_t **stack_buffer,
                                    configSTACK_DEPTH_TYPE *stack_size)
{
    static StaticTask_t timer_task;
    static StackType_t timer_stack[configTIMER_TASK_STACK_DEPTH];
    *task_buffer = &timer_task;
    *stack_buffer = timer_stack;
    *stack_size = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    diag_service_set_last_fault_code(DIAG_FAULT_CODE_STACK_OVERFLOW);
    app_stop_watchdog_kicks("stack overflow");
    printf("[FreeRTOS]\tStack overflow: %s\n", task_name);
    fflush(stdout);

    app_fault_loop(true, 500u);
}

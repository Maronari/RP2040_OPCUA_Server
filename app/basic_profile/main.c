/**
 * @file
 * @brief FreeRTOS application entry point for the `basic_profile` device
 *        profile: bare-metal startup, `InitTask` service/task bootstrap
 *        sequencing, and FreeRTOS hook implementations (fault/OOM/stack
 *        overflow handling).
 *
 * Responsibilities:
 * - `main()`: minimal pre-scheduler bring-up (clocks, LED, stdio/USB
 *   console wait, PSRAM allocator, lwIP PSRAM heap, exception handlers),
 *   then creates `InitTask` and starts the FreeRTOS scheduler. Deliberately
 *   does almost nothing else directly — see `app_init_task()`.
 * - `app_init_task()`: the actual bootstrap task. Initializes every
 *   service (config, diagnostics, channels, network, sensors, history,
 *   security, time, alarms), arms the watchdog, initializes the OPC UA
 *   model configuration, creates the remaining worker tasks
 *   (`NetworkTask`/`SensorTask`/`TimeTask`/`AlarmTask`/`OpcUaTask`/
 *   `WatchdogTask`), acknowledges a pending OTA boot, then deletes itself.
 * - Static device profile configuration (`g_basic_profile`): the
 *   compile-time default configuration seeded into `config_service_init()`.
 * - FreeRTOS hook implementations required by `FreeRTOSConfig.h`
 *   (`vApplicationMallocFailedHook`, `vApplicationGetIdleTaskMemory`,
 *   `vApplicationGetTimerTaskMemory`, `vApplicationStackOverflowHook`) and
 *   the project's `configASSERT` handler (`app_assert_handler`).
 *
 * Ownership:
 * Owns the six FreeRTOS worker tasks created here (`InitTask` transiently,
 * then `NetworkTask`, `SensorTask` (unconditionally as of v0.5.0 T4 -- a
 * transaction that activates the very first channel on a previously
 * channel-less device must have a task to hand polling off to, and an idle
 * `SensorTask` with zero channels simply polls nothing at negligible cost,
 * the same as after every channel is later removed), `TimeTask`,
 * `AlarmTask`, `OpcUaTask`, `WatchdogTask`) and the watchdog
 * kick/enable state (`g_watchdog_kick_enabled`,
 * `g_watchdog_timeout_ms_resolved`). Does not own any service's internal
 * state — this file only sequences service `_init()` calls.
 *
 * Concurrency:
 * `main()` runs pre-scheduler, single-threaded. `app_init_task()` runs as
 * FreeRTOS task `"InitTask"` at the highest application priority
 * (`INIT_TASK_PRIORITY`) but only briefly — it deletes itself
 * (`vTaskDelete(NULL)`) once all services/tasks are created. All service
 * `_init()` calls happen sequentially inside `InitTask`, before any worker
 * task runs, so there is no cross-task race during this phase. Fault/hook
 * handlers (`app_hardfault_handler` and friends, `vApplicationStackOverflowHook`,
 * `vApplicationMallocFailedHook`) may run from arbitrary task or exception
 * context and never return — see `docs/code/startup-flow.md` and
 * `docs/code/freertos.md` for the full task table and ordering notes,
 * including the documented gap that the watchdog is armed
 * (`watchdog_driver_init()`) before any worker task exists to feed it, and
 * that OTA boot acknowledgement is not a health check (see
 * `docs/code/ota-bootloader.md#health-confirmation-and-rollback--current-gap`).
 */

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
#include "lwip_psram_heap.h"
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
#include "industrial_board_bindings.h"
#include "industrial_seed_config.h"

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

/** Compile-time default device profile configuration for this device
 * target, passed to `config_service_init()` as the seed configuration used
 * when no persisted configuration is found (see `services/config_service.h`
 * for `device_profile_config_t` field semantics). Single channel: internal
 * RP2350 temperature sensor. Immutable at runtime. */
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

/** Effective watchdog timeout in milliseconds, resolved once in
 * `app_init_task()` (`app_resolve_watchdog_timeout_ms()`) and read by
 * `watchdog_task` to compute its kick interval. Written once during
 * startup, read-only afterward — no synchronization needed. */
static uint32_t g_watchdog_timeout_ms_resolved = 0u;
/** Gate for `watchdog_task`'s periodic `watchdog_driver_kick()` calls.
 * Set false by `app_stop_watchdog_kicks()` from any fault path (HardFault,
 * assert, malloc failure, stack overflow) so the hardware watchdog is
 * deliberately left un-kicked, forcing a watchdog reset as the recovery
 * mechanism. `volatile` because it is written from fault/exception context
 * and read from `watchdog_task`. */
static volatile bool g_watchdog_kick_enabled = true;

static void app_stop_watchdog_kicks(const char *reason);

/**
 * @brief Blocks (up to 15s) for a USB CDC console to connect before
 *        continuing boot, so early boot log lines are not lost.
 *
 * If a console does connect, waits an additional 500ms to let the host
 * terminal finish attaching before the first real log output. Always
 * returns eventually (never blocks forever) so a device with no attached
 * console still boots normally after the timeout.
 */
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

/**
 * @brief Initializes the PSRAM heap allocator used by open62541 and logs the
 *        result.
 *
 * @note Not fatal on failure: open62541 falls back to SRAM allocation if
 *       PSRAM is unavailable; boot continues either way.
 */
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

/**
 * @brief Terminal, non-returning fault loop: either blinks the onboard LED
 *        at @p blink_period_ms or busy-spins forever.
 *
 * The watchdog is expected to already be stopped by the caller (see
 * `app_stop_watchdog_kicks()`) so this loop is deliberately never fed,
 * making a watchdog reset the actual recovery mechanism rather than
 * anything this function does itself.
 *
 * @param blink_led If true and the LED driver is initialized, blink the LED
 *        as a visible fault indicator; otherwise spin with
 *        `tight_loop_contents()`.
 * @param blink_period_ms Delay between LED toggles, in milliseconds.
 * @note Never returns.
 */
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

/**
 * @brief Common fatal-error entry point: stops watchdog kicks, logs @p tag,
 *        then blinks the LED forever.
 *
 * Shared by every fault path in this file (hardfault/bus/usage/mem/secure
 * fault handlers, `app_assert_handler()`, PSRAM/heap init failures,
 * `vApplicationMallocFailedHook()`, `vApplicationStackOverflowHook()`).
 *
 * @param tag Short identifier logged as `[<tag>] fatal trap`; NULL is
 *        logged as `"FAULT"`.
 * @param blink_period_ms LED blink period passed through to
 *        `app_fault_loop()`.
 * @note Never returns. May be called from exception/interrupt context (a
 *       hard-fault handler) as well as task context.
 */
static void app_fault_trap(const char *tag, uint32_t blink_period_ms)
{
    app_stop_watchdog_kicks(tag);
    printf("[%s]\t\tfatal trap\n", tag != NULL ? tag : "FAULT");
    fflush(stdout);
    app_fault_loop(true, blink_period_ms);
}

/**
 * @brief RP2040-target HardFault exception handler, installed via
 *        `exception_set_exclusive_handler()` by
 *        `app_install_exception_handlers()`.
 *
 * @note Runs in exception (fault) context, not a FreeRTOS task; never
 *       returns.
 */
static void app_hardfault_handler(void)
{
    app_fault_trap("HardFault", 50u);
}

#if PICO_RP2350
/**
 * @brief RP2350 MemManage fault handler. Currently unused: on RP2350
 *        `app_install_exception_handlers()` intentionally leaves the
 *        SDK-installed vectors in place, so this function is never wired
 *        up (see that function's comment). Kept for future use.
 */
static void app_memmanage_handler(void)
{
    app_fault_trap("MemManage", 75u);
}

/** @brief RP2350 BusFault handler; currently unused, see
 * `app_memmanage_handler()`. */
static void app_busfault_handler(void)
{
    app_fault_trap("BusFault", 75u);
}

/** @brief RP2350 UsageFault handler; currently unused, see
 * `app_memmanage_handler()`. */
static void app_usagefault_handler(void)
{
    app_fault_trap("UsageFault", 75u);
}

/** @brief RP2350 SecureFault handler; currently unused, see
 * `app_memmanage_handler()`. */
static void app_securefault_handler(void)
{
    app_fault_trap("SecureFault", 75u);
}
#endif

/**
 * @brief Installs `app_hardfault_handler()` as the exclusive HardFault
 *        handler, on non-RP2350 targets only.
 *
 * @note On `PICO_RP2350`, this is a deliberate no-op: the RP2350
 *       Non-Secure-build SDK-installed exception vectors are left in place
 *       to avoid interfering with USB bring-up while debugging early boot.
 *       This means `app_memmanage_handler()`/`app_busfault_handler()`/
 *       `app_usagefault_handler()`/`app_securefault_handler()` and even
 *       `app_hardfault_handler()` are **not installed** on RP2350 builds
 *       despite being defined above.
 */
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

/**
 * @brief Logs current and historical-minimum FreeRTOS heap free bytes,
 *        tagged with @p stage.
 *
 * @param stage Label identifying the calling point (e.g. "After services
 *        init"), logged verbatim.
 */
static void print_heap_usage(const char *stage)
{
    printf("[APP]\t\t%s, free heap=%u, min ever free=%u\n",
           stage,
           (unsigned)xPortGetFreeHeapSize(),
           (unsigned)xPortGetMinimumEverFreeHeapSize());
    fflush(stdout);
}

/**
 * @brief Reads and logs the current OTA state record for diagnostics, once
 *        early in `main()`.
 *
 * Read-only: does not modify or acknowledge the OTA state (see
 * `app_acknowledge_ota_boot_if_needed()` for that).
 */
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

/**
 * @brief If the OTA state record is `READY_TO_BOOT`, unconditionally resets
 *        it to `IDLE`, marking the just-applied update as successfully
 *        booted.
 *
 * Called once from `app_init_task()`, after every worker task has already
 * been created and is running concurrently, immediately before `InitTask`
 * deletes itself. **This is not a health check**: it does not verify that
 * networking, OPC UA, or any other subsystem is actually functional — it
 * only records that boot reached this point in `InitTask` without a
 * watchdog reset having occurred first. See
 * `docs/code/ota-bootloader.md#health-confirmation-and-rollback--current-gap`
 * for the documented gap this represents relative to the `v0.6.0` roadmap
 * goal.
 *
 * @note Silently no-ops if the record cannot be read, is not
 *       `READY_TO_BOOT`, or the Flash write fails; no error is surfaced to
 *       the caller in any case.
 */
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

/**
 * @brief Creates a FreeRTOS task and registers it with the diagnostics
 *        service, logging success or failure.
 *
 * Wraps `xTaskCreate()` with no task parameter (`pvParameters = NULL`) and
 * registers the resulting handle via `diag_service_register_task()` so it
 * shows up in task diagnostics (stack high-watermark, etc.). Used for every
 * worker task created by `app_init_task()` except `WatchdogTask`, which is
 * created directly with a parameter.
 *
 * @param entry_point Task entry function.
 * @param name Task name; also used as the diagnostics registration key.
 * @param stack_size Stack depth, in `StackType_t` words (see
 *        `STACK_DEPTH_BYTES()`).
 * @param priority FreeRTOS task priority.
 * @param out_handle Receives the created task handle on success. Must not
 *        be NULL (dereferenced via `*out_handle` on success).
 * @return true if the task was created; false if `xTaskCreate()` failed
 *         (typically heap exhaustion) — logged but not fatal to the caller.
 */
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

/**
 * @brief Resolves the effective watchdog timeout: the configured value if
 *        non-zero, otherwise `APP_DEFAULT_WATCHDOG_TIMEOUT_MS`.
 *
 * @param config Active device profile configuration; may be NULL (falls
 *        back to the default).
 * @return Watchdog timeout in milliseconds, always non-zero.
 */
static uint32_t app_resolve_watchdog_timeout_ms(const device_profile_config_t *config)
{
    if (config != NULL && config->watchdog_timeout_ms > 0u)
    {
        return config->watchdog_timeout_ms;
    }

    return APP_DEFAULT_WATCHDOG_TIMEOUT_MS;
}

/**
 * @brief Permanently disables `watchdog_task`'s periodic kicks, so the
 *        hardware watchdog will reset the device instead of the current
 *        fault being silently survived.
 *
 * Called from every fault path (`app_fault_trap()`, `app_assert_handler()`,
 * `vApplicationMallocFailedHook()`, `vApplicationStackOverflowHook()`)
 * before entering a terminal loop. Idempotent; safe to call multiple times
 * or from exception context.
 *
 * @param reason Short description logged alongside the "watchdog armed for
 *        recovery" message; may be NULL.
 * @note Sets `g_watchdog_kick_enabled = false`, which `watchdog_task`
 *       polls; there is no explicit synchronization beyond the `volatile`
 *       qualifier on that flag.
 */
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
/**
 * @brief If build-time device identity assets are compiled in
 *        (`DEVICE_IDENTITY_PROVISIONING_ENABLED`), ensures the security
 *        service's persisted certificate/private key match them, writing
 *        them only if they differ.
 *
 * Compares the currently persisted device certificate and private key
 * (`security_service_get_device_certificate()`/
 * `..._get_device_private_key()`) byte-for-byte against the build-embedded
 * `g_provisioned_device_certificate`/`g_provisioned_device_private_key`; if
 * either differs (or is absent), overwrites persisted Flash storage via
 * `security_service_set_device_identity()`. When
 * `DEVICE_IDENTITY_PROVISIONING_ENABLED` is not defined, this compiles to a
 * no-op that always returns true.
 *
 * @return true if the identity already matched or was successfully
 *         (re)provisioned; false only if `security_service_set_device_identity()`
 *         failed. Called from `app_init_task()`, logged but non-fatal on
 *         failure.
 * @note Involves a Flash write on mismatch (via `security_service`), only
 *       once per boot when provisioning is enabled and assets changed.
 */
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

/**
 * @brief If a build-time trust list is compiled in
 *        (`DEVICE_TRUST_LIST_PROVISIONING_ENABLED`), ensures the security
 *        service's persisted trust list matches it, writing it only if it
 *        differs. Mirrors `provision_device_identity_if_configured()` but
 *        for `g_provisioned_trust_list` /
 *        `security_service_set_trust_list()`.
 *
 * @return true if the trust list already matched or was successfully
 *         (re)provisioned; false only on a `security_service_set_trust_list()`
 *         failure. No-op (returns true) when provisioning is not enabled.
 */
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

/**
 * @brief FreeRTOS task entry point `"WatchdogTask"`: periodically kicks the
 *        hardware watchdog to keep the device from resetting.
 *
 * Runs at `WATCHDOG_TASK_PRIORITY`, kick interval `min(1000ms,
 * timeout_ms/2)` (so the watchdog is fed at least twice per timeout
 * window). Created only if `watchdog_driver_is_enabled()` — see
 * `app_init_task()`. Every fault path in this file sets
 * `g_watchdog_kick_enabled = false` (via `app_stop_watchdog_kicks()`),
 * which this loop checks each iteration to stop kicking and let the
 * watchdog reset the device.
 *
 * @param argument Unused (task created with `pvParameters = NULL`).
 * @note Runs for the lifetime of the application; never returns under
 *       normal operation.
 */
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

/**
 * @brief FreeRTOS task entry point `"InitTask"`: the actual application
 *        bootstrap sequence, running after the scheduler has started.
 *
 * Runs once, at `INIT_TASK_PRIORITY` (the highest application priority),
 * then self-deletes (`vTaskDelete(NULL)`). Sequence (fatal steps call
 * `app_fault_loop()` and never return; others only log on failure and
 * continue — see `docs/code/startup-flow.md` for the authoritative
 * step-by-step table):
 * 1. `flash_operation_coordinator_init()` — **fatal**.
 * 2. `config_service_init(&g_basic_profile)` — **fatal**; resolves
 *    `config_service_get()` for later use.
 * 3. `diag_service_init()`, `config_import_service_init()`,
 *    `firmware_update_service_init()`,
 *    `security_identity_import_service_init()`/
 *    `security_trust_import_service_init()` (OPC UA builds only).
 * 4. `channel_manager_init()` — **fatal**.
 * 5. `network_manager_init()`, `sensor_manager_init()` — log-only.
 * 6. `history_service_init()` — **fatal**.
 * 7. `security_service_init()` + build-time identity/trust provisioning
 *    (`provision_device_identity_if_configured()`/
 *    `provision_trust_list_if_configured()`) — log-only (OPC UA builds
 *    only).
 * 8. `time_service_init()`, `alarm_service_init()` — log-only.
 * 9. Resolve and arm the hardware watchdog
 *    (`watchdog_driver_init()`) — **armed here, before any worker task
 *    exists to kick it** until `WatchdogTask` is created in step 11.
 * 10. `opcua_model_init(&model_config)` — builds the in-memory OPC UA
 *     server configuration only; the actual `UA_Server` instance is created
 *     later inside `OpcUaTask` (OPC UA builds only).
 * 11. Create worker tasks via `create_task()`: `NetworkTask`, `SensorTask`
 *     (unconditionally as of v0.5.0 T4 -- see the file-level Ownership note),
 *     `TimeTask`, `AlarmTask` always; `OpcUaTask` (OPC UA builds only);
 *     `WatchdogTask` directly via `xTaskCreate()` if
 *     `watchdog_driver_is_enabled()`.
 * 12. `app_acknowledge_ota_boot_if_needed()` — runs **after** every worker
 *     task above is already created and running concurrently; not a health
 *     check (see that function's documentation).
 * 13. `vTaskDelete(NULL)` — this task ceases to exist.
 *
 * @param argument Unused (task created with `pvParameters = NULL`).
 * @note Runs entirely in `InitTask` context; every service `_init()` call
 *       here executes sequentially before any worker task starts, so there
 *       is no cross-task race during this phase. Never returns (task
 *       deletes itself).
 */
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

    /* The industrial pack seeds every AI/AO/DI/DO/UIC channel the board
     * provides; the reference pack seeds the single internal-temperature
     * channel. Either way this only applies when no persisted configuration
     * exists -- a configured device keeps its own. */
#if APP_INDUSTRIAL_SEED_AVAILABLE
    if (!config_service_init(&g_industrial_profile))
#else
    if (!config_service_init(&g_basic_profile))
#endif
    {
        printf("[APP]\t\tConfiguration state allocation failed\n");
        fflush(stdout);
        app_fault_loop(true, 250u);
    }
    config = config_service_get();

    diag_service_init();
    config_import_service_init();
    firmware_update_service_init();
#ifndef W5X00_DIAGNOSTIC_NO_OPCUA
    security_identity_import_service_init();
    security_trust_import_service_init();
#endif
    if (!channel_manager_init())
    {
        printf("[APP]\t\tChannel registry PSRAM allocation failed\n");
        fflush(stdout);
        app_fault_loop(true, 250u);
    }
    diag_service_register_task("InitTask", xTaskGetCurrentTaskHandle());
    print_heap_usage("After services init");

    if (!network_manager_init())
    {
        printf("[APP]\t\tNetwork manager init failed\n");
    }

    /* Must precede sensor_manager_init(): that call activates the running
     * configuration, which starts channel drivers -- and an SPI-attached or
     * UIC counter-mode driver that has not been handed its backing fails
     * start() with INVALID_STATE. A no-op unless industrial drivers are
     * compiled in, so the reference build touches no industrial peripheral. */
    if (!industrial_board_bindings_apply())
    {
        printf("[APP]\t\tIndustrial board bindings failed\n");
    }

    if (!sensor_manager_init())
    {
        printf("[APP]\t\tSensor manager init failed\n");
    }

    if (!history_service_init())
    {
        printf("[APP]\t\tHistory service PSRAM allocation failed\n");
        fflush(stdout);
        app_fault_loop(true, 250u);
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

    /* v0.5.0 T4: create unconditionally, not gated on channel_manager_count()
     * > 0 at this boot instant. A later transaction can activate the first
     * channel on a previously channel-less device, and that transaction has
     * no other point at which to create this task -- an idle SensorTask
     * polling zero channels is cheap, and is already what happens whenever
     * every channel is later removed down to zero. */
    create_task(sensor_manager_task, "SensorTask", SENSOR_TASK_STACK_SIZE, SENSOR_TASK_PRIORITY, &task_handle);

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

/**
 * @brief Application entry point. Performs the minimal pre-scheduler
 *        bring-up, then hands off almost all initialization to `InitTask`.
 *
 * Sequence: `platform_clock_init()` -> `led_driver_init()` ->
 * `stdio_init_all()` -> `app_wait_for_usb_console()` (<=15s) ->
 * `app_init_psram_allocator()` -> `app_install_exception_handlers()` ->
 * `lwip_psram_heap_init()` (**fatal** via `app_fault_trap()` on failure) ->
 * log boot banner + `app_log_ota_state()` -> create `"InitTask"`
 * (**fatal** via `app_fault_loop()` if task creation fails) ->
 * `vTaskStartScheduler()`.
 *
 * All storage, network, service, and OPC UA initialization happens inside
 * `InitTask` *after* the scheduler starts — `main()` does not perform any
 * of it directly. See `docs/code/startup-flow.md` for the full flowchart.
 *
 * @return Never returns under normal operation: `vTaskStartScheduler()`
 *         does not return once the scheduler is running; the trailing
 *         `tight_loop_contents()` loop is a defensive catch-all for the
 *         (should-be-impossible) case that it does.
 */
int main(void)
{
    TaskHandle_t init_task_handle = NULL;

    platform_clock_init();
    led_driver_init(DEVICE_PROFILE_LED_PIN);
    stdio_init_all();
    app_wait_for_usb_console();
    app_init_psram_allocator();
    app_install_exception_handlers();
    if (!lwip_psram_heap_init())
    {
        app_fault_trap("lwIP PSRAM heap unavailable", 200u);
    }

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

/**
 * @brief Project-wide assertion failure handler (wired up as the
 *        `configASSERT`/project assert target in `FreeRTOSConfig.h`).
 *
 * Stops watchdog kicks, logs the failing file/line, then enters the
 * terminal fault loop. Not declared `static` because it is referenced from
 * `FreeRTOSConfig.h` (outside this translation unit); no header in this
 * file's assigned scope declares it.
 *
 * @param file Source file of the failed assertion; NULL logged as
 *        `"unknown"`.
 * @param line Source line of the failed assertion.
 * @note Never returns. May be invoked from any task or interrupt context
 *       depending on where the assertion fired.
 */
void app_assert_handler(const char *file, unsigned line)
{
    app_stop_watchdog_kicks("assert");
    printf("[ASSERT]\t\t%s:%u\n", file != NULL ? file : "unknown", line);
    fflush(stdout);
    app_fault_loop(true, 100u);
}

/**
 * @brief FreeRTOS heap-allocation-failure hook, invoked automatically by
 *        the FreeRTOS heap implementation whenever `pvPortMalloc()` cannot
 *        satisfy a request (requires `configUSE_MALLOC_FAILED_HOOK` in
 *        `FreeRTOSConfig.h`).
 *
 * Records `DIAG_FAULT_CODE_MALLOC_FAILED` via `diag_service_set_last_fault_code()`
 * (surfaced through diagnostics for post-mortem inspection), stops watchdog
 * kicks, logs detailed heap statistics, then enters the terminal fault
 * loop.
 *
 * @note Called directly from within the FreeRTOS allocator, which may be in
 *       an arbitrary task's context; never returns.
 */
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

/**
 * @brief FreeRTOS static-allocation callback supplying memory for the Idle
 *        task, required when `configSUPPORT_STATIC_ALLOCATION` is enabled.
 *
 * Provides statically-allocated (`.bss`, not heap) backing storage so the
 * Idle task's creation cannot itself fail due to heap exhaustion.
 *
 * @param task_buffer Receives a pointer to a static `StaticTask_t` TCB.
 * @param stack_buffer Receives a pointer to a static stack array of
 *        `configMINIMAL_STACK_SIZE` words.
 * @param stack_size Receives `configMINIMAL_STACK_SIZE`.
 * @note Called once by the FreeRTOS kernel during `vTaskStartScheduler()`,
 *       before any application task runs.
 */
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

/**
 * @brief FreeRTOS static-allocation callback supplying memory for the Timer
 *        Service task; see `vApplicationGetIdleTaskMemory()` for the
 *        equivalent Idle-task callback and its rationale.
 *
 * @param task_buffer Receives a pointer to a static `StaticTask_t` TCB.
 * @param stack_buffer Receives a pointer to a static stack array of
 *        `configTIMER_TASK_STACK_DEPTH` words.
 * @param stack_size Receives `configTIMER_TASK_STACK_DEPTH`.
 */
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

/**
 * @brief FreeRTOS stack-overflow hook, invoked automatically by the kernel
 *        when it detects that a task's stack has overflowed (requires
 *        `configCHECK_FOR_STACK_OVERFLOW` in `FreeRTOSConfig.h`).
 *
 * Records `DIAG_FAULT_CODE_STACK_OVERFLOW`, stops watchdog kicks, logs the
 * offending task's name, then enters the terminal fault loop.
 *
 * @param task Handle of the overflowing task (unused beyond the `(void)`
 *        cast — @p task_name is used for logging instead).
 * @param task_name Name of the overflowing task.
 * @note Called from kernel context at the point of detection, which may be
 *       well after the actual overflow occurred; never returns.
 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    diag_service_set_last_fault_code(DIAG_FAULT_CODE_STACK_OVERFLOW);
    app_stop_watchdog_kicks("stack overflow");
    printf("[FreeRTOS]\tStack overflow: %s\n", task_name);
    fflush(stdout);

    app_fault_loop(true, 500u);
}

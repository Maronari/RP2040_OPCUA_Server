/**
 * @file
 * @brief RTC/SNTP time service; see
 *        services/include/services/time_service.h for the public API
 *        contract.
 *
 * Ownership:
 * `g_status` (published time/sync status) and `g_initialized` are owned
 * exclusively by this module.
 *
 * Concurrency:
 * `g_status` has two concurrent writers: TimeTask (time_service_refresh())
 * and the lwIP SNTP callback (set_system_time(), on the tcpip thread). Each
 * updates fields inside `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`; neither
 * copies the whole struct out and back, which would let one silently
 * discard the other's update (a sync recorded by the callback between
 * TimeTask's read and write used to be lost). Hardware and SNTP calls stay
 * outside the critical sections, and the RTC driver serializes the hardware
 * itself (see rtc_driver.c).
 *
 * Both writers publish `rtc_valid` and `unix_time_s`, though, and TimeTask
 * observes the RTC *before* entering its critical section. That leaves a
 * stale-result race the field split alone cannot close: TimeTask reads the
 * RTC, the callback applies a sync and publishes it, and TimeTask then
 * publishes its older observation over the newer one -- a transient
 * `rtc_valid = false` / Bad after a successful sync until the next refresh.
 * `g_rtc_apply_generation` closes it: set_system_time() increments it with
 * every publication, time_service_refresh() samples it before observing the
 * RTC and publishes only if it is unchanged. A skipped publication loses
 * nothing -- the callback's own publication is at least as fresh, and
 * TimeTask observes again one second later.
 *
 * The lwIP SNTP client belongs to the lwIP core, not to either task that
 * starts it. sntp_setservername()/sntp_init()/sntp_stop(), and the
 * udp_new()/sys_timeout() calls inside them, are all
 * `LWIP_ASSERT_CORE_LOCKED()` in lwIP, and TimeTask (first start) and
 * OpcUaTask (time_service_request_sync_now()) are not the tcpip thread. Both
 * therefore go through time_service_start_sntp(), which holds
 * `LOCK_TCPIP_CORE()`. In this build pico-sdk's lwIP port maps that to
 * pico_lwip_custom_lock_tcpip_core(): the W5500 driver's async_context lock,
 * a recursive FreeRTOS mutex taken without a timeout, with priority
 * inheritance. Lock order: the tcpip thread holds the core lock while it
 * runs set_system_time(), which enters a critical section and takes the RTC
 * driver's lock, so the core lock is only ever taken with neither held. Until 2026-09-15 both call sites ran unlocked, racing the
 * tcpip thread's timeout list and UDP PCB list.
 *
 * `g_initialized` is set once by time_service_init() before "TimeTask"
 * starts and read afterward without synchronization (benign - written once
 * before any reader task runs).
 */
#include "services/time_service.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/rtc_driver.h"
#include "lwip/apps/sntp.h"
#include "lwip/tcpip.h"
#include "services/config_service.h"
#include "services/network_manager.h"

/* Without core locking lwIP's tcpip.h redefines LOCK_TCPIP_CORE() to
 * nothing, and the SNTP calls below would silently race the tcpip thread
 * again. lwIP's opt.h always defines the option in a real build; the
 * defined() test only keeps static analysis, which does not resolve opt.h,
 * from tripping on it. */
#if defined(LWIP_TCPIP_CORE_LOCKING) && !LWIP_TCPIP_CORE_LOCKING
#error "time_service requires LWIP_TCPIP_CORE_LOCKING to drive the SNTP client"
#endif

static time_service_status_t g_status;
static bool g_initialized = false;
/** Incremented under the critical section by every set_system_time() publication; see the file header. */
static uint32_t g_rtc_apply_generation;
/**
 * The SNTP server name as handed to lwIP. sntp_setservername() keeps the
 * pointer, not a copy, and with SNTP_SERVER_DNS lwIP re-reads the string on
 * every request (dns_gethostbyname()), so this storage must outlive every
 * call -- never a stack copy, which is what both call sites used to pass.
 * Written only under LOCK_TCPIP_CORE(), the only context lwIP reads it in.
 */
static char g_sntp_server_name[TIME_SERVICE_NTP_SERVER_MAX_LEN];

/**
 * @brief (Re)starts the lwIP SNTP client against @p server, under the lwIP
 *        core lock (see the file header).
 *
 * @param server Server name to copy into g_sntp_server_name; may point at a
 *        caller's stack copy, since only the copy is handed to lwIP.
 * @param restart true to stop a running client first (a forced re-sync);
 *        false for the first start. sntp_init() on an already-running client
 *        is a no-op in lwIP, so a first start that races a forced re-sync is
 *        harmless.
 * @pre network_manager_is_ready(): an IPv4 address implies
 *      lwip_freertos_init() -> tcpip_init() has created the core lock.
 * @pre The caller holds no critical section and not the RTC driver's lock.
 */
static void time_service_start_sntp(const char *server, bool restart)
{
    LOCK_TCPIP_CORE();
    if (restart)
    {
        sntp_stop();
    }
    strncpy(g_sntp_server_name, server, sizeof(g_sntp_server_name));
    g_sntp_server_name[sizeof(g_sntp_server_name) - 1] = '\0';
    sntp_setservername(0, g_sntp_server_name);
    sntp_init();
    UNLOCK_TCPIP_CORE();
}

/**
 * @brief Recomputes the derived fields from `rtc_valid` and
 *        `last_sync_epoch_s`; caller holds the critical section.
 */
static void time_service_derive_locked(time_service_status_t *status)
{
    status->synchronized = status->last_sync_epoch_s > 0u && status->rtc_valid;
    if (status->synchronized)
    {
        status->quality = SERVICE_QUALITY_GOOD;
    }
    else if (status->rtc_valid)
    {
        status->quality = SERVICE_QUALITY_UNCERTAIN;
    }
    else
    {
        status->quality = SERVICE_QUALITY_BAD;
    }
}

/** @brief Whole Unix seconds of a microsecond reading, clamped to the status field's range. */
static uint32_t time_service_seconds_of(uint64_t unix_time_us)
{
    const uint64_t seconds = unix_time_us / UINT64_C(1000000);

    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

bool time_service_init(void)
{
    const device_profile_config_t *config = config_service_get();
    uint64_t unix_time_us = 0u;

    memset(&g_status, 0, sizeof(g_status));
    g_rtc_apply_generation = 0u;
    g_initialized = rtc_driver_init();
    strncpy(g_status.ntp_server, config->ntp_server, sizeof(g_status.ntp_server));
    g_status.ntp_server[sizeof(g_status.ntp_server) - 1] = '\0';
    g_status.rtc_valid = g_initialized && rtc_driver_try_get_unix_time_us(&unix_time_us);
    if (g_status.rtc_valid)
    {
        g_status.unix_time_s = time_service_seconds_of(unix_time_us);
    }
    g_status.quality = g_status.rtc_valid ? SERVICE_QUALITY_UNCERTAIN : SERVICE_QUALITY_BAD;
    return g_initialized;
}

void set_system_time(uint32_t seconds)
{
    /* Only a time the RTC actually accepted counts as a sync: if the AON
     * timer rejects it, claiming synchronized/GOOD would advertise a wall
     * clock that time_service_get_utc_time_us() -- and so every OPC UA
     * timestamp -- cannot deliver. */
    const bool applied = rtc_driver_set_unix_time(seconds);
    const bool rtc_valid = rtc_driver_is_valid();

    taskENTER_CRITICAL();
    g_status.sntp_started = true;
    g_status.rtc_valid = rtc_valid;
    if (applied && rtc_valid)
    {
        g_status.unix_time_s = seconds;
        g_status.last_sync_epoch_s = seconds;
        g_status.sync_count++;
    }
    else
    {
        g_status.sync_error_count++;
    }
    time_service_derive_locked(&g_status);
    ++g_rtc_apply_generation;
    taskEXIT_CRITICAL();
}

uint32_t get_system_time(void)
{
    return rtc_driver_get_unix_time();
}

uint64_t get_system_time_us(void)
{
    return rtc_driver_get_unix_time_us();
}

bool time_service_get_utc_time_us(uint64_t *out_unix_time_us)
{
    time_service_status_t status = {0};
    uint64_t unix_time_us;

    if (out_unix_time_us == NULL)
    {
        return false;
    }

    time_service_get_status(&status);
    /* One atomic driver call: validity and the reading cannot be split by an
     * SNTP apply, and a valid reading of 0 is not mistaken for no reading. */
    if (!g_initialized || !status.rtc_valid || !rtc_driver_try_get_unix_time_us(&unix_time_us))
    {
        return false;
    }

    *out_unix_time_us = unix_time_us;
    return true;
}

int16_t get_system_timezone_offset_minutes(void)
{
    return config_service_get()->timezone_offset_minutes;
}

void time_service_refresh(void)
{
    time_service_status_t snapshot = {0};
    bool rtc_valid;
    uint32_t unix_time_s = 0u;
    uint64_t unix_time_us = 0u;
    uint32_t generation;

    time_service_get_status(&snapshot);
    if (network_manager_is_ready() && !snapshot.sntp_started)
    {
        time_service_start_sntp(snapshot.ntp_server, false);
        taskENTER_CRITICAL();
        g_status.sntp_started = true;
        taskEXIT_CRITICAL();
    }

    taskENTER_CRITICAL();
    generation = g_rtc_apply_generation;
    taskEXIT_CRITICAL();

    rtc_valid = rtc_driver_try_get_unix_time_us(&unix_time_us);
    if (rtc_valid)
    {
        unix_time_s = time_service_seconds_of(unix_time_us);
    }

    taskENTER_CRITICAL();
    /* An SNTP apply published since this observation began: it is at least
     * as fresh, so this observation must not overwrite it. */
    if (g_rtc_apply_generation == generation)
    {
        g_status.rtc_valid = rtc_valid;
        g_status.unix_time_s = unix_time_s;
        time_service_derive_locked(&g_status);
    }
    taskEXIT_CRITICAL();
}

void time_service_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();

    while (1)
    {
        time_service_refresh();
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1000));
    }
}

void time_service_get_status(time_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *out_status = g_status;
    taskEXIT_CRITICAL();
}

bool time_service_request_sync_now(void)
{
    time_service_status_t status = {0};

    if (!g_initialized || !network_manager_is_ready())
    {
        return false;
    }

    time_service_get_status(&status);
    time_service_start_sntp(status.ntp_server, true);
    taskENTER_CRITICAL();
    g_status.sntp_started = true;
    taskEXIT_CRITICAL();
    return true;
}

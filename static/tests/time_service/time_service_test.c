/**
 * @file
 * @brief Host tests for services/src/time_service.c's synchronization state.
 *
 * The RTC, SNTP, network and config dependencies are stubbed here so the
 * state machine can be driven directly: set_system_time() is what lwIP's
 * SNTP_SET_SYSTEM_TIME callback calls, time_service_refresh() is one
 * TimeTask iteration.
 */
#include "services/time_service.h"

#include "FreeRTOS.h"
#include "task.h"

#include "drivers/rtc_driver.h"
#include "lwip/apps/sntp.h"
#include "services/config_service.h"
#include "services/network_manager.h"

#include <assert.h>
#include <string.h>

/* ------------------------------------------------------------ stubs */

static int g_critical_depth;
static bool g_rtc_running;      /**< AON timer state. */
static bool g_rtc_accepts_time; /**< Whether the next AON start/set succeeds. */
static uint32_t g_rtc_time_s;
static bool g_network_ready;
static unsigned g_sntp_inits;
static device_profile_config_t g_config;

void time_service_test_enter_critical(void)
{
    assert(g_critical_depth == 0);
    ++g_critical_depth;
}

void time_service_test_exit_critical(void)
{
    assert(g_critical_depth == 1);
    --g_critical_depth;
}

TickType_t xTaskGetTickCount(void) { return 0u; }
void vTaskDelayUntil(TickType_t *previous_wake_time, TickType_t time_increment)
{
    (void)previous_wake_time;
    (void)time_increment;
}

bool rtc_driver_init(void)
{
    assert(g_critical_depth == 0);
    return true;
}

bool rtc_driver_is_valid(void)
{
    assert(g_critical_depth == 0);
    return g_rtc_running;
}

bool rtc_driver_set_unix_time(uint32_t seconds)
{
    assert(g_critical_depth == 0); /* hardware access never inside the lock */
    g_rtc_running = g_rtc_accepts_time;
    if (g_rtc_running)
    {
        g_rtc_time_s = seconds;
    }
    return g_rtc_running;
}

uint32_t rtc_driver_get_unix_time(void)
{
    assert(g_critical_depth == 0);
    return g_rtc_running ? g_rtc_time_s : 0u;
}

uint64_t rtc_driver_get_unix_time_us(void)
{
    return (uint64_t)rtc_driver_get_unix_time() * 1000000u;
}

const device_profile_config_t *config_service_get(void) { return &g_config; }
bool network_manager_is_ready(void) { return g_network_ready; }

void sntp_setservername(uint8_t idx, const char *server)
{
    (void)idx;
    assert(g_critical_depth == 0 && server != NULL);
}

void sntp_init(void)
{
    assert(g_critical_depth == 0);
    ++g_sntp_inits;
}

void sntp_stop(void) { assert(g_critical_depth == 0); }

/* ------------------------------------------------------------ helpers */

static time_service_status_t status(void)
{
    time_service_status_t snapshot;

    time_service_get_status(&snapshot);
    return snapshot;
}

static void boot(bool aon_already_running)
{
    g_critical_depth = 0;
    g_rtc_running = aon_already_running;
    g_rtc_accepts_time = true;
    g_rtc_time_s = aon_already_running ? 1789000000u : 0u;
    g_network_ready = false;
    g_sntp_inits = 0u;
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.ntp_server, "pool.ntp.org");
    assert(time_service_init());
}

/* ------------------------------------------------------------ tests */

static void test_cold_boot_is_bad_until_sntp(void)
{
    uint64_t utc = 1u;

    boot(false);
    time_service_refresh();
    assert(!status().rtc_valid && !status().synchronized && status().quality == SERVICE_QUALITY_BAD);
    assert(!time_service_get_utc_time_us(&utc) && utc == 1u);

    /* SNTP starts once, only when the network is up. */
    assert(g_sntp_inits == 0u);
    g_network_ready = true;
    time_service_refresh();
    time_service_refresh();
    assert(g_sntp_inits == 1u && status().sntp_started);

    set_system_time(1789303884u);
    assert(status().synchronized && status().quality == SERVICE_QUALITY_GOOD);
    assert(status().sync_count == 1u && status().sync_error_count == 0u);
    assert(status().last_sync_epoch_s == 1789303884u);
    time_service_refresh();
    assert(status().synchronized && status().quality == SERVICE_QUALITY_GOOD);
    assert(time_service_get_utc_time_us(&utc) && utc == UINT64_C(1789303884) * UINT64_C(1000000));
}

/** @brief The reviewed defect: an SNTP answer the AON timer rejects must not read as synchronized/GOOD. */
static void test_rejected_time_is_not_a_sync(void)
{
    uint64_t utc = 7u;

    boot(false);
    g_rtc_accepts_time = false;
    set_system_time(1789303884u);
    assert(!status().rtc_valid && !status().synchronized && status().quality == SERVICE_QUALITY_BAD);
    assert(status().sync_count == 0u && status().sync_error_count == 1u && status().last_sync_epoch_s == 0u);
    time_service_refresh(); /* the old sticky flag reappeared exactly here */
    assert(!status().synchronized && status().quality == SERVICE_QUALITY_BAD);
    assert(!time_service_get_utc_time_us(&utc) && utc == 7u);

    /* A later answer the RTC accepts recovers normally. */
    g_rtc_accepts_time = true;
    set_system_time(1789303900u);
    time_service_refresh();
    assert(status().synchronized && status().quality == SERVICE_QUALITY_GOOD);
    assert(status().sync_count == 1u && status().sync_error_count == 1u);
}

/** @brief A synchronized clock that later becomes invalid is no longer reported synchronized. */
static void test_losing_the_rtc_drops_synchronized(void)
{
    boot(false);
    set_system_time(1789303884u);
    time_service_refresh();
    assert(status().synchronized);

    g_rtc_running = false; /* AON timer stopped */
    time_service_refresh();
    assert(!status().rtc_valid && !status().synchronized && status().quality == SERVICE_QUALITY_BAD);
    assert(status().last_sync_epoch_s == 1789303884u); /* history of the last sync is kept */

    /* A resync after a failed apply while previously synchronized. */
    g_rtc_running = true;
    g_rtc_accepts_time = false;
    set_system_time(1789303999u);
    assert(!status().synchronized && status().sync_error_count == 1u);
    assert(status().last_sync_epoch_s == 1789303884u);
}

/** @brief An AON timer carried over a soft reset is a valid clock, but not an SNTP sync. */
static void test_warm_rtc_is_uncertain(void)
{
    boot(true);
    assert(status().rtc_valid && status().quality == SERVICE_QUALITY_UNCERTAIN);
    time_service_refresh();
    assert(status().rtc_valid && !status().synchronized && status().quality == SERVICE_QUALITY_UNCERTAIN);
}

/** @brief A sync recorded by the SNTP callback survives TimeTask's next refresh (no whole-struct write-back). */
static void test_sync_survives_concurrent_refresh(void)
{
    boot(false);
    g_network_ready = true;
    time_service_refresh();
    set_system_time(1789303884u);
    time_service_refresh();
    assert(status().sync_count == 1u && status().last_sync_epoch_s == 1789303884u && status().synchronized);
    assert(time_service_request_sync_now());
    assert(status().sync_count == 1u && status().synchronized && g_sntp_inits == 2u);
}

int main(void)
{
    test_cold_boot_is_bad_until_sntp();
    test_rejected_time_is_not_a_sync();
    test_losing_the_rtc_drops_synchronized();
    test_warm_rtc_is_uncertain();
    test_sync_survives_concurrent_refresh();
    return 0;
}

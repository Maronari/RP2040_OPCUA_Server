/**
 * @file
 * @brief Host tests for drivers/src/rtc_driver.c's AON-timer serialization.
 *
 * The fake AON timer below models pico-sdk 2.2.0 on RP2350, where
 * aon_timer_set_time() -> powman_timer_set_ms() stops the timer, rewrites it
 * and restarts it with no lock of its own. A one-shot hook runs *inside*
 * that stopped window, standing in for another task that preempts the
 * setter there (SensorTask or TimeTask preempting the tcpip thread's SNTP
 * callback). Any AON access while the window is open is counted as a
 * violation.
 *
 * A single-threaded test cannot block, so the fake mutex reports a contended
 * take as a timed-out one and counts it: "would have waited for the setter"
 * is the observable property, and the driver's "no valid time" result on a
 * timeout is what the hook sees.
 *
 * Built twice by static/tools/test_rtc_driver.py: once for the RP2350
 * timespec API (PICO_RP2040=0) and once for the RP2040 calendar API
 * (PICO_RP2040=1), whose date arithmetic is otherwise never compiled.
 */
#include "drivers/rtc_driver.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "pico/aon_timer.h"

#include <assert.h>
#include <string.h>

/* ------------------------------------------------------------ fake mutex */

static StaticSemaphore_t *g_mutex;
static unsigned g_contended_takes;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    assert(g_mutex == NULL); /* created once, however many times init runs */
    storage->held = false;
    g_mutex = storage;
    return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore == g_mutex);
    assert(ticks > 0u && ticks < 0xffffffffu); /* bounded, never portMAX_DELAY */
    if (semaphore->held)
    {
        /* A real task would block here until the holder gives the mutex. */
        ++g_contended_takes;
        return pdFALSE;
    }
    semaphore->held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == g_mutex && semaphore->held);
    semaphore->held = false;
    return pdTRUE;
}

static bool lock_is_released(void)
{
    return g_mutex == NULL || !g_mutex->held;
}

/* ------------------------------------------------------------ fake AON timer */

static bool g_running;
static bool g_start_accepts;
static time_t g_time_s;   /* timespec flavor */
static struct tm g_time_tm; /* calendar flavor: stored verbatim, the driver does the date math */
static bool g_in_set_window;
static unsigned g_window_violations;
static void (*g_inside_set_window)(void);

static void touch_hardware(void)
{
    if (g_in_set_window)
    {
        ++g_window_violations;
    }
}

static void open_set_window(void)
{
    void (*hook)(void) = g_inside_set_window;

    touch_hardware(); /* a set nested inside another set is a violation too */
    g_running = false;
    g_in_set_window = true;
    g_inside_set_window = NULL;
    if (hook != NULL)
    {
        hook();
    }
}

static void close_set_window(void)
{
    g_in_set_window = false;
    g_running = true;
}

bool aon_timer_is_running(void)
{
    touch_hardware();
    return g_running;
}

bool aon_timer_start(const struct timespec *ts)
{
    touch_hardware();
    if (!g_start_accepts)
    {
        return false;
    }
    g_time_s = ts->tv_sec;
    g_running = true;
    return true;
}

bool aon_timer_set_time(const struct timespec *ts)
{
    open_set_window();
    g_time_s = ts->tv_sec;
    close_set_window();
    return true;
}

bool aon_timer_get_time(struct timespec *ts)
{
    touch_hardware();
    ts->tv_sec = g_time_s;
    ts->tv_nsec = 0;
    return true;
}

bool aon_timer_start_calendar(const struct tm *tm)
{
    touch_hardware();
    if (!g_start_accepts)
    {
        return false;
    }
    g_time_tm = *tm;
    g_running = true;
    return true;
}

bool aon_timer_set_time_calendar(const struct tm *tm)
{
    open_set_window();
    g_time_tm = *tm;
    close_set_window();
    return true;
}

bool aon_timer_get_time_calendar(struct tm *tm)
{
    touch_hardware();
    *tm = g_time_tm;
    return true;
}

static void reset_hardware(bool running)
{
    g_running = running;
    g_start_accepts = true;
    g_time_s = 0;
    memset(&g_time_tm, 0, sizeof(g_time_tm));
    g_in_set_window = false;
    g_window_violations = 0u;
    g_contended_takes = 0u;
    g_inside_set_window = NULL;
}

/* ------------------------------------------------------------ concurrent callers */

static struct
{
    bool is_valid;
    uint32_t seconds;
    uint64_t microseconds;
    bool try_result;
    uint64_t try_output;
} g_seen_inside_window;

static void concurrent_readers(void)
{
    g_seen_inside_window.try_output = 12345u;
    g_seen_inside_window.is_valid = rtc_driver_is_valid();
    g_seen_inside_window.seconds = rtc_driver_get_unix_time();
    g_seen_inside_window.microseconds = rtc_driver_get_unix_time_us();
    g_seen_inside_window.try_result = rtc_driver_try_get_unix_time_us(&g_seen_inside_window.try_output);
}

static bool g_inner_set_result;

static void concurrent_setter(void)
{
    g_inner_set_result = rtc_driver_set_unix_time(4000000000u);
}

/* ------------------------------------------------------------ tests */

/** @brief Before init no lock exists yet; every read is "no time" and touches no hardware. */
static void test_reads_before_init_report_no_time(void)
{
    uint64_t unix_time_us = 7u;

    reset_hardware(true);
    assert(!rtc_driver_is_valid());
    assert(rtc_driver_get_unix_time() == 0u);
    assert(rtc_driver_get_unix_time_us() == 0u);
    assert(!rtc_driver_try_get_unix_time_us(&unix_time_us) && unix_time_us == 7u);
    assert(!rtc_driver_try_get_unix_time_us(NULL));
}

/**
 * @brief Regression: no reader may observe the AON timer while a set has it
 *        stopped.
 *
 * Before the driver lock, all four readers ran straight into the window:
 * aon_timer_is_running() returned false mid-set, so rtc_driver_is_valid()
 * and, through time_service, UA_DateTime_now() briefly reported no clock on
 * every SNTP correction -- a null OPC UA DateTime, or an invalid channel
 * SourceTimestamp.
 */
static void test_readers_are_excluded_from_the_set_window(void)
{
    const uint32_t first = 1789303884u;
    const uint32_t second = 1789307484u;
    uint64_t unix_time_us = 0u;

    reset_hardware(false);
    assert(rtc_driver_init());
    assert(rtc_driver_set_unix_time(first)); /* starts the timer */

    g_inside_set_window = concurrent_readers;
    assert(rtc_driver_set_unix_time(second)); /* stops, rewrites, restarts */
    assert(g_inside_set_window == NULL);      /* the readers really ran inside the window */
    assert(g_window_violations == 0u);
    assert(g_contended_takes == 4u); /* each one waited for the setter instead */

    /* A timed-out wait is "no time" -- never the stopped state, a stale
     * value, or a half-written one. */
    assert(!g_seen_inside_window.is_valid);
    assert(g_seen_inside_window.seconds == 0u && g_seen_inside_window.microseconds == 0u);
    assert(!g_seen_inside_window.try_result && g_seen_inside_window.try_output == 12345u);

    assert(lock_is_released());
    assert(rtc_driver_is_valid());
    assert(rtc_driver_try_get_unix_time_us(&unix_time_us) && unix_time_us == (uint64_t)second * 1000000u);
    assert(rtc_driver_get_unix_time() == second);
    assert(lock_is_released());
}

/** @brief A second setter cannot interleave with a set in progress, and timing out does not invalidate the clock. */
static void test_concurrent_setter_cannot_interleave_or_invalidate(void)
{
    reset_hardware(false);
    assert(rtc_driver_init());
    assert(rtc_driver_set_unix_time(1789303884u));

    g_inner_set_result = true;
    g_inside_set_window = concurrent_setter;
    assert(rtc_driver_set_unix_time(1789307484u));
    assert(g_window_violations == 0u && g_contended_takes == 1u);
    assert(!g_inner_set_result);
    assert(rtc_driver_is_valid());
    assert(rtc_driver_get_unix_time() == 1789307484u);
    assert(lock_is_released());
}

/** @brief Cold start: invalid until a set; a rejected start stays invalid; Unix epoch 0 is a valid reading. */
static void test_cold_start_rejected_start_and_epoch_zero(void)
{
    uint64_t unix_time_us = 99u;

    reset_hardware(false);
    assert(rtc_driver_init());
    assert(!rtc_driver_is_valid());

    g_start_accepts = false;
    assert(!rtc_driver_set_unix_time(1789303884u));
    assert(!rtc_driver_is_valid());
    assert(!rtc_driver_try_get_unix_time_us(&unix_time_us) && unix_time_us == 99u);

    g_start_accepts = true;
    assert(rtc_driver_set_unix_time(0u));
    assert(rtc_driver_try_get_unix_time_us(&unix_time_us) && unix_time_us == 0u); /* valid, and zero */
    assert(lock_is_released());
}

/** @brief An AON timer already running at init (carried over a soft reset) is valid; re-init does not recreate the lock. */
static void test_warm_timer_is_valid_at_init(void)
{
    reset_hardware(true);
    assert(rtc_driver_init());
    assert(rtc_driver_is_valid());
    assert(lock_is_released());
}

int main(void)
{
    test_reads_before_init_report_no_time(); /* must run first: no lock exists yet */
    test_readers_are_excluded_from_the_set_window();
    test_concurrent_setter_cannot_interleave_or_invalidate();
    test_cold_start_rejected_start_and_epoch_zero();
    test_warm_timer_is_valid_at_init();
    return 0;
}

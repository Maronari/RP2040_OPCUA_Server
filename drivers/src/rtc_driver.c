/**
 * @file
 * @brief RTC driver backed by the RP2350 Always-On (AON) timer.
 *
 * Responsibilities:
 * - Track whether the AON timer currently holds a validly-set time
 *   (g_rtc_valid), independent of the SDK's own running/not-running state.
 * - On RP2040, bridge between Unix epoch seconds and the SDK's
 *   `struct tm` calendar API with a self-contained Gregorian calendar
 *   calculation (the RP2040 aon_timer API has no epoch-seconds entry
 *   point); on other platforms (including RP2350) use the SDK's
 *   `struct timespec`-based API directly.
 *
 * Ownership: g_rtc_initialized/g_rtc_valid are process-lifetime globals
 * mirroring AON timer state; the AON timer hardware itself is the
 * authoritative time source. Both flags and every AON timer access are owned
 * by g_rtc_mutex.
 *
 * Concurrency: every public function is safe to call from any task, and
 * several do -- the lwIP tcpip thread (SNTP_SET_SYSTEM_TIME ->
 * set_system_time()), TimeTask, SensorTask (channel timestamps) and
 * OpcUaTask (UA_DateTime_now(), security time checks). The lock is required,
 * not defensive: on RP2350, pico-sdk 2.2.0's aon_timer_set_time() ->
 * powman_timer_set_ms() stops the timer, writes the new time in four 16-bit
 * halves and restarts it, with no locking of its own. A reader that ran
 * inside that window saw aon_timer_is_running() == false, so every SNTP
 * correction briefly made the clock read as invalid -- a null OPC UA
 * DateTime, or an invalid channel SourceTimestamp -- and a concurrent read
 * of the time itself could land between the halves.
 *
 * g_rtc_mutex is a leaf lock: nothing is called while holding it except AON
 * timer access, so it cannot form a lock-order cycle with lwIP's core lock
 * (held by the tcpip thread around the SNTP callback) or any service lock.
 * It is a FreeRTOS mutex, so priority inheritance lets a higher-priority
 * reader promote a lower-priority setter through the few register writes
 * it has left. Not callable from an ISR. Waits are bounded
 * (RTC_DRIVER_LOCK_TIMEOUT_MS); a caller that cannot take the lock reports
 * "no valid time" and changes nothing -- never a stale or partial reading.
 * rtc_driver_init() creates the lock and must complete once before any
 * concurrent use (time_service_init() does, from InitTask, before TimeTask
 * or SNTP start).
 */
#include "drivers/rtc_driver.h"

#include <limits.h>
#include <stddef.h>
#include <time.h>

#include <FreeRTOS.h>
#include <semphr.h>

#include "pico/aon_timer.h"

/* Every hold is a handful of AON register accesses (microseconds), so a
 * caller still waiting after this long is looking at a defect. It must then
 * degrade to "no valid time" rather than stall the tcpip thread or a task. */
#define RTC_DRIVER_LOCK_TIMEOUT_MS 100u

static StaticSemaphore_t g_rtc_mutex_storage;
static SemaphoreHandle_t g_rtc_mutex = NULL;
static bool g_rtc_initialized = false;
static bool g_rtc_valid = false;

#if PICO_RP2040 /* calendar arithmetic: only the RP2040 aon_timer API works in `struct tm` */
/** @brief Gregorian leap-year test (divisible by 4, not by 100 unless also by 400). */
static bool is_leap_year(int year)
{
    if ((year % 4) != 0)
    {
        return false;
    }

    if ((year % 100) != 0)
    {
        return true;
    }

    return (year % 400) == 0;
}

/** @brief Days in the given 1-based @p month of @p year, accounting for leap February. */
static uint8_t days_in_month(int year, int month)
{
    static const uint8_t days_per_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2 && is_leap_year(year))
    {
        return 29;
    }

    return days_per_month[month - 1];
}

/** @brief Decompose a Unix epoch-seconds value into a `struct tm` (UTC, no DST/yday). */
static void unix_to_calendar_tm(uint32_t seconds, struct tm *calendar_time)
{
    uint32_t days = seconds / 86400u;
    uint32_t remainder = seconds % 86400u;
    int year = 1970;
    int month = 1;

    while (days >= (uint32_t)(is_leap_year(year) ? 366 : 365))
    {
        days -= (uint32_t)(is_leap_year(year) ? 366 : 365);
        year++;
    }

    while (days >= days_in_month(year, month))
    {
        days -= days_in_month(year, month);
        month++;
    }

    calendar_time->tm_year = year - 1900;
    calendar_time->tm_mon = month - 1;
    calendar_time->tm_mday = (int)(days + 1u);
    calendar_time->tm_hour = (int)(remainder / 3600u);
    remainder %= 3600u;
    calendar_time->tm_min = (int)(remainder / 60u);
    calendar_time->tm_sec = (int)(remainder % 60u);
    calendar_time->tm_wday = (int)(((seconds / 86400u) + 4u) % 7u);
    calendar_time->tm_yday = 0;
    calendar_time->tm_isdst = 0;
}

/** @brief Inverse of unix_to_calendar_tm(): `struct tm` (UTC) back to Unix epoch seconds. */
static uint32_t calendar_tm_to_unix(const struct tm *calendar_time)
{
    const int year = calendar_time->tm_year + 1900;
    const int month = calendar_time->tm_mon + 1;
    uint32_t days = 0;

    for (int current_year = 1970; current_year < year; ++current_year)
    {
        days += (uint32_t)(is_leap_year(current_year) ? 366 : 365);
    }

    for (int current_month = 1; current_month < month; ++current_month)
    {
        days += days_in_month(year, current_month);
    }

    days += (uint32_t)(calendar_time->tm_mday - 1);

    return ((((days * 24u) + (uint32_t)calendar_time->tm_hour) * 60u) +
            (uint32_t)calendar_time->tm_min) *
               60u +
           (uint32_t)calendar_time->tm_sec;
}

/** @brief Start or update the AON timer (calendar API) from Unix seconds; caller holds g_rtc_mutex. */
static bool rtc_driver_apply_unix_time(uint32_t seconds)
{
    struct tm calendar_time = {0};

    unix_to_calendar_tm(seconds, &calendar_time);

    if (aon_timer_is_running())
    {
        return aon_timer_set_time_calendar(&calendar_time);
    }

    return aon_timer_start_calendar(&calendar_time);
}

/** @brief Read the AON timer via the calendar API as Unix seconds; caller holds g_rtc_mutex. */
static bool rtc_driver_read_unix_time(uint32_t *out_seconds)
{
    struct tm calendar_time = {0};

    if (!aon_timer_get_time_calendar(&calendar_time))
    {
        return false;
    }

    *out_seconds = calendar_tm_to_unix(&calendar_time);
    return true;
}

/** @brief RP2040 has no sub-second AON read; scales the whole-second value to microseconds. */
static bool rtc_driver_read_unix_time_us(uint64_t *out_unix_time_us)
{
    uint32_t seconds;

    if (!rtc_driver_read_unix_time(&seconds))
    {
        return false;
    }

    *out_unix_time_us = (uint64_t)seconds * 1000000ull;
    return true;
}
#else
/** @brief Start or update the AON timer (timespec API) from Unix seconds; caller holds g_rtc_mutex. */
static bool rtc_driver_apply_unix_time(uint32_t seconds)
{
    struct timespec current_time = {
        .tv_sec = (time_t)seconds,
        .tv_nsec = 0};

    if (aon_timer_is_running())
    {
        return aon_timer_set_time(&current_time);
    }

    return aon_timer_start(&current_time);
}

/** @brief Read the AON timer (timespec API) as Unix seconds, clamped to UINT32_MAX; caller holds g_rtc_mutex. */
static bool rtc_driver_read_unix_time(uint32_t *out_seconds)
{
    struct timespec current_time = {0};

    if (!aon_timer_get_time(&current_time) || current_time.tv_sec < 0)
    {
        return false;
    }

    *out_seconds = ((uint64_t)current_time.tv_sec > UINT32_MAX) ? UINT32_MAX : (uint32_t)current_time.tv_sec;
    return true;
}

/** @brief Read the AON timer (timespec API) with native sub-second (ns) resolution; caller holds g_rtc_mutex. */
static bool rtc_driver_read_unix_time_us(uint64_t *out_unix_time_us)
{
    struct timespec current_time = {0};

    if (!aon_timer_get_time(&current_time) || current_time.tv_sec < 0)
    {
        return false;
    }

    *out_unix_time_us = ((uint64_t)current_time.tv_sec * 1000000ull) +
                        ((uint64_t)current_time.tv_nsec / 1000ull);
    return true;
}
#endif

/** @brief Creates g_rtc_mutex on first use; not itself thread-safe (see rtc_driver_init()). */
static void rtc_driver_create_lock(void)
{
    if (g_rtc_mutex == NULL)
    {
        g_rtc_mutex = xSemaphoreCreateMutexStatic(&g_rtc_mutex_storage);
    }
}

/** @brief Bounded take of g_rtc_mutex; false if it does not exist yet or the wait timed out. */
static bool rtc_driver_lock(void)
{
    return g_rtc_mutex != NULL &&
           xSemaphoreTake(g_rtc_mutex, pdMS_TO_TICKS(RTC_DRIVER_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void rtc_driver_unlock(void)
{
    (void)xSemaphoreGive(g_rtc_mutex);
}

/** @brief rtc_driver_is_valid() for a caller that already holds g_rtc_mutex. */
static bool rtc_driver_is_valid_locked(void)
{
    return g_rtc_initialized && g_rtc_valid && aon_timer_is_running();
}

bool rtc_driver_init(void)
{
    rtc_driver_create_lock();
    if (!rtc_driver_lock())
    {
        return false;
    }

    g_rtc_initialized = true;
    g_rtc_valid = aon_timer_is_running();
    rtc_driver_unlock();
    return true;
}

bool rtc_driver_is_valid(void)
{
    bool valid;

    if (!rtc_driver_lock())
    {
        return false;
    }

    valid = rtc_driver_is_valid_locked();
    rtc_driver_unlock();
    return valid;
}

bool rtc_driver_set_unix_time(uint32_t seconds)
{
    bool applied;

    /* Lazy first use without rtc_driver_init(): the same single-caller
     * precondition as rtc_driver_init() itself applies. */
    rtc_driver_create_lock();
    if (!rtc_driver_lock())
    {
        /* Nothing reached the hardware, so a clock that was valid stays
         * valid; invalidating it would need the lock this call lacks. */
        return false;
    }

    g_rtc_initialized = true;
    applied = rtc_driver_apply_unix_time(seconds);
    g_rtc_valid = applied;
    rtc_driver_unlock();
    return applied;
}

uint32_t rtc_driver_get_unix_time(void)
{
    uint32_t seconds = 0u;

    if (!rtc_driver_lock())
    {
        return 0u;
    }

    if (!rtc_driver_is_valid_locked() || !rtc_driver_read_unix_time(&seconds))
    {
        seconds = 0u;
    }
    rtc_driver_unlock();
    return seconds;
}

uint64_t rtc_driver_get_unix_time_us(void)
{
    uint64_t unix_time_us = 0u;

    (void)rtc_driver_try_get_unix_time_us(&unix_time_us);
    return unix_time_us;
}

bool rtc_driver_try_get_unix_time_us(uint64_t *out_unix_time_us)
{
    uint64_t unix_time_us = 0u;
    bool valid;

    if (out_unix_time_us == NULL || !rtc_driver_lock())
    {
        return false;
    }

    valid = rtc_driver_is_valid_locked() && rtc_driver_read_unix_time_us(&unix_time_us);
    rtc_driver_unlock();
    if (valid)
    {
        *out_unix_time_us = unix_time_us;
    }
    return valid;
}

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
 * authoritative time source.
 *
 * Concurrency: no internal locking. Per docs/code/hardware.md, calls are
 * expected from TimeTask; callers must not call rtc_driver_set_unix_time()
 * concurrently with a read from another task without external
 * synchronization.
 */
#include "drivers/rtc_driver.h"

#include <limits.h>
#include <time.h>

#include "pico/aon_timer.h"

static bool g_rtc_initialized = false;
static bool g_rtc_valid = false;

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

#if PICO_RP2040
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

/** @brief Start or update the AON timer (calendar API) from Unix seconds; hardware side effect. */
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

/** @brief Read the AON timer via the calendar API and convert to Unix seconds; 0 on read failure. */
static uint32_t rtc_driver_read_unix_time(void)
{
    struct tm calendar_time = {0};

    if (!aon_timer_get_time_calendar(&calendar_time))
    {
        return 0;
    }

    return calendar_tm_to_unix(&calendar_time);
}

/** @brief RP2040 has no sub-second AON read; scales the whole-second value to microseconds. */
static uint64_t rtc_driver_read_unix_time_us(void)
{
    return (uint64_t)rtc_driver_read_unix_time() * 1000000ull;
}
#else
/** @brief Start or update the AON timer (timespec API) from Unix seconds; hardware side effect. */
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

/** @brief Read the AON timer (timespec API); clamps to UINT32_MAX, 0 on failure/negative time. */
static uint32_t rtc_driver_read_unix_time(void)
{
    struct timespec current_time = {0};

    if (!aon_timer_get_time(&current_time) || current_time.tv_sec < 0)
    {
        return 0;
    }

    if ((uint64_t)current_time.tv_sec > UINT32_MAX)
    {
        return UINT32_MAX;
    }

    return (uint32_t)current_time.tv_sec;
}

/** @brief Read the AON timer (timespec API) with native sub-second (ns) resolution. */
static uint64_t rtc_driver_read_unix_time_us(void)
{
    struct timespec current_time = {0};

    if (!aon_timer_get_time(&current_time) || current_time.tv_sec < 0)
    {
        return 0;
    }

    return ((uint64_t)current_time.tv_sec * 1000000ull) +
           ((uint64_t)current_time.tv_nsec / 1000ull);
}
#endif

bool rtc_driver_init(void)
{
    g_rtc_initialized = true;
    g_rtc_valid = aon_timer_is_running();
    return true;
}

bool rtc_driver_is_valid(void)
{
    return g_rtc_initialized && g_rtc_valid && aon_timer_is_running();
}

bool rtc_driver_set_unix_time(uint32_t seconds)
{
    if (!g_rtc_initialized)
    {
        rtc_driver_init();
    }

    g_rtc_valid = rtc_driver_apply_unix_time(seconds);
    return g_rtc_valid;
}

uint32_t rtc_driver_get_unix_time(void)
{
    if (!rtc_driver_is_valid())
    {
        return 0;
    }

    return rtc_driver_read_unix_time();
}

uint64_t rtc_driver_get_unix_time_us(void)
{
    if (!rtc_driver_is_valid())
    {
        return 0;
    }

    return rtc_driver_read_unix_time_us();
}

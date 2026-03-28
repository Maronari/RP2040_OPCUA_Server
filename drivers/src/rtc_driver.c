#include "drivers/rtc_driver.h"

#include "hardware/rtc.h"

static bool g_rtc_initialized = false;
static bool g_rtc_valid = false;

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

static uint8_t days_in_month(int year, int month)
{
    static const uint8_t days_per_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2 && is_leap_year(year))
    {
        return 29;
    }

    return days_per_month[month - 1];
}

static void unix_to_datetime(uint32_t seconds, datetime_t *datetime)
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

    datetime->year = (int16_t)year;
    datetime->month = (int8_t)month;
    datetime->day = (int8_t)(days + 1u);
    datetime->hour = (int8_t)(remainder / 3600u);
    remainder %= 3600u;
    datetime->min = (int8_t)(remainder / 60u);
    datetime->sec = (int8_t)(remainder % 60u);
    datetime->dotw = (int8_t)(((seconds / 86400u) + 4u) % 7u);
}

static uint32_t datetime_to_unix(const datetime_t *datetime)
{
    uint32_t days = 0;

    for (int year = 1970; year < datetime->year; ++year)
    {
        days += (uint32_t)(is_leap_year(year) ? 366 : 365);
    }

    for (int month = 1; month < datetime->month; ++month)
    {
        days += days_in_month(datetime->year, month);
    }

    days += (uint32_t)(datetime->day - 1);

    return ((((days * 24u) + (uint32_t)datetime->hour) * 60u) + (uint32_t)datetime->min) * 60u + (uint32_t)datetime->sec;
}

bool rtc_driver_init(void)
{
    rtc_init();
    g_rtc_initialized = true;
    return true;
}

bool rtc_driver_is_valid(void)
{
    return g_rtc_initialized && g_rtc_valid;
}

void rtc_driver_set_unix_time(uint32_t seconds)
{
    datetime_t datetime = {0};

    if (!g_rtc_initialized)
    {
        rtc_driver_init();
    }

    unix_to_datetime(seconds, &datetime);
    rtc_set_datetime(&datetime);
    g_rtc_valid = true;
}

uint32_t rtc_driver_get_unix_time(void)
{
    datetime_t datetime = {0};

    if (!g_rtc_initialized || !g_rtc_valid)
    {
        return 0;
    }

    if (!rtc_get_datetime(&datetime))
    {
        return 0;
    }

    return datetime_to_unix(&datetime);
}

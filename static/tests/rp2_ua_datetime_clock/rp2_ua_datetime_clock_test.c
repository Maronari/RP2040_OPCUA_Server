#include "rp2_ua_datetime_clock.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static bool g_clock_valid;
static uint64_t g_unix_time_us;

bool time_service_get_utc_time_us(uint64_t *out_unix_time_us)
{
    if (!g_clock_valid || out_unix_time_us == NULL)
    {
        return false;
    }

    *out_unix_time_us = g_unix_time_us;
    return true;
}

static void expect_datetime(uint64_t unix_time_us, int64_t expected_datetime_100ns)
{
    int64_t actual = INT64_C(-1);

    g_clock_valid = true;
    g_unix_time_us = unix_time_us;
    assert(rp2_ua_datetime_now(&actual));
    assert(actual == expected_datetime_100ns);
    assert((uint64_t)(actual - RP2_UA_DATETIME_UNIX_EPOCH_100NS) / UINT64_C(10) ==
           unix_time_us);
}

static void test_known_utc_dates(void)
{
    /* 1970-01-01T00:00:00.000000Z */
    expect_datetime(UINT64_C(0), INT64_C(116444736000000000));

    /* 2024-02-29T12:34:56.123456Z */
    expect_datetime(UINT64_C(1709210096123456), INT64_C(133536836961234560));

    /* 2026-07-30T00:00:00.000001Z */
    expect_datetime(UINT64_C(1785369600000001), INT64_C(134298432000000010));

    /* Last timestamp the uint32_t RTC Unix-seconds API can represent. */
    expect_datetime(UINT64_C(4294967295999999), INT64_C(159394408959999990));
}

static void test_invalid_and_output_lifetime(void)
{
    int64_t output = INT64_C(0x0123456789abcdef);

    g_clock_valid = false;
    assert(!rp2_ua_datetime_now(&output));
    assert(output == INT64_C(0x0123456789abcdef));
    assert(!rp2_ua_datetime_now(NULL));
}

static void test_out_of_range_input(void)
{
    int64_t output = INT64_C(0x0123456789abcdef);

    g_clock_valid = true;
    g_unix_time_us = UINT64_MAX;
    assert(!rp2_ua_datetime_now(&output));
    assert(output == INT64_C(0x0123456789abcdef));
}

static void test_reboot_stability(void)
{
    int64_t before_reboot;
    int64_t after_reboot;

    g_clock_valid = true;
    g_unix_time_us = UINT64_C(1709210096123456);
    assert(rp2_ua_datetime_now(&before_reboot));

    /* A retained valid RTC reading after reboot produces the same UTC value. */
    assert(rp2_ua_datetime_now(&after_reboot));
    assert(after_reboot == before_reboot);
}

int main(void)
{
    test_known_utc_dates();
    test_invalid_and_output_lifetime();
    test_out_of_range_input();
    test_reboot_stability();
    puts("RP2 OPC UA DateTime wall-clock boundary: PASS");
    return 0;
}

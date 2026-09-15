/**
 * @file
 * @brief Host tests for services/src/login_backoff.c.
 *
 * The clock is a parameter, so the scenarios drive "now" explicitly: a
 * lockout must end once the monotonic time passes its deadline, however
 * many attempts arrive in between. The companion runner
 * (static/tools/test_login_backoff.py) separately checks that
 * security_service.c feeds this module from the monotonic boot clock,
 * because the arithmetic below is only correct for a clock that advances.
 */
#include "services/login_backoff.h"

#include <assert.h>
#include <stddef.h>

#define MS_US 1000u

static void test_durations_double_and_cap(void)
{
    assert(login_backoff_duration_ms(0u) == 1000u);
    assert(login_backoff_duration_ms(1u) == 1000u);
    assert(login_backoff_duration_ms(2u) == 2000u);
    assert(login_backoff_duration_ms(3u) == 4000u);
    assert(login_backoff_duration_ms(6u) == 32000u);
    assert(login_backoff_duration_ms(7u) == 32000u);
    assert(login_backoff_duration_ms(UINT32_MAX) == 32000u);
}

/** @brief The reviewer's scenario, on a clock that advances: one wrong password, then the right one a second later. */
static void test_single_failure_expires_after_one_second(void)
{
    login_backoff_t backoff = {0};
    const uint64_t boot_plus_5s = 5000u * MS_US; /* early boot: no wall clock exists yet */

    assert(login_backoff_remaining_ms(&backoff, boot_plus_5s) == 0u);
    assert(login_backoff_record_failure(&backoff, boot_plus_5s) == 1000u);
    assert(login_backoff_remaining_ms(&backoff, boot_plus_5s) == 1000u);
    assert(login_backoff_remaining_ms(&backoff, boot_plus_5s + 999u * MS_US) == 1u);
    assert(login_backoff_remaining_ms(&backoff, boot_plus_5s + 1000u * MS_US) == 0u);
    assert(login_backoff_remaining_ms(&backoff, boot_plus_5s + 60000u * MS_US) == 0u);
}

/** @brief Rounds up: a deadline 1 us away still reports 1 ms, never 0 while locked. */
static void test_remaining_rounds_up(void)
{
    login_backoff_t backoff = {.failed_attempts = 1u, .blocked_until_us = 10u * MS_US + 1u};

    assert(login_backoff_remaining_ms(&backoff, 10u * MS_US) == 1u);
    assert(login_backoff_remaining_ms(&backoff, 10u * MS_US + 1u) == 0u);
}

/** @brief Consecutive failures grow the lockout; each is measured from its own failure time. */
static void test_consecutive_failures(void)
{
    login_backoff_t backoff = {0};
    uint64_t now = 100u * MS_US;
    uint32_t expected = 1000u;
    unsigned attempt;

    for (attempt = 1u; attempt <= 8u; ++attempt)
    {
        assert(login_backoff_record_failure(&backoff, now) == expected);
        assert(backoff.failed_attempts == attempt);
        assert(login_backoff_remaining_ms(&backoff, now) == expected);
        now += (uint64_t)expected * MS_US; /* wait out this lockout exactly */
        assert(login_backoff_remaining_ms(&backoff, now) == 0u);
        expected = expected < 32000u ? expected * 2u : 32000u;
    }

    login_backoff_reset(&backoff);
    assert(backoff.failed_attempts == 0u && backoff.blocked_until_us == 0u);
    assert(login_backoff_record_failure(&backoff, now) == 1000u);
}

/** @brief A clock that never advances never releases a lockout -- why the wall clock (0 before SNTP) must not be used. */
static void test_frozen_clock_never_releases(void)
{
    login_backoff_t backoff = {0};

    (void)login_backoff_record_failure(&backoff, 0u);
    for (unsigned poll = 0u; poll < 100u; ++poll)
    {
        assert(login_backoff_remaining_ms(&backoff, 0u) == 1000u);
    }
}

static void test_null_is_safe(void)
{
    login_backoff_reset(NULL);
    assert(login_backoff_remaining_ms(NULL, 0u) == 0u);
    assert(login_backoff_record_failure(NULL, 0u) == 0u);
}

int main(void)
{
    test_durations_double_and_cap();
    test_single_failure_expires_after_one_second();
    test_remaining_rounds_up();
    test_consecutive_failures();
    test_frozen_clock_never_releases();
    test_null_is_safe();
    return 0;
}

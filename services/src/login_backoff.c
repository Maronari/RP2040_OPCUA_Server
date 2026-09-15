/**
 * @file
 * @brief Implementation of services/include/services/login_backoff.h.
 */
#include "services/login_backoff.h"

#include <stddef.h>

void login_backoff_reset(login_backoff_t *backoff)
{
    if (backoff == NULL)
    {
        return;
    }
    backoff->failed_attempts = 0u;
    backoff->blocked_until_us = 0u;
}

uint32_t login_backoff_duration_ms(uint32_t failed_attempts)
{
    uint32_t backoff_ms = LOGIN_BACKOFF_BASE_MS;
    uint32_t steps = failed_attempts;

    while (steps > 1u && backoff_ms < LOGIN_BACKOFF_MAX_MS)
    {
        backoff_ms <<= 1u;
        if (backoff_ms > LOGIN_BACKOFF_MAX_MS)
        {
            backoff_ms = LOGIN_BACKOFF_MAX_MS;
            break;
        }
        --steps;
    }
    return backoff_ms;
}

uint32_t login_backoff_remaining_ms(const login_backoff_t *backoff, uint64_t now_us)
{
    uint64_t remaining_us;

    if (backoff == NULL || backoff->blocked_until_us <= now_us)
    {
        return 0u;
    }
    remaining_us = backoff->blocked_until_us - now_us;
    return (uint32_t)((remaining_us + 999u) / 1000u);
}

uint32_t login_backoff_record_failure(login_backoff_t *backoff, uint64_t now_us)
{
    uint32_t backoff_ms;

    if (backoff == NULL)
    {
        return 0u;
    }
    if (backoff->failed_attempts < UINT32_MAX)
    {
        ++backoff->failed_attempts;
    }
    backoff_ms = login_backoff_duration_ms(backoff->failed_attempts);
    backoff->blocked_until_us = now_us + (uint64_t)backoff_ms * 1000u;
    return backoff_ms;
}

/**
 * @file
 * @brief Exponential login backoff over a caller-supplied monotonic clock.
 *
 * Pure arithmetic with no clock of its own: every function takes `now_us`,
 * which must come from a monotonic source that advances from boot and never
 * jumps (on the device, `time_us_64()`). A wall clock is wrong here in both
 * directions: before SNTP it can read a constant 0, so a lockout never
 * expires, and an SNTP correction can end a lockout early or stretch it.
 *
 * Ownership: callers own every `login_backoff_t`; this module keeps no state.
 * Concurrency: none; callers serialize access to their records.
 */
#ifndef SERVICES_LOGIN_BACKOFF_H
#define SERVICES_LOGIN_BACKOFF_H

#include <stdint.h>

/** @brief First lockout after a failure, in milliseconds. */
#define LOGIN_BACKOFF_BASE_MS 1000u
/** @brief Longest lockout, in milliseconds; reached after six consecutive failures. */
#define LOGIN_BACKOFF_MAX_MS 32000u

/** @brief Backoff record for one account (or one shared unknown-username bucket). */
typedef struct
{
    uint32_t failed_attempts;
    uint64_t blocked_until_us; /**< Monotonic deadline; 0 when not locked. */
} login_backoff_t;

/** @brief Clear failures and any lockout; no-op for NULL. */
void login_backoff_reset(login_backoff_t *backoff);

/**
 * @brief Lockout length after @p failed_attempts consecutive failures:
 *        `LOGIN_BACKOFF_BASE_MS` doubled per additional failure, capped at
 *        `LOGIN_BACKOFF_MAX_MS`. 0 failures yields the base value.
 */
uint32_t login_backoff_duration_ms(uint32_t failed_attempts);

/**
 * @brief Milliseconds until @p backoff's lockout ends at monotonic time
 *        @p now_us, rounded up; 0 when not locked or NULL.
 */
uint32_t login_backoff_remaining_ms(const login_backoff_t *backoff, uint64_t now_us);

/**
 * @brief Record one failed attempt at monotonic time @p now_us and start the
 *        corresponding lockout.
 * @return The lockout length in milliseconds; 0 for NULL.
 */
uint32_t login_backoff_record_failure(login_backoff_t *backoff, uint64_t now_us);

#endif

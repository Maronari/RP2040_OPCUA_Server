/**
 * @file
 * @brief Fixed synthetic workload for v0.5.2 benchmark firmware images.
 *
 * This is a compile-selected test profile, not a persisted configuration
 * preset.  It builds a bounded channel layout over caller-owned configuration
 * storage and never allocates memory or accesses hardware.  Every active
 * channel owns its normal sensor-manager source context; reusing a synthetic
 * source selector after eight channels therefore does not share counters.
 */
#ifndef SERVICES_BENCHMARK_PROFILE_H
#define SERVICES_BENCHMARK_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

#include "services/config_service.h"

#ifndef RP2_BENCHMARK_PROFILE_ENABLED
#define RP2_BENCHMARK_PROFILE_ENABLED 0
#endif

#ifndef RP2_BENCHMARK_CHANNEL_COUNT
#define RP2_BENCHMARK_CHANNEL_COUNT 1
#endif

#ifndef RP2_BENCHMARK_RATE_HZ
#define RP2_BENCHMARK_RATE_HZ 1
#endif

#ifndef RP2_BENCHMARK_EVENTS_ENABLED
#define RP2_BENCHMARK_EVENTS_ENABLED 1
#endif

#ifndef RP2_BENCHMARK_HISTORY_ENABLED
#define RP2_BENCHMARK_HISTORY_ENABLED 0
#endif

#ifndef RP2_BENCHMARK_SECURITY_NONE
#define RP2_BENCHMARK_SECURITY_NONE 0
#endif

#ifndef RP2_BENCHMARK_CLIENT_COUNT
#define RP2_BENCHMARK_CLIENT_COUNT 1
#endif

#ifndef RP2_BENCHMARK_PROFILE_ID
#define RP2_BENCHMARK_PROFILE_ID "disabled"
#endif

#define BENCHMARK_PROFILE_SYNTHETIC_SOURCE_COUNT 8u

typedef enum
{
    BENCHMARK_PROFILE_BUILD_OK = 0,
    BENCHMARK_PROFILE_BUILD_DISABLED,
    BENCHMARK_PROFILE_BUILD_INVALID_ARGUMENT,
    BENCHMARK_PROFILE_BUILD_INSUFFICIENT_CAPACITY
} benchmark_profile_build_result_t;

/** @brief Whether this image has selected the fixed benchmark workload. */
bool benchmark_profile_is_enabled(void);

/**
 * @brief Builds the compile-selected synthetic workload in @p config.
 *
 * The caller retains all configuration storage.  On success, exactly
 * `RP2_BENCHMARK_CHANNEL_COUNT` leading slots are active and all remaining
 * slots in the supplied bounded array are zeroed.  A disabled profile leaves
 * @p config untouched.  The function returns insufficient capacity before
 * modifying @p config.
 */
benchmark_profile_build_result_t benchmark_profile_build(device_profile_config_t *config,
                                                         size_t channel_capacity);

/**
 * @brief Checks that a configuration still has the exact selected workload.
 *
 * This comparison is bounded by `PLATFORM_CHANNEL_LIMIT`; it does not read
 * or retain any caller-owned memory after return.
 */
bool benchmark_profile_matches_config(const device_profile_config_t *config);

#endif

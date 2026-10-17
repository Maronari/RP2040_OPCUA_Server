/** @file */
#include "services/benchmark_profile.h"

#include <stdio.h>
#include <string.h>

#if RP2_BENCHMARK_PROFILE_ENABLED != 0 && RP2_BENCHMARK_PROFILE_ENABLED != 1
#error "RP2_BENCHMARK_PROFILE_ENABLED must be 0 or 1"
#endif
#if RP2_BENCHMARK_CHANNEL_COUNT != 1 && RP2_BENCHMARK_CHANNEL_COUNT != 4 && \
    RP2_BENCHMARK_CHANNEL_COUNT != 8 && RP2_BENCHMARK_CHANNEL_COUNT != 16
#error "RP2_BENCHMARK_CHANNEL_COUNT must be 1, 4, 8, or 16"
#endif
#if RP2_BENCHMARK_RATE_HZ != 1 && RP2_BENCHMARK_RATE_HZ != 10
#error "RP2_BENCHMARK_RATE_HZ must be 1 or 10"
#endif
#if RP2_BENCHMARK_EVENTS_ENABLED != 0 && RP2_BENCHMARK_EVENTS_ENABLED != 1
#error "RP2_BENCHMARK_EVENTS_ENABLED must be 0 or 1"
#endif
#if RP2_BENCHMARK_HISTORY_ENABLED != 0 && RP2_BENCHMARK_HISTORY_ENABLED != 1
#error "RP2_BENCHMARK_HISTORY_ENABLED must be 0 or 1"
#endif
#if RP2_BENCHMARK_SECURITY_NONE != 0 && RP2_BENCHMARK_SECURITY_NONE != 1
#error "RP2_BENCHMARK_SECURITY_NONE must be 0 or 1"
#endif
#if RP2_BENCHMARK_CLIENT_COUNT != 1 && RP2_BENCHMARK_CLIENT_COUNT != 2 && \
    RP2_BENCHMARK_CLIENT_COUNT != 4
#error "RP2_BENCHMARK_CLIENT_COUNT must be 1, 2, or 4"
#endif

#if RP2_BENCHMARK_RATE_HZ == 1
#define BENCHMARK_PROFILE_PERIOD_MS 1000u
#else
#define BENCHMARK_PROFILE_PERIOD_MS 100u
#endif

_Static_assert(RP2_BENCHMARK_CHANNEL_COUNT <= PLATFORM_CHANNEL_LIMIT,
               "benchmark channel count exceeds PLATFORM_CHANNEL_LIMIT");

static uint32_t benchmark_profile_channel_flags(void)
{
    uint32_t flags = TELEMETRY_CHANNEL_FLAG_ENABLED;

#if RP2_BENCHMARK_HISTORY_ENABLED
    flags |= TELEMETRY_CHANNEL_FLAG_HISTORIZED;
#endif
#if RP2_BENCHMARK_EVENTS_ENABLED
    flags |= TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED;
#endif
    return flags;
}

static void benchmark_profile_populate_channel(config_channel_definition_t *channel,
                                               size_t index)
{
    *channel = (config_channel_definition_t){
        .in_use = 1u,
        .kind = (uint8_t)TELEMETRY_CHANNEL_KIND_ANALOG,
        .value_type = (uint8_t)TELEMETRY_CHANNEL_VALUE_DOUBLE,
        .driver_id = (uint8_t)CONFIG_CHANNEL_DRIVER_SYNTHETIC,
        .source_index = (uint8_t)(index % BENCHMARK_PROFILE_SYNTHETIC_SOURCE_COUNT),
        .source_flags = 0u,
        .flags = benchmark_profile_channel_flags(),
        .settings = {
            .enabled = true,
            .sample_period_ms = BENCHMARK_PROFILE_PERIOD_MS,
            .publish_period_ms = BENCHMARK_PROFILE_PERIOD_MS,
            .history_interval_ms = BENCHMARK_PROFILE_PERIOD_MS,
            .scale = 1.0,
        },
    };
    (void)snprintf(channel->id, sizeof(channel->id), "benchmark.synthetic.%u",
                   (unsigned)(index + 1u));
    (void)snprintf(channel->name, sizeof(channel->name), "BenchmarkSynthetic%u",
                   (unsigned)(index + 1u));
    (void)snprintf(channel->description, sizeof(channel->description),
                   "Synthetic benchmark channel %u", (unsigned)(index + 1u));
}

bool benchmark_profile_is_enabled(void)
{
    return RP2_BENCHMARK_PROFILE_ENABLED != 0;
}

benchmark_profile_build_result_t benchmark_profile_build(device_profile_config_t *config,
                                                         size_t channel_capacity)
{
    if (!benchmark_profile_is_enabled())
    {
        return BENCHMARK_PROFILE_BUILD_DISABLED;
    }
    if (config == NULL)
    {
        return BENCHMARK_PROFILE_BUILD_INVALID_ARGUMENT;
    }
    if (channel_capacity < RP2_BENCHMARK_CHANNEL_COUNT ||
        channel_capacity > PLATFORM_CHANNEL_LIMIT)
    {
        return BENCHMARK_PROFILE_BUILD_INSUFFICIENT_CAPACITY;
    }

    memset(config->channels, 0, sizeof(config->channels));
    for (size_t index = 0u; index < RP2_BENCHMARK_CHANNEL_COUNT; ++index)
    {
        benchmark_profile_populate_channel(&config->channels[index], index);
    }

    config->enable_heap_diagnostics = true;
    config->enable_task_diagnostics = true;
    config->enable_mqtt_pubsub = false;
    config->mqtt_pubsub_use_json = false;
    return BENCHMARK_PROFILE_BUILD_OK;
}

bool benchmark_profile_matches_config(const device_profile_config_t *config)
{
    if (!benchmark_profile_is_enabled() || config == NULL ||
        !config->enable_heap_diagnostics || !config->enable_task_diagnostics ||
        config->enable_mqtt_pubsub || config->mqtt_pubsub_use_json)
    {
        return false;
    }

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        config_channel_definition_t expected = {0};

        if (index < RP2_BENCHMARK_CHANNEL_COUNT)
        {
            benchmark_profile_populate_channel(&expected, index);
        }
        if (memcmp(&config->channels[index], &expected, sizeof(expected)) != 0)
        {
            return false;
        }
    }
    return true;
}

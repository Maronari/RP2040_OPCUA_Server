#include "services/benchmark_profile.h"

#include <assert.h>
#include <string.h>

static void test_rejects_invalid_arguments_without_mutation(void)
{
    device_profile_config_t config;
    device_profile_config_t before;

    memset(&config, 0xa5, sizeof(config));
    before = config;
    assert(benchmark_profile_build(NULL, PLATFORM_CHANNEL_LIMIT) ==
           BENCHMARK_PROFILE_BUILD_INVALID_ARGUMENT);
    assert(benchmark_profile_build(&config, RP2_BENCHMARK_CHANNEL_COUNT - 1u) ==
           BENCHMARK_PROFILE_BUILD_INSUFFICIENT_CAPACITY);
    assert(memcmp(&config, &before, sizeof(config)) == 0);
    assert(benchmark_profile_build(&config, PLATFORM_CHANNEL_LIMIT + 1u) ==
           BENCHMARK_PROFILE_BUILD_INSUFFICIENT_CAPACITY);
    assert(memcmp(&config, &before, sizeof(config)) == 0);
}

static void test_builds_exact_synthetic_workload(void)
{
    device_profile_config_t config = {0};
    const uint32_t expected_period = RP2_BENCHMARK_RATE_HZ == 1 ? 1000u : 100u;
    const uint32_t expected_flags = TELEMETRY_CHANNEL_FLAG_ENABLED |
#if RP2_BENCHMARK_HISTORY_ENABLED
                                    TELEMETRY_CHANNEL_FLAG_HISTORIZED |
#endif
#if RP2_BENCHMARK_EVENTS_ENABLED
                                    TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED |
#endif
                                    0u;

    assert(benchmark_profile_is_enabled());
    assert(benchmark_profile_build(&config, PLATFORM_CHANNEL_LIMIT) ==
           BENCHMARK_PROFILE_BUILD_OK);
    assert(config.enable_heap_diagnostics);
    assert(config.enable_task_diagnostics);
    assert(!config.enable_mqtt_pubsub);
    assert(!config.mqtt_pubsub_use_json);

    for (size_t index = 0u; index < RP2_BENCHMARK_CHANNEL_COUNT; ++index)
    {
        const config_channel_definition_t *channel = &config.channels[index];
        assert(channel->in_use == 1u);
        assert(channel->kind == TELEMETRY_CHANNEL_KIND_ANALOG);
        assert(channel->value_type == TELEMETRY_CHANNEL_VALUE_DOUBLE);
        assert(channel->driver_id == CONFIG_CHANNEL_DRIVER_SYNTHETIC);
        assert(channel->source_index == index % BENCHMARK_PROFILE_SYNTHETIC_SOURCE_COUNT);
        assert(channel->source_flags == 0u);
        assert(channel->flags == expected_flags);
        assert(channel->settings.enabled);
        assert(channel->settings.sample_period_ms == expected_period);
        assert(channel->settings.publish_period_ms == expected_period);
        assert(channel->settings.history_interval_ms == expected_period);
        assert(channel->settings.scale == 1.0);
    }
    for (size_t index = RP2_BENCHMARK_CHANNEL_COUNT;
         index < PLATFORM_CHANNEL_LIMIT;
         ++index)
    {
        config_channel_definition_t cleared = {0};
        assert(memcmp(&config.channels[index], &cleared, sizeof(cleared)) == 0);
    }
    assert(benchmark_profile_matches_config(&config));
    config.channels[0].source_index = 7u;
    assert(!benchmark_profile_matches_config(&config));
}

int main(void)
{
    test_rejects_invalid_arguments_without_mutation();
    test_builds_exact_synthetic_workload();
    return 0;
}

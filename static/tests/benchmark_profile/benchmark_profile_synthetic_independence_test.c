#include "drivers/native_channel_driver.h"
#include "drivers/synthetic_driver.h"
#include "services/benchmark_profile.h"

#include <assert.h>

static telemetry_channel_t make_channel(native_channel_source_t *source)
{
    return (telemetry_channel_t){
        .metadata = {.value_type = TELEMETRY_CHANNEL_VALUE_DOUBLE},
        .driver_context = source,
    };
}

int main(void)
{
    device_profile_config_t profile = {0};
    native_channel_source_t first_source = {0};
    native_channel_source_t ninth_source = {0};
    telemetry_channel_t first;
    telemetry_channel_t ninth;

    assert(benchmark_profile_build(&profile, PLATFORM_CHANNEL_LIMIT) ==
           BENCHMARK_PROFILE_BUILD_OK);
    assert(profile.channels[0].source_index == profile.channels[8].source_index);
    first_source.source_index = profile.channels[0].source_index;
    ninth_source.source_index = profile.channels[8].source_index;
    first = make_channel(&first_source);
    ninth = make_channel(&ninth_source);

    assert(native_synthetic_start(&first) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_start(&ninth) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&first, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&first, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(native_synthetic_read(&ninth, 0u) == TELEMETRY_DRIVER_RESULT_OK);
    assert(first.runtime.value.f64 == 2.0);
    assert(ninth.runtime.value.f64 == 1.0);
    return 0;
}

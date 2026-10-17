#include "services/benchmark_profile.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    device_profile_config_t config;
    device_profile_config_t before;
    memset(&config, 0xa5, sizeof(config));
    before = config;
    assert(!benchmark_profile_is_enabled());
    assert(benchmark_profile_build(&config, PLATFORM_CHANNEL_LIMIT) ==
           BENCHMARK_PROFILE_BUILD_DISABLED);
    assert(memcmp(&config, &before, sizeof(config)) == 0);
    assert(!benchmark_profile_matches_config(&config));
    return 0;
}

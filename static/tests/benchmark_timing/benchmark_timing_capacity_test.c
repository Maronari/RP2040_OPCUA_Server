#include "services/benchmark_timing.h"
#include "services/telemetry_driver.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_now_us;
static benchmark_timing_control_t g_control;
static char g_json[1024];
static size_t g_json_length;

uint64_t time_us_64(void) { return g_now_us; }
bool time_service_get_utc_time_us(uint64_t *out_unix_time_us)
{
    if (out_unix_time_us != NULL)
    {
        *out_unix_time_us = 0u;
    }
    return false;
}
bool benchmark_profile_is_enabled(void) { return true; }
void *psram_calloc(size_t count, size_t size) { return calloc(count, size); }

static telemetry_driver_result_t read_ok(telemetry_channel_t *channel, uint32_t now_ms)
{
    channel->runtime.value.f32 = (float)now_ms;
    return TELEMETRY_DRIVER_RESULT_OK;
}

static const telemetry_driver_vtbl_t g_driver = {.read = read_ok};

static benchmark_timing_control_t control_poll(void *context)
{
    const benchmark_timing_control_t control = g_control;

    (void)context;
    g_control = BENCHMARK_TIMING_CONTROL_NONE;
    return control;
}

static bool output(const char *bytes, size_t length, void *context)
{
    (void)context;
    assert(g_json_length + length < sizeof(g_json));
    memcpy(&g_json[g_json_length], bytes, length);
    g_json_length += length;
    g_json[g_json_length] = '\0';
    return true;
}

int main(void)
{
    assert(channel_manager_init());
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        char id[TELEMETRY_CHANNEL_ID_MAX_LEN];
        telemetry_channel_t channel = {0};

        (void)snprintf(id, sizeof(id), "benchmark.synthetic.%u", (unsigned)(index + 1u));
        channel.metadata.id = id;
        channel.metadata.name = id;
        channel.metadata.description = "test";
        channel.metadata.unit = "";
        channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
        channel.settings.enabled = true;
        channel.driver = &g_driver;
        assert(channel_manager_register(&channel, NULL));
    }
    assert(benchmark_timing_init());
    benchmark_timing_set_control(control_poll, NULL);
    benchmark_timing_set_output(output, NULL);
    g_control = BENCHMARK_TIMING_CONTROL_START;
    benchmark_timing_sensor_task_process_control();
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"records\":[]") != NULL);
    return 0;
}

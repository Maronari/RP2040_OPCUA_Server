#include "services/benchmark_timing.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static benchmark_timing_control_t g_control;
static char g_json[1024];
static size_t g_json_length;

uint64_t time_us_64(void) { return 0u; }
bool benchmark_profile_is_enabled(void) { return true; }
void *psram_calloc(size_t count, size_t size)
{
    (void)count;
    (void)size;
    return NULL;
}
bool channel_manager_copy_snapshot_by_index(size_t index, telemetry_channel_snapshot_t *out_snapshot)
{
    (void)index;
    (void)out_snapshot;
    return false;
}

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
    /* A startup success before main's boot marker has no trustworthy cold
     * start boundary.  The evidence must expose invalidity, never zero. */
    benchmark_timing_note_opcua_ready();
    assert(!benchmark_timing_init());
    benchmark_timing_set_control(control_poll, NULL);
    benchmark_timing_set_output(output, NULL);
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"unavailable\"") != NULL);
    assert(strstr(g_json, "\"records\":[]") != NULL);
    assert(strstr(g_json, "\"opcuaReadyMonotonicUs\":{\"state\":\"invalid\",\"reason\":\"OPC UA ready marker has invalid boot ordering\",\"unit\":\"microseconds\"}") != NULL);
    assert(strstr(g_json, "\"startupLatencyMs\":{\"state\":\"invalid\",\"reason\":\"OPC UA ready marker has invalid boot ordering\",\"unit\":\"milliseconds\"") != NULL);
    return 0;
}

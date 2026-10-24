#include "services/benchmark_timing.h"
#include "services/telemetry_driver.h"
#include "services/time_service.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_now_us;
static benchmark_timing_control_t g_control;
static char *g_json;
static size_t g_json_length;
static bool g_output_fail;
static bool g_fail_second_channel;

uint64_t time_us_64(void)
{
    return g_now_us;
}

bool time_service_get_utc_time_us(uint64_t *out_unix_time_us)
{
    if (out_unix_time_us != NULL)
    {
        *out_unix_time_us = 1700000000000000ULL;
    }
    return true;
}

bool benchmark_profile_is_enabled(void)
{
    return true;
}

void *psram_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static telemetry_driver_result_t read_ok(telemetry_channel_t *channel, uint32_t now_ms)
{
    if (g_fail_second_channel && strcmp(channel->metadata.id, "benchmark.synthetic.2") == 0)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    channel->runtime.value.f32 = (float)now_ms;
    return TELEMETRY_DRIVER_RESULT_OK;
}

static const telemetry_driver_vtbl_t g_driver = {.read = read_ok};

static benchmark_timing_control_t control_poll(void *context)
{
    (void)context;
    const benchmark_timing_control_t control = g_control;

    g_control = BENCHMARK_TIMING_CONTROL_NONE;
    return control;
}

static bool output(const char *bytes, size_t length, void *context)
{
    (void)context;
    if (g_output_fail)
    {
        return false;
    }
    assert(g_json_length + length < BENCHMARK_TIMING_MAX_JSON_BYTES);
    memcpy(&g_json[g_json_length], bytes, length);
    g_json_length += length;
    g_json[g_json_length] = '\0';
    return true;
}

static void register_channel(const char *id)
{
    telemetry_channel_t channel = {0};

    channel.metadata.id = id;
    channel.metadata.name = id;
    channel.metadata.description = "test";
    channel.metadata.unit = "";
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    channel.settings.enabled = true;
    channel.driver = &g_driver;
    assert(channel_manager_register(&channel, NULL));
}

static void poll_at(uint64_t now_us)
{
    g_now_us = now_us;
    const channel_manager_sample_observer_t observer = benchmark_timing_sensor_poll_begin();
    (void)channel_manager_poll_all_observed((uint32_t)(now_us / 1000u), observer);
    benchmark_timing_sensor_poll_end();
}

int main(int argc, char **argv)
{
    uint64_t started_monotonic_us;

    g_json = calloc(BENCHMARK_TIMING_MAX_JSON_BYTES, 1u);
    assert(g_json != NULL);
    assert(channel_manager_init());
    register_channel("benchmark.synthetic.1");
    register_channel("benchmark.synthetic.2");
    benchmark_timing_note_boot_started();
    assert(benchmark_timing_init());
    benchmark_timing_set_control(control_poll, NULL);
    benchmark_timing_set_output(output, NULL);
    g_now_us = 5123u;
    benchmark_timing_note_opcua_ready();
    /* A controlled-restart startup marker cannot replace the cold-start
     * boundary recorded above. */
    g_now_us = 6123u;
    benchmark_timing_note_opcua_ready();

    /* Legacy activation/verify-style polling never has a timing observer. */
    assert(channel_manager_poll_all(9000u));
    g_control = BENCHMARK_TIMING_CONTROL_START;
    benchmark_timing_sensor_task_process_control();
    poll_at(9000000ULL);  /* still in the configured warmup */
    poll_at(10010000ULL); /* starts capture: sequence one */
    /* A start command is illegal during capture and cannot reset it. */
    g_control = BENCHMARK_TIMING_CONTROL_START;
    benchmark_timing_sensor_task_process_control();
    poll_at(11010000ULL); /* sequence two */
    poll_at(41010000ULL); /* deadline: this poll adds no record */
    assert(!benchmark_timing_dump_if_ready());
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"complete\"") != NULL);
    assert(strstr(g_json, "\"profileId\":\"native-test\"") != NULL);
    assert(strstr(g_json, "\"captureId\":1") != NULL);
    assert(strstr(g_json, "\"opcuaReadyMonotonicUs\":{\"state\":\"measured\",\"value\":5123,\"unit\":\"microseconds\"}") != NULL);
    assert(strstr(g_json, "\"startupLatencyMs\":{\"state\":\"measured\",\"value\":5.123,\"unit\":\"milliseconds\",\"boundary\":\"main-entry-to-UA_Server_run_startup-success\"}") != NULL);
    assert(strstr(g_json, "\"channelId\":\"benchmark.synthetic.1\",\"sequence\":1,\"sampleMonotonicUs\":10010000") != NULL);
    assert(strstr(g_json, "\"channelId\":\"benchmark.synthetic.2\",\"sequence\":2,\"sampleMonotonicUs\":11010000") != NULL);
    assert(strstr(g_json, "\"sampleMonotonicUs\":9000000") == NULL);
    assert(strstr(g_json, "\"sampleMonotonicUs\":41010000") == NULL);
    if (argc >= 2)
    {
        FILE *const output_file = fopen(argv[1], "wb");

        assert(output_file != NULL);
        assert(fwrite(g_json, 1u, g_json_length, output_file) == g_json_length);
        assert(fclose(output_file) == 0);
    }

    /* A second explicit capture gets a fresh identity and never reuses the
     * completed run's records or per-channel sequence. */
    g_json_length = 0u;
    g_json[0] = '\0';
    g_now_us = 50000000ULL;
    g_control = BENCHMARK_TIMING_CONTROL_START;
    benchmark_timing_sensor_task_process_control();
    poll_at(60000000ULL);
    poll_at(90000000ULL);
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    /* A failed serial write keeps the retained document for an explicit
     * retry; it does not make a partial document successful. */
    g_output_fail = true;
    assert(!benchmark_timing_dump_if_ready());
    g_output_fail = false;
    assert(!benchmark_timing_dump_if_ready());
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"captureId\":2") != NULL);
    assert(strstr(g_json, "\"sequence\":1,\"sampleMonotonicUs\":60000000") != NULL);

    /* A deadline with an expected channel never successfully published is a
     * failed run, never a complete distribution with a missing channel. */
    g_json_length = 0u;
    g_json[0] = '\0';
    g_now_us = 100000000ULL;
    g_fail_second_channel = true;
    g_control = BENCHMARK_TIMING_CONTROL_START;
    benchmark_timing_sensor_task_process_control();
    poll_at(110000000ULL);
    poll_at(140000000ULL);
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"captureId\":3") != NULL);
    g_fail_second_channel = false;

    /* A leased (therefore skipped) slot is indistinguishable from no valid
     * sample to the capture and fails the full-window observation bitmap. */
    g_json_length = 0u;
    g_json[0] = '\0';
    g_now_us = 150000000ULL;
    g_control = BENCHMARK_TIMING_CONTROL_START;
    benchmark_timing_sensor_task_process_control();
    channel_manager_driver_operation_t held_operation = {0};
    assert(channel_manager_acquire_driver_operation(1u, &held_operation));
    poll_at(160000000ULL);
    channel_manager_release_driver_operation(&held_operation);
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"captureId\":4") != NULL);

    /* The fixed record limit is terminal and emitted as exhaustion, never a
     * silently shortened successful distribution. */
    g_json_length = 0u;
    g_json[0] = '\0';
    g_now_us = 200000000ULL;
    g_control = BENCHMARK_TIMING_CONTROL_START;
    benchmark_timing_sensor_task_process_control();
    for (size_t index = 0u; index < BENCHMARK_TIMING_MAX_RECORDS / 2u; ++index)
    {
        poll_at(210000000ULL);
    }
    poll_at(210000000ULL);
    g_control = BENCHMARK_TIMING_CONTROL_DUMP;
    benchmark_timing_sensor_task_process_control();
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"exhausted\"") != NULL);
    assert(strstr(g_json, "\"captureId\":5") != NULL);

    /* The independent T2 budget capture measures one hundred successful
     * coherent snapshots.  No channel timing command starts this capture and
     * its JSON is emitted only after the final sample. */
    g_json_length = 0u;
    g_json[0] = '\0';
    g_now_us = 300000000ULL;
    for (uint64_t duration_us = 1u;
         duration_us <= BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT;
         ++duration_us)
    {
        const uint64_t started_monotonic_us = benchmark_timing_diag_sample_begin();
        const uint64_t phases_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT] = {
            duration_us,
            duration_us + 1u,
            duration_us + 2u,
            duration_us + 3u,
            duration_us + 4u,
            duration_us + 5u,
            duration_us + 6u,
        };
        g_now_us += duration_us;
        benchmark_timing_diag_sample_set_phase_durations(phases_us);
        benchmark_timing_diag_sample_end(started_monotonic_us, true);
        if (duration_us == BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT)
        {
            /* The final HWM makes the completed record visible. */
            assert(!benchmark_timing_dump_if_ready());
        }
        benchmark_timing_diag_sample_set_opcua_task_stack_high_water(8192u - duration_us);
    }
    /* The completed snapshot is immutable while SensorTask may be preparing
     * its serial output. */
    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(1u);
    assert(benchmark_timing_dump_if_ready());
    assert(g_json_length <= BENCHMARK_TIMING_DIAG_BUDGET_MAX_JSON_BYTES);
    assert(strstr(g_json, "\"kind\":\"v052-diag-snapshot-budget-evidence\"") != NULL);
    assert(strstr(g_json, "\"state\":\"complete\"") != NULL);
    assert(strstr(g_json, "\"sampleTarget\":100") != NULL);
    assert(strstr(g_json, "\"captureLimitSeconds\":120") != NULL);
    assert(strstr(g_json, "\"sampleCount\":100") != NULL);
    assert(strstr(g_json, "\"p99\":99") != NULL);
    assert(strstr(g_json, "\"max\":100") != NULL);
    assert(strstr(g_json, "\"opcUaTaskStackHighWaterBytes\":8092") != NULL);
    assert(strstr(g_json, "\"phaseMaxUs\":{\"acquire\":100,\"copy\":101,\"heap\":102,\"psram\":103,\"lwip\":104,\"tasks\":105,\"publish\":106}") != NULL);
    assert(strstr(g_json, "\"maxSamplePhaseUs\":{\"acquire\":100,\"copy\":101,\"heap\":102,\"psram\":103,\"lwip\":104,\"tasks\":105,\"publish\":106}") != NULL);
    assert(strstr(g_json, "\"baselineComparisonState\":\"not-run\"") != NULL);
    if (argc >= 3)
    {
        FILE *const budget_output_file = fopen(argv[2], "wb");

        assert(budget_output_file != NULL);
        assert(fwrite(g_json, 1u, g_json_length, budget_output_file) == g_json_length);
        assert(fclose(budget_output_file) == 0);
    }

    /* A failed coherent snapshot is terminal and emitted as failed, never as
     * a shortened successful timing distribution. */
    assert(benchmark_timing_init());
    g_json_length = 0u;
    g_json[0] = '\0';
    g_now_us += 1000u;
    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(8000u);
    benchmark_timing_diag_sample_end(benchmark_timing_diag_sample_begin(), false);
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"kind\":\"v052-diag-snapshot-budget-evidence\"") != NULL);
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"sampleCount\":0") != NULL);

    /* Phase aggregates are serialized as UInt32 microseconds. A malformed
     * 64-bit phase value fails the bounded capture before any narrowing. */
    assert(benchmark_timing_init());
    g_json_length = 0u;
    g_json[0] = '\0';
    uint64_t invalid_phases_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT] = {0};
    invalid_phases_us[BENCHMARK_TIMING_DIAG_PHASE_TASKS] = UINT64_MAX;
    benchmark_timing_diag_sample_set_phase_durations(invalid_phases_us);
    benchmark_timing_diag_sample_end(benchmark_timing_diag_sample_begin(), true);
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"sampleCount\":0") != NULL);

    /* Duration storage is compact uint32 microseconds: the inclusive limit
     * is retained exactly, while the first value that cannot be represented
     * terminates the evidence rather than narrowing it. */
    assert(benchmark_timing_init());
    g_json_length = 0u;
    g_json[0] = '\0';
    started_monotonic_us = benchmark_timing_diag_sample_begin();
    g_now_us += UINT32_MAX;
    benchmark_timing_diag_sample_end(started_monotonic_us, true);
    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(7997u);
    benchmark_timing_diag_sample_end(benchmark_timing_diag_sample_begin(), false);
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"sampleCount\":1") != NULL);

    assert(benchmark_timing_init());
    g_json_length = 0u;
    g_json[0] = '\0';
    started_monotonic_us = benchmark_timing_diag_sample_begin();
    g_now_us += UINT64_C(1);
    g_now_us += UINT32_MAX;
    benchmark_timing_diag_sample_end(started_monotonic_us, true);
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"sampleCount\":0") != NULL);

    /* The automatic capture cannot quietly extend past its bounded 120 s
     * window if a nominal one-second snapshot cadence stalls. */
    assert(benchmark_timing_init());
    g_json_length = 0u;
    g_json[0] = '\0';
    g_now_us += 1000u;
    started_monotonic_us = benchmark_timing_diag_sample_begin();
    ++g_now_us;
    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(7999u);
    benchmark_timing_diag_sample_end(started_monotonic_us, true);
    g_now_us += UINT64_C(120000001);
    started_monotonic_us = benchmark_timing_diag_sample_begin();
    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(7998u);
    benchmark_timing_diag_sample_end(started_monotonic_us, true);
    assert(benchmark_timing_dump_if_ready());
    assert(strstr(g_json, "\"state\":\"failed\"") != NULL);
    assert(strstr(g_json, "\"sampleCount\":1") != NULL);

    free(g_json);
    puts("benchmark timing native suite: PASS");
    return 0;
}

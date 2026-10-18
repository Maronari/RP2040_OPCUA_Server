/**
 * @file
 * @brief PSRAM-only bounded native sampling evidence for benchmark images.
 */
#include "services/benchmark_timing.h"

#if RP2_BENCHMARK_PROFILE_ENABLED

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "pico/stdlib.h"
#include "psram_allocator.h"
#include "services/benchmark_profile.h"
#include "services/platform_limits.h"

#if RP2_BENCHMARK_TIMING_WARMUP_SECONDS > 120u
#error "benchmark warmup must not exceed the T0 contract bound"
#endif
#if RP2_BENCHMARK_TIMING_CAPTURE_SECONDS == 0u || RP2_BENCHMARK_TIMING_CAPTURE_SECONDS > 900u
#error "benchmark capture duration must be in the T0 contract range 1..900 seconds"
#endif

typedef enum
{
    BENCHMARK_TIMING_STATE_UNINITIALIZED = 0,
    BENCHMARK_TIMING_STATE_READY,
    BENCHMARK_TIMING_STATE_WARMUP,
    BENCHMARK_TIMING_STATE_CAPTURING,
    BENCHMARK_TIMING_STATE_COMPLETE,
    BENCHMARK_TIMING_STATE_EXHAUSTED,
    BENCHMARK_TIMING_STATE_FAILED,
    BENCHMARK_TIMING_STATE_UNAVAILABLE
} benchmark_timing_state_t;

typedef enum
{
    BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING = 0,
    /* Quantiles are ready, but the terminating OpcUaTask HWM observation is
     * still pending outside the measured snapshot interval. */
    BENCHMARK_TIMING_DIAG_BUDGET_FINALIZING_STACK_HWM,
    BENCHMARK_TIMING_DIAG_BUDGET_COMPLETE,
    BENCHMARK_TIMING_DIAG_BUDGET_FAILED
} benchmark_timing_diag_budget_state_t;

/* 16 bytes: 8192 observations consume precisely 131072 bytes of PSRAM. */
typedef struct
{
    uint64_t sample_monotonic_us;
    uint32_t sequence;
    uint8_t channel_slot;
    uint8_t reserved[3];
} benchmark_timing_record_t;

typedef struct
{
    char channel_ids[PLATFORM_CHANNEL_LIMIT][TELEMETRY_CHANNEL_ID_MAX_LEN];
    uint32_t sequences[PLATFORM_CHANNEL_LIMIT];
    benchmark_timing_record_t records[BENCHMARK_TIMING_MAX_RECORDS];
} benchmark_timing_storage_t;

typedef struct
{
    benchmark_timing_storage_t *storage;
    benchmark_timing_state_t state;
    benchmark_timing_control_poll_fn control_poll;
    void *control_context;
    benchmark_timing_output_fn output;
    void *output_context;
    uint64_t boot_started_monotonic_us;
    uint64_t capture_started_monotonic_us;
    uint64_t capture_ended_monotonic_us;
    uint64_t warmup_deadline_monotonic_us;
    uint64_t capture_deadline_monotonic_us;
    uint64_t capture_id;
    size_t record_count;
    size_t mapped_channel_count;
    uint32_t expected_channel_mask;
    uint32_t current_poll_seen_mask;
    bool current_poll_open;
    bool current_poll_deadline_crossed;
    bool dump_pending;
} benchmark_timing_context_t;

/* This state is shared only by OpcUaTask (sample) and SensorTask (dump).
 * A short FreeRTOS critical section protects each coherent transfer. The
 * explicit fixed layout makes the synthetic-only internal-SRAM cost exact. */
typedef struct
{
    /* Snapshot durations are bounded to UINT32_MAX before storage. The
     * public evidence fields remain uint64_t device-monotonic values. */
    uint32_t durations_us[BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT];
    uint32_t sort_scratch_us[BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT];
    uint64_t started_monotonic_us;
    uint64_t ended_monotonic_us;
    uint64_t p99_us;
    uint64_t max_us;
    uint64_t opcua_task_stack_high_water_bytes;
    uint64_t generation;
    uint32_t phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT];
    uint32_t max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT];
    uint32_t current_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT];
    uint32_t sample_count;
    uint8_t state;
    bool output_attempted;
    uint8_t reserved[2];
} benchmark_timing_diag_budget_storage_t;

typedef struct
{
    benchmark_timing_diag_budget_state_t state;
    uint64_t started_monotonic_us;
    uint64_t ended_monotonic_us;
    uint64_t p99_us;
    uint64_t max_us;
    uint64_t opcua_task_stack_high_water_bytes;
    uint32_t phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT];
    uint32_t max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT];
    uint32_t sample_count;
} benchmark_timing_diag_budget_snapshot_t;

typedef struct
{
    benchmark_timing_output_fn output;
    void *context;
    size_t bytes;
    bool ok;
} benchmark_timing_writer_t;

static benchmark_timing_context_t g_timing;
static benchmark_timing_diag_budget_storage_t g_diag_budget;

_Static_assert(sizeof(benchmark_timing_record_t) == 16u,
               "native sampling record size is part of the PSRAM bound");
_Static_assert(sizeof(benchmark_timing_storage_t) == 131648u,
               "native sampling storage size is part of the PSRAM bound");
_Static_assert(sizeof(benchmark_timing_diag_budget_storage_t) == 944u,
               "T2 diagnostic timing storage must remain a fixed SRAM bound");
_Static_assert(PLATFORM_CHANNEL_LIMIT <= 32u,
               "sampling observation bitmap must cover every channel slot");

static uint64_t benchmark_timing_now_us(void)
{
    return time_us_64();
}

static bool benchmark_timing_add_seconds(uint64_t start_us, uint32_t seconds, uint64_t *out_deadline)
{
    const uint64_t duration_us = (uint64_t)seconds * UINT64_C(1000000);

    if (out_deadline == NULL || UINT64_MAX - start_us < duration_us)
    {
        return false;
    }
    *out_deadline = start_us + duration_us;
    return true;
}

static bool benchmark_timing_reached(uint64_t now_us, uint64_t deadline_us)
{
    return now_us >= deadline_us;
}

static const char *benchmark_timing_state_text(benchmark_timing_state_t state)
{
    switch (state)
    {
    case BENCHMARK_TIMING_STATE_COMPLETE: return "complete";
    case BENCHMARK_TIMING_STATE_EXHAUSTED: return "exhausted";
    case BENCHMARK_TIMING_STATE_FAILED: return "failed";
    case BENCHMARK_TIMING_STATE_UNAVAILABLE: return "unavailable";
    default: return "not-ready";
    }
}

static void benchmark_timing_writer_append(benchmark_timing_writer_t *writer, const char *bytes, size_t length)
{
    if (writer == NULL || !writer->ok || bytes == NULL || length > BENCHMARK_TIMING_MAX_JSON_BYTES - writer->bytes)
    {
        if (writer != NULL)
        {
            writer->ok = false;
        }
        return;
    }
    if (writer->output != NULL && !writer->output(bytes, length, writer->context))
    {
        writer->ok = false;
        return;
    }
    writer->bytes += length;
}

static void benchmark_timing_writer_literal(benchmark_timing_writer_t *writer, const char *literal)
{
    benchmark_timing_writer_append(writer, literal, strlen(literal));
}

static void benchmark_timing_writer_u64(benchmark_timing_writer_t *writer, uint64_t value)
{
    char digits[20];
    size_t length = 0u;

    do
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && length < sizeof(digits));
    while (length > 0u)
    {
        const char digit = digits[--length];
        benchmark_timing_writer_append(writer, &digit, 1u);
    }
}

static void benchmark_timing_writer_json_string(benchmark_timing_writer_t *writer, const char *text)
{
    benchmark_timing_writer_literal(writer, "\"");
    if (text != NULL)
    {
        for (size_t index = 0u; text[index] != '\0'; ++index)
        {
            const unsigned char character = (unsigned char)text[index];

            if (character == '"' || character == '\\')
            {
                const char escaped[2] = {'\\', (char)character};
                benchmark_timing_writer_append(writer, escaped, sizeof(escaped));
            }
            else if (character < 0x20u)
            {
                static const char hex[] = "0123456789abcdef";
                const char escaped[6] = {'\\', 'u', '0', '0', hex[character >> 4u], hex[character & 0x0fu]};
                benchmark_timing_writer_append(writer, escaped, sizeof(escaped));
            }
            else
            {
                benchmark_timing_writer_append(writer, (const char *)&text[index], 1u);
            }
        }
    }
    benchmark_timing_writer_literal(writer, "\"");
}

static bool benchmark_timing_map_channels(void)
{
    telemetry_channel_snapshot_t snapshot;
    size_t mapped = 0u;

    memset(g_timing.storage->channel_ids, 0, sizeof(g_timing.storage->channel_ids));
    memset(g_timing.storage->sequences, 0, sizeof(g_timing.storage->sequences));
    g_timing.expected_channel_mask = 0u;
    for (size_t slot = 0u; slot < PLATFORM_CHANNEL_LIMIT; ++slot)
    {
        if (!channel_manager_copy_snapshot_by_index(slot, &snapshot))
        {
            continue;
        }
        if (snapshot.channel.metadata.id == NULL || snapshot.channel.metadata.id[0] == '\0')
        {
            return false;
        }
        memcpy(g_timing.storage->channel_ids[slot], snapshot.channel.metadata.id,
               sizeof(g_timing.storage->channel_ids[slot]));
        g_timing.storage->channel_ids[slot][TELEMETRY_CHANNEL_ID_MAX_LEN - 1u] = '\0';
        g_timing.expected_channel_mask |= UINT32_C(1) << slot;
        ++mapped;
    }
    g_timing.mapped_channel_count = mapped;
    return mapped == RP2_BENCHMARK_CHANNEL_COUNT;
}

static bool benchmark_timing_capacity_is_sufficient(void)
{
    const uint64_t samples_per_channel =
        (uint64_t)RP2_BENCHMARK_RATE_HZ * RP2_BENCHMARK_TIMING_CAPTURE_SECONDS + 1u;
    const uint64_t required = samples_per_channel * RP2_BENCHMARK_CHANNEL_COUNT;

    return required <= BENCHMARK_TIMING_MAX_RECORDS;
}

static bool benchmark_timing_has_complete_population(void)
{
    size_t populated = 0u;

    if (g_timing.storage == NULL)
    {
        return false;
    }
    for (size_t slot = 0u; slot < PLATFORM_CHANNEL_LIMIT; ++slot)
    {
        if (g_timing.storage->channel_ids[slot][0] == '\0')
        {
            continue;
        }
        if (g_timing.storage->sequences[slot] == 0u)
        {
            return false;
        }
        ++populated;
    }
    return populated == g_timing.mapped_channel_count;
}

static bool benchmark_timing_start(void)
{
    const uint64_t now_us = benchmark_timing_now_us();

    if (g_timing.state != BENCHMARK_TIMING_STATE_READY || g_timing.storage == NULL ||
        !benchmark_timing_capacity_is_sufficient() || !benchmark_timing_map_channels() ||
        g_timing.capture_id == UINT64_MAX ||
        !benchmark_timing_add_seconds(now_us, RP2_BENCHMARK_TIMING_WARMUP_SECONDS,
                                      &g_timing.warmup_deadline_monotonic_us))
    {
        g_timing.state = BENCHMARK_TIMING_STATE_FAILED;
        return false;
    }
    g_timing.record_count = 0u;
    g_timing.capture_started_monotonic_us = 0u;
    g_timing.capture_ended_monotonic_us = 0u;
    g_timing.current_poll_open = false;
    g_timing.current_poll_seen_mask = 0u;
    g_timing.current_poll_deadline_crossed = false;
    g_timing.dump_pending = false;
    ++g_timing.capture_id;
    g_timing.state = BENCHMARK_TIMING_STATE_WARMUP;
    return true;
}

static void benchmark_timing_sample_published(size_t channel_slot, const telemetry_channel_t *channel)
{
    benchmark_timing_record_t *record;
    uint32_t sequence;
    const uint64_t now_us = benchmark_timing_now_us();

    (void)channel;
    if (g_timing.state != BENCHMARK_TIMING_STATE_CAPTURING || g_timing.storage == NULL ||
        channel_slot >= PLATFORM_CHANNEL_LIMIT || g_timing.storage->channel_ids[channel_slot][0] == '\0')
    {
        return;
    }
    if (benchmark_timing_reached(now_us, g_timing.capture_deadline_monotonic_us))
    {
        /* SensorTask completes the run after this whole poll, so a sibling
         * channel cannot be mistaken for missing merely because this slot
         * was visited first at the deadline. */
        g_timing.current_poll_deadline_crossed = true;
        return;
    }
    if (g_timing.record_count >= BENCHMARK_TIMING_MAX_RECORDS)
    {
        g_timing.state = BENCHMARK_TIMING_STATE_EXHAUSTED;
        return;
    }
    sequence = g_timing.storage->sequences[channel_slot];
    if (sequence == UINT32_MAX)
    {
        g_timing.state = BENCHMARK_TIMING_STATE_FAILED;
        return;
    }
    record = &g_timing.storage->records[g_timing.record_count++];
    record->sample_monotonic_us = now_us;
    record->sequence = ++sequence;
    record->channel_slot = (uint8_t)channel_slot;
    memset(record->reserved, 0, sizeof(record->reserved));
    g_timing.storage->sequences[channel_slot] = sequence;
    g_timing.current_poll_seen_mask |= UINT32_C(1) << channel_slot;
}

static bool benchmark_timing_write_json(benchmark_timing_writer_t *writer)
{
    benchmark_timing_writer_literal(writer, "{\"schemaVersion\":1,\"kind\":\"v052-native-sampling-evidence\",\"state\":");
    benchmark_timing_writer_json_string(writer, benchmark_timing_state_text(g_timing.state));
    benchmark_timing_writer_literal(writer, ",\"clock\":{\"domain\":\"device-monotonic\",\"unit\":\"microseconds\",\"valid\":true},\"bootStartedMonotonicUs\":");
    benchmark_timing_writer_u64(writer, g_timing.boot_started_monotonic_us);
    benchmark_timing_writer_literal(writer, ",\"captureStartedMonotonicUs\":");
    benchmark_timing_writer_u64(writer, g_timing.capture_started_monotonic_us);
    benchmark_timing_writer_literal(writer, ",\"captureEndedMonotonicUs\":");
    benchmark_timing_writer_u64(writer, g_timing.capture_ended_monotonic_us);
    benchmark_timing_writer_literal(writer, ",\"captureId\":");
    benchmark_timing_writer_u64(writer, g_timing.capture_id);
    benchmark_timing_writer_literal(writer, ",\"profileId\":");
    benchmark_timing_writer_json_string(writer, RP2_BENCHMARK_PROFILE_ID);
    benchmark_timing_writer_literal(writer, ",\"warmupSeconds\":");
    benchmark_timing_writer_u64(writer, RP2_BENCHMARK_TIMING_WARMUP_SECONDS);
    benchmark_timing_writer_literal(writer, ",\"durationSeconds\":");
    benchmark_timing_writer_u64(writer, RP2_BENCHMARK_TIMING_CAPTURE_SECONDS);
    benchmark_timing_writer_literal(writer, ",\"recordCapacity\":");
    benchmark_timing_writer_u64(writer, BENCHMARK_TIMING_MAX_RECORDS);
    benchmark_timing_writer_literal(writer, ",\"records\":[");
    for (size_t index = 0u; index < g_timing.record_count; ++index)
    {
        const benchmark_timing_record_t *record = &g_timing.storage->records[index];

        if (record->channel_slot >= PLATFORM_CHANNEL_LIMIT ||
            g_timing.storage->channel_ids[record->channel_slot][0] == '\0')
        {
            return false;
        }
        if (index != 0u)
        {
            benchmark_timing_writer_literal(writer, ",");
        }
        benchmark_timing_writer_literal(writer, "{\"channelId\":");
        benchmark_timing_writer_json_string(writer, g_timing.storage->channel_ids[record->channel_slot]);
        benchmark_timing_writer_literal(writer, ",\"sequence\":");
        benchmark_timing_writer_u64(writer, record->sequence);
        benchmark_timing_writer_literal(writer, ",\"sampleMonotonicUs\":");
        benchmark_timing_writer_u64(writer, record->sample_monotonic_us);
        benchmark_timing_writer_literal(writer, "}");
    }
    benchmark_timing_writer_literal(writer, "]}\n");
    return writer->ok;
}

static const char *benchmark_timing_diag_budget_state_text(
    benchmark_timing_diag_budget_state_t state)
{
    switch (state)
    {
    case BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING: return "capturing";
    case BENCHMARK_TIMING_DIAG_BUDGET_FINALIZING_STACK_HWM: return "capturing";
    case BENCHMARK_TIMING_DIAG_BUDGET_COMPLETE: return "complete";
    case BENCHMARK_TIMING_DIAG_BUDGET_FAILED: return "failed";
    }
    return "failed";
}

static void benchmark_timing_diag_budget_reset(void)
{
    taskENTER_CRITICAL();
    g_diag_budget.generation =
        g_diag_budget.generation == UINT64_MAX ? UINT64_C(1) : g_diag_budget.generation + 1u;
    g_diag_budget.state = (uint8_t)BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING;
    g_diag_budget.started_monotonic_us = 0u;
    g_diag_budget.ended_monotonic_us = 0u;
    g_diag_budget.p99_us = 0u;
    g_diag_budget.max_us = 0u;
    g_diag_budget.opcua_task_stack_high_water_bytes = 0u;
    memset(g_diag_budget.phase_max_us, 0, sizeof(g_diag_budget.phase_max_us));
    memset(g_diag_budget.max_sample_phase_us, 0, sizeof(g_diag_budget.max_sample_phase_us));
    memset(g_diag_budget.current_sample_phase_us, 0,
           sizeof(g_diag_budget.current_sample_phase_us));
    g_diag_budget.sample_count = 0u;
    g_diag_budget.output_attempted = false;
    taskEXIT_CRITICAL();
}

static void benchmark_timing_diag_budget_calculate_quantiles(void)
{
    /* sort_scratch_us is static synthetic-only storage so this work does not
     * consume or alter the measured OpcUaTask stack high-water mark. */
    for (size_t index = 1u; index < BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT; ++index)
    {
        const uint32_t value = g_diag_budget.sort_scratch_us[index];
        size_t insert = index;
        while (insert > 0u && g_diag_budget.sort_scratch_us[insert - 1u] > value)
        {
            g_diag_budget.sort_scratch_us[insert] =
                g_diag_budget.sort_scratch_us[insert - 1u];
            --insert;
        }
        g_diag_budget.sort_scratch_us[insert] = value;
    }
}

static bool benchmark_timing_write_diag_budget_json(
    benchmark_timing_writer_t *writer,
    const benchmark_timing_diag_budget_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return false;
    }
    benchmark_timing_writer_literal(writer,
                                    "{\"schemaVersion\":1,\"kind\":\"v052-diag-snapshot-budget-evidence\",\"state\":");
    benchmark_timing_writer_json_string(writer,
                                        benchmark_timing_diag_budget_state_text(snapshot->state));
    benchmark_timing_writer_literal(writer, ",\"profileId\":");
    benchmark_timing_writer_json_string(writer, RP2_BENCHMARK_PROFILE_ID);
    benchmark_timing_writer_literal(writer,
                                    ",\"clock\":{\"domain\":\"device-monotonic\",\"unit\":\"microseconds\",\"valid\":true},\"sampleTarget\":");
    benchmark_timing_writer_u64(writer, BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT);
    benchmark_timing_writer_literal(writer, ",\"captureLimitSeconds\":");
    benchmark_timing_writer_u64(writer, BENCHMARK_TIMING_DIAG_BUDGET_MAX_CAPTURE_SECONDS);
    benchmark_timing_writer_literal(writer, ",\"sampleCount\":");
    benchmark_timing_writer_u64(writer, snapshot->sample_count);
    benchmark_timing_writer_literal(writer, ",\"captureStartedMonotonicUs\":");
    benchmark_timing_writer_u64(writer, snapshot->started_monotonic_us);
    benchmark_timing_writer_literal(writer, ",\"captureEndedMonotonicUs\":");
    benchmark_timing_writer_u64(writer, snapshot->ended_monotonic_us);
    benchmark_timing_writer_literal(writer, ",\"captureDurationUs\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->ended_monotonic_us >= snapshot->started_monotonic_us
                                    ? snapshot->ended_monotonic_us - snapshot->started_monotonic_us
                                    : 0u);
    benchmark_timing_writer_literal(writer, ",\"diagServiceSampleDurationUs\":{\"p99\":");
    benchmark_timing_writer_u64(writer, snapshot->p99_us);
    benchmark_timing_writer_literal(writer, ",\"max\":");
    benchmark_timing_writer_u64(writer, snapshot->max_us);
    benchmark_timing_writer_literal(writer, "},\"opcUaTaskStackHighWaterBytes\":");
    benchmark_timing_writer_u64(writer, snapshot->opcua_task_stack_high_water_bytes);
    benchmark_timing_writer_literal(writer, ",\"phaseMaxUs\":{\"acquire\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_ACQUIRE]);
    benchmark_timing_writer_literal(writer, ",\"copy\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_COPY]);
    benchmark_timing_writer_literal(writer, ",\"heap\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_HEAP]);
    benchmark_timing_writer_literal(writer, ",\"psram\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_PSRAM]);
    benchmark_timing_writer_literal(writer, ",\"lwip\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_LWIP]);
    benchmark_timing_writer_literal(writer, ",\"tasks\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_TASKS]);
    benchmark_timing_writer_literal(writer, ",\"publish\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->phase_max_us[BENCHMARK_TIMING_DIAG_PHASE_PUBLISH]);
    benchmark_timing_writer_literal(writer, "},\"maxSamplePhaseUs\":{\"acquire\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_ACQUIRE]);
    benchmark_timing_writer_literal(writer, ",\"copy\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_COPY]);
    benchmark_timing_writer_literal(writer, ",\"heap\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_HEAP]);
    benchmark_timing_writer_literal(writer, ",\"psram\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_PSRAM]);
    benchmark_timing_writer_literal(writer, ",\"lwip\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_LWIP]);
    benchmark_timing_writer_literal(writer, ",\"tasks\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_TASKS]);
    benchmark_timing_writer_literal(writer, ",\"publish\":");
    benchmark_timing_writer_u64(writer,
                                snapshot->max_sample_phase_us[BENCHMARK_TIMING_DIAG_PHASE_PUBLISH]);
    benchmark_timing_writer_literal(writer, "},\"baselineComparisonState\":\"not-run\"}\n");
    return writer->ok && writer->bytes <= BENCHMARK_TIMING_DIAG_BUDGET_MAX_JSON_BYTES;
}

static bool benchmark_timing_dump_diag_budget_if_ready(void)
{
    benchmark_timing_writer_t writer = {0};
    benchmark_timing_diag_budget_snapshot_t snapshot;

    if (g_timing.state == BENCHMARK_TIMING_STATE_WARMUP ||
        g_timing.state == BENCHMARK_TIMING_STATE_CAPTURING ||
        g_timing.output == NULL)
    {
        return false;
    }
    taskENTER_CRITICAL();
    if ((benchmark_timing_diag_budget_state_t)g_diag_budget.state ==
            BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING ||
        (benchmark_timing_diag_budget_state_t)g_diag_budget.state ==
            BENCHMARK_TIMING_DIAG_BUDGET_FINALIZING_STACK_HWM ||
        g_diag_budget.output_attempted)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    snapshot.state = (benchmark_timing_diag_budget_state_t)g_diag_budget.state;
    snapshot.started_monotonic_us = g_diag_budget.started_monotonic_us;
    snapshot.ended_monotonic_us = g_diag_budget.ended_monotonic_us;
    snapshot.p99_us = g_diag_budget.p99_us;
    snapshot.max_us = g_diag_budget.max_us;
    snapshot.opcua_task_stack_high_water_bytes =
        g_diag_budget.opcua_task_stack_high_water_bytes;
    memcpy(snapshot.phase_max_us, g_diag_budget.phase_max_us,
           sizeof(snapshot.phase_max_us));
    memcpy(snapshot.max_sample_phase_us, g_diag_budget.max_sample_phase_us,
           sizeof(snapshot.max_sample_phase_us));
    snapshot.sample_count = g_diag_budget.sample_count;
    /* Mark before output: a failed serial writer never causes automatic
     * retries from SensorTask. The complete snapshot remains locally valid. */
    g_diag_budget.output_attempted = true;
    taskEXIT_CRITICAL();
    /* First count the bounded document so no partial serial JSON is emitted. */
    writer.ok = true;
    if (!benchmark_timing_write_diag_budget_json(&writer, &snapshot))
    {
        return false;
    }
    writer = (benchmark_timing_writer_t){
        .output = g_timing.output,
        .context = g_timing.output_context,
        .ok = true,
    };
    return benchmark_timing_write_diag_budget_json(&writer, &snapshot);
}

void benchmark_timing_note_boot_started(void)
{
    g_timing.boot_started_monotonic_us = benchmark_timing_now_us();
}

bool benchmark_timing_init(void)
{
    if (!benchmark_profile_is_enabled())
    {
        return true;
    }
    benchmark_timing_diag_budget_reset();
    if (g_timing.storage == NULL)
    {
        g_timing.storage = psram_calloc(1u, sizeof(*g_timing.storage));
    }
    if (g_timing.storage == NULL)
    {
        g_timing.state = BENCHMARK_TIMING_STATE_UNAVAILABLE;
        return false;
    }
    g_timing.state = BENCHMARK_TIMING_STATE_READY;
    return true;
}

void benchmark_timing_set_control(benchmark_timing_control_poll_fn poll, void *context)
{
    g_timing.control_poll = poll;
    g_timing.control_context = context;
}

void benchmark_timing_set_output(benchmark_timing_output_fn output, void *context)
{
    g_timing.output = output;
    g_timing.output_context = context;
}

void benchmark_timing_sensor_task_process_control(void)
{
    benchmark_timing_control_t control = BENCHMARK_TIMING_CONTROL_NONE;

    if ((g_timing.state != BENCHMARK_TIMING_STATE_READY &&
         g_timing.state != BENCHMARK_TIMING_STATE_COMPLETE &&
         g_timing.state != BENCHMARK_TIMING_STATE_EXHAUSTED &&
         g_timing.state != BENCHMARK_TIMING_STATE_FAILED &&
         g_timing.state != BENCHMARK_TIMING_STATE_UNAVAILABLE) ||
        g_timing.control_poll == NULL)
    {
        return;
    }
    control = g_timing.control_poll(g_timing.control_context);
    if (control == BENCHMARK_TIMING_CONTROL_START && g_timing.state == BENCHMARK_TIMING_STATE_READY)
    {
        (void)benchmark_timing_start();
    }
    else if (control == BENCHMARK_TIMING_CONTROL_DUMP &&
             (g_timing.state == BENCHMARK_TIMING_STATE_COMPLETE ||
              g_timing.state == BENCHMARK_TIMING_STATE_EXHAUSTED ||
              g_timing.state == BENCHMARK_TIMING_STATE_FAILED ||
              g_timing.state == BENCHMARK_TIMING_STATE_UNAVAILABLE))
    {
        g_timing.dump_pending = true;
    }
}

channel_manager_sample_observer_t benchmark_timing_sensor_poll_begin(void)
{
    const uint64_t now_us = benchmark_timing_now_us();

    if (g_timing.state == BENCHMARK_TIMING_STATE_WARMUP &&
        benchmark_timing_reached(now_us, g_timing.warmup_deadline_monotonic_us))
    {
        g_timing.capture_started_monotonic_us = now_us;
        if (!benchmark_timing_add_seconds(now_us, RP2_BENCHMARK_TIMING_CAPTURE_SECONDS,
                                          &g_timing.capture_deadline_monotonic_us))
        {
            g_timing.state = BENCHMARK_TIMING_STATE_FAILED;
            return NULL;
        }
        g_timing.state = BENCHMARK_TIMING_STATE_CAPTURING;
    }
    if (g_timing.state == BENCHMARK_TIMING_STATE_CAPTURING &&
        benchmark_timing_reached(now_us, g_timing.capture_deadline_monotonic_us))
    {
        return NULL;
    }
    if (g_timing.state == BENCHMARK_TIMING_STATE_CAPTURING)
    {
        g_timing.current_poll_seen_mask = 0u;
        g_timing.current_poll_deadline_crossed = false;
        g_timing.current_poll_open = true;
        return benchmark_timing_sample_published;
    }
    return NULL;
}

void benchmark_timing_sensor_poll_end(void)
{
    const uint64_t now_us = benchmark_timing_now_us();

    if (g_timing.state != BENCHMARK_TIMING_STATE_CAPTURING)
    {
        return;
    }
    if (g_timing.current_poll_open && !g_timing.current_poll_deadline_crossed &&
        !benchmark_timing_reached(now_us, g_timing.capture_deadline_monotonic_us) &&
        g_timing.current_poll_seen_mask != g_timing.expected_channel_mask)
    {
        g_timing.current_poll_open = false;
        g_timing.capture_ended_monotonic_us = now_us;
        g_timing.state = BENCHMARK_TIMING_STATE_FAILED;
        return;
    }
    g_timing.current_poll_open = false;
    if (benchmark_timing_reached(now_us, g_timing.capture_deadline_monotonic_us) ||
        g_timing.current_poll_deadline_crossed)
    {
        g_timing.capture_ended_monotonic_us = now_us;
        g_timing.state = benchmark_timing_has_complete_population()
                             ? BENCHMARK_TIMING_STATE_COMPLETE
                             : BENCHMARK_TIMING_STATE_FAILED;
    }
}

bool benchmark_timing_dump_if_ready(void)
{
    benchmark_timing_writer_t writer = {0};

    if (benchmark_timing_dump_diag_budget_if_ready())
    {
        return true;
    }
    if (!g_timing.dump_pending || g_timing.output == NULL)
    {
        return false;
    }
    writer.ok = true;
    if (!benchmark_timing_write_json(&writer))
    {
        /* Do not transmit a partial, apparently valid evidence document. */
        g_timing.state = BENCHMARK_TIMING_STATE_FAILED;
        g_timing.record_count = 0u;
        writer = (benchmark_timing_writer_t){.ok = true};
        if (!benchmark_timing_write_json(&writer))
        {
            return false;
        }
    }
    /* The count-only preflight has consumed its own byte budget.  Start the
     * actual bounded serial document from byte zero. */
    writer = (benchmark_timing_writer_t){
        .output = g_timing.output,
        .context = g_timing.output_context,
        .ok = true,
    };
    if (!benchmark_timing_write_json(&writer))
    {
        /* Preserve records but require a new explicit dump command.  A
         * partial USB write must never turn into unbounded automatic retries
         * from SensorTask. */
        g_timing.dump_pending = false;
        return false;
    }
    g_timing.dump_pending = false;
    if (g_timing.state == BENCHMARK_TIMING_STATE_COMPLETE ||
        g_timing.state == BENCHMARK_TIMING_STATE_EXHAUSTED ||
        g_timing.state == BENCHMARK_TIMING_STATE_FAILED)
    {
        g_timing.state = BENCHMARK_TIMING_STATE_READY;
    }
    return true;
}

uint64_t benchmark_timing_diag_sample_begin(void)
{
    return benchmark_timing_now_us();
}

uint64_t benchmark_timing_diag_phase_now_us(void)
{
    return benchmark_timing_now_us();
}

void benchmark_timing_diag_sample_set_phase_durations(
    const uint64_t durations_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT])
{
    bool out_of_range = false;
    uint64_t failed_at_monotonic_us = 0u;

    if (durations_us == NULL)
    {
        return;
    }
    for (size_t phase = 0u; phase < BENCHMARK_TIMING_DIAG_PHASE_COUNT; ++phase)
    {
        if (durations_us[phase] > UINT32_MAX)
        {
            out_of_range = true;
            break;
        }
    }
    if (out_of_range)
    {
        failed_at_monotonic_us = benchmark_timing_now_us();
    }
    taskENTER_CRITICAL();
    if ((benchmark_timing_diag_budget_state_t)g_diag_budget.state ==
        BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING)
    {
        if (out_of_range)
        {
            g_diag_budget.ended_monotonic_us = failed_at_monotonic_us;
            g_diag_budget.state = (uint8_t)BENCHMARK_TIMING_DIAG_BUDGET_FAILED;
        }
        else
        {
            for (size_t phase = 0u; phase < BENCHMARK_TIMING_DIAG_PHASE_COUNT; ++phase)
            {
                g_diag_budget.current_sample_phase_us[phase] =
                    (uint32_t)durations_us[phase];
            }
        }
    }
    taskEXIT_CRITICAL();
}

void benchmark_timing_diag_sample_end(uint64_t started_monotonic_us,
                                      bool snapshot_published)
{
    const uint64_t ended_monotonic_us = benchmark_timing_now_us();
    const uint64_t duration_us = ended_monotonic_us >= started_monotonic_us
                                     ? ended_monotonic_us - started_monotonic_us
                                     : 0u;
    uint64_t completed_generation = 0u;
    bool calculate_quantiles = false;

    taskENTER_CRITICAL();
    if ((benchmark_timing_diag_budget_state_t)g_diag_budget.state ==
        BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING)
    {
        if (!snapshot_published || ended_monotonic_us < started_monotonic_us ||
            duration_us > UINT32_MAX ||
            g_diag_budget.sample_count >= BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT)
        {
            g_diag_budget.ended_monotonic_us = ended_monotonic_us;
            g_diag_budget.state = (uint8_t)BENCHMARK_TIMING_DIAG_BUDGET_FAILED;
        }
        else if (g_diag_budget.sample_count == 0u)
        {
            g_diag_budget.started_monotonic_us = started_monotonic_us;
        }
        else if (ended_monotonic_us - g_diag_budget.started_monotonic_us >
                 (uint64_t)BENCHMARK_TIMING_DIAG_BUDGET_MAX_CAPTURE_SECONDS *
                     UINT64_C(1000000))
        {
            g_diag_budget.ended_monotonic_us = ended_monotonic_us;
            g_diag_budget.state = (uint8_t)BENCHMARK_TIMING_DIAG_BUDGET_FAILED;
        }
        if ((benchmark_timing_diag_budget_state_t)g_diag_budget.state ==
            BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING)
        {
            g_diag_budget.durations_us[g_diag_budget.sample_count++] =
                (uint32_t)duration_us;
            for (size_t phase = 0u; phase < BENCHMARK_TIMING_DIAG_PHASE_COUNT; ++phase)
            {
                if (g_diag_budget.current_sample_phase_us[phase] >
                    g_diag_budget.phase_max_us[phase])
                {
                    g_diag_budget.phase_max_us[phase] =
                        g_diag_budget.current_sample_phase_us[phase];
                }
            }
            if (duration_us > g_diag_budget.max_us)
            {
                g_diag_budget.max_us = duration_us;
                memcpy(g_diag_budget.max_sample_phase_us,
                       g_diag_budget.current_sample_phase_us,
                       sizeof(g_diag_budget.max_sample_phase_us));
            }
            if (g_diag_budget.sample_count == BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT)
            {
                /* The fixed 400-byte copy is protected so reset cannot tear
                 * it. Sorting occurs after the critical section. */
                memcpy(g_diag_budget.sort_scratch_us, g_diag_budget.durations_us,
                       sizeof(g_diag_budget.sort_scratch_us));
                completed_generation = g_diag_budget.generation;
                calculate_quantiles = true;
            }
        }
    }
    taskEXIT_CRITICAL();

    if (!calculate_quantiles)
    {
        return;
    }
    benchmark_timing_diag_budget_calculate_quantiles();
    taskENTER_CRITICAL();
    if (g_diag_budget.generation == completed_generation &&
        (benchmark_timing_diag_budget_state_t)g_diag_budget.state ==
            BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING &&
        g_diag_budget.sample_count == BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT)
    {
        g_diag_budget.p99_us = g_diag_budget.sort_scratch_us[98u];
        g_diag_budget.max_us =
            g_diag_budget.sort_scratch_us[BENCHMARK_TIMING_DIAG_BUDGET_SAMPLE_COUNT - 1u];
        g_diag_budget.ended_monotonic_us = ended_monotonic_us;
        /* `diag_service_sample()` records OpcUaTask's HWM after ending the
         * measured snapshot interval. Keep the finished duration set private
         * until that final evidence value is atomically stored. */
        g_diag_budget.state =
            (uint8_t)BENCHMARK_TIMING_DIAG_BUDGET_FINALIZING_STACK_HWM;
    }
    taskEXIT_CRITICAL();
}

void benchmark_timing_diag_sample_set_opcua_task_stack_high_water(
    uint64_t opcua_task_stack_high_water_bytes)
{
    taskENTER_CRITICAL();
    const benchmark_timing_diag_budget_state_t state =
        (benchmark_timing_diag_budget_state_t)g_diag_budget.state;
    if (state == BENCHMARK_TIMING_DIAG_BUDGET_CAPTURING ||
        state == BENCHMARK_TIMING_DIAG_BUDGET_FINALIZING_STACK_HWM)
    {
        g_diag_budget.opcua_task_stack_high_water_bytes =
            opcua_task_stack_high_water_bytes;
        if (state == BENCHMARK_TIMING_DIAG_BUDGET_FINALIZING_STACK_HWM)
        {
            g_diag_budget.state = (uint8_t)BENCHMARK_TIMING_DIAG_BUDGET_COMPLETE;
        }
    }
    taskEXIT_CRITICAL();
}

#else

void benchmark_timing_note_boot_started(void) {}
bool benchmark_timing_init(void) { return true; }
void benchmark_timing_set_control(benchmark_timing_control_poll_fn poll, void *context)
{
    (void)poll;
    (void)context;
}
void benchmark_timing_set_output(benchmark_timing_output_fn output, void *context)
{
    (void)output;
    (void)context;
}
void benchmark_timing_sensor_task_process_control(void) {}
channel_manager_sample_observer_t benchmark_timing_sensor_poll_begin(void) { return NULL; }
void benchmark_timing_sensor_poll_end(void) {}
bool benchmark_timing_dump_if_ready(void) { return false; }
uint64_t benchmark_timing_diag_sample_begin(void) { return 0u; }
uint64_t benchmark_timing_diag_phase_now_us(void) { return 0u; }
void benchmark_timing_diag_sample_set_phase_durations(
    const uint64_t durations_us[BENCHMARK_TIMING_DIAG_PHASE_COUNT])
{
    (void)durations_us;
}
void benchmark_timing_diag_sample_end(uint64_t started_monotonic_us,
                                      bool snapshot_published)
{
    (void)started_monotonic_us;
    (void)snapshot_published;
}
void benchmark_timing_diag_sample_set_opcua_task_stack_high_water(
    uint64_t opcua_task_stack_high_water_bytes)
{
    (void)opcua_task_stack_high_water_bytes;
}

#endif

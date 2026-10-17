#include "services/resource_exhaustion_test.h"

#if RP2_RESOURCE_EXHAUSTION_TEST_ENABLED

#include <limits.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "psram_allocator.h"
#include "services/diag_service.h"

#define RESOURCE_EXHAUSTION_TEST_OOM_REASON 1u

/* The one retained-pointer table is intentionally the only permanent runner
 * allocation.  At most one allocator is pressured at a time. */
static void *g_retained[RESOURCE_EXHAUSTION_TEST_POINTER_CAPACITY];

static uint64_t resource_exhaustion_now_ms(void *context)
{
    (void)context;
    return ((uint64_t)xTaskGetTickCount() * 1000u) / configTICK_RATE_HZ;
}

static bool resource_exhaustion_heap_stats(resource_exhaustion_test_stats_t *out_stats,
                                           void *context)
{
    diag_service_snapshot_t snapshot;
    (void)context;
    if (out_stats == NULL ||
        !diag_service_sample(resource_exhaustion_now_ms(NULL), false, 0u) ||
        !diag_service_get_snapshot(&snapshot) ||
        snapshot.heap.quality != DIAG_SERVICE_QUALITY_GOOD)
    {
        return false;
    }
    out_stats->available = true;
    out_stats->ready = true;
    out_stats->free_bytes = snapshot.heap.free_bytes;
    out_stats->used_bytes = snapshot.heap.used_bytes;
    out_stats->peak_used_bytes = snapshot.heap.peak_used_bytes;
    /* FreeRTOS heap_4 exposes no live caller-requested-byte gauge.  Do not
     * relabel allocator-consumed UsedBytes as requested bytes. */
    out_stats->requested_bytes_available = false;
    out_stats->requested_bytes = 0u;
    out_stats->minimum_ever_free_bytes_available = true;
    out_stats->minimum_ever_free_bytes = snapshot.heap.minimum_ever_free_bytes;
    out_stats->failed_allocations.value = snapshot.heap.failed_allocations.value;
    out_stats->failed_allocations.wrap_count = snapshot.heap.failed_allocations.wrap_count;
    out_stats->last_failure_reason = (uint32_t)snapshot.heap.last_failure_reason;
    return true;
}

static bool resource_exhaustion_psram_stats(resource_exhaustion_test_stats_t *out_stats,
                                            void *context)
{
    psram_allocator_stats_t stats;
    (void)context;
    if (out_stats == NULL || !psram_allocator_get_stats(&stats))
    {
        return false;
    }
    out_stats->available = true;
    out_stats->ready = stats.compiled && stats.ready;
    out_stats->requested_bytes_available = true;
    out_stats->free_bytes = stats.free_bytes;
    out_stats->used_bytes = stats.used_bytes;
    out_stats->peak_used_bytes = stats.peak_used_bytes;
    out_stats->requested_bytes = stats.requested_bytes_in_use;
    /* The PSRAM allocator deliberately has no lifetime minimum-free gauge.
     * Its terminal free bytes are compared separately after each test OOM. */
    out_stats->minimum_ever_free_bytes_available = false;
    out_stats->minimum_ever_free_bytes = 0u;
    out_stats->failed_allocations.value = stats.failed_allocations;
    out_stats->failed_allocations.wrap_count = stats.failed_allocations_wrap_count;
    out_stats->last_failure_reason = (uint32_t)stats.last_failure_reason;
    return true;
}

static void *resource_exhaustion_heap_allocate(size_t size, void *context)
{
    (void)context;
    return pvPortMalloc(size);
}

static void resource_exhaustion_heap_free(void *pointer, void *context)
{
    (void)context;
    vPortFree(pointer);
}

static void *resource_exhaustion_psram_allocate(size_t size, void *context)
{
    (void)context;
    return psram_malloc(size);
}

static void resource_exhaustion_psram_free(void *pointer, void *context)
{
    (void)context;
    psram_free(pointer);
}

static bool resource_exhaustion_counter_is_exact_increment(
    const resource_exhaustion_test_counter_t before,
    const resource_exhaustion_test_counter_t after,
    bool *out_wrap_ambiguous)
{
    if (out_wrap_ambiguous != NULL)
    {
        *out_wrap_ambiguous = false;
    }
    if (after.wrap_count == before.wrap_count)
    {
        return before.value != UINT64_MAX && after.value == before.value + 1u;
    }
    if (before.wrap_count != UINT64_MAX &&
        after.wrap_count == before.wrap_count + 1u &&
        before.value == UINT64_MAX && after.value == 0u)
    {
        return true;
    }
    if (out_wrap_ambiguous != NULL)
    {
        *out_wrap_ambiguous = true;
    }
    return false;
}

static bool resource_exhaustion_stats_returned(const resource_exhaustion_test_stats_t *before,
                                                const resource_exhaustion_test_stats_t *after)
{
    return before->free_bytes == after->free_bytes &&
           before->used_bytes == after->used_bytes &&
           before->requested_bytes_available == after->requested_bytes_available &&
           (!before->requested_bytes_available ||
            before->requested_bytes == after->requested_bytes);
}

static uint8_t resource_exhaustion_pattern(uint32_t cycle, uint32_t block, size_t offset)
{
    return (uint8_t)((cycle * 29u) ^ (block * 71u) ^ ((uint32_t)offset * 17u) ^ 0xa5u);
}

static void resource_exhaustion_fill(void *pointer,
                                     size_t size,
                                     uint32_t cycle,
                                     uint32_t block)
{
    uint8_t *bytes = pointer;
    size_t offset;
    for (offset = 0u; offset < size; ++offset)
    {
        bytes[offset] = resource_exhaustion_pattern(cycle, block, offset);
    }
}

static bool resource_exhaustion_verify(const void *pointer,
                                       size_t size,
                                       uint32_t cycle,
                                       uint32_t block)
{
    const uint8_t *bytes = pointer;
    size_t offset;
    for (offset = 0u; offset < size; ++offset)
    {
        if (bytes[offset] != resource_exhaustion_pattern(cycle, block, offset))
        {
            return false;
        }
    }
    return true;
}

static void resource_exhaustion_release_all(const resource_exhaustion_test_allocator_ops_t *ops,
                                            uint32_t count)
{
    while (count > 0u)
    {
        --count;
        ops->release(g_retained[count], ops->context);
        g_retained[count] = NULL;
    }
}

static void resource_exhaustion_set_failure(resource_exhaustion_test_allocator_result_t *result,
                                            resource_exhaustion_test_status_t status,
                                            uint32_t cycle)
{
    result->status = status;
    if (result->first_failure_cycle == 0u)
    {
        result->first_failure_cycle = cycle + 1u;
    }
}

static void resource_exhaustion_run_allocator(const resource_exhaustion_test_allocator_ops_t *ops,
                                              bool requires_ready,
                                              size_t request_bytes,
                                              resource_exhaustion_test_allocator_result_t *result)
{
    uint32_t cycle;
    uint64_t established_minimum = 0u;
    uint64_t established_terminal_free = 0u;
    bool have_established_minimum = false;
    bool have_established_terminal_free = false;

    memset(result, 0, sizeof(*result));
    result->status = RESOURCE_EXHAUSTION_TEST_STATUS_NOT_RUN;
    result->requested_cycles = RESOURCE_EXHAUSTION_TEST_CYCLES;
    result->baseline_returned = true;
    result->counter_exact = true;
    result->oom_reason_exact = true;
    result->payload_verified = true;
    result->minimum_ever_stable = true;

    if (ops == NULL || ops->allocate == NULL || ops->release == NULL || ops->get_stats == NULL ||
        !ops->get_stats(&result->initial_stats, ops->context) ||
        !result->initial_stats.available)
    {
        resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE, 0u);
        return;
    }
    if (requires_ready && !result->initial_stats.ready)
    {
        resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_PSRAM_UNAVAILABLE, 0u);
        return;
    }
    if (!requires_ready && !result->initial_stats.minimum_ever_free_bytes_available)
    {
        resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE, 0u);
        return;
    }

    for (cycle = 0u; cycle < RESOURCE_EXHAUSTION_TEST_CYCLES; ++cycle)
    {
        resource_exhaustion_test_stats_t before;
        resource_exhaustion_test_stats_t after_oom;
        resource_exhaustion_test_stats_t after_free;
        uint32_t block_count = 0u;
        bool saw_oom = false;
        bool local_failure = false;

        if (!ops->get_stats(&before, ops->context) || !before.available)
        {
            resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE, cycle);
            break;
        }
        while (block_count < RESOURCE_EXHAUSTION_TEST_POINTER_CAPACITY)
        {
            void *pointer = ops->allocate(request_bytes, ops->context);
            if (pointer == NULL)
            {
                saw_oom = true;
                break;
            }
            g_retained[block_count] = pointer;
            ++block_count;
            resource_exhaustion_fill(pointer, request_bytes, cycle, block_count);
            if (ops->after_fill != NULL)
            {
                ops->after_fill(pointer, request_bytes, cycle, block_count, ops->context);
            }
            if (!resource_exhaustion_verify(pointer, request_bytes, cycle, block_count))
            {
                result->payload_verified = false;
                resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_CORRUPTION, cycle);
                local_failure = true;
                break;
            }
        }
        if (!saw_oom && !local_failure)
        {
            resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_POINTER_CAPACITY, cycle);
            local_failure = true;
        }
        if (saw_oom)
        {
            bool wrap_ambiguous = false;
            if (!ops->get_stats(&after_oom, ops->context) || !after_oom.available)
            {
                resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE, cycle);
                local_failure = true;
            }
            else if (!resource_exhaustion_counter_is_exact_increment(before.failed_allocations,
                                                                       after_oom.failed_allocations,
                                                                       &wrap_ambiguous))
            {
                result->counter_exact = false;
                resource_exhaustion_set_failure(result,
                                                wrap_ambiguous
                                                    ? RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_WRAP_AMBIGUOUS
                                                    : RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_MISMATCH,
                                                cycle);
                local_failure = true;
            }
            else if (after_oom.last_failure_reason != RESOURCE_EXHAUSTION_TEST_OOM_REASON)
            {
                result->oom_reason_exact = false;
                resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_OOM_REASON, cycle);
                local_failure = true;
            }
            else if (requires_ready)
            {
                if (have_established_terminal_free &&
                    after_oom.free_bytes < established_terminal_free)
                {
                    result->minimum_ever_stable = false;
                    resource_exhaustion_set_failure(result,
                                                    RESOURCE_EXHAUSTION_TEST_STATUS_MINIMUM_EVER_UNSTABLE,
                                                    cycle);
                    local_failure = true;
                }
                established_terminal_free = after_oom.free_bytes;
                have_established_terminal_free = true;
            }
        }

        /* A successful allocation can corrupt an older retained block.  A
         * second complete readback after the terminal NULL catches overlap or
         * delayed corruption before any pointer is returned to the allocator. */
        if (saw_oom && !local_failure)
        {
            uint32_t verify_index;
            for (verify_index = 0u; verify_index < block_count; ++verify_index)
            {
                if (!resource_exhaustion_verify(g_retained[verify_index], request_bytes,
                                                cycle, verify_index + 1u))
                {
                    result->payload_verified = false;
                    resource_exhaustion_set_failure(result,
                                                    RESOURCE_EXHAUSTION_TEST_STATUS_CORRUPTION,
                                                    cycle);
                    local_failure = true;
                    break;
                }
            }
        }

        resource_exhaustion_release_all(ops, block_count);
        if (!ops->get_stats(&after_free, ops->context) || !after_free.available)
        {
            resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE, cycle);
            break;
        }
        result->final_stats = after_free;
        if (!resource_exhaustion_stats_returned(&before, &after_free))
        {
            result->baseline_returned = false;
            resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_BASELINE_MISMATCH, cycle);
            local_failure = true;
        }
        if (!requires_ready)
        {
            if (!after_free.minimum_ever_free_bytes_available)
            {
                resource_exhaustion_set_failure(result,
                                                RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE,
                                                cycle);
                local_failure = true;
            }
            else if (have_established_minimum &&
                after_free.minimum_ever_free_bytes < established_minimum)
            {
                result->minimum_ever_stable = false;
                resource_exhaustion_set_failure(result,
                                                RESOURCE_EXHAUSTION_TEST_STATUS_MINIMUM_EVER_UNSTABLE,
                                                cycle);
                local_failure = true;
            }
            established_minimum = after_free.minimum_ever_free_bytes;
            have_established_minimum = true;
        }
        if (block_count < result->min_block_count || result->completed_cycles == 0u)
        {
            result->min_block_count = block_count;
        }
        if (block_count > result->max_block_count)
        {
            result->max_block_count = block_count;
        }
        if (!local_failure && saw_oom)
        {
            ++result->completed_cycles;
        }
        else
        {
            break;
        }
    }
    if (result->final_stats.available &&
        !resource_exhaustion_stats_returned(&result->initial_stats, &result->final_stats))
    {
        result->baseline_returned = false;
        resource_exhaustion_set_failure(result, RESOURCE_EXHAUSTION_TEST_STATUS_BASELINE_MISMATCH,
                                        RESOURCE_EXHAUSTION_TEST_CYCLES - 1u);
    }
    if (result->completed_cycles == RESOURCE_EXHAUSTION_TEST_CYCLES &&
        result->status == RESOURCE_EXHAUSTION_TEST_STATUS_NOT_RUN)
    {
        result->status = RESOURCE_EXHAUSTION_TEST_STATUS_PASS;
    }
}

bool resource_exhaustion_test_run_with_ops(
    const resource_exhaustion_test_allocator_ops_t *heap_ops,
    const resource_exhaustion_test_allocator_ops_t *psram_ops,
    const char *profile_id,
    resource_exhaustion_test_result_t *out_result)
{
    if (heap_ops == NULL || psram_ops == NULL || out_result == NULL || profile_id == NULL ||
        heap_ops->now_monotonic_ms == NULL)
    {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->profile_id = profile_id;
    out_result->start_monotonic_ms = heap_ops->now_monotonic_ms(heap_ops->context);
    resource_exhaustion_run_allocator(heap_ops, false, RESOURCE_EXHAUSTION_TEST_HEAP_REQUEST_BYTES,
                                      &out_result->heap);
    resource_exhaustion_run_allocator(psram_ops, true, RESOURCE_EXHAUSTION_TEST_PSRAM_REQUEST_BYTES,
                                      &out_result->psram);
    out_result->end_monotonic_ms = heap_ops->now_monotonic_ms(heap_ops->context);
    out_result->all_passed = out_result->heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_PASS &&
                             out_result->psram.status == RESOURCE_EXHAUSTION_TEST_STATUS_PASS;
    return true;
}

bool resource_exhaustion_test_run(resource_exhaustion_test_result_t *out_result)
{
    uint64_t stack_high_water_start;
    bool ran;
    const resource_exhaustion_test_allocator_ops_t heap_ops = {
        .allocate = resource_exhaustion_heap_allocate,
        .release = resource_exhaustion_heap_free,
        .get_stats = resource_exhaustion_heap_stats,
        .now_monotonic_ms = resource_exhaustion_now_ms,
        .after_fill = NULL,
        .context = NULL};
    const resource_exhaustion_test_allocator_ops_t psram_ops = {
        .allocate = resource_exhaustion_psram_allocate,
        .release = resource_exhaustion_psram_free,
        .get_stats = resource_exhaustion_psram_stats,
        .now_monotonic_ms = resource_exhaustion_now_ms,
        .after_fill = NULL,
        .context = NULL};
    if (out_result == NULL)
    {
        return false;
    }
    stack_high_water_start =
        (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    ran = resource_exhaustion_test_run_with_ops(&heap_ops, &psram_ops,
                                                RP2_RESOURCE_EXHAUSTION_PROFILE_ID, out_result);
    if (ran)
    {
        out_result->init_task_stack_high_water_start_bytes = stack_high_water_start;
        out_result->init_task_stack_high_water_end_bytes =
            (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    }
    return ran;
}

typedef struct
{
    resource_exhaustion_test_write_fn write;
    void *context;
    size_t bytes;
    bool ok;
} resource_exhaustion_json_writer_t;

static bool resource_exhaustion_json_put(resource_exhaustion_json_writer_t *writer,
                                         const char *text)
{
    const size_t length = strlen(text);
    if (!writer->ok || length > RESOURCE_EXHAUSTION_TEST_JSON_MAX_BYTES - writer->bytes)
    {
        writer->ok = false;
        return false;
    }
    writer->ok = writer->write(text, length, writer->context);
    if (writer->ok)
    {
        writer->bytes += length;
    }
    return writer->ok;
}

static bool resource_exhaustion_json_u64(resource_exhaustion_json_writer_t *writer, uint64_t value)
{
    char text[21];
    size_t used = sizeof(text);
    text[--used] = '\0';
    do
    {
        text[--used] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    return resource_exhaustion_json_put(writer, &text[used]);
}

static const char *resource_exhaustion_status_name(resource_exhaustion_test_status_t status)
{
    switch (status)
    {
    case RESOURCE_EXHAUSTION_TEST_STATUS_NOT_RUN: return "not-run";
    case RESOURCE_EXHAUSTION_TEST_STATUS_PASS: return "pass";
    case RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE: return "stats-unavailable";
    case RESOURCE_EXHAUSTION_TEST_STATUS_PSRAM_UNAVAILABLE: return "psram-unavailable";
    case RESOURCE_EXHAUSTION_TEST_STATUS_POINTER_CAPACITY: return "pointer-capacity";
    case RESOURCE_EXHAUSTION_TEST_STATUS_OOM_REASON: return "oom-reason";
    case RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_MISMATCH: return "counter-mismatch";
    case RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_WRAP_AMBIGUOUS: return "counter-wrap-ambiguous";
    case RESOURCE_EXHAUSTION_TEST_STATUS_CORRUPTION: return "corruption";
    case RESOURCE_EXHAUSTION_TEST_STATUS_BASELINE_MISMATCH: return "baseline-mismatch";
    case RESOURCE_EXHAUSTION_TEST_STATUS_MINIMUM_EVER_UNSTABLE: return "minimum-ever-unstable";
    case RESOURCE_EXHAUSTION_TEST_STATUS_INVALID_ARGUMENT: return "invalid-argument";
    }
    return "invalid-argument";
}

static bool resource_exhaustion_json_boolean(resource_exhaustion_json_writer_t *writer, bool value)
{
    return resource_exhaustion_json_put(writer, value ? "true" : "false");
}

static bool resource_exhaustion_json_stats(resource_exhaustion_json_writer_t *writer,
                                           const resource_exhaustion_test_stats_t *stats)
{
    return resource_exhaustion_json_put(writer, "{\"available\":") &&
           resource_exhaustion_json_boolean(writer, stats->available) &&
           resource_exhaustion_json_put(writer, ",\"ready\":") &&
           resource_exhaustion_json_boolean(writer, stats->ready) &&
           resource_exhaustion_json_put(writer, ",\"requestedBytesAvailable\":") &&
           resource_exhaustion_json_boolean(writer, stats->requested_bytes_available) &&
           resource_exhaustion_json_put(writer, ",\"minimumEverFreeBytesAvailable\":") &&
           resource_exhaustion_json_boolean(writer, stats->minimum_ever_free_bytes_available) &&
           resource_exhaustion_json_put(writer, ",\"freeBytes\":") &&
           resource_exhaustion_json_u64(writer, stats->free_bytes) &&
           resource_exhaustion_json_put(writer, ",\"usedBytes\":") &&
           resource_exhaustion_json_u64(writer, stats->used_bytes) &&
           resource_exhaustion_json_put(writer, ",\"peakUsedBytes\":") &&
           resource_exhaustion_json_u64(writer, stats->peak_used_bytes) &&
           resource_exhaustion_json_put(writer, ",\"requestedBytes\":") &&
           (stats->requested_bytes_available
                ? resource_exhaustion_json_u64(writer, stats->requested_bytes)
                : resource_exhaustion_json_put(writer, "null")) &&
           resource_exhaustion_json_put(writer, ",\"minimumEverFreeBytes\":") &&
           (stats->minimum_ever_free_bytes_available
                ? resource_exhaustion_json_u64(writer, stats->minimum_ever_free_bytes)
                : resource_exhaustion_json_put(writer, "null")) &&
           resource_exhaustion_json_put(writer, ",\"failedAllocations\":{\"value\":") &&
           resource_exhaustion_json_u64(writer, stats->failed_allocations.value) &&
           resource_exhaustion_json_put(writer, ",\"wrapCount\":") &&
           resource_exhaustion_json_u64(writer, stats->failed_allocations.wrap_count) &&
           resource_exhaustion_json_put(writer, "},\"lastFailureReason\":") &&
           resource_exhaustion_json_u64(writer, stats->last_failure_reason) &&
           resource_exhaustion_json_put(writer, "}");
}

static bool resource_exhaustion_json_result(resource_exhaustion_json_writer_t *writer,
                                            const resource_exhaustion_test_allocator_result_t *result)
{
    return resource_exhaustion_json_put(writer, "{\"status\":\"") &&
           resource_exhaustion_json_put(writer, resource_exhaustion_status_name(result->status)) &&
           resource_exhaustion_json_put(writer, "\",\"requestedCycles\":") &&
           resource_exhaustion_json_u64(writer, result->requested_cycles) &&
           resource_exhaustion_json_put(writer, ",\"completedCycles\":") &&
           resource_exhaustion_json_u64(writer, result->completed_cycles) &&
           resource_exhaustion_json_put(writer, ",\"minBlockCount\":") &&
           resource_exhaustion_json_u64(writer, result->min_block_count) &&
           resource_exhaustion_json_put(writer, ",\"maxBlockCount\":") &&
           resource_exhaustion_json_u64(writer, result->max_block_count) &&
           resource_exhaustion_json_put(writer, ",\"firstFailureCycle\":") &&
           resource_exhaustion_json_u64(writer, result->first_failure_cycle) &&
           resource_exhaustion_json_put(writer, ",\"baselineReturned\":") &&
           resource_exhaustion_json_boolean(writer, result->baseline_returned) &&
           resource_exhaustion_json_put(writer, ",\"counterExact\":") &&
           resource_exhaustion_json_boolean(writer, result->counter_exact) &&
           resource_exhaustion_json_put(writer, ",\"oomReasonExact\":") &&
           resource_exhaustion_json_boolean(writer, result->oom_reason_exact) &&
           resource_exhaustion_json_put(writer, ",\"payloadVerified\":") &&
           resource_exhaustion_json_boolean(writer, result->payload_verified) &&
           resource_exhaustion_json_put(writer, ",\"minimumEverStable\":") &&
           resource_exhaustion_json_boolean(writer, result->minimum_ever_stable) &&
           resource_exhaustion_json_put(writer, ",\"initial\":") &&
           resource_exhaustion_json_stats(writer, &result->initial_stats) &&
           resource_exhaustion_json_put(writer, ",\"final\":") &&
           resource_exhaustion_json_stats(writer, &result->final_stats) &&
           resource_exhaustion_json_put(writer, "}");
}

bool resource_exhaustion_test_write_json(const resource_exhaustion_test_result_t *result,
                                         resource_exhaustion_test_write_fn write,
                                         void *context)
{
    resource_exhaustion_json_writer_t writer;
    if (result == NULL || result->profile_id == NULL || write == NULL ||
        strlen(result->profile_id) > 128u)
    {
        return false;
    }
    writer.write = write;
    writer.context = context;
    writer.bytes = 0u;
    writer.ok = true;
    return resource_exhaustion_json_put(&writer, "{\"schemaVersion\":1,\"kind\":\"v052-resource-exhaustion\",\"profileId\":\"") &&
           resource_exhaustion_json_put(&writer, result->profile_id) &&
           resource_exhaustion_json_put(&writer, "\",\"constants\":{\"cycles\":100,\"pointerCapacity\":512,\"heapRequestBytes\":1024,\"psramRequestBytes\":16384},\"startMonotonicMs\":") &&
           resource_exhaustion_json_u64(&writer, result->start_monotonic_ms) &&
           resource_exhaustion_json_put(&writer, ",\"endMonotonicMs\":") &&
           resource_exhaustion_json_u64(&writer, result->end_monotonic_ms) &&
           resource_exhaustion_json_put(&writer, ",\"durationMs\":") &&
           resource_exhaustion_json_u64(&writer,
                                        result->end_monotonic_ms >= result->start_monotonic_ms
                                            ? result->end_monotonic_ms - result->start_monotonic_ms
                                            : 0u) &&
           resource_exhaustion_json_put(&writer, ",\"initTaskStackHighWaterStartBytes\":") &&
           resource_exhaustion_json_u64(&writer,
                                        result->init_task_stack_high_water_start_bytes) &&
           resource_exhaustion_json_put(&writer, ",\"initTaskStackHighWaterEndBytes\":") &&
           resource_exhaustion_json_u64(&writer,
                                        result->init_task_stack_high_water_end_bytes) &&
           resource_exhaustion_json_put(&writer, ",\"heap\":") &&
           resource_exhaustion_json_result(&writer, &result->heap) &&
           resource_exhaustion_json_put(&writer, ",\"psram\":") &&
           resource_exhaustion_json_result(&writer, &result->psram) &&
           resource_exhaustion_json_put(&writer, ",\"allPassed\":") &&
           resource_exhaustion_json_boolean(&writer, result->all_passed) &&
           resource_exhaustion_json_put(&writer, "}\n") && writer.ok;
}

#else

bool resource_exhaustion_test_run(resource_exhaustion_test_result_t *out_result)
{
    (void)out_result;
    return false;
}

bool resource_exhaustion_test_run_with_ops(
    const resource_exhaustion_test_allocator_ops_t *heap_ops,
    const resource_exhaustion_test_allocator_ops_t *psram_ops,
    const char *profile_id,
    resource_exhaustion_test_result_t *out_result)
{
    (void)heap_ops;
    (void)psram_ops;
    (void)profile_id;
    (void)out_result;
    return false;
}

bool resource_exhaustion_test_write_json(const resource_exhaustion_test_result_t *result,
                                         resource_exhaustion_test_write_fn write,
                                         void *context)
{
    (void)result;
    (void)write;
    (void)context;
    return false;
}

#endif

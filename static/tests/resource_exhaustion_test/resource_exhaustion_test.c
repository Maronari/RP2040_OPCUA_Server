#include "services/resource_exhaustion_test.h"
#include "FreeRTOS.h"
#include "psram_allocator.h"
#include "services/diag_service.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    uint32_t capacity;
    uint32_t active;
    uint32_t allocations;
    uint32_t free_cycles;
    uint64_t failure_value;
    uint64_t failure_wrap;
    uint32_t failure_reason;
    bool stats_available;
    bool ready;
    bool increment_failure;
    bool double_increment;
    bool baseline_mismatch;
    bool minimum_unstable;
    bool terminal_free_unstable;
    bool minimum_available;
    bool corrupt;
    bool delayed_corrupt;
    void *first_pointer;
    uint64_t now;
} fake_allocator_t;

static void fake_reset(fake_allocator_t *fake, uint32_t capacity)
{
    memset(fake, 0, sizeof(*fake));
    fake->capacity = capacity;
    fake->stats_available = true;
    fake->ready = true;
    fake->increment_failure = true;
    fake->minimum_available = true;
    fake->failure_reason = 1u;
}

static void *fake_allocate(size_t size, void *context)
{
    fake_allocator_t *fake = context;
    if (fake->active >= fake->capacity)
    {
        if (fake->increment_failure)
        {
            uint32_t increment = fake->double_increment ? 2u : 1u;
            while (increment-- > 0u)
            {
                ++fake->failure_value;
                if (fake->failure_value == 0u)
                {
                    ++fake->failure_wrap;
                }
            }
        }
        return NULL;
    }
    ++fake->active;
    ++fake->allocations;
    return malloc(size);
}

static void fake_release(void *pointer, void *context)
{
    fake_allocator_t *fake = context;
    free(pointer);
    assert(fake->active > 0u);
    --fake->active;
    if (fake->active == 0u)
    {
        ++fake->free_cycles;
    }
}

static bool fake_stats(resource_exhaustion_test_stats_t *out_stats, void *context)
{
    fake_allocator_t *fake = context;
    if (!fake->stats_available)
    {
        return false;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->available = true;
    out_stats->ready = fake->ready;
    out_stats->requested_bytes_available = true;
    out_stats->minimum_ever_free_bytes_available = fake->minimum_available;
    out_stats->free_bytes = 1000000u - (uint64_t)fake->active * 1024u;
    if (fake->terminal_free_unstable && fake->active != 0u && fake->free_cycles >= 1u)
    {
        --out_stats->free_bytes;
    }
    out_stats->used_bytes = 4000u + (uint64_t)fake->active * 1024u;
    out_stats->peak_used_bytes = 4000u + (uint64_t)fake->capacity * 1024u;
    out_stats->requested_bytes = out_stats->used_bytes;
    if (fake->baseline_mismatch && fake->active == 0u && fake->free_cycles > 0u)
    {
        ++out_stats->used_bytes;
    }
    out_stats->minimum_ever_free_bytes = 900000u;
    if (fake->minimum_unstable && fake->free_cycles >= 2u)
    {
        --out_stats->minimum_ever_free_bytes;
    }
    out_stats->failed_allocations.value = fake->failure_value;
    out_stats->failed_allocations.wrap_count = fake->failure_wrap;
    out_stats->last_failure_reason = fake->failure_reason;
    return true;
}

static uint64_t fake_now(void *context)
{
    fake_allocator_t *fake = context;
    return ++fake->now;
}

static void fake_after_fill(void *pointer, size_t size, uint32_t cycle, uint32_t block, void *context)
{
    fake_allocator_t *fake = context;
    (void)cycle;
    (void)block;
    if (fake->corrupt && size != 0u)
    {
        ((unsigned char *)pointer)[size / 2u] ^= 0x5au;
    }
    if (block == 1u)
    {
        fake->first_pointer = pointer;
    }
    else if (fake->delayed_corrupt && fake->first_pointer != NULL)
    {
        ((unsigned char *)fake->first_pointer)[0] ^= 0x5au;
        fake->delayed_corrupt = false;
    }
}

static resource_exhaustion_test_allocator_ops_t fake_ops(fake_allocator_t *fake)
{
    resource_exhaustion_test_allocator_ops_t ops = {
        .allocate = fake_allocate,
        .release = fake_release,
        .get_stats = fake_stats,
        .now_monotonic_ms = fake_now,
        .after_fill = fake_after_fill,
        .context = fake};
    return ops;
}

static char g_json[RESOURCE_EXHAUSTION_TEST_JSON_MAX_BYTES + 1u];
static size_t g_json_length;

static bool collect_json(const char *bytes, size_t length, void *context)
{
    (void)context;
    assert(g_json_length + length <= RESOURCE_EXHAUSTION_TEST_JSON_MAX_BYTES);
    memcpy(g_json + g_json_length, bytes, length);
    g_json_length += length;
    g_json[g_json_length] = '\0';
    return true;
}

static resource_exhaustion_test_result_t run_pair(fake_allocator_t *heap, fake_allocator_t *psram)
{
    resource_exhaustion_test_result_t result;
    resource_exhaustion_test_allocator_ops_t heap_ops = fake_ops(heap);
    resource_exhaustion_test_allocator_ops_t psram_ops = fake_ops(psram);
    assert(resource_exhaustion_test_run_with_ops(&heap_ops, &psram_ops, "native-test", &result));
    return result;
}

int main(int argc, char **argv)
{
    fake_allocator_t heap;
    fake_allocator_t psram;
    resource_exhaustion_test_result_t result;

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    psram.minimum_available = false;
    result = run_pair(&heap, &psram);
    assert(result.all_passed);
    assert(result.heap.completed_cycles == RESOURCE_EXHAUSTION_TEST_CYCLES);
    assert(result.psram.completed_cycles == RESOURCE_EXHAUSTION_TEST_CYCLES);
    assert(result.heap.final_stats.failed_allocations.value == RESOURCE_EXHAUSTION_TEST_CYCLES);
    assert(result.psram.final_stats.failed_allocations.value == RESOURCE_EXHAUSTION_TEST_CYCLES);
    assert(result.heap.initial_stats.peak_used_bytes == 4000u + 3u * 1024u);
    assert(result.psram.final_stats.peak_used_bytes == 4000u + 3u * 1024u);
    assert(result.heap.baseline_returned && result.psram.baseline_returned);
    assert(result.heap.minimum_ever_stable);
    assert(result.psram.minimum_ever_stable);
    result.heap.initial_stats.requested_bytes_available = false;
    result.heap.initial_stats.requested_bytes = 0u;
    result.heap.final_stats.requested_bytes_available = false;
    result.heap.final_stats.requested_bytes = 0u;
    g_json_length = 0u;
    assert(resource_exhaustion_test_write_json(&result, collect_json, NULL));
    assert(g_json_length <= RESOURCE_EXHAUSTION_TEST_JSON_MAX_BYTES);
    assert(strstr(g_json, "\"allPassed\":true") != NULL);
    assert(strstr(g_json, "\"peakUsedBytes\":7072") != NULL);
    assert(strstr(g_json, "\"minimumEverFreeBytesAvailable\":false") != NULL);
    assert(strstr(g_json, "\"minimumEverFreeBytes\":null") != NULL);
    assert(strstr(g_json, "\"requestedBytesAvailable\":false") != NULL);
    assert(strstr(g_json, "\"requestedBytes\":null") != NULL);
    if (argc == 2)
    {
        FILE *file = fopen(argv[1], "wb");
        assert(file != NULL);
        assert(fwrite(g_json, 1u, g_json_length, file) == g_json_length);
        assert(fclose(file) == 0);
    }

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    psram.failure_reason = 2u;
    result = run_pair(&heap, &psram);
    assert(result.psram.status == RESOURCE_EXHAUSTION_TEST_STATUS_OOM_REASON);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    heap.double_increment = true;
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_MISMATCH);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    heap.corrupt = true;
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_CORRUPTION);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    heap.delayed_corrupt = true;
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_CORRUPTION);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    heap.baseline_mismatch = true;
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_BASELINE_MISMATCH);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    psram.ready = false;
    result = run_pair(&heap, &psram);
    assert(result.psram.status == RESOURCE_EXHAUSTION_TEST_STATUS_PSRAM_UNAVAILABLE);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    heap.failure_value = UINT64_MAX;
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_PASS);
    assert(result.all_passed);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    heap.failure_value = UINT64_MAX;
    heap.double_increment = true;
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_COUNTER_WRAP_AMBIGUOUS);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    heap.minimum_unstable = true;
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_MINIMUM_EVER_UNSTABLE);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    psram.minimum_available = false;
    psram.terminal_free_unstable = true;
    result = run_pair(&heap, &psram);
    assert(result.psram.status == RESOURCE_EXHAUSTION_TEST_STATUS_MINIMUM_EVER_UNSTABLE);

    fake_reset(&heap, RESOURCE_EXHAUSTION_TEST_POINTER_CAPACITY);
    fake_reset(&psram, 3u);
    result = run_pair(&heap, &psram);
    assert(result.heap.status == RESOURCE_EXHAUSTION_TEST_STATUS_POINTER_CAPACITY);

    fake_reset(&heap, 3u);
    fake_reset(&psram, 3u);
    psram.stats_available = false;
    result = run_pair(&heap, &psram);
    assert(result.psram.status == RESOURCE_EXHAUSTION_TEST_STATUS_STATS_UNAVAILABLE);
    return 0;
}

/* The native wrappers are linked by the same object as the test seam. */
void *pvPortMalloc(size_t size) { return malloc(size); }
void vPortFree(void *pointer) { free(pointer); }
TickType_t xTaskGetTickCount(void) { return 0u; }
UBaseType_t uxTaskGetStackHighWaterMark(void *task) { (void)task; return 0u; }
void *psram_malloc(size_t size) { return malloc(size); }
void psram_free(void *pointer) { free(pointer); }
bool psram_allocator_get_stats(psram_allocator_stats_t *out_stats) { (void)out_stats; return false; }
bool diag_service_sample(uint64_t monotonic_ms, bool timestamp_valid, uint64_t timestamp_unix_us) { (void)monotonic_ms; (void)timestamp_valid; (void)timestamp_unix_us; return false; }
bool diag_service_get_snapshot(diag_service_snapshot_t *out_snapshot) { (void)out_snapshot; return false; }

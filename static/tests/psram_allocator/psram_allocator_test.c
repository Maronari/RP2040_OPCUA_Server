#include "psram_allocator.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

static bool g_force_lock_timeout;
static bool g_interrupt_context;
static unsigned g_critical_depth;
static TickType_t g_tick = 100u;

void psram_test_enter_critical(void)
{
    ++g_critical_depth;
}

void psram_test_exit_critical(void)
{
    assert(g_critical_depth > 0u);
    --g_critical_depth;
}

UBaseType_t psram_test_enter_critical_from_isr(void)
{
    ++g_critical_depth;
    return 0x33u;
}

void psram_test_exit_critical_from_isr(UBaseType_t mask)
{
    assert(mask == 0x33u);
    psram_test_exit_critical();
}

BaseType_t xPortIsInsideInterrupt(void)
{
    return g_interrupt_context ? pdTRUE : pdFALSE;
}

TickType_t xTaskGetTickCount(void)
{
    return g_tick;
}

TickType_t xTaskGetTickCountFromISR(void)
{
    return g_tick;
}

BaseType_t xTaskGetSchedulerState(void)
{
    return taskSCHEDULER_RUNNING;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    storage->marker = 1u;
    return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    (void)timeout;
    return handle != NULL && !g_force_lock_timeout ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    return handle != NULL ? pdTRUE : pdFALSE;
}

int main(void)
{
    static union
    {
        max_align_t alignment;
        unsigned char bytes[4096];
    } arena;
    psram_allocator_stats_t stats;

    assert(!psram_allocator_test_init(NULL, sizeof(arena.bytes)));
    assert(psram_allocator_test_init(arena.bytes, sizeof(arena.bytes)));
    assert(psram_allocator_get_stats(&stats));
    const size_t baseline_used = stats.used_bytes;

    unsigned char *first = (unsigned char *)psram_malloc(100u);
    assert(first != NULL);
    memset(first, 0xa5, 100u);
    assert(psram_allocator_get_stats(&stats));
    assert(stats.requested_bytes_in_use == 100u);
    assert(stats.successful_allocations == 1u);

    g_force_lock_timeout = true;
    psram_free(first);
    g_force_lock_timeout = false;
    assert(psram_allocator_get_stats(&stats));
    assert(stats.lock_timeouts == 1u);
    assert(stats.successful_frees == 1u);
    assert(stats.requested_bytes_in_use == 0u);
    assert(stats.used_bytes == baseline_used);

    psram_free(first);
    assert(psram_allocator_get_stats(&stats));
    assert(stats.invalid_context_failures == 1u);
    assert(stats.failed_allocations == 0u);

    unsigned char *old = (unsigned char *)psram_malloc(64u);
    assert(old != NULL);
    for (size_t index = 0u; index < 64u; ++index)
    {
        old[index] = (unsigned char)index;
    }
    unsigned char *grown = (unsigned char *)psram_realloc(old, 128u);
    assert(grown != NULL);
    for (size_t index = 0u; index < 64u; ++index)
    {
        assert(grown[index] == (unsigned char)index);
    }
    assert(psram_allocator_get_stats(&stats));
    assert(stats.requested_bytes_in_use == 128u);
    assert(stats.successful_allocations == 3u);
    assert(stats.successful_frees == 1u);

    assert(psram_realloc(grown, SIZE_MAX) == NULL);
    assert(grown[17] == 17u);
    assert(psram_allocator_get_stats(&stats));
    assert(stats.last_failure_reason == PSRAM_ALLOCATOR_FAILURE_SIZE_OVERFLOW);
    assert(stats.requested_bytes_in_use == 128u);

    assert(psram_calloc(SIZE_MAX, 2u) == NULL);
    assert(psram_allocator_get_stats(&stats));
    assert(stats.last_failure_reason == PSRAM_ALLOCATOR_FAILURE_SIZE_OVERFLOW);

    g_force_lock_timeout = true;
    assert(psram_malloc(8u) == NULL);
    g_force_lock_timeout = false;
    assert(psram_allocator_get_stats(&stats));
    assert(stats.last_failure_reason == PSRAM_ALLOCATOR_FAILURE_LOCK_TIMEOUT);
    assert(stats.lock_timeouts == 2u);

    g_interrupt_context = true;
    assert(psram_malloc(8u) == NULL);
    assert(psram_calloc(SIZE_MAX, 2u) == NULL);
    g_interrupt_context = false;
    assert(psram_allocator_get_stats(&stats));
    assert(stats.last_failure_reason == PSRAM_ALLOCATOR_FAILURE_INVALID_CONTEXT);
    assert(stats.invalid_context_failures == 3u);

    psram_free(grown);
    assert(psram_allocator_get_stats(&stats));
    assert(stats.requested_bytes_in_use == 0u);
    assert(stats.used_bytes == baseline_used);

    unsigned char *zeroed = (unsigned char *)psram_calloc(4u, 16u);
    assert(zeroed != NULL);
    for (size_t index = 0u; index < 64u; ++index)
    {
        assert(zeroed[index] == 0u);
    }
    assert(psram_realloc(zeroed, 0u) == NULL);
    assert(psram_allocator_get_stats(&stats));
    assert(stats.requested_bytes_in_use == 0u);
    assert(stats.used_bytes == baseline_used);

    for (size_t cycle = 0u; cycle < 5u; ++cycle)
    {
        void *blocks[128] = {0};
        size_t count = 0u;
        while (count < 128u && (blocks[count] = psram_malloc(32u)) != NULL)
        {
            ++count;
        }
        assert(count > 0u && count < 128u);
        assert(psram_allocator_get_stats(&stats));
        assert(stats.last_failure_reason == PSRAM_ALLOCATOR_FAILURE_OUT_OF_MEMORY);
        for (size_t index = 0u; index < count; ++index)
        {
            psram_free(blocks[index]);
        }
        assert(psram_allocator_get_stats(&stats));
        assert(stats.requested_bytes_in_use == 0u);
        assert(stats.used_bytes == baseline_used);
    }
    assert(g_critical_depth == 0u);
    return 0;
}

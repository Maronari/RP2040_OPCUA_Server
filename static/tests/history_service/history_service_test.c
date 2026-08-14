#include "services/history_service.h"
#include "psram_allocator.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static TickType_t g_tick;
static unsigned g_critical_entries;
static unsigned g_mutex_take_calls;
static bool g_force_mutex_busy;
#if defined(PICO_ON_DEVICE)
static bool g_psram_ready;
static bool g_psram_allocation_fails;

bool psram_allocator_is_ready(void) { return g_psram_ready; }
void *psram_calloc(size_t count, size_t size)
{
    return g_psram_allocation_fails ? NULL : calloc(count, size);
}
#endif

TickType_t xTaskGetTickCount(void)
{
    return g_tick;
}

void history_service_test_enter_critical(void)
{
    g_critical_entries++;
}

void history_service_test_exit_critical(void)
{
    g_critical_entries++;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    storage->locked = 0;
    return storage;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)ticks;
    g_mutex_take_calls++;
    if (g_force_mutex_busy)
    {
        return pdFALSE;
    }
    if (semaphore->locked)
    {
        return pdFALSE;
    }
    semaphore->locked = 1;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    semaphore->locked = 0;
    return pdTRUE;
}

static void overwrite_stack(char *buffer, size_t size)
{
    memset(buffer, 'x', size);
}

int main(void)
{
    history_service_channel_sample_t samples[2] = {0};
    char short_lived_id[HISTORY_SERVICE_CHANNEL_ID_MAX_LEN] = "stack-channel";
    bool truncated = false;

#if defined(PICO_ON_DEVICE)
    assert(!history_service_init());
    g_psram_ready = true;
    g_psram_allocation_fails = true;
    assert(!history_service_init());
    g_psram_allocation_fails = false;
#endif
    assert(history_service_init());
    assert(g_critical_entries == 0u);
    g_tick = 10u;
    assert(history_service_record_channel_sample(short_lived_id, 1.5f, SERVICE_QUALITY_GOOD,
                                                 1700000000123456ULL, 1u));
    overwrite_stack(short_lived_id, sizeof(short_lived_id));
    assert(history_service_read_channel_samples("stack-channel", 1700000000u, 1700000000u,
                                                false, 2u, samples, 2u, &truncated, NULL) == 1u);
    assert(!truncated && strcmp(samples[0].channel_id, "stack-channel") == 0);
    assert(samples[0].source_timestamp_unix_us == 1700000000123456ULL);

    g_tick = 20u;
    assert(history_service_record_channel_sample("epoch-zero", 2.0f, SERVICE_QUALITY_GOOD, 0u, 1u));
    assert(history_service_read_channel_samples("epoch-zero", 0u, 0u,
                                                false, 1u, samples, 1u, &truncated, NULL) == 1u);
    assert(samples[0].source_timestamp_unix_us == 0u);
    {
        bool busy = false;
        g_force_mutex_busy = true;
        assert(history_service_read_channel_samples("epoch-zero", 0u, 0u, false,
                                                    1u, samples, 1u, &truncated, &busy) == 0u);
        assert(busy);
        g_force_mutex_busy = false;
    }
    assert(!history_service_record_channel_sample("bad", 0.0f, SERVICE_QUALITY_GOOD,
                                                  TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID, 1u));
    assert(g_mutex_take_calls >= 3u);
    assert(g_critical_entries == 0u);
    return 0;
}

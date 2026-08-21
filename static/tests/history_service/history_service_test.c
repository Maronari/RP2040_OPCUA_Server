#include "services/history_service.h"
#include "services/channel_manager.h"
#include "psram_allocator.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static TickType_t g_tick;
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
void *psram_malloc(size_t size)
{
    return g_psram_allocation_fails ? NULL : malloc(size);
}
void psram_free(void *ptr)
{
    free(ptr);
}

/* The real drivers/history_flash_backend.c binds to hardware-only headers
 * (ota_layout.h, drivers/flash_operation_coordinator.h) that do not exist on
 * the host. This device-allocation test variant only exercises
 * history_service.c's PSRAM allocation-failure gating, so it links against
 * trivial stand-ins that report "unavailable" instead of the real backend;
 * history_service.c never calls bind/restore/append once
 * history_flash_backend_init_flash() reports ready == false. */
#include "drivers/history_flash_backend.h"

void history_flash_backend_init_flash(history_flash_backend_t *backend)
{
    memset(backend, 0, sizeof(*backend));
    backend->ready = false;
}

history_flash_backend_result_t history_flash_backend_bind_channel(history_flash_backend_t *backend, uint8_t slot,
                                                                   const char *channel_id)
{
    (void)backend;
    (void)slot;
    (void)channel_id;
    return HISTORY_FLASH_BACKEND_NOT_BOUND;
}

history_flash_backend_result_t history_flash_backend_restore(history_flash_backend_t *backend, uint8_t slot,
                                                              history_flash_backend_scan_entry_t *scratch,
                                                              size_t scratch_capacity,
                                                              history_flash_backend_restore_cb callback,
                                                              void *user_context)
{
    (void)backend;
    (void)slot;
    (void)scratch;
    (void)scratch_capacity;
    (void)callback;
    (void)user_context;
    return HISTORY_FLASH_BACKEND_NOT_BOUND;
}

history_flash_backend_result_t history_flash_backend_append(history_flash_backend_t *backend, uint8_t slot,
                                                             const history_flash_backend_record_t *record)
{
    (void)backend;
    (void)slot;
    (void)record;
    return HISTORY_FLASH_BACKEND_NOT_BOUND;
}
#endif

bool time_service_get_utc_time_us(uint64_t *out_unix_time_us)
{
    if (out_unix_time_us != NULL)
    {
        *out_unix_time_us = 0u;
    }
    return false;
}

TickType_t xTaskGetTickCount(void)
{
    return g_tick;
}

/* channel_manager.c uses bounded taskENTER_CRITICAL/EXIT_CRITICAL sections;
 * this test's FreeRTOS.h stub maps them to these counting hooks. */
void history_service_test_enter_critical(void) { }
void history_service_test_exit_critical(void) { }

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

static void register_channel(const char *id, telemetry_channel_value_type_t value_type, uint32_t flags)
{
    telemetry_channel_t channel;
    channel_manager_handle_t handle;

    memset(&channel, 0, sizeof(channel));
    channel.metadata.id = id;
    channel.metadata.name = id;
    channel.metadata.kind = TELEMETRY_CHANNEL_KIND_ANALOG;
    channel.metadata.value_type = value_type;
    channel.metadata.flags = flags;
    channel.settings.enabled = true;
    assert(channel_manager_register(&channel, &handle));
}

int main(void)
{
    history_service_channel_sample_t samples[300] = {0};
    uint32_t generation = 0u;
    bool more = false;
    bool busy = false;

#if defined(PICO_ON_DEVICE)
    assert(!history_service_init());
    g_psram_ready = true;
    g_psram_allocation_fails = true;
    assert(!history_service_init());
    g_psram_allocation_fails = false;
#endif

    assert(channel_manager_init());
    register_channel("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                     TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED);
    register_channel("channel.bool", TELEMETRY_CHANNEL_VALUE_BOOLEAN,
                     TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED);
    register_channel("channel.string.excluded", TELEMETRY_CHANNEL_VALUE_STRING,
                     TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED);
    register_channel("channel.not.historized", TELEMETRY_CHANNEL_VALUE_INT32, TELEMETRY_CHANNEL_FLAG_ENABLED);

    assert(history_service_init());
    assert(g_mutex_take_calls > 0u);

    /* Mixed value types: float and boolean channels are independently
     * claimed and read back with the correct owned data. */
    g_tick = 10u;
    assert(history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                  &(history_service_value_t){.f32 = 1.5f},
                                                  SERVICE_QUALITY_GOOD, 1700000000123456ULL, 1u));
    assert(history_service_record_channel_sample("channel.bool", TELEMETRY_CHANNEL_VALUE_BOOLEAN,
                                                  &(history_service_value_t){.boolean = true},
                                                  SERVICE_QUALITY_GOOD, 1700000000123456ULL, 1u));

    assert(history_service_read_channel_samples("channel.float", 0u, 0u, false, 10u, 0u, samples, 10u,
                                                &generation, &more, &busy) == 1u);
    assert(!more && !busy);
    assert(samples[0].value_type == TELEMETRY_CHANNEL_VALUE_FLOAT);
    assert(samples[0].value.f32 == 1.5f);

    assert(history_service_read_channel_samples("channel.bool", 0u, 0u, false, 10u, 0u, samples, 10u,
                                                &generation, &more, &busy) == 1u);
    assert(samples[0].value_type == TELEMETRY_CHANNEL_VALUE_BOOLEAN);
    assert(samples[0].value.boolean == true);

    /* String channels are never historized, regardless of the HISTORIZED
     * flag: the write is rejected outright. */
    assert(!history_service_record_channel_sample("channel.string.excluded", TELEMETRY_CHANNEL_VALUE_STRING,
                                                   &(history_service_value_t){0},
                                                   SERVICE_QUALITY_GOOD, 1700000000123456ULL, 1u));

    /* Quality gate and invalid-source-timestamp gate are preserved from the
     * v0.4.2 behavior. */
    assert(!history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                   &(history_service_value_t){.f32 = 9.0f},
                                                   SERVICE_QUALITY_UNCERTAIN, 1700000000123456ULL, 1u));
    assert(!history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                   &(history_service_value_t){.f32 = 9.0f},
                                                   SERVICE_QUALITY_GOOD,
                                                   TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID, 1u));

    /* Busy read: lock cannot be acquired at all -> zero samples, out_busy. */
    {
        g_force_mutex_busy = true;
        assert(history_service_read_channel_samples("channel.float", 0u, 0u, false, 10u, 0u, samples, 10u,
                                                     &generation, &more, &busy) == 0u);
        assert(busy);
        g_force_mutex_busy = false;
    }

    /* Capacity wraparound + generation + continuation pagination on a fresh
     * channel claimed dynamically at first write (not pre-bound via
     * channel_manager, exercising the record-time claim fallback path). */
    {
        const size_t total = (size_t)HISTORY_SERVICE_CHANNEL_CAPACITY + 5u;
        uint32_t generation_before_wrap = 0u;
        uint32_t generation_after_wrap = 0u;

        for (size_t i = 0u; i < total; ++i)
        {
            g_tick = (TickType_t)(100u + i);
            assert(history_service_record_channel_sample(
                "channel.dynamic", TELEMETRY_CHANNEL_VALUE_FLOAT,
                &(history_service_value_t){.f32 = (float)i}, SERVICE_QUALITY_GOOD,
                1700000000000000ULL + (uint64_t)i * 1000000ULL, 1u));
            if (i == HISTORY_SERVICE_CHANNEL_CAPACITY - 1u)
            {
                assert(history_service_get_channel_generation("channel.dynamic", &generation_before_wrap));
            }
        }
        assert(history_service_get_channel_generation("channel.dynamic", &generation_after_wrap));
        assert(generation_before_wrap == 0u);
        assert(generation_after_wrap == 5u);

        {
            /* A single call never returns more than
             * HISTORY_SERVICE_MAX_RETURN_COUNT (the frozen per-call cap),
             * even though both max_samples and out_capacity here allow for
             * more; a continuation is signaled instead. */
            const size_t returned = history_service_read_channel_samples(
                "channel.dynamic", 0u, 0u, false, 300u, 0u, samples, 300u, &generation, &more, &busy);
            assert(returned == HISTORY_SERVICE_MAX_RETURN_COUNT);
            assert(more && !busy);
            /* The oldest 5 samples (i=0..4) were evicted; the window starts
             * at value 5.0 ascending. */
            assert(samples[0].value.f32 == 5.0f);
            assert(samples[returned - 1u].value.f32 == (float)(5u + HISTORY_SERVICE_MAX_RETURN_COUNT - 1u));
        }

        /* Continuation pagination: page through in fixed-size chunks and
         * confirm every sample across all pages matches the expected
         * ascending, post-eviction sequence (5.0 .. total-1). */
        {
            history_service_channel_sample_t page[50];
            size_t skip = 0u;
            size_t total_collected = 0u;
            uint32_t page_generation = 0u;
            bool page_more = true;

            while (page_more)
            {
                const size_t got = history_service_read_channel_samples(
                    "channel.dynamic", 0u, 0u, false, 50u, skip, page, 50u, &page_generation, &page_more, &busy);

                assert(page_generation == generation_after_wrap);
                for (size_t i = 0u; i < got; ++i)
                {
                    assert(page[i].value.f32 == (float)(5u + total_collected + i));
                }
                total_collected += got;
                skip += got;
                assert(got > 0u || !page_more);
            }
            assert(total_collected == HISTORY_SERVICE_CHANNEL_CAPACITY);
        }
    }

    /* Debounce: a second sample inside record_interval_ms is dropped. */
    {
        size_t before;
        size_t after;

        g_tick = 5000u;
        assert(history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                      &(history_service_value_t){.f32 = 42.0f},
                                                      SERVICE_QUALITY_GOOD, 1800000000000000ULL, 1000u));
        before = history_service_read_channel_samples("channel.float", 0u, 0u, false, 100u, 0u, samples, 100u,
                                                       &generation, &more, &busy);
        g_tick = 5100u; /* only 100 ticks later, under the 1000-tick interval */
        assert(!history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                       &(history_service_value_t){.f32 = 43.0f},
                                                       SERVICE_QUALITY_GOOD, 1800000000100000ULL, 1000u));
        after = history_service_read_channel_samples("channel.float", 0u, 0u, false, 100u, 0u, samples, 100u,
                                                      &generation, &more, &busy);
        assert(after == before);
    }

    /* A never-recorded/unknown channel reads back nothing and no generation. */
    {
        uint32_t unknown_generation = 123u;

        assert(!history_service_get_channel_generation("channel.never.seen", &unknown_generation));
        assert(history_service_read_channel_samples("channel.never.seen", 0u, 0u, false, 10u, 0u, samples, 10u,
                                                     &unknown_generation, &more, &busy) == 0u);
        assert(unknown_generation == 0u);
    }

    return 0;
}

#include "services/history_service.h"
#include "services/channel_manager.h"
#include "psram_allocator.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#if defined(PICO_ON_DEVICE)
#include "drivers/history_flash_backend.h"
#endif

static TickType_t g_tick;
static unsigned g_mutex_take_calls;
static bool g_force_mutex_busy;

/* Deterministic within-call interleaving support for the chunk-boundary
 * race test below. history_service_read_channel_samples() calls
 * history_service_test_after_chunk() (only in this HISTORY_SERVICE_TEST_HOOKS
 * build) right after releasing the lock that guarded one chunk's copy, and
 * right before re-locking/re-checking generation for the next chunk. Arming
 * this for one channel/one chunk boundary lets the test inject writes from
 * an independent "writer task" at exactly the formerly-unsafe point,
 * without relying on OS thread scheduling. */
#define RACE_CHANNEL_ID "channel.race"
#define RACE_EVICTIONS 40u
#define RACE_SENTINEL_BASE 9000.0f
#define RACE_WRITER_TICK_BASE 500000u
#define RACE_WRITER_TS_BASE 3000000000000000ULL
static bool g_race_armed;
static bool g_race_fired;
static bool g_prescan_race_armed;
static bool g_prescan_race_fired;

static unsigned g_race_pass;

static void race_inject_evictions(void)
{
    /* Stand-in for sensor_manager.c's sampling task racing with a read: the
     * ring is already full, so each of these writes evicts the then-oldest
     * sample and bumps generation once, exactly the condition the fix must
     * detect before trusting a ring_count/oldest_index mapping captured
     * before these writes ran. Each call uses a fresh, later tick/timestamp
     * range so a second injection on the same channel stays monotonic
     * rather than relying on unsigned tick wraparound to satisfy debounce. */
    const uint32_t pass_tick_base = RACE_WRITER_TICK_BASE + g_race_pass * (RACE_EVICTIONS + 10u);
    const uint64_t pass_ts_base = RACE_WRITER_TS_BASE + (uint64_t)g_race_pass * RACE_EVICTIONS * 2000000ULL;

    ++g_race_pass;
    for (size_t i = 0u; i < RACE_EVICTIONS; ++i)
    {
        g_tick = (TickType_t)(pass_tick_base + i);
        assert(history_service_record_channel_sample(
            RACE_CHANNEL_ID, TELEMETRY_CHANNEL_VALUE_FLOAT,
            &(history_service_value_t){.f32 = RACE_SENTINEL_BASE + (float)i},
            SERVICE_QUALITY_GOOD, pass_ts_base + (uint64_t)i * 1000000ULL, 1u));
    }
}

void history_service_test_after_chunk(const char *channel_id, size_t chunk_start_scanned)
{
    if (!g_race_armed || g_race_fired || channel_id == NULL ||
        strcmp(channel_id, RACE_CHANNEL_ID) != 0 || chunk_start_scanned != 0u)
    {
        return;
    }
    g_race_fired = true;
    race_inject_evictions();
}

/* Fires in the gap between the call's initial snapshot and its first
 * chunk's lock/generation check -- before any chunk has been copied, so a
 * detected mismatch here must take the "nothing collected yet" busy path
 * rather than the "return what was already collected" more path. */
void history_service_test_before_scan(const char *channel_id)
{
    if (!g_prescan_race_armed || g_prescan_race_fired || channel_id == NULL ||
        strcmp(channel_id, RACE_CHANNEL_ID) != 0)
    {
        return;
    }
    g_prescan_race_fired = true;
    race_inject_evictions();
}
#if defined(PICO_ON_DEVICE)
static bool g_psram_ready;
static bool g_psram_allocation_fails;
static bool g_flash_backend_ready;
static history_flash_backend_result_t g_flash_bind_result = HISTORY_FLASH_BACKEND_OK;
static history_flash_backend_result_t g_flash_restore_result = HISTORY_FLASH_BACKEND_OK;
static history_flash_backend_result_t g_flash_append_result = HISTORY_FLASH_BACKEND_OK;
/* v0.5.0 T7: track history_flash_backend_bind_channel() calls so the test
 * can prove history_service_clear_channel() unbinds the Flash slot
 * (bind_channel(slot, NULL)), not just the PSRAM ring. */
static unsigned g_bind_calls;
static bool g_last_bind_channel_id_was_null;

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
 * the host. These device-profile stand-ins deliberately exercise both an
 * unavailable backend and ready-backend bind/restore/append failures. */

void history_flash_backend_init_flash(history_flash_backend_t *backend)
{
    memset(backend, 0, sizeof(*backend));
    backend->ready = g_flash_backend_ready;
}

history_flash_backend_result_t history_flash_backend_bind_channel(history_flash_backend_t *backend, uint8_t slot,
                                                                   const char *channel_id)
{
    (void)slot;
    ++g_bind_calls;
    g_last_bind_channel_id_was_null = (channel_id == NULL);
    return backend->ready ? g_flash_bind_result : HISTORY_FLASH_BACKEND_NOT_BOUND;
}

history_flash_backend_result_t history_flash_backend_restore(history_flash_backend_t *backend, uint8_t slot,
                                                              history_flash_backend_scan_entry_t *scratch,
                                                              size_t scratch_capacity,
                                                              history_flash_backend_restore_cb callback,
                                                              void *user_context)
{
    (void)slot;
    (void)scratch;
    (void)scratch_capacity;
    (void)callback;
    (void)user_context;
    return backend->ready ? g_flash_restore_result : HISTORY_FLASH_BACKEND_NOT_BOUND;
}

history_flash_backend_result_t history_flash_backend_append(history_flash_backend_t *backend, uint8_t slot,
                                                             const history_flash_backend_record_t *record)
{
    (void)slot;
    (void)record;
    return backend->ready ? g_flash_append_result : HISTORY_FLASH_BACKEND_NOT_BOUND;
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

#if defined(PICO_ON_DEVICE)
    /* Persistence must fail closed when Flash initialization is unavailable:
     * a caller can observe this before attempting HistoryRead. */
    assert(history_service_get_channel_restore_status("channel.float") ==
           HISTORY_SERVICE_RESTORE_STATUS_FLASH_UNAVAILABLE);

    /* A ready backend plus a temporary restore-scratch allocation failure is
     * also explicit; init succeeds only in the documented degraded state. */
    g_flash_backend_ready = true;
    g_psram_allocation_fails = true;
    assert(history_service_init());
    assert(history_service_get_channel_restore_status("channel.float") ==
           HISTORY_SERVICE_RESTORE_STATUS_SCRATCH_ALLOCATION_FAILED);
    g_psram_allocation_fails = false;

    /* Ready-backend failures are never silently converted to an empty,
     * apparently-good history window. */
    g_flash_bind_result = HISTORY_FLASH_BACKEND_IO;
    assert(history_service_init());
    assert(history_service_get_channel_restore_status("channel.float") ==
           HISTORY_SERVICE_RESTORE_STATUS_FLASH_BIND_FAILED);
    g_flash_bind_result = HISTORY_FLASH_BACKEND_OK;
    g_flash_restore_result = HISTORY_FLASH_BACKEND_IO;
    assert(history_service_init());
    assert(history_service_get_channel_restore_status("channel.float") ==
           HISTORY_SERVICE_RESTORE_STATUS_FLASH_RESTORE_FAILED);
    g_flash_restore_result = HISTORY_FLASH_BACKEND_OK;
    assert(history_service_init());
    assert(history_service_get_channel_restore_status("channel.float") ==
           HISTORY_SERVICE_RESTORE_STATUS_READY);
#endif

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

    /* Within-call chunk-boundary race: a concurrent writer evicts/overwrites
     * ring slots between two chunks of a single multi-chunk scan (not just
     * between separate continuation-point calls). Fill "channel.race" to
     * exactly its capacity first, so oldest_index/write_index are both 0 and
     * the ascending scan order maps position p directly to physical slot p
     * -- this makes the corruption this test proves against easy to reason
     * about precisely. */
    {
        const size_t capacity = (size_t)HISTORY_SERVICE_CHANNEL_CAPACITY;
        uint32_t race_generation = 0u;
        uint32_t post_race_generation = 0u;
        size_t returned;

        for (size_t i = 0u; i < capacity; ++i)
        {
            g_tick = (TickType_t)(200000u + i);
            assert(history_service_record_channel_sample(
                RACE_CHANNEL_ID, TELEMETRY_CHANNEL_VALUE_FLOAT, &(history_service_value_t){.f32 = (float)i},
                SERVICE_QUALITY_GOOD, 2000000000000000ULL + (uint64_t)i * 1000000ULL, 1u));
        }
        assert(history_service_get_channel_generation(RACE_CHANNEL_ID, &race_generation));
        assert(race_generation == 0u); /* exactly full, nothing evicted yet */

        g_race_armed = true;
        g_race_fired = false;
        returned = history_service_read_channel_samples(RACE_CHANNEL_ID, 0u, 0u, false, 300u, 0u, samples, 300u,
                                                         &generation, &more, &busy);
        g_race_armed = false;
        assert(g_race_fired);

        /* The writer injected by the hook ran between chunk 0 (positions
         * 0..31) and chunk 1 (positions 32..63), evicting RACE_EVICTIONS
         * (40) samples and bumping generation 40 times -- past chunk 1's
         * first 8 entries (physical slots 32..39, which the stale
         * oldest_index=0 mapping would still address as positions 32..39).
         * The fix must detect this at chunk 1's boundary and stop instead
         * of paging into now-overwritten slots. */
        assert(!busy);
        assert(more);
        assert(returned == HISTORY_SERVICE_READ_CHUNK_SAMPLES); /* only chunk 0 was safe to return */
        assert(generation == 0u); /* the generation captured at this call's start, not the post-race value */
        for (size_t i = 0u; i < returned; ++i)
        {
            assert(samples[i].value.f32 == (float)i); /* chunk 0's data is untouched and correct */
        }

        assert(history_service_get_channel_generation(RACE_CHANNEL_ID, &post_race_generation));
        assert(post_race_generation == (uint32_t)RACE_EVICTIONS);

        /* Prove the corruption this prevents: had the scan continued with
         * the stale mapping, chunk 1's first entry (position 32) would have
         * read physical slot 32 (sample_index = (oldest_index=0 + 32) % 256).
         * The 40 interleaved writes above overwrote physical slots 0..39 in
         * order (write_index started at 0), so slot 32 now holds the 33rd
         * interleaved write's value, RACE_SENTINEL_BASE + 32. A correct,
         * consistent read of the channel's *current* state confirms that
         * sample actually belongs at logical position 216 + 32 = 248 (216
         * survivors of the original fill, then the sentinels in write
         * order) -- nowhere near position 32. Presenting it at position 32,
         * immediately after the genuinely-32-samples-old value 31.0f, would
         * have silently spliced a same-instant "future" sample into the
         * middle of an otherwise strictly ascending, ~2e15-microsecond-old
         * sequence: exactly the corrupted/out-of-order data the fix exists
         * to prevent. */
        {
            history_service_channel_sample_t full_window[300];
            const size_t survivors = capacity - RACE_EVICTIONS;
            history_service_channel_sample_t page[HISTORY_SERVICE_MAX_RETURN_COUNT];
            size_t skip = 0u;
            size_t full_collected = 0u;
            uint32_t page_generation = 0u;
            bool page_more = true;

            /* One call is capped at HISTORY_SERVICE_MAX_RETURN_COUNT (128),
             * so paging (as the "Continuation pagination" case above does)
             * is required to see the whole 256-sample window. */
            while (page_more)
            {
                const size_t got = history_service_read_channel_samples(
                    RACE_CHANNEL_ID, 0u, 0u, false, HISTORY_SERVICE_MAX_RETURN_COUNT, skip, page,
                    HISTORY_SERVICE_MAX_RETURN_COUNT, &page_generation, &page_more, &busy);

                assert(!busy);
                assert(page_generation == post_race_generation);
                assert(full_collected + got <= 300u);
                memcpy(&full_window[full_collected], page, got * sizeof(page[0]));
                full_collected += got;
                skip += got;
                assert(got > 0u || !page_more);
            }
            assert(full_collected == capacity);
            for (size_t i = 0u; i < survivors; ++i)
            {
                assert(full_window[i].value.f32 == (float)(RACE_EVICTIONS + i));
            }
            for (size_t i = 0u; i < RACE_EVICTIONS; ++i)
            {
                assert(full_window[survivors + i].value.f32 == RACE_SENTINEL_BASE + (float)i);
            }
            /* This is the concrete counterfactual: physical slot 32's
             * content (what the stale mapping would have handed back as
             * "position 32") is truly at position 248, not 32. */
            assert(full_window[survivors + 32u].value.f32 == RACE_SENTINEL_BASE + 32.0f);
        }

        /* The existing cross-call continuation protocol (model_history.c's
         * observed_generation != requested_generation check) is what turns
         * this stop into a client-visible, safe restart signal: a follow-up
         * call quoting the *original* generation (0) now observes the
         * post-race generation instead, which is exactly the mismatch that
         * check is designed to catch. */
        {
            uint32_t followup_generation = 0u;
            const size_t followup_returned = history_service_read_channel_samples(
                RACE_CHANNEL_ID, 0u, 0u, false, 300u, HISTORY_SERVICE_READ_CHUNK_SAMPLES, samples, 300u,
                &followup_generation, &more, &busy);

            assert(!busy);
            assert(followup_generation == post_race_generation);
            assert(followup_generation != 0u); /* differs from the first call's out_generation (0) */
            (void)followup_returned;
        }

        /* A generation change detected before any chunk has been copied at
         * all (the gap between this call's initial snapshot and its first
         * chunk's lock) must not fabricate a partial page: nothing is safe
         * to return yet, so the fix reports busy/retry instead of "more"
         * with zero samples (which callers such as model_history_read_raw
         * treat as an internal-error condition, not a retryable one). */
        {
            size_t busy_returned;
            uint32_t post_second_race_generation = 0u;

            g_prescan_race_armed = true;
            g_prescan_race_fired = false;
            busy_returned = history_service_read_channel_samples(RACE_CHANNEL_ID, 0u, 0u, false, 300u, 0u, samples,
                                                                  300u, &generation, &more, &busy);
            g_prescan_race_armed = false;
            assert(g_prescan_race_fired);
            assert(busy_returned == 0u);
            assert(busy);
            assert(!more);
            assert(generation == post_race_generation); /* still the value captured at this call's start */

            assert(history_service_get_channel_generation(RACE_CHANNEL_ID, &post_second_race_generation));
            assert(post_second_race_generation == post_race_generation + (uint32_t)RACE_EVICTIONS);
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

#if defined(PICO_ON_DEVICE)
    /* Append is outside the history mutex, but a ready backend I/O failure
     * still makes persistence explicitly degraded for subsequent reads. */
    g_flash_append_result = HISTORY_FLASH_BACKEND_IO;
    g_tick = 7000u;
    assert(history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                  &(history_service_value_t){.f32 = 99.0f},
                                                  SERVICE_QUALITY_GOOD, 1900000000000000ULL, 1u));
    assert(history_service_get_channel_restore_status("channel.float") ==
           HISTORY_SERVICE_RESTORE_STATUS_FLASH_APPEND_FAILED);
    g_flash_append_result = HISTORY_FLASH_BACKEND_OK;

    /* MEM-02 regression: a transient append failure (e.g. the shared Flash
     * coordinator momentarily busy with another channel) must not
     * permanently gate HistoryRead for this channel -- the very next
     * successful append clears the degraded status back to READY. */
    g_tick = 8000u;
    assert(history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                  &(history_service_value_t){.f32 = 100.0f},
                                                  SERVICE_QUALITY_GOOD, 1900000001000000ULL, 1u));
    assert(history_service_get_channel_restore_status("channel.float") ==
           HISTORY_SERVICE_RESTORE_STATUS_READY);

    /* MEM-02 regression: a channel history_service_bind_configured_channels_locked()
     * never saw at boot (e.g. because sensor_manager's activation verify-gate
     * had not yet reported it SERVICE_QUALITY_GOOD) must not be left with its
     * Flash slot permanently bound to "no channel" forever. The first lazy
     * PSRAM-ring claim for a never-before-seen channel_id must also bind and
     * restore its Flash slot. */
    {
        const unsigned bind_calls_before = g_bind_calls;

        g_flash_bind_result = HISTORY_FLASH_BACKEND_OK;
        g_flash_restore_result = HISTORY_FLASH_BACKEND_OK;
        g_tick = 9000u;
        assert(history_service_record_channel_sample("channel.lazy_bind", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                      &(history_service_value_t){.f32 = 21.0f},
                                                      SERVICE_QUALITY_GOOD, 1900000002000000ULL, 1u));
        assert(g_bind_calls == bind_calls_before + 1u);
        assert(!g_last_bind_channel_id_was_null);
        assert(history_service_get_channel_restore_status("channel.lazy_bind") ==
               HISTORY_SERVICE_RESTORE_STATUS_READY);
    }

    /* Same scenario, but the Flash slot's bind itself fails: the channel
     * must be reported degraded rather than silently READY. */
    {
        g_flash_bind_result = HISTORY_FLASH_BACKEND_IO;
        g_tick = 9001u;
        assert(history_service_record_channel_sample("channel.lazy_bind_fails", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                      &(history_service_value_t){.f32 = 22.0f},
                                                      SERVICE_QUALITY_GOOD, 1900000002001000ULL, 1u));
        assert(history_service_get_channel_restore_status("channel.lazy_bind_fails") ==
               HISTORY_SERVICE_RESTORE_STATUS_FLASH_BIND_FAILED);
        g_flash_bind_result = HISTORY_FLASH_BACKEND_OK;
    }

    /* Same scenario, but the restore (of nonexistent prior data, since this
     * slot was never bound before) fails: also degraded, not silently READY. */
    {
        g_flash_restore_result = HISTORY_FLASH_BACKEND_IO;
        g_tick = 9002u;
        assert(history_service_record_channel_sample("channel.lazy_restore_fails", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                      &(history_service_value_t){.f32 = 23.0f},
                                                      SERVICE_QUALITY_GOOD, 1900000002002000ULL, 1u));
        assert(history_service_get_channel_restore_status("channel.lazy_restore_fails") ==
               HISTORY_SERVICE_RESTORE_STATUS_FLASH_RESTORE_FAILED);
        g_flash_restore_result = HISTORY_FLASH_BACKEND_OK;
    }
#endif

    /* --- v0.5.0 T7: history_service_clear_channel() --- */
    {
        size_t got;

        /* Bad arguments are a safe no-op. */
        assert(!history_service_clear_channel(NULL));
        assert(!history_service_clear_channel(""));
        /* A channel id that was never claimed reports false. */
        assert(!history_service_clear_channel("channel.never.claimed"));

        /* "channel.float" has recorded samples from earlier in this test;
         * clearing it must drop both its PSRAM ring (reads back nothing,
         * generation lookup fails) and its restore-status tracking (reverts
         * to CHANNEL_NOT_BOUND, indistinguishable from never having been
         * claimed at all). */
        assert(history_service_clear_channel("channel.float"));
        got = history_service_read_channel_samples("channel.float", 0u, 0u, false, 10u, 0u, samples, 10u,
                                                    &generation, &more, &busy);
        assert(got == 0u);
        assert(!busy && !more);
        {
            uint32_t cleared_generation = 999u;

            assert(!history_service_get_channel_generation("channel.float", &cleared_generation));
        }
        assert(history_service_get_channel_restore_status("channel.float") ==
               HISTORY_SERVICE_RESTORE_STATUS_CHANNEL_NOT_BOUND);

        /* Clearing twice in a row is a safe, reported no-op the second
         * time. */
        assert(!history_service_clear_channel("channel.float"));

        /* The freed slot is reclaimed by a fresh record for the same id --
         * a cleared channel is not permanently unusable, mirroring
         * history_service_claim_ring_locked()'s "channel_id[0] == '\0'"
         * free-slot test. */
        g_tick = 8000u;
        assert(history_service_record_channel_sample("channel.float", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                                      &(history_service_value_t){.f32 = 7.0f},
                                                      SERVICE_QUALITY_GOOD, 2000000000000000ULL, 1u));
        got = history_service_read_channel_samples("channel.float", 0u, 0u, false, 10u, 0u, samples, 10u,
                                                    &generation, &more, &busy);
        assert(got == 1u);
        assert(samples[0].value.f32 == 7.0f);

        /* Lock-busy: clear must fail closed (no state mutated) when the
         * mutex cannot be acquired -- "channel.bool" (still claimed, never
         * cleared) must be left exactly as it was. */
        g_force_mutex_busy = true;
        assert(!history_service_clear_channel("channel.bool"));
        g_force_mutex_busy = false;
        assert(history_service_get_channel_restore_status("channel.bool") !=
               HISTORY_SERVICE_RESTORE_STATUS_CHANNEL_NOT_BOUND);

#if defined(PICO_ON_DEVICE)
        /* On device, clearing must also unbind the channel's Flash slot
         * (history_flash_backend_bind_channel(slot, NULL)) under the same
         * lock hold -- not just reset the PSRAM ring -- since bind_channel()
         * is a pure in-memory reset with no Flash I/O of its own. */
        {
            const unsigned bind_calls_before = g_bind_calls;

            assert(history_service_clear_channel("channel.bool"));
            assert(g_bind_calls == bind_calls_before + 1u);
            assert(g_last_bind_channel_id_was_null);
        }
#endif
    }

    return 0;
}

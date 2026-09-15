/**
 * @file
 * @brief Per-channel PSRAM history ring implementation, mirrored to
 *        `drivers/history_flash_backend.h`; see
 *        services/include/services/history_service.h for the public API
 *        contract.
 *
 * Ownership:
 * `g_store` (PSRAM-allocated array of per-channel rings) and, on device,
 * `g_flash_backend` are owned exclusively by this module. Off-device
 * (host tests), `g_store` points at the static `g_host_store` instead of a
 * PSRAM allocation.
 *
 * Concurrency:
 * `g_history_mutex` (a static FreeRTOS mutex, 10 ms timeout -
 * HISTORY_SERVICE_LOCK_TIMEOUT_MS) bounds every lock hold to O(1) (write/
 * claim/lookup) or to one bounded chunk of a single channel's ring (read,
 * HISTORY_SERVICE_READ_CHUNK_SAMPLES entries). The lock is never held
 * across a whole channel scan, let alone across more than one channel -
 * see history_service_read_channel_samples() for the generation-based
 * consistency check this enables. The Flash append in
 * history_service_record_channel_sample() happens strictly after the PSRAM
 * write and its lock are released, so a slow Flash program/erase can never
 * block other channels' PSRAM reads/writes. See
 * docs/code/state-ownership.md ("История") for the authoritative
 * description of this locking discipline.
 */
#include "services/history_service.h"

#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include "psram_allocator.h"
#include "services/channel_manager.h"

#if defined(PICO_ON_DEVICE)
#include "drivers/history_flash_backend.h"
#endif

/**
 * @brief One channel's fixed-capacity PSRAM sample ring plus its Flash
 *        persistence status.
 *
 * An empty `channel_id[0] == '\0'` marks an unclaimed slot. `generation` is
 * bumped whenever a write evicts the current oldest sample (i.e. the ring
 * is full and wraps); readers use it to detect a stale continuation point
 * or a mid-scan eviction (see history_service_read_channel_samples()).
 */
typedef struct
{
    char channel_id[HISTORY_SERVICE_CHANNEL_ID_MAX_LEN]; /* empty string => unclaimed slot */
    telemetry_channel_value_type_t value_type;
    size_t count;
    size_t write_index;
    uint32_t generation; /* bumped whenever a write evicts the current oldest sample */
    uint32_t last_recorded_ms;
    history_service_restore_status_t restore_status;
    history_service_channel_sample_t samples[HISTORY_SERVICE_CHANNEL_CAPACITY];
} history_service_channel_ring_t;

/** @brief Fixed array of HISTORY_SERVICE_MAX_CHANNELS independent channel rings; the service's entire PSRAM-resident state. */
typedef struct
{
    history_service_channel_ring_t channels[HISTORY_SERVICE_MAX_CHANNELS];
} history_service_store_t;

static history_service_store_t *g_store;
#if !defined(PICO_ON_DEVICE)
static history_service_store_t g_host_store;
#endif
static bool g_initialized = false;

/* Bounds every lock hold to O(1) (write/claim/lookup) or to one bounded
 * chunk of a single channel's ring (read). Never held across a whole
 * channel scan, and never across more than one channel. */
static StaticSemaphore_t g_history_mutex_storage;
static SemaphoreHandle_t g_history_mutex;
#define HISTORY_SERVICE_LOCK_TIMEOUT_MS 10u

#if defined(PICO_ON_DEVICE)
static history_flash_backend_t g_flash_backend;
static bool g_flash_ready = false;
#endif

/** @brief Acquires `g_history_mutex` with a 10 ms timeout. @return true if acquired; false on timeout or if the mutex was never created. */
static bool history_service_lock(void)
{
    return g_history_mutex != NULL &&
           xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(HISTORY_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE;
}

/** @brief Releases `g_history_mutex`. @note Must only be called after a successful history_service_lock(). */
static void history_service_unlock(void)
{
    (void)xSemaphoreGive(g_history_mutex);
}

/** @brief Checks that @p channel_id is non-NULL, non-empty, and NUL-terminated within HISTORY_SERVICE_CHANNEL_ID_MAX_LEN. */
static bool history_service_channel_id_is_bounded(const char *channel_id)
{
    size_t length = 0u;

    if (channel_id == NULL)
    {
        return false;
    }
    while (length < HISTORY_SERVICE_CHANNEL_ID_MAX_LEN && channel_id[length] != '\0')
    {
        ++length;
    }
    return length > 0u && length < HISTORY_SERVICE_CHANNEL_ID_MAX_LEN;
}

/** @brief NULL-safe string equality check for channel ids. */
static bool history_service_ids_equal(const char *lhs, const char *rhs)
{
    return lhs != NULL && rhs != NULL && strcmp(lhs, rhs) == 0;
}

/**
 * @brief Finds the ring slot bound to @p channel_id.
 * @return Slot index, or HISTORY_SERVICE_MAX_CHANNELS if no ring is bound to this id.
 * @note Caller must hold the lock. Bounded to HISTORY_SERVICE_MAX_CHANNELS.
 */
static size_t history_service_find_ring_locked(const char *channel_id)
{
    for (size_t index = 0u; index < HISTORY_SERVICE_MAX_CHANNELS; ++index)
    {
        if (g_store->channels[index].channel_id[0] != '\0' &&
            history_service_ids_equal(g_store->channels[index].channel_id, channel_id))
        {
            return index;
        }
    }
    return HISTORY_SERVICE_MAX_CHANNELS;
}

/**
 * @brief Claims the first free ring slot for @p channel_id/@p value_type,
 *        initializing it to an empty ring in READY restore status.
 * @return Newly claimed slot index, or HISTORY_SERVICE_MAX_CHANNELS if no
 *         free slot remains.
 * @note Caller must hold the lock. Bounded to HISTORY_SERVICE_MAX_CHANNELS.
 */
static size_t history_service_claim_ring_locked(const char *channel_id,
                                                 telemetry_channel_value_type_t value_type)
{
    for (size_t index = 0u; index < HISTORY_SERVICE_MAX_CHANNELS; ++index)
    {
        history_service_channel_ring_t *ring = &g_store->channels[index];

        if (ring->channel_id[0] != '\0')
        {
            continue;
        }
        memcpy(ring->channel_id, channel_id, strlen(channel_id) + 1u);
        ring->value_type = value_type;
        ring->count = 0u;
        ring->write_index = 0u;
        ring->generation = 0u;
        ring->last_recorded_ms = 0u;
        ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_READY;
        return index;
    }
    return HISTORY_SERVICE_MAX_CHANNELS;
}

/**
 * @brief Appends @p sample to @p ring's write position, advancing (wrapping)
 *        the write index and bumping `count`/`generation` as appropriate.
 *
 * Once `count` reaches HISTORY_SERVICE_CHANNEL_CAPACITY, every further push
 * overwrites the oldest retained sample and bumps `generation` - this is
 * the ring's wraparound point, and the sole place generation changes.
 * @note Caller must hold the lock. O(1).
 */
static void history_service_push_locked(history_service_channel_ring_t *ring,
                                        const history_service_channel_sample_t *sample)
{
    ring->samples[ring->write_index] = *sample;
    ring->write_index = (ring->write_index + 1u) % HISTORY_SERVICE_CHANNEL_CAPACITY;
    if (ring->count < HISTORY_SERVICE_CHANNEL_CAPACITY)
    {
        ring->count++;
    }
    else
    {
        ring->generation++;
    }
}

/** @brief Checks whether a channel snapshot is eligible for historization: `HISTORIZED` flag set, non-STRING value type, and a bounded id. */
static bool history_service_channel_is_historizable(const telemetry_channel_snapshot_t *snapshot)
{
    const telemetry_channel_t *channel = &snapshot->channel;

    return (channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) != 0u &&
           channel->metadata.value_type != TELEMETRY_CHANNEL_VALUE_STRING &&
           history_service_channel_id_is_bounded(channel->metadata.id);
}

#if defined(PICO_ON_DEVICE)
/** @brief Closure passed to history_flash_backend_restore()'s callback, identifying which PSRAM ring to restore into. */
typedef struct
{
    history_service_channel_ring_t *ring;
} history_service_restore_context_t;

/**
 * @brief Flash-backend restore callback: pushes one recovered Flash record
 *        into the corresponding PSRAM ring.
 *
 * Invoked by `history_flash_backend_restore()` (from history_service_init(),
 * on-device only) once per recovered record, in the backend's chosen order.
 * Defensively drops a record whose encoded value type does not match the
 * currently configured channel type, even though the record already passed
 * the backend's own CRC/validity check - a type mismatch means the channel
 * was reconfigured to a different type since the record was written.
 * `recorded_at_ms` is set to 0 because the local monotonic clock is not
 * reconstructible across reboot; debounce for restored channels effectively
 * keys off the next live write only.
 *
 * @param user_context A `history_service_restore_context_t*`; ignored (no-op) if NULL or its `ring` is NULL.
 * @param record Decoded Flash record; ignored (no-op) if NULL.
 * @note Calls history_service_push_locked() (normally lock-required)
 *       *without* holding `g_history_mutex`: history_service_init() runs
 *       the whole Flash bind/restore pass outside the mutex (backend I/O
 *       may be slow) and before setting `g_initialized = true`, so no
 *       concurrent reader/writer can observe the ring mid-restore - see
 *       the comment above the `#if defined(PICO_ON_DEVICE)` restore block
 *       in history_service_init().
 */
static void history_service_restore_sample(void *user_context, const history_flash_backend_record_t *record)
{
    history_service_restore_context_t *context = (history_service_restore_context_t *)user_context;
    history_service_channel_sample_t sample = {0};

    if (context == NULL || context->ring == NULL || record == NULL)
    {
        return;
    }
    if (record->value_type != context->ring->value_type)
    {
        /* Defensive: a hash-verified record whose encoded type does not
         * match the currently configured channel type is not trusted. */
        return;
    }
    memcpy(&sample.value, &record->value, sizeof(sample.value));
    sample.value_type = record->value_type;
    sample.quality = record->quality;
    sample.source_timestamp_unix_us = record->source_timestamp_unix_us;
    sample.recorded_at_ms = 0u; /* Not reconstructible across reboot; debounce keys off the next live write only. */
    history_service_push_locked(context->ring, &sample);
}
#endif

/**
 * @brief Claims a ring for every currently-configured, historization-eligible
 *        channel that does not already have one.
 *
 * Iterates the full channel registry (via `channel_manager_copy_snapshot_by_index()`)
 * up to HISTORY_SERVICE_MAX_CHANNELS, claiming a PSRAM ring for each
 * eligible channel (history_service_channel_is_historizable()) not already
 * bound. Called once from history_service_init(); does not itself touch Flash.
 * @note Caller must hold the lock.
 */
static void history_service_bind_configured_channels_locked(void)
{
    for (size_t index = 0u; index < HISTORY_SERVICE_MAX_CHANNELS; ++index)
    {
        telemetry_channel_snapshot_t snapshot = {0};

        if (!channel_manager_copy_snapshot_by_index(index, &snapshot) ||
            !history_service_channel_is_historizable(&snapshot))
        {
            continue;
        }
        if (history_service_find_ring_locked(snapshot.channel.metadata.id) < HISTORY_SERVICE_MAX_CHANNELS)
        {
            continue;
        }
        (void)history_service_claim_ring_locked(snapshot.channel.metadata.id, snapshot.channel.metadata.value_type);
    }
}

bool history_service_init(void)
{
    if (g_history_mutex == NULL)
    {
        g_history_mutex = xSemaphoreCreateMutexStatic(&g_history_mutex_storage);
        if (g_history_mutex == NULL)
        {
            return false;
        }
    }
    if (g_store == NULL)
    {
#if defined(PICO_ON_DEVICE)
        if (!psram_allocator_is_ready())
        {
            return false;
        }
        g_store = psram_calloc(1u, sizeof(*g_store));
        if (g_store == NULL)
        {
            return false;
        }
#else
        g_store = &g_host_store;
#endif
    }
    g_initialized = false;
    history_service_reset();

    if (!history_service_lock())
    {
        return false;
    }
    history_service_bind_configured_channels_locked();
    history_service_unlock();

#if defined(PICO_ON_DEVICE)
    history_flash_backend_init_flash(&g_flash_backend);
    g_flash_ready = g_flash_backend.ready;
    if (!g_flash_ready)
    {
        for (size_t index = 0u; index < HISTORY_SERVICE_MAX_CHANNELS; ++index)
        {
            history_service_channel_ring_t *ring = &g_store->channels[index];

            if (ring->channel_id[0] != '\0')
            {
                ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_FLASH_UNAVAILABLE;
            }
        }
    }
    else
    {
        /* One temporary PSRAM scratch buffer is reused sequentially across
         * every channel restore, avoiding a permanent internal-SRAM cost. */
        history_flash_backend_scan_entry_t *restore_scratch =
            (history_flash_backend_scan_entry_t *)psram_malloc(HISTORY_SERVICE_CHANNEL_CAPACITY *
                                                                sizeof(*restore_scratch));

        for (size_t index = 0u; index < HISTORY_SERVICE_MAX_CHANNELS; ++index)
        {
            history_service_channel_ring_t *ring = &g_store->channels[index];

            if (ring->channel_id[0] == '\0')
            {
                /* Bind every physical slot once. A dynamic claim that cannot
                 * later persist will transition to an explicit degraded state. */
                (void)history_flash_backend_bind_channel(&g_flash_backend, (uint8_t)index, NULL);
                continue;
            }

            if (history_flash_backend_bind_channel(&g_flash_backend, (uint8_t)index, ring->channel_id) !=
                HISTORY_FLASH_BACKEND_OK)
            {
                ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_FLASH_BIND_FAILED;
                continue;
            }
            if (restore_scratch == NULL)
            {
                /* Do not expose a partial RAM-only window as restored
                 * persistent history. Recording remains available, while the
                 * adapter receives a deterministic resource failure. */
                ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_SCRATCH_ALLOCATION_FAILED;
                continue;
            }
            {
                history_service_restore_context_t restore_context = {.ring = ring};
                const history_flash_backend_result_t restore_result =
                    history_flash_backend_restore(&g_flash_backend, (uint8_t)index, restore_scratch,
                                                  HISTORY_SERVICE_CHANNEL_CAPACITY,
                                                  history_service_restore_sample, &restore_context);

                if (restore_result != HISTORY_FLASH_BACKEND_OK)
                {
                    ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_FLASH_RESTORE_FAILED;
                }
            }
        }
        if (restore_scratch != NULL)
        {
            psram_free(restore_scratch);
        }
    }
#endif

    /* Flash bind/scan occurs before HistoryRead is exposed, but not while
     * holding the history mutex: backend I/O may be slow. */
    g_initialized = true;
    return true;
}

void history_service_reset(void)
{
    if (g_store == NULL)
    {
        return;
    }
    if (!history_service_lock())
    {
        return;
    }
    memset(g_store, 0, sizeof(*g_store));
    history_service_unlock();
}

history_service_restore_status_t history_service_get_channel_restore_status(const char *channel_id)
{
    size_t ring_index;
    history_service_restore_status_t status = HISTORY_SERVICE_RESTORE_STATUS_CHANNEL_NOT_BOUND;

    if (!g_initialized)
    {
        return HISTORY_SERVICE_RESTORE_STATUS_NOT_INITIALIZED;
    }
    if (!history_service_channel_id_is_bounded(channel_id))
    {
        return HISTORY_SERVICE_RESTORE_STATUS_CHANNEL_NOT_BOUND;
    }
    if (!history_service_lock())
    {
        return HISTORY_SERVICE_RESTORE_STATUS_LOCK_BUSY;
    }
    ring_index = history_service_find_ring_locked(channel_id);
    if (ring_index != HISTORY_SERVICE_MAX_CHANNELS)
    {
        status = g_store->channels[ring_index].restore_status;
    }
    history_service_unlock();
    return status;
}

bool history_service_record_channel_sample(const char *channel_id,
                                            telemetry_channel_value_type_t value_type,
                                            const history_service_value_t *value,
                                            service_quality_t quality,
                                            uint64_t source_timestamp_unix_us,
                                            uint32_t record_interval_ms)
{
    const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool recorded = false;
    size_t ring_index = HISTORY_SERVICE_MAX_CHANNELS;
#if defined(PICO_ON_DEVICE)
    bool freshly_claimed = false;
    history_flash_backend_record_t flash_record;
#endif

    if (!g_initialized || !history_service_channel_id_is_bounded(channel_id) || value == NULL ||
        record_interval_ms == 0u || quality != SERVICE_QUALITY_GOOD ||
        value_type == TELEMETRY_CHANNEL_VALUE_STRING ||
        source_timestamp_unix_us == TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID)
    {
        return false;
    }

    if (!history_service_lock())
    {
        return false;
    }

    ring_index = history_service_find_ring_locked(channel_id);
    if (ring_index == HISTORY_SERVICE_MAX_CHANNELS)
    {
        ring_index = history_service_claim_ring_locked(channel_id, value_type);
#if defined(PICO_ON_DEVICE)
        freshly_claimed = (ring_index != HISTORY_SERVICE_MAX_CHANNELS);
#endif
    }
#if defined(PICO_ON_DEVICE)
    if (freshly_claimed && g_flash_ready)
    {
        /* This channel became historizable only after history_service_init()'s
         * one-time boot Flash-bind/restore pass already ran -- e.g. its
         * driver's first reading was not yet SERVICE_QUALITY_GOOD when
         * sensor_manager's activation verify-gate first ran, so
         * channel_manager did not yet report it as eligible at that exact
         * moment (MEM-02). Without this, the channel's Flash slot is left
         * permanently bound to "no channel" from that one-time boot pass,
         * and every future append fails NOT_BOUND forever, gating
         * HistoryRead shut for the rest of the device's uptime even though
         * the PSRAM ring itself is otherwise fine.
         *
         * Bind and restore now, exactly as the boot pass would have. Held
         * under the same lock as the boot pass's equivalent scan would
         * have implicitly had exclusivity for (nothing else has ever seen
         * this ring_index yet, since it was just claimed above) -- kept
         * inside the lock here specifically because, unlike at boot,
         * other tasks (e.g. OpcUaTask serving a concurrent HistoryRead)
         * are already running and could otherwise race a lock-free replay
         * into this same ring. This is safe unconditionally: reaching this
         * branch means no ring existed for this channel_id moments ago, so
         * the boot pass's fallback loop is guaranteed to have already
         * bound this exact slot to "no channel" and never restored or
         * wrote to it -- there is no armed write cursor or prior data here
         * to clobber. */
        history_service_channel_ring_t *ring = &g_store->channels[ring_index];

        if (history_flash_backend_bind_channel(&g_flash_backend, (uint8_t)ring_index, channel_id) !=
            HISTORY_FLASH_BACKEND_OK)
        {
            ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_FLASH_BIND_FAILED;
        }
        else
        {
            history_flash_backend_scan_entry_t *restore_scratch =
                (history_flash_backend_scan_entry_t *)psram_malloc(HISTORY_SERVICE_CHANNEL_CAPACITY *
                                                                    sizeof(*restore_scratch));

            if (restore_scratch == NULL)
            {
                ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_SCRATCH_ALLOCATION_FAILED;
            }
            else
            {
                history_service_restore_context_t restore_context = {.ring = ring};
                const history_flash_backend_result_t restore_result = history_flash_backend_restore(
                    &g_flash_backend, (uint8_t)ring_index, restore_scratch, HISTORY_SERVICE_CHANNEL_CAPACITY,
                    history_service_restore_sample, &restore_context);

                if (restore_result != HISTORY_FLASH_BACKEND_OK)
                {
                    ring->restore_status = HISTORY_SERVICE_RESTORE_STATUS_FLASH_RESTORE_FAILED;
                }
                psram_free(restore_scratch);
            }
        }
    }
#endif
    if (ring_index != HISTORY_SERVICE_MAX_CHANNELS)
    {
        history_service_channel_ring_t *ring = &g_store->channels[ring_index];

        if (ring->value_type == value_type &&
            (ring->count == 0u || ring->last_recorded_ms == 0u ||
             (uint32_t)(now_ms - ring->last_recorded_ms) >= record_interval_ms))
        {
            history_service_channel_sample_t sample;

            sample.value = *value;
            sample.value_type = value_type;
            sample.quality = quality;
            sample.source_timestamp_unix_us = source_timestamp_unix_us;
            sample.recorded_at_ms = now_ms;
            history_service_push_locked(ring, &sample);
            ring->last_recorded_ms = now_ms;
            recorded = true;
        }
    }
    history_service_unlock();

#if defined(PICO_ON_DEVICE)
    if (recorded && g_flash_ready)
    {
        memcpy(&flash_record.value, value, sizeof(flash_record.value));
        flash_record.value_type = value_type;
        flash_record.quality = quality;
        flash_record.source_timestamp_unix_us = source_timestamp_unix_us;
        /* Best-effort persistence performed outside the PSRAM lock so a slow
         * Flash program/erase never blocks other channels' reads/writes.
         * PSRAM already holds the sample regardless of this call's result. */
        const history_flash_backend_result_t append_result =
            history_flash_backend_append(&g_flash_backend, (uint8_t)ring_index, &flash_record);

        if (append_result != HISTORY_FLASH_BACKEND_OK)
        {
            /* A failed append means that persistence is no longer known
             * complete; do not hide it behind the PSRAM cache. */
            if (history_service_lock())
            {
                if (ring_index < HISTORY_SERVICE_MAX_CHANNELS)
                {
                    g_store->channels[ring_index].restore_status =
                        HISTORY_SERVICE_RESTORE_STATUS_FLASH_APPEND_FAILED;
                }
                history_service_unlock();
            }
        }
        else if (history_service_lock())
        {
            /* A Flash append failure can be transient (the shared Flash
             * coordinator momentarily busy with another channel/subsystem,
             * not a hardware fault). Without this, one such hiccup would
             * permanently gate this channel out of HistoryRead for the rest
             * of the device's uptime even though every following sample
             * persists fine -- MEM-02. Clearing back to READY only on a
             * ring this call itself found degraded, and only via a real
             * subsequent success, keeps the fail-closed HistoryRead contract
             * (services/include/services/history_service.h) intact: it still
             * fails closed for as long as persistence is actually degraded. */
            if (ring_index < HISTORY_SERVICE_MAX_CHANNELS &&
                g_store->channels[ring_index].restore_status == HISTORY_SERVICE_RESTORE_STATUS_FLASH_APPEND_FAILED)
            {
                g_store->channels[ring_index].restore_status = HISTORY_SERVICE_RESTORE_STATUS_READY;
            }
            history_service_unlock();
        }
    }
#endif

    return recorded;
}

bool history_service_get_channel_generation(const char *channel_id, uint32_t *out_generation)
{
    size_t ring_index;
    bool found = false;

    if (!g_initialized || channel_id == NULL)
    {
        return false;
    }
    if (!history_service_lock())
    {
        return false;
    }
    ring_index = history_service_find_ring_locked(channel_id);
    if (ring_index != HISTORY_SERVICE_MAX_CHANNELS)
    {
        found = true;
        if (out_generation != NULL)
        {
            *out_generation = g_store->channels[ring_index].generation;
        }
    }
    history_service_unlock();
    return found;
}

/**
 * @brief Checks whether a sample's source timestamp (converted to Unix
 *        seconds) falls within [@p start_epoch_s, @p end_epoch_s].
 *
 * Both bounds 0 means unbounded (always matches). A single zero bound means
 * "open" on that side only. If both are nonzero but out of order, the
 * smaller/larger are used as the effective lower/upper bound (order-
 * tolerant), so a caller-swapped range still produces a sane inclusive
 * window rather than always failing to match.
 */
static bool history_service_sample_in_range(uint64_t sample_timestamp_unix_us,
                                            uint32_t start_epoch_s,
                                            uint32_t end_epoch_s)
{
    const uint64_t sample_epoch_s = sample_timestamp_unix_us / 1000000u;
    uint32_t lower_bound = 0u;
    uint32_t upper_bound = 0u;

    if (start_epoch_s == 0u && end_epoch_s == 0u)
    {
        return true;
    }
    if (start_epoch_s == 0u)
    {
        return sample_epoch_s <= end_epoch_s;
    }
    if (end_epoch_s == 0u)
    {
        return sample_epoch_s >= start_epoch_s;
    }
    lower_bound = start_epoch_s < end_epoch_s ? start_epoch_s : end_epoch_s;
    upper_bound = start_epoch_s > end_epoch_s ? start_epoch_s : end_epoch_s;
    return sample_epoch_s >= lower_bound && sample_epoch_s <= upper_bound;
}

size_t history_service_read_channel_samples(const char *channel_id,
                                            uint32_t start_epoch_s,
                                            uint32_t end_epoch_s,
                                            bool descending,
                                            size_t max_samples,
                                            size_t skip_matches,
                                            history_service_channel_sample_t *out_samples,
                                            size_t out_capacity,
                                            uint32_t *out_generation,
                                            bool *out_more,
                                            bool *out_busy)
{
    size_t ring_index;
    size_t ring_count = 0u;
    size_t oldest_index = 0u;
    size_t requested_limit;
    size_t collected = 0u;
    size_t matched = 0u;
    size_t scanned = 0u;
    uint32_t captured_generation = 0u;
    bool more = false;
    bool first_chunk = true;

    if (out_generation != NULL)
    {
        *out_generation = 0u;
    }
    if (out_more != NULL)
    {
        *out_more = false;
    }
    if (out_busy != NULL)
    {
        *out_busy = false;
    }

    if (!g_initialized || channel_id == NULL || out_samples == NULL || out_capacity == 0u)
    {
        return 0u;
    }

    requested_limit = (max_samples == 0u || max_samples > out_capacity) ? out_capacity : max_samples;
    if (requested_limit > HISTORY_SERVICE_MAX_RETURN_COUNT)
    {
        requested_limit = HISTORY_SERVICE_MAX_RETURN_COUNT;
    }

    if (!history_service_lock())
    {
        if (out_busy != NULL)
        {
            *out_busy = true;
        }
        return 0u;
    }
    ring_index = history_service_find_ring_locked(channel_id);
    if (ring_index != HISTORY_SERVICE_MAX_CHANNELS)
    {
        const history_service_channel_ring_t *ring = &g_store->channels[ring_index];

        ring_count = ring->count;
        oldest_index = (ring->count >= HISTORY_SERVICE_CHANNEL_CAPACITY) ? ring->write_index : 0u;
        captured_generation = ring->generation;
        if (out_generation != NULL)
        {
            *out_generation = captured_generation;
        }
    }
    history_service_unlock();

    if (ring_index == HISTORY_SERVICE_MAX_CHANNELS)
    {
        return 0u;
    }

#if defined(HISTORY_SERVICE_TEST_HOOKS)
    /* Host-only deterministic interleaving hook. Runs outside the lock,
     * modelling an independent writer task racing between this call's
     * initial ring_count/oldest_index/generation snapshot and its first
     * chunk's lock/generation re-check below -- the gap before any chunk
     * has been copied at all. */
    extern void history_service_test_before_scan(const char *channel_id);
    history_service_test_before_scan(channel_id);
#endif

    while (scanned < ring_count)
    {
        history_service_channel_sample_t chunk[HISTORY_SERVICE_READ_CHUNK_SAMPLES];
        const size_t chunk_size = (ring_count - scanned) < HISTORY_SERVICE_READ_CHUNK_SAMPLES
                                      ? (ring_count - scanned)
                                      : HISTORY_SERVICE_READ_CHUNK_SAMPLES;

        if (!history_service_lock())
        {
            if (first_chunk && collected == 0u)
            {
                if (out_busy != NULL)
                {
                    *out_busy = true;
                }
                return 0u;
            }
            /* Data already collected is still valid; conservatively report
             * that more may remain rather than silently truncating. */
            more = true;
            break;
        }
        first_chunk = false;

        /* oldest_index/ring_count were captured outside this lock hold (and,
         * for chunk 2+, outside every earlier chunk's lock hold too). The
         * writer task (history_service_record_channel_sample, driven from
         * sensor_manager.c's sampling path) can run in the gap between lock
         * holds and evict/overwrite ring slots via history_service_push_locked,
         * which bumps generation on every eviction. If that happened, the
         * oldest_index/ring_count mapping no longer describes the physical
         * ring: sample_index below could now name a slot the writer has
         * already overwritten with a newer, unrelated sample. Re-checking
         * generation here, still under the lock that guards the copy below,
         * catches that before trusting the mapping -- this is what stops a
         * single multi-chunk call from silently returning corrupted,
         * duplicated, or out-of-order samples. */
        if (g_store->channels[ring_index].generation != captured_generation)
        {
            history_service_unlock();
            if (collected == 0u)
            {
                /* Nothing from this call can be trusted yet; ask the caller
                 * to retry the whole request rather than hand back samples
                 * read through a mapping already known to be stale. */
                if (out_busy != NULL)
                {
                    *out_busy = true;
                }
                return 0u;
            }
            /* Samples already collected were each copied atomically while
             * the ring's generation still matched this call's snapshot, so
             * they remain valid; only continuing the scan with the now-stale
             * mapping is unsafe. out_generation still reports the originally
             * captured value, so a follow-up call built on this page's
             * continuation point will observe a different generation and be
             * rejected by the existing cross-call check (see
             * history_service.h and model_history_read_raw's
             * observed_generation != requested_generation check) instead of
             * silently paging over the change. */
            more = true;
            break;
        }

        for (size_t offset = 0u; offset < chunk_size; ++offset)
        {
            const size_t position = scanned + offset;
            size_t sample_index;

            if (descending)
            {
                sample_index = (oldest_index + ring_count - 1u - position) % HISTORY_SERVICE_CHANNEL_CAPACITY;
            }
            else
            {
                sample_index = (oldest_index + position) % HISTORY_SERVICE_CHANNEL_CAPACITY;
            }
            chunk[offset] = g_store->channels[ring_index].samples[sample_index];
        }
        history_service_unlock();

#if defined(HISTORY_SERVICE_TEST_HOOKS)
        /* Host-only deterministic interleaving hook. Runs outside the lock,
         * modelling an independent writer task racing between this chunk's
         * copy and the next chunk's lock/generation re-check above. */
        extern void history_service_test_after_chunk(const char *channel_id, size_t chunk_start_scanned);
        history_service_test_after_chunk(channel_id, scanned);
#endif

        for (size_t offset = 0u; offset < chunk_size; ++offset)
        {
            if (!history_service_sample_in_range(chunk[offset].source_timestamp_unix_us, start_epoch_s, end_epoch_s))
            {
                continue;
            }
            if (matched < skip_matches)
            {
                ++matched;
                continue;
            }
            ++matched;
            if (collected < requested_limit)
            {
                out_samples[collected++] = chunk[offset];
            }
            else
            {
                more = true;
                break;
            }
        }
        scanned += chunk_size;
        if (more)
        {
            break;
        }
    }

    if (out_more != NULL)
    {
        *out_more = more;
    }
    return collected;
}

bool history_service_clear_channel(const char *channel_id)
{
    size_t ring_index;
    bool found = false;

    if (!g_initialized || !history_service_channel_id_is_bounded(channel_id))
    {
        return false;
    }
    if (!history_service_lock())
    {
        return false;
    }

    ring_index = history_service_find_ring_locked(channel_id);
    if (ring_index != HISTORY_SERVICE_MAX_CHANNELS)
    {
        /* Zeroing channel_id[0] to '\0' is what history_service_claim_ring_locked()
         * treats as "unclaimed" -- this makes the slot reclaimable exactly
         * like a never-used one. */
        memset(&g_store->channels[ring_index], 0, sizeof(g_store->channels[ring_index]));
#if defined(PICO_ON_DEVICE)
        /* bind_channel(slot, NULL) is a pure in-memory reset (no Flash I/O,
         * see history_flash_backend_bind_channel()) -- cheap enough to do
         * while still holding the lock, unlike history_flash_backend_append()
         * which is deliberately called outside it. */
        if (g_flash_ready)
        {
            (void)history_flash_backend_bind_channel(&g_flash_backend, (uint8_t)ring_index, NULL);
        }
#endif
        found = true;
    }

    history_service_unlock();
    return found;
}

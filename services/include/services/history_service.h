/**
 * @file
 * @brief Per-channel historical-data service: a PSRAM ring per channel,
 *        mirrored to a Flash-backed ring via
 *        `drivers/history_flash_backend.h`.
 *
 * This is the Flash-backed history ring subsystem referenced by
 * docs/code/persistence.md ("Кольцевой буфер истории по каналу"): the
 * Flash mirror is explicitly **non-transactional** - a locked, documented
 * project decision, not a gap to fix. A torn/unreadable sample is skipped
 * and never surfaced; the most recent unwritten samples are lost on power
 * loss between recording and the Flash append completing. See
 * docs/channel_model.md#history-v043 and docs/known_limits.md for the
 * user-facing description of this tradeoff, and
 * drivers/include/drivers/history_flash_backend.h for the backend this
 * service drives.
 */
#ifndef SERVICES_HISTORY_SERVICE_H
#define SERVICES_HISTORY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/platform_limits.h"
#include "services/service_types.h"
#include "services/telemetry_channel.h"

/*
 * v0.4.3 per-channel history record store.
 *
 * Storage is one typed, owned ring per channel (keyed by the channel's
 * stable id, never by registry index or memory address), not a single
 * shared float ring.  Every value type accepted by the channel runtime
 * except STRING is supported; string channels are never historized (see
 * TELEMETRY_CHANNEL_FLAG_HISTORIZED handling in sensor_manager.c and the
 * rejection in history_service_record_channel_sample()).
 *
 * Capacity is shared with the Flash-backed ring
 * (drivers/history_flash_backend) via PLATFORM_HISTORY_SAMPLES_PER_CHANNEL
 * so both backends agree on the same fixed retention window per channel.
 */
#define HISTORY_SERVICE_MAX_CHANNELS PLATFORM_CHANNEL_LIMIT
#define HISTORY_SERVICE_CHANNEL_CAPACITY PLATFORM_HISTORY_SAMPLES_PER_CHANNEL
#define HISTORY_SERVICE_CHANNEL_ID_MAX_LEN TELEMETRY_CHANNEL_ID_MAX_LEN

/*
 * OPC UA HistoryRead bounds, frozen for v0.4.3 (see docs/releases/v0.4.3.md
 * T1/T4).  A single HistoryRead response page never returns more than
 * HISTORY_SERVICE_MAX_RETURN_COUNT samples for one node, regardless of the
 * client-requested numValuesPerNode; a bounded continuation point lets the
 * client page through the rest of a channel's retained history.  Because
 * HISTORY_SERVICE_MAX_RETURN_COUNT * 2 >= HISTORY_SERVICE_CHANNEL_CAPACITY,
 * two pages are always sufficient to exhaust one channel's retained window.
 */
#define HISTORY_SERVICE_MAX_RETURN_COUNT 128u

/* Bounded per-chunk critical section size used internally while scanning a
 * channel's ring; a read never holds one lock across the whole per-channel
 * scan, let alone across a multi-channel scan. */
#define HISTORY_SERVICE_READ_CHUNK_SAMPLES 32u

/**
 * @brief Compact retained-value union for one history sample.
 *
 * A compact value union deliberately excludes the STRING variant: string
 * channels are never historized, so this keeps each retained sample small
 * regardless of TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN. The active member
 * is selected by the enclosing sample's `value_type`
 * (telemetry_channel_value_type_t).
 */
typedef union
{
    float f32;
    double f64;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    bool boolean;
} history_service_value_t;

/**
 * @brief One retained history sample: value, type, quality, and both a
 *        source (UTC) and a local monotonic recording timestamp.
 *
 * This is the record shape used both in the PSRAM ring and (via
 * `drivers/history_flash_backend.h`) mirrored to Flash; only
 * SERVICE_QUALITY_GOOD samples with a valid `source_timestamp_unix_us` are
 * ever recorded (see history_service_record_channel_sample()).
 */
typedef struct
{
    history_service_value_t value;
    telemetry_channel_value_type_t value_type;
    service_quality_t quality;
    uint64_t source_timestamp_unix_us;
    uint32_t recorded_at_ms;
} history_service_channel_sample_t;

/**
 * @brief Persistence readiness for one historizing channel's Flash-backed ring.
 *
 * HistoryRead adapters must fail closed rather than present an incomplete
 * RAM-only window when this is anything other than READY - i.e. a
 * non-READY status must be surfaced as a HistoryRead failure, not silently
 * degrade to PSRAM-only results.
 */
typedef enum
{
    HISTORY_SERVICE_RESTORE_STATUS_READY = 0,
    HISTORY_SERVICE_RESTORE_STATUS_NOT_INITIALIZED,
    HISTORY_SERVICE_RESTORE_STATUS_CHANNEL_NOT_BOUND,
    HISTORY_SERVICE_RESTORE_STATUS_FLASH_UNAVAILABLE,
    HISTORY_SERVICE_RESTORE_STATUS_SCRATCH_ALLOCATION_FAILED,
    HISTORY_SERVICE_RESTORE_STATUS_FLASH_BIND_FAILED,
    HISTORY_SERVICE_RESTORE_STATUS_FLASH_RESTORE_FAILED,
    HISTORY_SERVICE_RESTORE_STATUS_FLASH_APPEND_FAILED,
    HISTORY_SERVICE_RESTORE_STATUS_LOCK_BUSY
} history_service_restore_status_t;

/**
 * @brief Allocate the per-channel PSRAM rings and bind/restore the Flash
 *        backend.
 *
 * Allocates HISTORY_SERVICE_MAX_CHANNELS independent rings from PSRAM,
 * creates the internal FreeRTOS mutex, and (on-device) binds and restores
 * each channel's Flash-backed mirror before returning. Call once during
 * platform init, before any HistoryRead surface is exposed. Failure leaves
 * the service uninitialized; it never falls back to internal SRAM.
 *
 * @return true on success.
 */
bool history_service_init(void);

/**
 * @brief Clear every channel ring's in-memory state (not the Flash mirror).
 *
 * Intended for host-test reset between cases; not used in normal firmware
 * operation.
 */
void history_service_reset(void);

/**
 * @brief Returns the bounded, service-owned persistence state for @p channel_id.
 *
 * A configured historizing channel is READY only after its Flash ring was
 * bound and restored successfully during initialization.
 *
 * @param channel_id Stable channel id string.
 * @return HISTORY_SERVICE_RESTORE_STATUS_READY if the channel's Flash ring
 *         is bound/restored and fully usable; a more specific status
 *         otherwise, including HISTORY_SERVICE_RESTORE_STATUS_CHANNEL_NOT_BOUND
 *         if no ring has ever been claimed for this id.
 */
history_service_restore_status_t history_service_get_channel_restore_status(const char *channel_id);

/**
 * @brief Records one sample for @p channel_id, applying a caller-supplied debounce interval.
 *
 * Only SERVICE_QUALITY_GOOD samples with a valid source timestamp and a
 * non-STRING @p value_type are recorded; PSRAM and (when available) the
 * Flash-backed ring are both updated. The Flash append happens *after* the
 * PSRAM write completes and outside the PSRAM lock (see
 * docs/code/persistence.md); a Flash append failure sets the channel's
 * restore status rather than failing silently, so HistoryRead can fail
 * closed for that channel.
 *
 * @param channel_id Stable channel id string; claims a ring for this id on
 *        first use if one is not already bound.
 * @param value_type Value type of @p value; STRING is always rejected.
 * @param value Sample value; interpreted per @p value_type.
 * @param quality Sample quality; only SERVICE_QUALITY_GOOD is recorded.
 * @param source_timestamp_unix_us UTC microsecond source timestamp;
 *        TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID is rejected.
 * @param record_interval_ms Minimum spacing between recorded samples for
 *        this channel (debounce); 0 disables debouncing.
 * @return true if the sample was recorded (to at least PSRAM); false if
 *         rejected by quality/timestamp/type/debounce checks or if no ring
 *         could be claimed.
 * @note May perform a Flash write (via the Flash-backed ring) as part of
 *       recording.
 */
bool history_service_record_channel_sample(const char *channel_id,
                                            telemetry_channel_value_type_t value_type,
                                            const history_service_value_t *value,
                                            service_quality_t quality,
                                            uint64_t source_timestamp_unix_us,
                                            uint32_t record_interval_ms);

/**
 * @brief Returns @p channel_id's current ring generation counter.
 *
 * Bumped whenever the ring evicts a previously retained sample (i.e.
 * whenever the logical "oldest" boundary moves). Used to validate a
 * continuation point issued for a still-open scan; a generation mismatch
 * means the retained window changed since the point was issued and the
 * caller must restart the query.
 *
 * @param channel_id Stable channel id string.
 * @param out_generation Receives the current generation on success.
 * @return true if the channel has a claimed ring and @p out_generation was
 *         written; false if the channel has no claimed ring (no history
 *         ever recorded/restored for it).
 */
bool history_service_get_channel_generation(const char *channel_id, uint32_t *out_generation);

/**
 * @brief Scans one channel's ring for samples in a time range, in bounded
 *        lock-chunked pages, with continuation-point support.
 *
 * Scans @p channel_id's ring for samples within [@p start_epoch_s,
 * @p end_epoch_s] (0/0 means unbounded), in ascending or descending
 * source-timestamp order, skipping the first @p skip_matches
 * already-delivered matches (continuation support) and collecting up to
 * `min(max_samples, out_capacity)` further matches into @p out_samples.
 * The scan is performed in bounded chunks of at most
 * HISTORY_SERVICE_READ_CHUNK_SAMPLES ring entries per lock hold; no lock is
 * ever held across a channel's full scan, let alone across more than one
 * channel.
 *
 * @p out_generation always receives the ring generation observed at the
 * start of this call (see history_service_get_channel_generation()).
 * @p out_more is set true when additional matches remain beyond what was
 * collected (the caller should issue a follow-up call with a larger
 * @p skip_matches, quoting the same @p out_generation as a validity check).
 * @p out_busy is set true only when a lock could not be acquired at all and
 * zero samples were returned.
 *
 * Because the scan releases and reacquires its lock between chunks, a
 * concurrent write (history_service_record_channel_sample()) can evict/
 * overwrite ring slots mid-scan, not just between separate calls sharing a
 * continuation point. Every eviction bumps the ring's generation, so each
 * chunk re-validates the generation captured at the start of this call,
 * still under the lock that guards that chunk's copy, before trusting the
 * oldest-index mapping. If the generation changed: with nothing collected
 * yet, this call returns zero samples with @p out_busy set (the caller
 * should retry the whole request); with some samples already collected,
 * this call stops early with @p out_more set and @p out_generation still
 * reporting the value captured at this call's start, so a follow-up call
 * quoting it back observes the new generation and is rejected as a stale
 * continuation point rather than silently paging over the change. Samples
 * already placed in @p out_samples before such a stop are unaffected -
 * each chunk is copied atomically while its generation check still
 * matches.
 *
 * @param channel_id Stable channel id string.
 * @param start_epoch_s Inclusive lower bound (Unix seconds), or 0 with
 *        @p end_epoch_s 0 for unbounded.
 * @param end_epoch_s Inclusive upper bound (Unix seconds), or 0 with
 *        @p start_epoch_s 0 for unbounded.
 * @param descending If true, scan/return newest-first; otherwise oldest-first.
 * @param max_samples Maximum number of matches to collect this call.
 * @param skip_matches Number of already-delivered matches to skip (continuation offset).
 * @param out_samples Destination array for collected samples.
 * @param out_capacity Capacity of @p out_samples.
 * @param out_generation Receives the ring generation observed at scan start; must be non-NULL.
 * @param out_more Receives true if further matches remain; must be non-NULL.
 * @param out_busy Receives true only if no lock could be acquired and zero samples were returned; must be non-NULL.
 * @return Number of samples written to @p out_samples (0 on failure/busy/no match).
 */
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
                                            bool *out_busy);

#endif

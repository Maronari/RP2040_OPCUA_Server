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

/* A compact value union deliberately excludes the STRING variant: string
 * channels are never historized, so this keeps each retained sample small
 * regardless of TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN. */
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

typedef struct
{
    history_service_value_t value;
    telemetry_channel_value_type_t value_type;
    service_quality_t quality;
    uint64_t source_timestamp_unix_us;
    uint32_t recorded_at_ms;
} history_service_channel_sample_t;

/* Persistence readiness for one historizing channel. HistoryRead adapters
 * must fail closed rather than present an incomplete RAM-only window when
 * this is anything other than READY. */
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

bool history_service_init(void);
void history_service_reset(void);

/* Returns the bounded, service-owned persistence state for channel_id. A
 * configured historizing channel is READY only after its Flash ring was
 * bound and restored successfully during initialization. */
history_service_restore_status_t history_service_get_channel_restore_status(const char *channel_id);

/* Records one sample for channel_id, applying the caller-supplied
 * record_interval_ms debounce.  Only SERVICE_QUALITY_GOOD samples with a
 * valid source timestamp and a non-STRING value_type are recorded; PSRAM and
 * (when available) the Flash-backed ring are both updated. */
bool history_service_record_channel_sample(const char *channel_id,
                                            telemetry_channel_value_type_t value_type,
                                            const history_service_value_t *value,
                                            service_quality_t quality,
                                            uint64_t source_timestamp_unix_us,
                                            uint32_t record_interval_ms);

/* Returns the channel's current ring generation, bumped whenever the ring
 * evicts a previously retained sample (i.e. whenever the logical "oldest"
 * boundary moves).  Used to validate a continuation point issued for a
 * still-open scan; a generation mismatch means the retained window changed
 * since the point was issued and the caller must restart the query.
 * Returns false if the channel has no claimed ring (no history ever
 * recorded/restored for it). */
bool history_service_get_channel_generation(const char *channel_id, uint32_t *out_generation);

/*
 * Scans channel_id's ring for samples within [start_epoch_s, end_epoch_s]
 * (0/0 means unbounded), in ascending or descending source-timestamp order,
 * skipping the first skip_matches already-delivered matches (continuation
 * support) and collecting up to min(max_samples, out_capacity) further
 * matches into out_samples.  The scan is performed in bounded chunks of at
 * most HISTORY_SERVICE_READ_CHUNK_SAMPLES ring entries per lock hold; no
 * lock is ever held across a channel's full scan, let alone across more
 * than one channel.
 *
 * out_generation always receives the ring generation observed at the start
 * of this call (see history_service_get_channel_generation).  out_more is
 * set true when additional matches remain beyond what was collected (the
 * caller should issue a follow-up call with a larger skip_matches, quoting
 * the same out_generation as a validity check).  out_busy is set true only
 * when a lock could not be acquired at all and zero samples were returned.
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

/**
 * @file
 * @brief Shared telemetry channel data model: metadata, settings, runtime
 *        state, and value representations used by `channel_manager`,
 *        `sensor_manager`, channel drivers, and the OPC UA adapter layer.
 *
 * Mostly plain-data structs/enums; the one function here
 * (telemetry_channel_snapshot_clone()) is implemented in
 * `services/src/channel_manager.c`, not in a `.c` file owned by this header.
 */
#ifndef SERVICES_TELEMETRY_CHANNEL_H
#define SERVICES_TELEMETRY_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/service_types.h"

#define TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN 64u
/** @brief Sentinel for "no valid source timestamp".
 *
 * Source time is UTC Unix time in microseconds. Zero is a valid Unix epoch
 * sample; UINT64_MAX represents an unavailable/invalid source clock.
 */
#define TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID UINT64_MAX

#define TELEMETRY_CHANNEL_ID_MAX_LEN 32u
#define TELEMETRY_CHANNEL_NAME_MAX_LEN 32u
#define TELEMETRY_CHANNEL_DESCRIPTION_MAX_LEN 64u
#define TELEMETRY_CHANNEL_UNIT_MAX_LEN 16u

/** @brief Broad category of a telemetry channel, used for OPC UA type mapping and driver dispatch. */
typedef enum
{
    TELEMETRY_CHANNEL_KIND_ANALOG = 0,
    TELEMETRY_CHANNEL_KIND_DISCRETE = 1,
    TELEMETRY_CHANNEL_KIND_COUNTER = 2,
    TELEMETRY_CHANNEL_KIND_STATE = 3,
    TELEMETRY_CHANNEL_KIND_COMPUTED = 4
} telemetry_channel_kind_t;

/** @brief Discriminant for the active member of #telemetry_channel_value_t. */
typedef enum
{
    TELEMETRY_CHANNEL_VALUE_FLOAT = 0,
    TELEMETRY_CHANNEL_VALUE_DOUBLE = 1,
    TELEMETRY_CHANNEL_VALUE_INT32 = 2,
    TELEMETRY_CHANNEL_VALUE_UINT32 = 3,
    TELEMETRY_CHANNEL_VALUE_INT64 = 4,
    TELEMETRY_CHANNEL_VALUE_UINT64 = 5,
    TELEMETRY_CHANNEL_VALUE_BOOLEAN = 6,
    TELEMETRY_CHANNEL_VALUE_STRING = 7
} telemetry_channel_value_type_t;

/** @brief Bitmask flags stored in telemetry_channel_metadata_t::flags. */
typedef enum
{
    TELEMETRY_CHANNEL_FLAG_ENABLED = (1u << 0u),
    TELEMETRY_CHANNEL_FLAG_HISTORIZED = (1u << 1u),
    TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED = (1u << 2u),
    TELEMETRY_CHANNEL_FLAG_WRITABLE = (1u << 3u)
} telemetry_channel_flags_t;

/** @brief Tagged-union-style value storage; the active member is selected by telemetry_channel_metadata_t::value_type. */
typedef union
{
    float f32;
    double f64;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    bool boolean;
    char string[TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN];
} telemetry_channel_value_t;

/**
 * @brief Identifies which driver instance/source a channel is bound to.
 *
 * This value is copied into the channel registry with its metadata. It is
 * deliberately not a driver_context view: callers can report the configured
 * source without depending on driver-private storage or registry slot order.
 */
typedef struct
{
    uint8_t driver_id;
    uint8_t source_index;
    uint8_t source_flags;
    uint8_t reserved;
} telemetry_channel_source_descriptor_t;

/** @brief Optional engineering-unit min/max bounds for a channel; each bound is only meaningful if its `has_*` flag is set. */
typedef struct
{
    bool has_minimum;
    bool has_maximum;
    double minimum;
    double maximum;
} telemetry_channel_range_t;

/**
 * @brief Static, mostly-configuration-derived identity of a channel.
 *
 * String pointers (`id`/`name`/`description`/`unit`) are not owned by this
 * struct; they point either into caller-provided static storage or, for a
 * telemetry_channel_snapshot_t, into arrays embedded in the same snapshot.
 */
typedef struct
{
    const char *id;
    const char *name;
    const char *description;
    telemetry_channel_kind_t kind;
    telemetry_channel_value_type_t value_type;
    const char *unit;
    telemetry_channel_source_descriptor_t source;
    telemetry_channel_range_t range;
    uint32_t flags;
} telemetry_channel_metadata_t;

/** @brief Mutable, frequently-updated per-channel runtime state (last value, quality, counters, alarm state). */
typedef struct
{
    telemetry_channel_value_t value;
    service_quality_t quality;
    uint64_t source_timestamp_unix_us;
    uint32_t last_update_ms;
    uint32_t sample_count;
    uint32_t error_count;
    uint32_t runtime_flags;
    uint32_t alarm_state;
} telemetry_channel_runtime_t;

/**
 * @brief Per-channel configuration applied by `sensor_manager`/`channel_manager`.
 *
 * v0.4.3 alarm tuning fields (`low_threshold` onward) are appended after the
 * v0.4.2 fields above so the common prefix of this struct keeps a stable
 * byte layout for the `config_compat.c` migration decoder. Migrated
 * v0.4.0-v0.4.2 records use a finite disabled-lower-side sentinel because
 * those layouts had no low thresholds; their hysteresis and debounce fields
 * remain zero, reproducing the instantaneous v0.4.2 high-side behavior.
 * Native v0.4.3 configurations may explicitly set either lower threshold to
 * zero when it is coherently ordered with the high threshold.
 */
typedef struct
{
    bool enabled;
    uint32_t sample_period_ms;
    uint32_t publish_period_ms;
    uint32_t history_interval_ms;
    double scale;
    double offset;
    double warning_threshold;
    double critical_threshold;
    double low_threshold;
    double low_low_threshold;
    double alarm_hysteresis;
    uint32_t alarm_debounce_ms;
} telemetry_channel_settings_t;

struct telemetry_driver_vtbl;
typedef struct telemetry_driver_vtbl telemetry_driver_vtbl_t;

/**
 * @brief Live channel record combining metadata, settings, driver binding, and runtime state.
 *
 * `driver`/`driver_context` bind the channel to a specific driver
 * implementation (see telemetry_driver.h); `driver_context` is owned by
 * that driver instance, not by this struct.
 */
typedef struct telemetry_channel
{
    telemetry_channel_metadata_t metadata;
    telemetry_channel_settings_t settings;
    const telemetry_driver_vtbl_t *driver;
    void *driver_context;
    telemetry_channel_runtime_t runtime;
} telemetry_channel_t;

/**
 * @brief Caller-owned immutable copy of a channel, self-contained (no pointers into registry storage).
 *
 * `channel.metadata`'s `id`/`name`/`description`/`unit` pointers always
 * point to the `id`/`name`/`description`/`unit` arrays embedded in this same
 * struct, never to registry storage. This makes the struct
 * self-referential: use telemetry_channel_snapshot_clone() rather than a
 * plain C struct assignment or `memcpy` when duplicating a snapshot, since
 * a naive copy would leave the copy's metadata pointers referring to the
 * source object's embedded arrays.
 */
typedef struct
{
    telemetry_channel_t channel;
    char id[TELEMETRY_CHANNEL_ID_MAX_LEN];
    char name[TELEMETRY_CHANNEL_NAME_MAX_LEN];
    char description[TELEMETRY_CHANNEL_DESCRIPTION_MAX_LEN];
    char unit[TELEMETRY_CHANNEL_UNIT_MAX_LEN];
} telemetry_channel_snapshot_t;

/**
 * @brief Deep-copies a channel snapshot, re-pointing the destination's
 *        embedded metadata string pointers at its own arrays.
 *
 * A plain struct assignment or memcpy() would leave `destination`'s
 * `channel.metadata` pointers referring to `source`'s embedded string
 * arrays; this function fixes them up to point at `destination`'s own
 * `id`/`name`/`description`/`unit` arrays after the bulk copy.
 *
 * @param destination Snapshot to overwrite. No-op if NULL.
 * @param source Snapshot to copy from. No-op if NULL.
 *
 * @note Implemented in `services/src/channel_manager.c`.
 */
void telemetry_channel_snapshot_clone(telemetry_channel_snapshot_t *destination,
                                      const telemetry_channel_snapshot_t *source);

#endif

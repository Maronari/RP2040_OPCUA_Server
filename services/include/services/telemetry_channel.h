#ifndef SERVICES_TELEMETRY_CHANNEL_H
#define SERVICES_TELEMETRY_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/service_types.h"

#define TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN 64u
/* Source time is UTC Unix time in microseconds.  Zero is a valid Unix epoch
 * sample; UINT64_MAX represents an unavailable/invalid source clock. */
#define TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID UINT64_MAX

#define TELEMETRY_CHANNEL_ID_MAX_LEN 32u
#define TELEMETRY_CHANNEL_NAME_MAX_LEN 32u
#define TELEMETRY_CHANNEL_DESCRIPTION_MAX_LEN 64u
#define TELEMETRY_CHANNEL_UNIT_MAX_LEN 16u

typedef enum
{
    TELEMETRY_CHANNEL_KIND_ANALOG = 0,
    TELEMETRY_CHANNEL_KIND_DISCRETE = 1,
    TELEMETRY_CHANNEL_KIND_COUNTER = 2,
    TELEMETRY_CHANNEL_KIND_STATE = 3,
    TELEMETRY_CHANNEL_KIND_COMPUTED = 4
} telemetry_channel_kind_t;

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

typedef enum
{
    TELEMETRY_CHANNEL_FLAG_ENABLED = (1u << 0u),
    TELEMETRY_CHANNEL_FLAG_HISTORIZED = (1u << 1u),
    TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED = (1u << 2u),
    TELEMETRY_CHANNEL_FLAG_WRITABLE = (1u << 3u)
} telemetry_channel_flags_t;

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

/* This value is copied into the channel registry with its metadata.  It is
 * deliberately not a driver_context view: callers can report the configured
 * source without depending on driver-private storage or registry slot order. */
typedef struct
{
    uint8_t driver_id;
    uint8_t source_index;
    uint8_t source_flags;
    uint8_t reserved;
} telemetry_channel_source_descriptor_t;

typedef struct
{
    bool has_minimum;
    bool has_maximum;
    double minimum;
    double maximum;
} telemetry_channel_range_t;

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
} telemetry_channel_settings_t;

struct telemetry_driver_vtbl;
typedef struct telemetry_driver_vtbl telemetry_driver_vtbl_t;

typedef struct telemetry_channel
{
    telemetry_channel_metadata_t metadata;
    telemetry_channel_settings_t settings;
    const telemetry_driver_vtbl_t *driver;
    void *driver_context;
    telemetry_channel_runtime_t runtime;
} telemetry_channel_t;

/* A caller-owned immutable copy.  Metadata pointers in channel always point
 * to the arrays embedded in this object, never to registry storage.  This is
 * self-referential: use telemetry_channel_snapshot_clone() rather than C
 * assignment when duplicating a snapshot. */
typedef struct
{
    telemetry_channel_t channel;
    char id[TELEMETRY_CHANNEL_ID_MAX_LEN];
    char name[TELEMETRY_CHANNEL_NAME_MAX_LEN];
    char description[TELEMETRY_CHANNEL_DESCRIPTION_MAX_LEN];
    char unit[TELEMETRY_CHANNEL_UNIT_MAX_LEN];
} telemetry_channel_snapshot_t;

void telemetry_channel_snapshot_clone(telemetry_channel_snapshot_t *destination,
                                      const telemetry_channel_snapshot_t *source);

#endif

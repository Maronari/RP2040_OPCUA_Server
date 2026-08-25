/**
 * @file
 * @brief Implementation of the prior-schema binary decoder declared in
 * `services/config_compat.h`.
 *
 * Responsibilities: describe three frozen, byte-exact prior on-Flash
 * `device_profile_config_t` layouts (v0.4.0 in two variants,
 * v0.4.1/v0.4.2) as local struct types, and expand each into the current
 * live layout by copying ABI-stable prefixes/suffixes and per-channel
 * settings via `config_compat_expand_channel()`.
 *
 * Ownership: no persistent state; every function operates on caller-owned
 * buffers.
 *
 * Concurrency: no internal synchronization; pure functions over
 * caller-provided memory.
 *
 * Frozen-layout discipline: `config_compat_channel_settings_t`,
 * `config_compat_channel_definition_t`, `config_compat_v040_profile_t`, and
 * `config_compat_v042_profile_t` must never be edited to track the live
 * struct — they exist specifically to describe historical on-Flash byte
 * layouts. The `_Static_assert`s below pin the ABI assumptions this file
 * relies on.
 */
#include "services/config_compat.h"

#include <float.h>
#include <stddef.h>
#include <string.h>

#define CONFIG_COMPAT_LEGACY_ASSET_ID_MAX_LEN 32u

/**
 * @brief Frozen pre-v0.4.3 `telemetry_channel_settings_t` layout.
 *
 * v0.4.3 appends new alarm-tuning fields (low/low-low thresholds,
 * hysteresis, debounce) after `critical_threshold`; every field up to and
 * including `critical_threshold` keeps the exact same offsets in the live
 * struct, so this frozen shape is only needed to describe the *old*,
 * shorter on-Flash record for migration — it must never be changed to
 * track the live struct.
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
} config_compat_channel_settings_t;

/**
 * @brief Frozen pre-v0.4.3 `config_channel_definition_t` layout (same field
 * list as the live struct except for the trailing settings sub-struct
 * above).
 *
 * Used to decode both the v0.4.0 legacy record and the v0.4.1/v0.4.2
 * record, since `telemetry_channel_settings_t` was unchanged across those
 * releases.
 */
typedef struct
{
    uint8_t in_use;
    uint8_t kind;
    uint8_t value_type;
    uint8_t driver_id;
    uint8_t source_index;
    uint8_t source_flags;
    uint8_t has_minimum;
    uint8_t has_maximum;
    char id[CONFIG_SERVICE_CHANNEL_ID_MAX_LEN];
    char name[CONFIG_SERVICE_CHANNEL_NAME_MAX_LEN];
    char description[CONFIG_SERVICE_CHANNEL_DESCRIPTION_MAX_LEN];
    char unit[CONFIG_SERVICE_CHANNEL_UNIT_MAX_LEN];
    uint32_t flags;
    double minimum;
    double maximum;
    config_compat_channel_settings_t settings;
} config_compat_channel_definition_t;

/**
 * @brief Frozen native payload written by v0.4.0 on the same ARM ABI.
 *
 * This is a compatibility decoder only; new records always use
 * `device_profile_config_t`. Two distinct v0.4.0 on-Flash sizes exist (see
 * `config_compat_decode_v040_legacy()`): this struct's exact size, and an
 * older size with a shorter `asset_id`.
 */
typedef struct
{
    char manufacturer[CONFIG_SERVICE_TEXT_MAX_LEN];
    char model[CONFIG_SERVICE_TEXT_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char build_time[CONFIG_SERVICE_TEXT_MAX_LEN];
    char git_hash[CONFIG_SERVICE_TEXT_MAX_LEN];
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_broker_url[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_topic_root[CONFIG_SERVICE_TEXT_MAX_LEN];
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
    bool enable_alarm_ack_method;
    bool enable_mqtt_pubsub;
    bool mqtt_pubsub_use_json;
    uint16_t opcua_port;
    int16_t timezone_offset_minutes;
    uint32_t watchdog_timeout_ms;
    uint8_t mac[6];
    network_mode_t network_mode;
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
    config_compat_channel_definition_t channels[CONFIG_COMPAT_V040_CHANNEL_LIMIT];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
    char hierarchical_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char operational_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
} config_compat_v040_profile_t;

/**
 * @brief Frozen v0.4.1/v0.4.2 `device_profile_config_t` layout: identical to
 * the live struct in every respect except that `telemetry_channel_settings_t`
 * (nested inside each channel) has not yet grown the v0.4.3 alarm-tuning
 * fields.
 *
 * `PLATFORM_CHANNEL_LIMIT` is shared with the live struct because the
 * channel capacity did not change between v0.4.1 and v0.4.3.
 */
typedef struct
{
    char manufacturer[CONFIG_SERVICE_TEXT_MAX_LEN];
    char model[CONFIG_SERVICE_TEXT_MAX_LEN];
    char serial_number[CONFIG_SERVICE_TEXT_MAX_LEN];
    char hw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char fw_revision[CONFIG_SERVICE_TEXT_MAX_LEN];
    char build_time[CONFIG_SERVICE_TEXT_MAX_LEN];
    char git_hash[CONFIG_SERVICE_TEXT_MAX_LEN];
    char device_name[CONFIG_SERVICE_TEXT_MAX_LEN];
    char opcua_hostname[CONFIG_SERVICE_TEXT_MAX_LEN];
    char ntp_server[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_broker_url[CONFIG_SERVICE_TEXT_MAX_LEN];
    char mqtt_topic_root[CONFIG_SERVICE_TEXT_MAX_LEN];
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
    bool enable_alarm_ack_method;
    bool enable_mqtt_pubsub;
    bool mqtt_pubsub_use_json;
    uint16_t opcua_port;
    int16_t timezone_offset_minutes;
    uint32_t watchdog_timeout_ms;
    uint8_t mac[6];
    network_mode_t network_mode;
    uint8_t static_ip[4];
    uint8_t static_netmask[4];
    uint8_t static_gateway[4];
    uint8_t static_dns[4];
    config_compat_channel_definition_t channels[PLATFORM_CHANNEL_LIMIT];
    char product_instance_uri[CONFIG_SERVICE_URI_MAX_LEN];
    char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN];
    int64_t manufacture_date_utc;
    int64_t calibration_date_utc;
    char location[CONFIG_SERVICE_LOCATION_MAX_LEN];
    char asset_class[CONFIG_SERVICE_ASSET_CLASS_MAX_LEN];
    char hierarchical_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char operational_location_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
} config_compat_v042_profile_t;

_Static_assert(offsetof(config_channel_definition_t, settings) ==
                   offsetof(config_compat_channel_definition_t, settings),
               "frozen and live channel prefixes must remain ABI-compatible");
_Static_assert(offsetof(device_profile_config_t, channels) ==
                   offsetof(config_compat_v040_profile_t, channels),
               "frozen v0.4.0 profile prefix must remain ABI-compatible");
_Static_assert(offsetof(device_profile_config_t, channels) ==
                   offsetof(config_compat_v042_profile_t, channels),
               "frozen v0.4.2 profile prefix must remain ABI-compatible");

/** @brief Rounds @p value up to the next multiple of @p alignment (returns @p value unchanged if @p alignment is 0). */
static size_t config_compat_align_up(size_t value, size_t alignment)
{
    return alignment == 0u ? value : ((value + alignment - 1u) / alignment) * alignment;
}

/**
 * @brief Copies one frozen channel definition (old, shorter settings) into
 * a live `config_channel_definition_t`.
 *
 * The destination is assumed already zero-initialized by the caller, so the
 * new v0.4.3 settings fields land as 0.0/0u, matching the v0.4.2
 * instantaneous no-hysteresis/no-debounce behavior exactly for every
 * migrated channel — except the two lower-alarm thresholds, which are set
 * explicitly below.
 *
 * @param old_channel Pointer into the raw payload buffer at this channel's
 * frozen-layout offset; may be unaligned for `config_compat_channel_definition_t`,
 * so this copies via `memcpy()` rather than casting.
 * @param out_channel Live-layout destination to fill.
 */
static void config_compat_expand_channel(const uint8_t *old_channel,
                                         config_channel_definition_t *out_channel)
{
    /* The source may be a byte buffer with no suitable alignment for the
     * frozen type.  Copy the two ABI-stable portions rather than casting it;
     * this also avoids a profile-sized scratch object on the caller's stack. */
    memcpy(out_channel, old_channel, offsetof(config_compat_channel_definition_t, settings));
    memcpy(&out_channel->settings,
           old_channel + offsetof(config_compat_channel_definition_t, settings),
           sizeof(config_compat_channel_settings_t));
    /* The v0.4.0-v0.4.2 layout has no lower alarm fields.  DBL_MAX is a
     * finite, persistable migration sentinel: the alarm service disables the
     * lower side when low is above the high entry threshold.  This avoids
     * turning every negative good-quality value into a new alarm, while an
     * explicit native v0.4.3 zero threshold remains an enabled lower limit. */
    out_channel->settings.low_threshold = DBL_MAX;
    out_channel->settings.low_low_threshold = DBL_MAX;
    /* alarm_hysteresis and alarm_debounce_ms stay at their zero-initialized
     * v0.4.2-compatible defaults. */
}

/**
 * @brief Checks that a v0.4.0 payload's channel slots beyond
 * `PLATFORM_CHANNEL_LIMIT` (if the current build's limit is smaller than
 * `CONFIG_COMPAT_V040_CHANNEL_LIMIT`) are all unused, so they can be safely
 * dropped during migration.
 * @return true if every such slot has `in_use == 0`, or if
 * `PLATFORM_CHANNEL_LIMIT >= CONFIG_COMPAT_V040_CHANNEL_LIMIT` (nothing to
 * check); false if a configured channel would be silently discarded.
 */
static bool config_compat_v040_channels_fit(const uint8_t *payload)
{
    const size_t channels_offset = offsetof(config_compat_v040_profile_t, channels);
    const size_t copied_channels = PLATFORM_CHANNEL_LIMIT < CONFIG_COMPAT_V040_CHANNEL_LIMIT ?
                                       PLATFORM_CHANNEL_LIMIT : CONFIG_COMPAT_V040_CHANNEL_LIMIT;

    for (size_t index = copied_channels; index < CONFIG_COMPAT_V040_CHANNEL_LIMIT; ++index)
    {
        const size_t channel_offset = channels_offset +
                                      index * sizeof(config_compat_channel_definition_t);
        if (payload[channel_offset + offsetof(config_compat_channel_definition_t, in_use)] != 0u)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Expands a frozen v0.4.0-sized (`sizeof(config_compat_v040_profile_t)`)
 * payload into @p out_config.
 * @return false if `config_compat_v040_channels_fit()` rejects the channel
 * array; true otherwise.
 * @note @p out_config is fully zeroed first, then the ABI-stable prefix,
 * each migrated channel, and the tail suffix (from `product_instance_uri`
 * onward, whatever length @p payload_size provides) are copied in.
 */
static bool config_compat_expand_v040(const uint8_t *payload,
                                      size_t payload_size,
                                      device_profile_config_t *out_config)
{
    const size_t prefix_size = offsetof(config_compat_v040_profile_t, channels);
    const size_t channels_offset = offsetof(config_compat_v040_profile_t, channels);
    const size_t product_uri_offset = offsetof(config_compat_v040_profile_t, product_instance_uri);
    const size_t output_tail_offset = offsetof(device_profile_config_t, product_instance_uri);
    const size_t copied_channels = PLATFORM_CHANNEL_LIMIT < CONFIG_COMPAT_V040_CHANNEL_LIMIT ?
                                       PLATFORM_CHANNEL_LIMIT : CONFIG_COMPAT_V040_CHANNEL_LIMIT;

    if (!config_compat_v040_channels_fit(payload))
    {
        return false;
    }

    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config, payload, prefix_size);
    for (size_t index = 0u; index < copied_channels; ++index)
    {
        config_compat_expand_channel(payload + channels_offset +
                                         index * sizeof(config_compat_channel_definition_t),
                                     &out_config->channels[index]);
    }
    /* A v0.4.0 record may end immediately before hierarchical_location_path.
     * Every available field after product_instance_uri retains its frozen
     * layout, so transfer exactly the validated source suffix. */
    memcpy((uint8_t *)out_config + output_tail_offset,
           payload + product_uri_offset,
           payload_size - product_uri_offset);
    return true;
}

/**
 * @brief Expands a frozen v0.4.1/v0.4.2-sized payload into @p out_config.
 *
 * v0.4.1/v0.4.2 records share `PLATFORM_CHANNEL_LIMIT` with the live
 * struct, so this migration is a straight per-channel expand plus a
 * by-name copy of the fields that follow the channel array (their absolute
 * offsets differ from the live struct only because of the settings size
 * change).
 * @return Always true (no channel-count mismatch is possible for this
 * layout, unlike the v0.4.0 case).
 */
static bool config_compat_expand_v042(const uint8_t *payload,
                                      device_profile_config_t *out_config)
{
    const size_t prefix_size = offsetof(config_compat_v042_profile_t, channels);
    const size_t channels_offset = offsetof(config_compat_v042_profile_t, channels);
    const size_t product_uri_offset = offsetof(config_compat_v042_profile_t, product_instance_uri);
    const size_t output_tail_offset = offsetof(device_profile_config_t, product_instance_uri);

    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config, payload, prefix_size);
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        config_compat_expand_channel(payload + channels_offset +
                                         index * sizeof(config_compat_channel_definition_t),
                                     &out_config->channels[index]);
    }
    memcpy((uint8_t *)out_config + output_tail_offset,
           payload + product_uri_offset,
           sizeof(config_compat_v042_profile_t) - product_uri_offset);
    return true;
}

/**
 * @brief Decodes either v0.4.0 payload size variant: the "previous" size
 * (delegated to `config_compat_expand_v040()`) or the oldest, shorter
 * `asset_id` variant (decoded directly here).
 *
 * The oldest record shortened only `asset_id` (to
 * `CONFIG_COMPAT_LEGACY_ASSET_ID_MAX_LEN` bytes); this function decodes its
 * fixed prefix and channels directly, then maps its shifted-but-otherwise
 * unchanged tail (from `manufacture_date_utc` onward) explicitly, since the
 * shorter `asset_id` shifts every subsequent field's absolute offset.
 *
 * @return false if @p payload_size matches neither known v0.4.0 size, or if
 * `config_compat_v040_channels_fit()` rejects the channel array for the
 * legacy-size path; true on successful decode.
 */
static bool config_compat_decode_v040_legacy(const uint8_t *payload,
                                             size_t payload_size,
                                             device_profile_config_t *out_config)
{
    const size_t asset_offset = offsetof(config_compat_v040_profile_t, asset_id);
    const size_t tail_offset = offsetof(config_compat_v040_profile_t, manufacture_date_utc);
    const size_t previous_size = offsetof(config_compat_v040_profile_t,
                                          hierarchical_location_path);
    const size_t legacy_tail_offset =
        config_compat_align_up(asset_offset + CONFIG_COMPAT_LEGACY_ASSET_ID_MAX_LEN,
                               _Alignof(int64_t));
    const size_t legacy_size = legacy_tail_offset + previous_size - tail_offset;

    if (payload_size == previous_size)
    {
        return config_compat_expand_v040(payload, payload_size, out_config);
    }
    if (payload_size != legacy_size)
    {
        return false;
    }
    if (!config_compat_v040_channels_fit(payload))
    {
        return false;
    }

    /* The oldest record shortened only asset_id.  Decode its fixed prefix and
     * channels directly, then map its shifted bounded tail explicitly. */
    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config, payload, offsetof(config_compat_v040_profile_t, channels));
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        const size_t channel_offset = offsetof(config_compat_v040_profile_t, channels) +
                                      index * sizeof(config_compat_channel_definition_t);
        if (index >= CONFIG_COMPAT_V040_CHANNEL_LIMIT)
        {
            break;
        }
        config_compat_expand_channel(payload + channel_offset, &out_config->channels[index]);
    }
    memcpy(out_config->product_instance_uri,
           payload + offsetof(config_compat_v040_profile_t, product_instance_uri),
           sizeof(out_config->product_instance_uri));
    memcpy(out_config->asset_id, payload + asset_offset, CONFIG_COMPAT_LEGACY_ASSET_ID_MAX_LEN);
    memcpy((uint8_t *)out_config + offsetof(device_profile_config_t, manufacture_date_utc),
           payload + legacy_tail_offset,
           previous_size - tail_offset);
    return true;
}

bool config_compat_decode_binary(const uint8_t *payload,
                                 size_t payload_size,
                                 device_profile_config_t *out_config,
                                 bool *out_migrated)
{
    if (payload == NULL || out_config == NULL)
    {
        return false;
    }
    if (out_migrated != NULL)
    {
        *out_migrated = false;
    }
    if (payload_size == sizeof(*out_config))
    {
        if ((const void *)out_config != (const void *)payload)
        {
            memmove(out_config, payload, sizeof(*out_config));
        }
        return true;
    }
    if (payload_size == sizeof(config_compat_v042_profile_t))
    {
        if (!config_compat_expand_v042(payload, out_config))
        {
            return false;
        }
    }
    else if (payload_size == sizeof(config_compat_v040_profile_t))
    {
        if (!config_compat_expand_v040(payload, payload_size, out_config))
        {
            return false;
        }
    }
    else if (!config_compat_decode_v040_legacy(payload, payload_size, out_config))
    {
        return false;
    }

    if (out_migrated != NULL)
    {
        *out_migrated = true;
    }
    return true;
}

size_t config_compat_v040_payload_size(void)
{
    return sizeof(config_compat_v040_profile_t);
}

size_t config_compat_v042_payload_size(void)
{
    return sizeof(config_compat_v042_profile_t);
}

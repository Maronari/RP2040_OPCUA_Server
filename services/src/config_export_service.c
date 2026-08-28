/**
 * @file
 * @brief Implementation of configuration-to-JSON serialization declared in
 * `services/config_export_service.h`.
 *
 * Responsibilities:
 * - A bounded, allocation-free streaming JSON writer
 *   (`config_json_writer_t` + `config_json_writer_*` helpers) that never
 *   writes past the caller-supplied buffer and tracks overflow rather than
 *   producing partial/incorrect JSON silently.
 * - `config_export_service_write_profile_json()` formats exactly the
 *   schema `config_validate_service`'s JSON parser expects, field-for-field
 *   in the same fixed key order.
 *
 * Ownership: no persistent state.
 *
 * Concurrency: no internal synchronization; reads configuration state via
 * `config_service_get()`/`config_service_get_staged()`, which return live
 * pointers without their own copy — see the layering note on those
 * functions in `services/config_service.h`.
 */
#include "services/config_export_service.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "services/config_import_service.h"
#include "services/config_service.h"
#include "services/config_store_service.h"

/**
 * @brief Bounded, allocation-free streaming JSON writer state.
 *
 * All `config_json_writer_append_*()` helpers below are no-ops once
 * `overflow` is set, so a caller only needs to check `writer.overflow` (or
 * the boolean return of the top-level export function) once at the end
 * rather than after every append. Owned entirely by the calling export
 * function; not shared across threads.
 */
typedef struct
{
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} config_json_writer_t;

/** @brief Maps `network_mode_t` to its JSON schema string (`"dhcp"`/`"static"`). */
static const char *config_export_network_mode_to_string(network_mode_t mode)
{
    return mode == NETWORK_MODE_STATIC ? "static" : "dhcp";
}

/**
 * @brief Initializes @p writer to write into @p buffer, or into an
 * already-overflowed state if @p buffer/@p capacity are invalid.
 * @note No-op if @p writer is NULL.
 */
static void config_json_writer_init(config_json_writer_t *writer, char *buffer, size_t capacity)
{
    if (writer == NULL)
    {
        return;
    }

    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->length = 0u;
    writer->overflow = (buffer == NULL || capacity == 0u);

    if (!writer->overflow)
    {
        writer->buffer[0] = '\0';
    }
}

/**
 * @brief Appends exactly @p text_length raw bytes, or sets `writer->overflow`
 * if they wouldn't fit (leaving one byte reserved for the NUL terminator).
 * @note No-op if @p writer is NULL, already overflowed, or @p text is NULL.
 */
static void config_json_writer_append_bytes(config_json_writer_t *writer, const char *text, size_t text_length)
{
    if (writer == NULL || writer->overflow || text == NULL)
    {
        return;
    }

    /* Keep one byte for NUL. This avoids format-parser overhead for every
     * literal JSON token and has a fixed linear copy bound. */
    if (text_length >= writer->capacity - writer->length)
    {
        writer->overflow = true;
        return;
    }

    memcpy(writer->buffer + writer->length, text, text_length);
    writer->length += text_length;
    writer->buffer[writer->length] = '\0';
}

/** @brief Appends a NUL-terminated literal string (JSON punctuation/keys). No-op if @p text is NULL. */
static void config_json_writer_append_raw(config_json_writer_t *writer, const char *text)
{
    if (text != NULL)
    {
        config_json_writer_append_bytes(writer, text, strlen(text));
    }
}

/** @brief Appends a single character. */
static void config_json_writer_append_char(config_json_writer_t *writer, char value)
{
    config_json_writer_append_bytes(writer, &value, 1u);
}

/**
 * @brief Appends the decimal digits of @p value without invoking
 * `snprintf()`/newlib formatting, used for high-frequency small integers
 * (e.g. per-channel period/flag fields) to keep JSON export fast and
 * allocation-free.
 */
static void config_json_writer_append_u64(config_json_writer_t *writer, uint64_t value)
{
    char digits[20];
    size_t length = 0u;

    do
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && length < sizeof(digits));

    while (length > 0u)
    {
        config_json_writer_append_char(writer, digits[--length]);
    }
}

/**
 * @brief Appends a signed decimal integer, handling `INT64_MIN` without
 * signed-overflow UB when negating it.
 */
static void config_json_writer_append_i64(config_json_writer_t *writer, int64_t value)
{
    uint64_t magnitude;

    if (value < 0)
    {
        config_json_writer_append_char(writer, '-');
        /* Avoid signed overflow for INT64_MIN. */
        magnitude = (uint64_t)(-(value + 1)) + 1u;
    }
    else
    {
        magnitude = (uint64_t)value;
    }
    config_json_writer_append_u64(writer, magnitude);
}

/**
 * @brief `vsnprintf()`-based formatted append for compound JSON fragments
 * (e.g. `"port":%u,"enableAlarmAckMethod":%s`) where the fixed
 * byte/character helpers above would be more verbose than useful.
 * @note Sets `writer->overflow` on truncation or a `vsnprintf()` encoding
 * error (negative return).
 */
static void config_json_writer_append_format(config_json_writer_t *writer, const char *format, ...)
{
    va_list args;
    int written = 0;

    if (writer == NULL || writer->overflow || format == NULL)
    {
        return;
    }

    va_start(args, format);
    written = vsnprintf(writer->buffer + writer->length,
                        writer->capacity - writer->length,
                        format,
                        args);
    va_end(args);
    if (written < 0 || (size_t)written >= (writer->capacity - writer->length))
    {
        writer->overflow = true;
        return;
    }

    writer->length += (size_t)written;
}

/**
 * @brief Appends @p text as a JSON string literal, escaping `\`, `"`, `\n`,
 * `\r`, `\t`.
 * @param text Source text; a NULL pointer is treated as an empty string.
 * @param text_capacity Fixed-size buffer capacity of @p text (as stored in
 * `device_profile_config_t`), used only as a hard upper bound on the scan —
 * @p text is still expected to be NUL-terminated within that capacity.
 * @note Fails closed: if no NUL terminator is found within @p text_capacity
 * bytes, this sets `writer->overflow` rather than scanning past the
 * fixed-width field into adjacent memory.
 */
static void config_json_writer_append_quoted(config_json_writer_t *writer,
                                             const char *text,
                                             size_t text_capacity)
{
    const char *cursor = text;
    size_t index = 0u;

    if (writer == NULL || writer->overflow)
    {
        return;
    }

    if (cursor == NULL)
    {
        cursor = "";
        text_capacity = 1u;
    }

    config_json_writer_append_char(writer, '\"');
    while (index < text_capacity && cursor[index] != '\0' && !writer->overflow)
    {
        switch (cursor[index])
        {
        case '\\':
            config_json_writer_append_raw(writer, "\\\\");
            break;
        case '\"':
            config_json_writer_append_raw(writer, "\\\"");
            break;
        case '\n':
            config_json_writer_append_raw(writer, "\\n");
            break;
        case '\r':
            config_json_writer_append_raw(writer, "\\r");
            break;
        case '\t':
            config_json_writer_append_raw(writer, "\\t");
            break;
        default:
            config_json_writer_append_char(writer, cursor[index]);
            break;
        }

        index++;
    }
    if (index == text_capacity)
    {
        /* Persisted fixed-width text must be NUL terminated. Failing closed
         * prevents an unbounded scan into adjacent configuration/PSRAM. */
        writer->overflow = true;
        return;
    }
    config_json_writer_append_char(writer, '\"');
}

/**
 * @brief Appends a finite double as a JSON number, with fast paths for
 * common values and exact literals for the `DBL_MAX` sentinel.
 *
 * `DBL_MAX` is the persisted high-only lower-alarm sentinel
 * (`config_compat_expand_channel()`). Formatting it via `%.6f` would
 * produce more than 300 decimal digits, once per configured channel slot;
 * these common values (`0`, `1`, `-1`, `±DBL_MAX`, whole integers) avoid
 * newlib soft-float formatting entirely, and the finite `DBL_MAX` spelling
 * round-trips exactly and remains valid JSON.
 * @note Non-finite (NaN/Inf) values set `writer->overflow` rather than
 * being emitted as invalid JSON.
 */
static void config_json_writer_append_double(config_json_writer_t *writer, double value)
{
    if (writer == NULL || writer->overflow)
    {
        return;
    }
    if (!isfinite(value))
    {
        writer->overflow = true;
        return;
    }

    if (value == 0.0)
    {
        config_json_writer_append_raw(writer, "0");
    }
    else if (value == 1.0)
    {
        config_json_writer_append_raw(writer, "1");
    }
    else if (value == -1.0)
    {
        config_json_writer_append_raw(writer, "-1");
    }
    else if (value == DBL_MAX)
    {
        config_json_writer_append_raw(writer, "1.7976931348623157e308");
    }
    else if (value == -DBL_MAX)
    {
        config_json_writer_append_raw(writer, "-1.7976931348623157e308");
    }
    else if (value > -9223372036854775808.0 && value < 9223372036854775808.0)
    {
        const int64_t integer = (int64_t)value;

        if ((double)integer == value)
        {
            /* The live profile's thresholds and ranges are commonly whole
             * degrees/counts. Emit them without invoking newlib dtoa. */
            config_json_writer_append_i64(writer, integer);
        }
        else
        {
            config_json_writer_append_format(writer, "%.17g", value);
        }
    }
    else
    {
        /* A finite non-default value is bounded by the fixed channel count;
         * 17 significant digits preserve a double round trip. */
        config_json_writer_append_format(writer, "%.17g", value);
    }
}

/** @brief Appends a quoted dotted-quad IPv4 string; `"0.0.0.0"` if @p address is NULL. */
static void config_json_writer_append_ipv4(config_json_writer_t *writer, const uint8_t address[4])
{
    if (address == NULL)
    {
        config_json_writer_append_quoted(writer, "0.0.0.0", sizeof("0.0.0.0"));
        return;
    }

    config_json_writer_append_format(writer,
                                     "\"%u.%u.%u.%u\"",
                                     (unsigned)address[0],
                                     (unsigned)address[1],
                                     (unsigned)address[2],
                                     (unsigned)address[3]);
}

/** @brief Appends a quoted colon-separated MAC string; `"00:00:00:00:00:00"` if @p mac is NULL. */
static void config_json_writer_append_mac(config_json_writer_t *writer, const uint8_t mac[6])
{
    if (mac == NULL)
    {
        config_json_writer_append_quoted(writer, "00:00:00:00:00:00", sizeof("00:00:00:00:00:00"));
        return;
    }

    config_json_writer_append_format(writer,
                                     "\"%02X:%02X:%02X:%02X:%02X:%02X\"",
                                     (unsigned)mac[0],
                                     (unsigned)mac[1],
                                     (unsigned)mac[2],
                                     (unsigned)mac[3],
                                     (unsigned)mac[4],
                                     (unsigned)mac[5]);
}

/**
 * @brief Serializes @p config as the public configuration JSON schema:
 * `schemaVersion`, `device`, `network`, `time`, `mqtt`, `diagnostics`,
 * `opcua`, `channels` (`PLATFORM_CHANNEL_LIMIT` elements, including the
 * v0.4.3 lower-alarm fields), in exactly the key order
 * `config_validate_service`'s JSON parser expects.
 * @return true if the document fit in @p buffer without any writer
 * overflow; false on NULL/zero-sized arguments or truncation.
 */
static bool config_export_service_write_profile_json(const device_profile_config_t *config,
                                                     char *buffer,
                                                     size_t buffer_size)
{
    config_json_writer_t writer;

    if (config == NULL || buffer == NULL || buffer_size == 0u)
    {
        return false;
    }

    config_json_writer_init(&writer, buffer, buffer_size);
    config_json_writer_append_raw(&writer, "{");

    config_json_writer_append_format(&writer, "\"schemaVersion\":%u,", (unsigned)CONFIG_STORE_SCHEMA_VERSION_CURRENT);
    config_json_writer_append_raw(&writer, "\"device\":{");
    config_json_writer_append_raw(&writer, "\"manufacturer\":");
    config_json_writer_append_quoted(&writer, config->manufacturer, sizeof(config->manufacturer));
    config_json_writer_append_raw(&writer, ",\"model\":");
    config_json_writer_append_quoted(&writer, config->model, sizeof(config->model));
    config_json_writer_append_raw(&writer, ",\"deviceName\":");
    config_json_writer_append_quoted(&writer, config->device_name, sizeof(config->device_name));
    config_json_writer_append_raw(&writer, ",\"opcuaHostname\":");
    config_json_writer_append_quoted(&writer, config->opcua_hostname, sizeof(config->opcua_hostname));
    config_json_writer_append_raw(&writer, ",\"serialNumber\":");
    config_json_writer_append_quoted(&writer, config->serial_number, sizeof(config->serial_number));
    config_json_writer_append_raw(&writer, ",\"productInstanceUri\":");
    config_json_writer_append_quoted(&writer, config->product_instance_uri, sizeof(config->product_instance_uri));
    config_json_writer_append_raw(&writer, ",\"assetId\":");
    config_json_writer_append_quoted(&writer, config->asset_id, sizeof(config->asset_id));
    config_json_writer_append_raw(&writer, ",\"hwRevision\":");
    config_json_writer_append_quoted(&writer, config->hw_revision, sizeof(config->hw_revision));
    config_json_writer_append_raw(&writer, ",\"fwRevision\":");
    config_json_writer_append_quoted(&writer, config->fw_revision, sizeof(config->fw_revision));
    config_json_writer_append_raw(&writer, ",\"buildTime\":");
    config_json_writer_append_quoted(&writer, config->build_time, sizeof(config->build_time));
    config_json_writer_append_raw(&writer, ",\"gitHash\":");
    config_json_writer_append_quoted(&writer, config->git_hash, sizeof(config->git_hash));
    config_json_writer_append_raw(&writer, ",\"location\":");
    config_json_writer_append_quoted(&writer, config->location, sizeof(config->location));
    config_json_writer_append_raw(&writer, ",\"assetClass\":");
    config_json_writer_append_quoted(&writer, config->asset_class, sizeof(config->asset_class));
    config_json_writer_append_format(&writer,
                                     ",\"manufactureDateUtc\":%lld,\"calibrationDateUtc\":%lld",
                                     (long long)config->manufacture_date_utc,
                                     (long long)config->calibration_date_utc);
    config_json_writer_append_raw(&writer, ",\"hierarchicalLocationPath\":");
    config_json_writer_append_quoted(&writer, config->hierarchical_location_path,
                                     sizeof(config->hierarchical_location_path));
    config_json_writer_append_raw(&writer, ",\"operationalLocationPath\":");
    config_json_writer_append_quoted(&writer, config->operational_location_path,
                                     sizeof(config->operational_location_path));
    config_json_writer_append_raw(&writer, "},");

    config_json_writer_append_raw(&writer, "\"network\":{");
    config_json_writer_append_raw(&writer, "\"mode\":");
    config_json_writer_append_quoted(&writer, config_export_network_mode_to_string(config->network_mode),
                                     sizeof("static"));
    config_json_writer_append_raw(&writer, ",\"mac\":");
    config_json_writer_append_mac(&writer, config->mac);
    config_json_writer_append_raw(&writer, ",\"staticIp\":");
    config_json_writer_append_ipv4(&writer, config->static_ip);
    config_json_writer_append_raw(&writer, ",\"staticNetmask\":");
    config_json_writer_append_ipv4(&writer, config->static_netmask);
    config_json_writer_append_raw(&writer, ",\"staticGateway\":");
    config_json_writer_append_ipv4(&writer, config->static_gateway);
    config_json_writer_append_raw(&writer, ",\"staticDns\":");
    config_json_writer_append_ipv4(&writer, config->static_dns);
    config_json_writer_append_raw(&writer, "},");

    config_json_writer_append_raw(&writer, "\"time\":{");
    config_json_writer_append_raw(&writer, "\"ntpServer\":");
    config_json_writer_append_quoted(&writer, config->ntp_server, sizeof(config->ntp_server));
    config_json_writer_append_format(&writer,
                                     ",\"timezoneOffsetMinutes\":%d},",
                                     (int)config->timezone_offset_minutes);

    config_json_writer_append_raw(&writer, "\"mqtt\":{");
    config_json_writer_append_format(&writer,
                                     "\"enabled\":%s,\"useJson\":%s,",
                                     config->enable_mqtt_pubsub ? "true" : "false",
                                     config->mqtt_pubsub_use_json ? "true" : "false");
    config_json_writer_append_raw(&writer, "\"brokerUrl\":");
    config_json_writer_append_quoted(&writer, config->mqtt_broker_url, sizeof(config->mqtt_broker_url));
    config_json_writer_append_raw(&writer, ",\"topicRoot\":");
    config_json_writer_append_quoted(&writer, config->mqtt_topic_root, sizeof(config->mqtt_topic_root));
    config_json_writer_append_raw(&writer, "},");

    config_json_writer_append_raw(&writer, "\"diagnostics\":{");
    config_json_writer_append_format(&writer,
                                     "\"enableHeapDiagnostics\":%s,\"enableTaskDiagnostics\":%s,\"watchdogTimeoutMs\":%lu},",
                                     config->enable_heap_diagnostics ? "true" : "false",
                                     config->enable_task_diagnostics ? "true" : "false",
                                     (unsigned long)config->watchdog_timeout_ms);

    config_json_writer_append_raw(&writer, "\"opcua\":{");
    config_json_writer_append_format(&writer,
                                     "\"port\":%u,\"enableAlarmAckMethod\":%s},",
                                     (unsigned)config->opcua_port,
                                     config->enable_alarm_ack_method ? "true" : "false");

    config_json_writer_append_raw(&writer, "\"channels\":[");
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        const config_channel_definition_t *channel = &config->channels[index];

        if (index > 0u)
        {
            config_json_writer_append_raw(&writer, ",");
        }

        config_json_writer_append_raw(&writer, "{");
        config_json_writer_append_format(&writer,
                                         "\"slot\":%u,\"inUse\":%s,\"kind\":%u,\"valueType\":%u,\"driverId\":%u,\"sourceIndex\":%u,\"sourceFlags\":%u,",
                                         (unsigned)index,
                                         channel->in_use != 0u ? "true" : "false",
                                         (unsigned)channel->kind,
                                         (unsigned)channel->value_type,
                                         (unsigned)channel->driver_id,
                                         (unsigned)channel->source_index,
                                         (unsigned)channel->source_flags);
        config_json_writer_append_raw(&writer, "\"id\":");
        config_json_writer_append_quoted(&writer, channel->id, sizeof(channel->id));
        config_json_writer_append_raw(&writer, ",\"name\":");
        config_json_writer_append_quoted(&writer, channel->name, sizeof(channel->name));
        config_json_writer_append_raw(&writer, ",\"description\":");
        config_json_writer_append_quoted(&writer, channel->description, sizeof(channel->description));
        config_json_writer_append_raw(&writer, ",\"unit\":");
        config_json_writer_append_quoted(&writer, channel->unit, sizeof(channel->unit));
        config_json_writer_append_format(&writer, ",\"flags\":%lu,\"minimum\":", (unsigned long)channel->flags);
        config_json_writer_append_double(&writer, channel->minimum);
        config_json_writer_append_raw(&writer, ",\"maximum\":");
        config_json_writer_append_double(&writer, channel->maximum);
        config_json_writer_append_format(&writer,
                                         ",\"hasMinimum\":%s,\"hasMaximum\":%s,\"enabled\":%s,"
                                         "\"samplePeriodMs\":%lu,\"publishPeriodMs\":%lu,\"historyIntervalMs\":%lu,"
                                         "\"scale\":",
                                         channel->has_minimum != 0u ? "true" : "false",
                                         channel->has_maximum != 0u ? "true" : "false",
                                         channel->settings.enabled ? "true" : "false",
                                         (unsigned long)channel->settings.sample_period_ms,
                                         (unsigned long)channel->settings.publish_period_ms,
                                         (unsigned long)channel->settings.history_interval_ms);
        config_json_writer_append_double(&writer, channel->settings.scale);
        config_json_writer_append_raw(&writer, ",\"offset\":");
        config_json_writer_append_double(&writer, channel->settings.offset);
        config_json_writer_append_raw(&writer, ",\"warningThreshold\":");
        config_json_writer_append_double(&writer, channel->settings.warning_threshold);
        config_json_writer_append_raw(&writer, ",\"criticalThreshold\":");
        config_json_writer_append_double(&writer, channel->settings.critical_threshold);
        config_json_writer_append_raw(&writer, ",\"lowThreshold\":");
        config_json_writer_append_double(&writer, channel->settings.low_threshold);
        config_json_writer_append_raw(&writer, ",\"lowLowThreshold\":");
        config_json_writer_append_double(&writer, channel->settings.low_low_threshold);
        config_json_writer_append_raw(&writer, ",\"alarmHysteresis\":");
        config_json_writer_append_double(&writer, channel->settings.alarm_hysteresis);
        config_json_writer_append_format(&writer, ",\"alarmDebounceMs\":%lu}",
                                         (unsigned long)channel->settings.alarm_debounce_ms);
    }
    config_json_writer_append_raw(&writer, "]}");

    return !writer.overflow;
}

bool config_export_service_write_running_json(char *buffer, size_t buffer_size)
{
    return config_export_service_write_profile_json(config_service_get(), buffer, buffer_size);
}

bool config_export_service_write_staged_json(char *buffer, size_t buffer_size)
{
    return config_export_service_write_profile_json(config_service_get_staged(), buffer, buffer_size);
}

bool config_export_service_write_state_json(char *buffer, size_t buffer_size, bool reboot_required)
{
    config_store_record_info_t info = {0};
    config_import_service_status_t import_status = {0};
    const uint32_t running_revision = config_service_get_running_revision();
    const uint32_t staged_revision = config_service_get_staged_revision();
    int written = 0;

    if (buffer == NULL || buffer_size == 0u)
    {
        return false;
    }

    config_import_service_get_status(&import_status);

    if (!config_store_get_active_info(&info))
    {
        written = snprintf(buffer,
                           buffer_size,
                           "{\"runningRevision\":%u,\"stagedRevision\":%u,\"rebootRequired\":%s,\"stagedDiffers\":%s,\"persistedActive\":false,\"importSessionActive\":%s,\"importExpectedSize\":%u,\"importExpectedCrc32\":\"0x%08lx\",\"importSchemaVersion\":%u,\"importBytesWritten\":%u,\"importRemainingBytes\":%u,\"importPayloadComplete\":%s,\"importPayloadCrc32Matches\":%s,\"importLastResult\":\"%s\",\"importMaxChunkBytes\":%u}",
                           (unsigned)running_revision,
                           (unsigned)staged_revision,
                           reboot_required ? "true" : "false",
                           running_revision != staged_revision ? "true" : "false",
                           import_status.session_active ? "true" : "false",
                           (unsigned)import_status.expected_payload_size,
                           (unsigned long)import_status.expected_payload_crc32,
                           (unsigned)import_status.schema_version,
                           (unsigned)import_status.bytes_written,
                           (unsigned)import_status.remaining_bytes,
                           import_status.payload_complete ? "true" : "false",
                           import_status.payload_crc32_matches ? "true" : "false",
                           config_import_service_result_to_string(import_status.last_result),
                           (unsigned)CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES);
        return written > 0 && (size_t)written < buffer_size;
    }

    written = snprintf(buffer,
                       buffer_size,
                       "{\"runningRevision\":%u,\"stagedRevision\":%u,\"rebootRequired\":%s,\"stagedDiffers\":%s,\"persistedActive\":true,\"activeSlot\":%u,\"generation\":%u,\"payloadFormat\":%u,\"schemaVersion\":%u,\"payloadSize\":%u,\"payloadCrc32\":\"0x%08lx\",\"importSessionActive\":%s,\"importExpectedSize\":%u,\"importExpectedCrc32\":\"0x%08lx\",\"importSchemaVersion\":%u,\"importBytesWritten\":%u,\"importRemainingBytes\":%u,\"importPayloadComplete\":%s,\"importPayloadCrc32Matches\":%s,\"importLastResult\":\"%s\",\"importMaxChunkBytes\":%u}",
                       (unsigned)running_revision,
                       (unsigned)staged_revision,
                       reboot_required ? "true" : "false",
                       running_revision != staged_revision ? "true" : "false",
                       (unsigned)info.slot_index,
                       (unsigned)info.generation,
                       (unsigned)info.payload_format,
                       (unsigned)info.schema_version,
                       (unsigned)info.payload_size,
                       (unsigned long)info.payload_crc32,
                       import_status.session_active ? "true" : "false",
                       (unsigned)import_status.expected_payload_size,
                       (unsigned long)import_status.expected_payload_crc32,
                       (unsigned)import_status.schema_version,
                       (unsigned)import_status.bytes_written,
                       (unsigned)import_status.remaining_bytes,
                       import_status.payload_complete ? "true" : "false",
                       import_status.payload_crc32_matches ? "true" : "false",
                       config_import_service_result_to_string(import_status.last_result),
                       (unsigned)CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES);
    return written > 0 && (size_t)written < buffer_size;
}




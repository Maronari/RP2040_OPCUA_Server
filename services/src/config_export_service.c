#include "services/config_export_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "services/config_import_service.h"
#include "services/config_service.h"
#include "services/config_store_service.h"

typedef struct
{
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} config_json_writer_t;

static const char *config_export_network_mode_to_string(network_mode_t mode)
{
    return mode == NETWORK_MODE_STATIC ? "static" : "dhcp";
}

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

static void config_json_writer_append_raw(config_json_writer_t *writer, const char *text)
{
    int written = 0;

    if (writer == NULL || writer->overflow || text == NULL)
    {
        return;
    }

    written = snprintf(writer->buffer + writer->length,
                       writer->capacity - writer->length,
                       "%s",
                       text);
    if (written < 0 || (size_t)written >= (writer->capacity - writer->length))
    {
        writer->overflow = true;
        return;
    }

    writer->length += (size_t)written;
}

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

static void config_json_writer_append_quoted(config_json_writer_t *writer, const char *text)
{
    const char *cursor = text != NULL ? text : "";

    if (writer == NULL || writer->overflow)
    {
        return;
    }

    config_json_writer_append_raw(writer, "\"");
    while (*cursor != '\0' && !writer->overflow)
    {
        switch (*cursor)
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
            config_json_writer_append_format(writer, "%c", *cursor);
            break;
        }

        cursor++;
    }
    config_json_writer_append_raw(writer, "\"");
}

static void config_json_writer_append_ipv4(config_json_writer_t *writer, const uint8_t address[4])
{
    if (address == NULL)
    {
        config_json_writer_append_quoted(writer, "0.0.0.0");
        return;
    }

    config_json_writer_append_format(writer,
                                     "\"%u.%u.%u.%u\"",
                                     (unsigned)address[0],
                                     (unsigned)address[1],
                                     (unsigned)address[2],
                                     (unsigned)address[3]);
}

static void config_json_writer_append_mac(config_json_writer_t *writer, const uint8_t mac[6])
{
    if (mac == NULL)
    {
        config_json_writer_append_quoted(writer, "00:00:00:00:00:00");
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

    config_json_writer_append_raw(&writer, "\"schemaVersion\":1,");
    config_json_writer_append_raw(&writer, "\"device\":{");
    config_json_writer_append_raw(&writer, "\"manufacturer\":");
    config_json_writer_append_quoted(&writer, config->manufacturer);
    config_json_writer_append_raw(&writer, ",\"model\":");
    config_json_writer_append_quoted(&writer, config->model);
    config_json_writer_append_raw(&writer, ",\"deviceName\":");
    config_json_writer_append_quoted(&writer, config->device_name);
    config_json_writer_append_raw(&writer, ",\"opcuaHostname\":");
    config_json_writer_append_quoted(&writer, config->opcua_hostname);
    config_json_writer_append_raw(&writer, ",\"serialNumber\":");
    config_json_writer_append_quoted(&writer, config->serial_number);
    config_json_writer_append_raw(&writer, ",\"productInstanceUri\":");
    config_json_writer_append_quoted(&writer, config->product_instance_uri);
    config_json_writer_append_raw(&writer, ",\"assetId\":");
    config_json_writer_append_quoted(&writer, config->asset_id);
    config_json_writer_append_raw(&writer, ",\"hwRevision\":");
    config_json_writer_append_quoted(&writer, config->hw_revision);
    config_json_writer_append_raw(&writer, ",\"fwRevision\":");
    config_json_writer_append_quoted(&writer, config->fw_revision);
    config_json_writer_append_raw(&writer, ",\"buildTime\":");
    config_json_writer_append_quoted(&writer, config->build_time);
    config_json_writer_append_raw(&writer, ",\"gitHash\":");
    config_json_writer_append_quoted(&writer, config->git_hash);
    config_json_writer_append_raw(&writer, ",\"location\":");
    config_json_writer_append_quoted(&writer, config->location);
    config_json_writer_append_raw(&writer, ",\"assetClass\":");
    config_json_writer_append_quoted(&writer, config->asset_class);
    config_json_writer_append_format(&writer,
                                     ",\"manufactureDateUtc\":%lld,\"calibrationDateUtc\":%lld},",
                                     (long long)config->manufacture_date_utc,
                                     (long long)config->calibration_date_utc);

    config_json_writer_append_raw(&writer, "\"network\":{");
    config_json_writer_append_raw(&writer, "\"mode\":");
    config_json_writer_append_quoted(&writer, config_export_network_mode_to_string(config->network_mode));
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
    config_json_writer_append_quoted(&writer, config->ntp_server);
    config_json_writer_append_format(&writer,
                                     ",\"timezoneOffsetMinutes\":%d},",
                                     (int)config->timezone_offset_minutes);

    config_json_writer_append_raw(&writer, "\"mqtt\":{");
    config_json_writer_append_format(&writer,
                                     "\"enabled\":%s,\"useJson\":%s,",
                                     config->enable_mqtt_pubsub ? "true" : "false",
                                     config->mqtt_pubsub_use_json ? "true" : "false");
    config_json_writer_append_raw(&writer, "\"brokerUrl\":");
    config_json_writer_append_quoted(&writer, config->mqtt_broker_url);
    config_json_writer_append_raw(&writer, ",\"topicRoot\":");
    config_json_writer_append_quoted(&writer, config->mqtt_topic_root);
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
    for (size_t index = 0u; index < CONFIG_SERVICE_MAX_CHANNELS; ++index)
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
        config_json_writer_append_quoted(&writer, channel->id);
        config_json_writer_append_raw(&writer, ",\"name\":");
        config_json_writer_append_quoted(&writer, channel->name);
        config_json_writer_append_raw(&writer, ",\"description\":");
        config_json_writer_append_quoted(&writer, channel->description);
        config_json_writer_append_raw(&writer, ",\"unit\":");
        config_json_writer_append_quoted(&writer, channel->unit);
        config_json_writer_append_format(&writer,
                                         ",\"flags\":%lu,\"minimum\":%.6f,\"maximum\":%.6f,\"hasMinimum\":%s,\"hasMaximum\":%s,"
                                         "\"enabled\":%s,\"samplePeriodMs\":%lu,\"publishPeriodMs\":%lu,\"historyIntervalMs\":%lu,"
                                         "\"scale\":%.6f,\"offset\":%.6f,\"warningThreshold\":%.6f,\"criticalThreshold\":%.6f}",
                                         (unsigned long)channel->flags,
                                         channel->minimum,
                                         channel->maximum,
                                         channel->has_minimum != 0u ? "true" : "false",
                                         channel->has_maximum != 0u ? "true" : "false",
                                         channel->settings.enabled ? "true" : "false",
                                         (unsigned long)channel->settings.sample_period_ms,
                                         (unsigned long)channel->settings.publish_period_ms,
                                         (unsigned long)channel->settings.history_interval_ms,
                                         channel->settings.scale,
                                         channel->settings.offset,
                                         channel->settings.warning_threshold,
                                         channel->settings.critical_threshold);
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




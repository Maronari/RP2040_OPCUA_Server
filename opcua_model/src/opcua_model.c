#include "opcua_model/opcua_model.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "open62541.h"
#include "services/alarm_service.h"
#include "services/config_service.h"
#include "services/diag_service.h"
#include "services/network_manager.h"
#include "services/sensor_manager.h"
#include "services/time_service.h"

typedef enum
{
    MODEL_NODE_IDENT_MANUFACTURER = 0,
    MODEL_NODE_IDENT_MODEL,
    MODEL_NODE_IDENT_SERIAL_NUMBER,
    MODEL_NODE_IDENT_HW_REVISION,
    MODEL_NODE_IDENT_FW_REVISION,
    MODEL_NODE_IDENT_BUILD_TIME,
    MODEL_NODE_IDENT_GIT_HASH,
    MODEL_NODE_IDENT_DEVICE_NAME,
    MODEL_NODE_PROCESS_TEMPERATURE_VALUE,
    MODEL_NODE_PROCESS_TEMPERATURE_STATUS,
    MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP,
    MODEL_NODE_PROCESS_TEMPERATURE_UNITS,
    MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS,
    MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE,
    MODEL_NODE_DIAG_UPTIME_SEC,
    MODEL_NODE_DIAG_RESET_CAUSE,
    MODEL_NODE_DIAG_FREE_HEAP,
    MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP,
    MODEL_NODE_DIAG_CPU_LOAD_APPROX,
    MODEL_NODE_DIAG_LAST_FAULT_CODE,
    MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER,
    MODEL_NODE_NETWORK_MAC_ADDRESS,
    MODEL_NODE_NETWORK_IP_MODE,
    MODEL_NODE_NETWORK_IPV4,
    MODEL_NODE_NETWORK_MASK,
    MODEL_NODE_NETWORK_GATEWAY,
    MODEL_NODE_NETWORK_LINK_UP,
    MODEL_NODE_NETWORK_DHCP_STATE,
    MODEL_NODE_NETWORK_RECONNECT_COUNT,
    MODEL_NODE_TIME_SYNCED,
    MODEL_NODE_TIME_NTP_SERVER,
    MODEL_NODE_TIME_LAST_SYNC_EPOCH,
    MODEL_NODE_TIME_CLOCK_QUALITY,
    MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS,
    MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS,
    MODEL_NODE_CONFIG_TEMP_OFFSET,
    MODEL_NODE_CONFIG_TEMP_SCALE,
    MODEL_NODE_CONFIG_HIGH_ALARM,
    MODEL_NODE_CONFIG_HIGH_HIGH_ALARM
} model_node_kind_t;

typedef struct
{
    model_node_kind_t kind;
    const char *task_name;
} model_node_context_t;

enum
{
    FIXED_CTX_IDENT_MANUFACTURER = 0,
    FIXED_CTX_IDENT_MODEL,
    FIXED_CTX_IDENT_SERIAL_NUMBER,
    FIXED_CTX_IDENT_HW_REVISION,
    FIXED_CTX_IDENT_FW_REVISION,
    FIXED_CTX_IDENT_BUILD_TIME,
    FIXED_CTX_IDENT_GIT_HASH,
    FIXED_CTX_IDENT_DEVICE_NAME,
    FIXED_CTX_PROCESS_TEMPERATURE_VALUE,
    FIXED_CTX_PROCESS_TEMPERATURE_STATUS,
    FIXED_CTX_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP,
    FIXED_CTX_PROCESS_TEMPERATURE_UNITS,
    FIXED_CTX_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS,
    FIXED_CTX_PROCESS_TEMPERATURE_ALARM_STATE,
    FIXED_CTX_DIAG_UPTIME_SEC,
    FIXED_CTX_DIAG_RESET_CAUSE,
    FIXED_CTX_DIAG_FREE_HEAP,
    FIXED_CTX_DIAG_MIN_EVER_FREE_HEAP,
    FIXED_CTX_DIAG_CPU_LOAD_APPROX,
    FIXED_CTX_DIAG_LAST_FAULT_CODE,
    FIXED_CTX_NETWORK_MAC_ADDRESS,
    FIXED_CTX_NETWORK_IP_MODE,
    FIXED_CTX_NETWORK_IPV4,
    FIXED_CTX_NETWORK_MASK,
    FIXED_CTX_NETWORK_GATEWAY,
    FIXED_CTX_NETWORK_LINK_UP,
    FIXED_CTX_NETWORK_DHCP_STATE,
    FIXED_CTX_NETWORK_RECONNECT_COUNT,
    FIXED_CTX_TIME_SYNCED,
    FIXED_CTX_TIME_NTP_SERVER,
    FIXED_CTX_TIME_LAST_SYNC_EPOCH,
    FIXED_CTX_TIME_CLOCK_QUALITY,
    FIXED_CTX_CONFIG_SAMPLING_PERIOD_MS,
    FIXED_CTX_CONFIG_PUBLISH_PERIOD_MS,
    FIXED_CTX_CONFIG_TEMP_OFFSET,
    FIXED_CTX_CONFIG_TEMP_SCALE,
    FIXED_CTX_CONFIG_HIGH_ALARM,
    FIXED_CTX_CONFIG_HIGH_HIGH_ALARM,
    FIXED_CTX_COUNT
};

#define OPCUA_SEND_BUFFER_SIZE 8192u
#define OPCUA_RECV_BUFFER_SIZE 8192u

static void log_opcua_heap(const char *stage)
{
    printf("[OPC UA]\t%s, free heap=%u, min ever free=%u\n",
           stage,
           (unsigned)xPortGetFreeHeapSize(),
           (unsigned)xPortGetMinimumEverFreeHeapSize());
    fflush(stdout);
}

static opcua_model_config_t g_model_config;
static bool g_model_initialized = false;
static model_node_context_t g_fixed_node_contexts[FIXED_CTX_COUNT] = {
    {MODEL_NODE_IDENT_MANUFACTURER, NULL},
    {MODEL_NODE_IDENT_MODEL, NULL},
    {MODEL_NODE_IDENT_SERIAL_NUMBER, NULL},
    {MODEL_NODE_IDENT_HW_REVISION, NULL},
    {MODEL_NODE_IDENT_FW_REVISION, NULL},
    {MODEL_NODE_IDENT_BUILD_TIME, NULL},
    {MODEL_NODE_IDENT_GIT_HASH, NULL},
    {MODEL_NODE_IDENT_DEVICE_NAME, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_VALUE, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_STATUS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_UNITS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS, NULL},
    {MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE, NULL},
    {MODEL_NODE_DIAG_UPTIME_SEC, NULL},
    {MODEL_NODE_DIAG_RESET_CAUSE, NULL},
    {MODEL_NODE_DIAG_FREE_HEAP, NULL},
    {MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP, NULL},
    {MODEL_NODE_DIAG_CPU_LOAD_APPROX, NULL},
    {MODEL_NODE_DIAG_LAST_FAULT_CODE, NULL},
    {MODEL_NODE_NETWORK_MAC_ADDRESS, NULL},
    {MODEL_NODE_NETWORK_IP_MODE, NULL},
    {MODEL_NODE_NETWORK_IPV4, NULL},
    {MODEL_NODE_NETWORK_MASK, NULL},
    {MODEL_NODE_NETWORK_GATEWAY, NULL},
    {MODEL_NODE_NETWORK_LINK_UP, NULL},
    {MODEL_NODE_NETWORK_DHCP_STATE, NULL},
    {MODEL_NODE_NETWORK_RECONNECT_COUNT, NULL},
    {MODEL_NODE_TIME_SYNCED, NULL},
    {MODEL_NODE_TIME_NTP_SERVER, NULL},
    {MODEL_NODE_TIME_LAST_SYNC_EPOCH, NULL},
    {MODEL_NODE_TIME_CLOCK_QUALITY, NULL},
    {MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS, NULL},
    {MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS, NULL},
    {MODEL_NODE_CONFIG_TEMP_OFFSET, NULL},
    {MODEL_NODE_CONFIG_TEMP_SCALE, NULL},
    {MODEL_NODE_CONFIG_HIGH_ALARM, NULL},
    {MODEL_NODE_CONFIG_HIGH_HIGH_ALARM, NULL}};
static model_node_context_t g_task_node_contexts[3] = {
    {MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER, "OpcUaTask"},
    {MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER, "NetworkTask"},
    {MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER, "SensorTask"}};

static UA_StatusCode model_quality_to_status(service_quality_t quality)
{
    switch (quality)
    {
    case SERVICE_QUALITY_GOOD:
        return UA_STATUSCODE_GOOD;
    case SERVICE_QUALITY_UNCERTAIN:
        return UA_STATUSCODE_UNCERTAIN;
    case SERVICE_QUALITY_BAD:
    default:
        return UA_STATUSCODE_BADNOTCONNECTED;
    }
}

static const char *model_quality_to_string(service_quality_t quality)
{
    switch (quality)
    {
    case SERVICE_QUALITY_GOOD:
        return "Good";
    case SERVICE_QUALITY_UNCERTAIN:
        return "Uncertain";
    case SERVICE_QUALITY_BAD:
    default:
        return "Bad";
    }
}

static const char *model_alarm_state_to_string(temperature_alarm_state_t alarm_state)
{
    switch (alarm_state)
    {
    case TEMPERATURE_ALARM_STATE_NORMAL:
        return "Normal";
    case TEMPERATURE_ALARM_STATE_HIGH:
        return "High";
    case TEMPERATURE_ALARM_STATE_HIGH_HIGH:
        return "HighHigh";
    case TEMPERATURE_ALARM_STATE_INVALID:
    default:
        return "Invalid";
    }
}

static void model_apply_now_source_timestamp(UA_Boolean include_source_timestamp, UA_DataValue *value)
{
    if (!include_source_timestamp)
    {
        return;
    }

    value->hasSourceTimestamp = true;
    value->sourceTimestamp = UA_DateTime_now();
}

static void model_apply_epoch_source_timestamp(UA_Boolean include_source_timestamp, uint32_t epoch_seconds, UA_DataValue *value)
{
    if (!include_source_timestamp)
    {
        return;
    }

    value->hasSourceTimestamp = true;
    if (epoch_seconds > 0u)
    {
        value->sourceTimestamp = UA_DateTime_fromUnixTime((UA_Int64)epoch_seconds);
    }
    else
    {
        value->sourceTimestamp = UA_DateTime_now();
    }
}

static UA_StatusCode model_set_uint32_value(UA_DataValue *value, UA_UInt32 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_UINT32]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_float_value(UA_DataValue *value, UA_Float data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_FLOAT]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_boolean_value(UA_DataValue *value, UA_Boolean data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_BOOLEAN]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_string_value(UA_DataValue *value, const char *data, UA_StatusCode status)
{
    UA_String string_value = UA_STRING((char *)(data != NULL ? data : ""));
    UA_Variant_setScalarCopy(&value->value, &string_value, &UA_TYPES[UA_TYPES_STRING]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_set_datetime_value(UA_DataValue *value, UA_DateTime data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_DATETIME]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

static void model_set_default_variant_value(UA_VariableAttributes *attributes, UA_NodeId data_type)
{
    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_BOOLEAN].typeId))
    {
        UA_Boolean default_value = false;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_BOOLEAN]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_UINT32].typeId))
    {
        UA_UInt32 default_value = 0u;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_UINT32]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_FLOAT].typeId))
    {
        UA_Float default_value = 0.0f;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_FLOAT]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_DATETIME].typeId))
    {
        UA_DateTime default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_DATETIME]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_STRING].typeId))
    {
        UA_String default_value = UA_STRING((char *)"");
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_STRING]);
    }
}

static UA_StatusCode model_read_value(UA_Server *server,
                                      const UA_NodeId *session_id,
                                      void *session_context,
                                      const UA_NodeId *node_id,
                                      void *node_context,
                                      UA_Boolean include_source_timestamp,
                                      const UA_NumericRange *range,
                                      UA_DataValue *value)
{
    model_node_context_t *context = (model_node_context_t *)node_context;
    const device_profile_config_t *config = config_service_get();
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)node_id;

    if (context == NULL)
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINTERNALERROR;
        return UA_STATUSCODE_GOOD;
    }

    if (range != NULL)
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

    switch (context->kind)
    {
    case MODEL_NODE_IDENT_MANUFACTURER:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->manufacturer, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_MODEL:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->model, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_SERIAL_NUMBER:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->serial_number, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_HW_REVISION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->hw_revision, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_FW_REVISION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->fw_revision, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_BUILD_TIME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->build_time, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_GIT_HASH:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->git_hash, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_DEVICE_NAME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->device_name, UA_STATUSCODE_GOOD);
    case MODEL_NODE_PROCESS_TEMPERATURE_VALUE:
    {
        sensor_manager_snapshot_t snapshot = {0};
        sensor_manager_get_snapshot(&snapshot);
        model_apply_epoch_source_timestamp(include_source_timestamp, snapshot.source_timestamp_epoch_s, value);
        return model_set_float_value(value, snapshot.temperature_c, model_quality_to_status(snapshot.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_STATUS:
    {
        sensor_manager_snapshot_t snapshot = {0};
        sensor_manager_get_snapshot(&snapshot);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_quality_to_string(snapshot.quality),
                                      model_quality_to_status(snapshot.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP:
    {
        sensor_manager_snapshot_t snapshot = {0};
        UA_DateTime timestamp = 0;
        sensor_manager_get_snapshot(&snapshot);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (snapshot.source_timestamp_epoch_s > 0u)
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)snapshot.source_timestamp_epoch_s);
        }
        return model_set_datetime_value(value, timestamp, model_quality_to_status(snapshot.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_UNITS:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, "degC", UA_STATUSCODE_GOOD);
    case MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, config->sampling_period_ms, UA_STATUSCODE_GOOD);
    case MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE:
    {
        alarm_service_status_t status = {0};
        alarm_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_alarm_state_to_string(status.alarm_state),
                                      model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_IPV4:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ipv4_address, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_MAC_ADDRESS:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.mac_address, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_IP_MODE:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ip_mode, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_NETWORK_MASK:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.netmask, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_GATEWAY:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.gateway, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_LINK_UP:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, status.link_up, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_DHCP_STATE:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.dhcp_state, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_NETWORK_RECONNECT_COUNT:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, status.reconnect_count, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_TIME_LAST_SYNC_EPOCH:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, status.last_sync_epoch_s, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_SYNCED:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, status.synchronized, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_NTP_SERVER:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ntp_server, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_TIME_CLOCK_QUALITY:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_quality_to_string(status.quality),
                                      model_quality_to_status(status.quality));
    }
    case MODEL_NODE_DIAG_FREE_HEAP:
    {
        diag_service_heap_stats_t stats = {0};
        diag_service_get_heap_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.available_heap_bytes, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP:
    {
        diag_service_heap_stats_t stats = {0};
        diag_service_get_heap_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.minimum_ever_free_bytes, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_UPTIME_SEC:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.uptime_sec, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_RESET_CAUSE:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, stats.reset_cause, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_CPU_LOAD_APPROX:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.cpu_like_load_approx, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_LAST_FAULT_CODE:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.last_fault_code, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER:
    {
        diag_service_task_info_t task_info = {0};
        if (context->task_name == NULL || !diag_service_get_task_info_by_name(context->task_name, &task_info))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNOTFOUND;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, task_info.stack_high_water_mark, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, config->sampling_period_ms, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, config->publish_period_ms, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_TEMP_OFFSET:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_float_value(value, config->temp_offset_c, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_TEMP_SCALE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_float_value(value, config->temp_scale, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_HIGH_ALARM:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_float_value(value, config->high_temperature_threshold_c, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_HIGH_HIGH_ALARM:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_float_value(value, config->high_high_temperature_threshold_c, UA_STATUSCODE_GOOD);
    default:
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNOTSUPPORTED;
        return UA_STATUSCODE_GOOD;
    }
}

static UA_StatusCode model_acknowledge_alarms(UA_Server *server,
                                              const UA_NodeId *session_id,
                                              void *session_context,
                                              const UA_NodeId *method_id,
                                              void *method_context,
                                              const UA_NodeId *object_id,
                                              void *object_context,
                                              size_t input_size,
                                              const UA_Variant *input,
                                              size_t output_size,
                                              UA_Variant *output)
{
    UA_Boolean result = true;
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    alarm_service_acknowledge_all();

    if (output_size > 0)
    {
        UA_Variant_setScalarCopy(&output[0], &result, &UA_TYPES[UA_TYPES_BOOLEAN]);
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_add_object(UA_Server *server,
                                      const char *node_id,
                                      const char *browse_name,
                                      const char *display_name,
                                      UA_NodeId parent_node_id,
                                      UA_NodeId reference_type_id,
                                      UA_NodeId type_definition_id)
{
    UA_ObjectAttributes attributes = UA_ObjectAttributes_default;
    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);

    return UA_Server_addObjectNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   parent_node_id,
                                   reference_type_id,
                                   UA_QUALIFIEDNAME(1, (char *)browse_name),
                                   type_definition_id,
                                   attributes,
                                   NULL,
                                   NULL);
}

static UA_StatusCode model_add_folder(UA_Server *server,
                                      const char *node_id,
                                      const char *display_name,
                                      UA_NodeId parent_node_id,
                                      UA_NodeId reference_type_id)
{
    return model_add_object(server,
                            node_id,
                            display_name,
                            display_name,
                            parent_node_id,
                            reference_type_id,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE));
}

static UA_StatusCode model_add_readonly_variable(UA_Server *server,
                                                 const char *node_id,
                                                 const char *display_name,
                                                 UA_NodeId parent_node_id,
                                                 UA_NodeId data_type,
                                                 model_node_context_t *context)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_DataSource data_source;
    UA_NodeId created_node_id = UA_NODEID_NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    memset(&data_source, 0, sizeof(data_source));
    data_source.read = model_read_value;
    data_source.write = NULL;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.dataType = data_type;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    model_set_default_variant_value(&attributes, data_type);

    status = UA_Server_addVariableNode(server,
                                       UA_NODEID_STRING(1, (char *)node_id),
                                       parent_node_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                       UA_QUALIFIEDNAME(1, (char *)display_name),
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                                       attributes,
                                       context,
                                       &created_node_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return UA_Server_setVariableNode_dataSource(server, created_node_id, data_source);
}

static UA_StatusCode model_add_alarm_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_MethodAttributes attributes = UA_MethodAttributes_default;
    UA_Argument output_argument;

    UA_Argument_init(&output_argument);
    output_argument.name = UA_STRING((char *)"Success");
    output_argument.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"True when alarms are acknowledged");
    output_argument.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
    output_argument.valueRank = UA_VALUERANK_SCALAR;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"AcknowledgeAllAlarms");
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"Acknowledges all active alarms");
    attributes.executable = true;
    attributes.userExecutable = true;

    return UA_Server_addMethodNode(server,
                                   UA_NODEID_STRING(1, (char *)"Device.Maintenance.AcknowledgeAllAlarms"),
                                   maintenance_folder_id,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(1, (char *)"AcknowledgeAllAlarms"),
                                   attributes,
                                   model_acknowledge_alarms,
                                   0,
                                   NULL,
                                   1,
                                   &output_argument,
                                   NULL,
                                   NULL);
}

static UA_StatusCode model_add_task_stack_high_water_node(UA_Server *server,
                                                          UA_NodeId task_folder_id,
                                                          const char *task_object_id,
                                                          const char *task_display_name,
                                                          model_node_context_t *task_context)
{
    char stack_node_id[96] = {0};
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    status = model_add_object(server,
                              task_object_id,
                              task_display_name,
                              task_display_name,
                              task_folder_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    snprintf(stack_node_id, sizeof(stack_node_id), "%s.StackHighWatermark", task_object_id);

    return model_add_readonly_variable(server,
                                       stack_node_id,
                                       "StackHighWatermark",
                                       UA_NODEID_STRING(1, (char *)task_object_id),
                                       UA_TYPES[UA_TYPES_UINT32].typeId,
                                       task_context);
}

static UA_StatusCode model_build_address_space(UA_Server *server)
{
    const device_profile_config_t *device_config = config_service_get();
    const char *device_display_name = device_config->device_name[0] != '\0' ? device_config->device_name : "Device";
    UA_NodeId device_id = UA_NODEID_STRING(1, (char *)"Device");
    UA_NodeId identification_id = UA_NODEID_STRING(1, (char *)"Device.Identification");
    UA_NodeId process_values_id = UA_NODEID_STRING(1, (char *)"Device.ProcessValues");
    UA_NodeId temperature1_id = UA_NODEID_STRING(1, (char *)"Device.ProcessValues.Temperature1");
    UA_NodeId diagnostics_id = UA_NODEID_STRING(1, (char *)"Device.Diagnostics");
    UA_NodeId task_id = UA_NODEID_STRING(1, (char *)"Device.Diagnostics.Task");
    UA_NodeId network_id = UA_NODEID_STRING(1, (char *)"Device.Network");
    UA_NodeId time_id = UA_NODEID_STRING(1, (char *)"Device.Time");
    UA_NodeId configuration_id = UA_NODEID_STRING(1, (char *)"Device.Configuration");
    UA_NodeId maintenance_id = UA_NODEID_STRING(1, (char *)"Device.Maintenance");
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    status = model_add_object(server,
                              "Device",
                              "Device",
                              device_display_name,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Identification", "Identification", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.ProcessValues", "ProcessValues", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Diagnostics", "Diagnostics", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Network", "Network", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Time", "Time", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Configuration", "Configuration", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Maintenance", "Maintenance", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_object(server,
                              "Device.ProcessValues.Temperature1",
                              "Temperature1",
                              "Temperature1",
                              process_values_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.Manufacturer",
                                         "Manufacturer",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MANUFACTURER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.Model",
                                         "Model",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MODEL]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.SerialNumber",
                                         "SerialNumber",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_SERIAL_NUMBER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.HWRevision",
                                         "HWRevision",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_HW_REVISION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.FWRevision",
                                         "FWRevision",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_FW_REVISION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.BuildTime",
                                         "BuildTime",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_BUILD_TIME]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.GitHash",
                                         "GitHash",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_GIT_HASH]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.DeviceName",
                                         "DeviceName",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_DEVICE_NAME]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.ProcessValues.Temperature1.Value",
                                         "Value",
                                         temperature1_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_PROCESS_TEMPERATURE_VALUE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.ProcessValues.Temperature1.Status",
                                         "Status",
                                         temperature1_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_PROCESS_TEMPERATURE_STATUS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.ProcessValues.Temperature1.SourceTimestamp",
                                         "SourceTimestamp",
                                         temperature1_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.ProcessValues.Temperature1.Units",
                                         "Units",
                                         temperature1_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_PROCESS_TEMPERATURE_UNITS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.ProcessValues.Temperature1.UpdatePeriodMs",
                                         "UpdatePeriodMs",
                                         temperature1_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.ProcessValues.Temperature1.AlarmState",
                                         "AlarmState",
                                         temperature1_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_PROCESS_TEMPERATURE_ALARM_STATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.UptimeSec",
                                         "UptimeSec",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_UPTIME_SEC]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.ResetCause",
                                         "ResetCause",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_RESET_CAUSE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (g_model_config.enable_heap_diagnostics)
    {
        status = model_add_readonly_variable(server,
                                             "Device.Diagnostics.FreeHeap",
                                             "FreeHeap",
                                             diagnostics_id,
                                             UA_TYPES[UA_TYPES_UINT32].typeId,
                                             &g_fixed_node_contexts[FIXED_CTX_DIAG_FREE_HEAP]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_readonly_variable(server,
                                             "Device.Diagnostics.MinEverFreeHeap",
                                             "MinEverFreeHeap",
                                             diagnostics_id,
                                             UA_TYPES[UA_TYPES_UINT32].typeId,
                                             &g_fixed_node_contexts[FIXED_CTX_DIAG_MIN_EVER_FREE_HEAP]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.CpuLikeLoadApprox",
                                         "CpuLikeLoadApprox",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_CPU_LOAD_APPROX]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.LastFaultCode",
                                         "LastFaultCode",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_LAST_FAULT_CODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (g_model_config.enable_task_diagnostics)
    {
        status = model_add_folder(server, "Device.Diagnostics.Task", "Task", diagnostics_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_task_stack_high_water_node(server,
                                                      task_id,
                                                      "Device.Diagnostics.Task.OPC",
                                                      "OPC",
                                                      &g_task_node_contexts[0]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_task_stack_high_water_node(server,
                                                      task_id,
                                                      "Device.Diagnostics.Task.SPI",
                                                      "SPI",
                                                      &g_task_node_contexts[1]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_task_stack_high_water_node(server,
                                                      task_id,
                                                      "Device.Diagnostics.Task.TEMP",
                                                      "TEMP",
                                                      &g_task_node_contexts[2]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.MacAddress",
                                         "MacAddress",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_MAC_ADDRESS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.IpMode",
                                         "IpMode",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_IP_MODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.IPv4",
                                         "IPv4",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_IPV4]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.Mask",
                                         "Mask",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_MASK]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.Gateway",
                                         "Gateway",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_GATEWAY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.LinkUp",
                                         "LinkUp",
                                         network_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_LINK_UP]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.DhcpState",
                                         "DhcpState",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_DHCP_STATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Network.ReconnectCount",
                                         "ReconnectCount",
                                         network_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_RECONNECT_COUNT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.TimeSynced",
                                         "TimeSynced",
                                         time_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_SYNCED]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.NtpServer",
                                         "NtpServer",
                                         time_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_NTP_SERVER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.LastSyncEpoch",
                                         "LastSyncEpoch",
                                         time_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_LAST_SYNC_EPOCH]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Time.ClockQuality",
                                         "ClockQuality",
                                         time_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_CLOCK_QUALITY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.SamplingPeriodMs",
                                         "SamplingPeriodMs",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_SAMPLING_PERIOD_MS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.PublishPeriodMs",
                                         "PublishPeriodMs",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_PUBLISH_PERIOD_MS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.TempOffset",
                                         "TempOffset",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_TEMP_OFFSET]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.TempScale",
                                         "TempScale",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_TEMP_SCALE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.HighAlarm",
                                         "HighAlarm",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_HIGH_ALARM]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.HighHighAlarm",
                                         "HighHighAlarm",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_HIGH_HIGH_ALARM]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (g_model_config.enable_alarm_methods)
    {
        status = model_add_alarm_method_node(server, maintenance_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    return UA_STATUSCODE_GOOD;
}

bool opcua_model_init(const opcua_model_config_t *config)
{
    if (config == NULL)
    {
        return false;
    }

    g_model_config = *config;
    g_model_initialized = true;
    return true;
}

void opcua_model_task(void *argument)
{
    UA_Server *server = NULL;
    UA_ServerConfig *config = NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;
    bool wait_for_network_logged = false;
    (void)argument;

    if (!g_model_initialized)
    {
        printf("[OPC UA]\tModel is not initialized\n");
        fflush(stdout);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Task started");
    while (!network_manager_is_ready())
    {
        if (!wait_for_network_logged)
        {
            printf("[OPC UA]\tWaiting for IPv4 before server startup\n");
            fflush(stdout);
            wait_for_network_logged = true;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("[OPC UA]\tIPv4 is ready, continuing startup\n");
    fflush(stdout);
    server = UA_Server_new();
    if (server == NULL)
    {
        printf("[OPC UA]\tUA_Server_new returned NULL\n");
        fflush(stdout);
        vTaskDelete(NULL);
    }

    log_opcua_heap("UA_Server_new completed");
    config = UA_Server_getConfig(server);
    status = UA_ServerConfig_setMinimalCustomBuffer(config,
                                                    g_model_config.port,
                                                    0,
                                                    OPCUA_SEND_BUFFER_SIZE,
                                                    OPCUA_RECV_BUFFER_SIZE);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_ServerConfig_setMinimalCustomBuffer failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_Server_delete(server);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Network layer configured");

    /* Embedded profile: keep the default server configuration modest.
       This avoids spending heap on limits that are irrelevant for a single
       local device endpoint. */
    config->maxSecureChannels = 2;
    config->maxSessions = 4;
    config->maxSubscriptions = 8;
    config->maxSubscriptionsPerSession = 4;
    config->maxMonitoredItems = 64;
    config->maxMonitoredItemsPerSubscription = 32;
    config->maxNotificationsPerPublish = 32;
    config->enableRetransmissionQueue = false;

    if (config->accessControl.clear != NULL)
    {
        config->accessControl.clear(&config->accessControl);
        memset(&config->accessControl, 0, sizeof(config->accessControl));
    }

    status = UA_AccessControl_default(config,
                                      true,
                                      NULL,
                                      &UA_SECURITY_POLICY_NONE_URI,
                                      0,
                                      NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_AccessControl_default failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_Server_delete(server);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Access control configured");

    if (g_model_config.hostname != NULL && g_model_config.hostname[0] != '\0')
    {
        network_manager_status_t network_status = {0};
        const char *hostname_text = g_model_config.hostname;

        network_manager_get_status(&network_status);
        if (network_status.has_ipv4 && network_status.ipv4_address[0] != '\0')
        {
            hostname_text = network_status.ipv4_address;
        }

        UA_String hostname = UA_STRING((char *)hostname_text);
        UA_String_clear(&config->customHostname);
        UA_String_copy(&hostname, &config->customHostname);
        printf("[OPC UA]\tUsing endpoint host %s\n", hostname_text);
        fflush(stdout);
    }

    status = model_build_address_space(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tFailed to build address space: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_Server_delete(server);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Address space ready");
    printf("[OPC UA]\tStarting server on port %u\n", (unsigned)g_model_config.port);
    fflush(stdout);

    status = UA_Server_run_startup(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_Server_run_startup failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_Server_delete(server);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Server startup complete");

    while (1)
    {
        UA_UInt16 wait_ms = UA_Server_run_iterate(server, false);
        if (wait_ms == 0u)
        {
            wait_ms = 10u;
        }
        else if (wait_ms > 100u)
        {
            wait_ms = 100u;
        }

        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    status = UA_Server_run_shutdown(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_Server_run_shutdown failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }

    UA_Server_delete(server);
    vTaskDelete(NULL);
}



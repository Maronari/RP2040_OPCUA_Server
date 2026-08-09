#include "opcua_model_internal.h"

const char *model_log_level_name(UA_LogLevel level)
{
    switch (level)
    {
    case UA_LOGLEVEL_FATAL: return "fatal";
    case UA_LOGLEVEL_ERROR: return "error";
    case UA_LOGLEVEL_WARNING: return "warn";
    case UA_LOGLEVEL_INFO: return "info";
    case UA_LOGLEVEL_DEBUG: return "debug";
    case UA_LOGLEVEL_TRACE: return "trace";
    default: return "log";
    }
}

void model_ua_logger_log(void *context,
                                UA_LogLevel level,
                                UA_LogCategory category,
                                const char *msg,
                                va_list args)
{
    char message_buffer[768];
    UA_String formatted = {
        .length = sizeof(message_buffer) - 1u,
        .data = (UA_Byte *)message_buffer,
    };
    const UA_StatusCode status = UA_String_vformat(&formatted, msg, args);

    (void)context;
    (void)category;

    if (formatted.length >= sizeof(message_buffer))
    {
        formatted.length = sizeof(message_buffer) - 1u;
    }
    message_buffer[formatted.length] = '\0';

    if (status == UA_STATUSCODE_GOOD ||
        status == UA_STATUSCODE_BADENCODINGLIMITSEXCEEDED)
    {
        printf("[open62541/%s]\t%.*s\n",
               model_log_level_name(level),
               (int)formatted.length,
               message_buffer);
    }
    else
    {
        printf("[open62541/%s]\t<log formatting failed: 0x%08lx>\n",
               model_log_level_name(level),
               (unsigned long)status);
    }
    stdio_flush();
}

void log_opcua_heap(const char *stage)
{
    printf("[OPC UA]\t%s, free heap=%u, min ever free=%u\n",
           stage,
           (unsigned)xPortGetFreeHeapSize(),
           (unsigned)xPortGetMinimumEverFreeHeapSize());
    fflush(stdout);
}

void model_log_node_id_compact(const UA_NodeId *node_id)
{
    if (node_id == NULL)
    {
        printf("null");
        return;
    }

    printf("ns=%u;", (unsigned)node_id->namespaceIndex);
    if (node_id->identifierType == UA_NODEIDTYPE_NUMERIC)
    {
        printf("i=%lu", (unsigned long)node_id->identifier.numeric);
    }
    else if (node_id->identifierType == UA_NODEIDTYPE_STRING)
    {
        printf("s=%.*s",
               (int)node_id->identifier.string.length,
               (const char *)node_id->identifier.string.data);
    }
    else
    {
        printf("type=%u", (unsigned)node_id->identifierType);
    }
}

void model_monitored_item_register_callback(UA_Server *server,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *node_id,
                                                   void *node_context,
                                                   UA_UInt32 attribute_id,
                                                   UA_Boolean removed)
{
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)node_context;

    printf("[OPC UA]\tMonitoredItem %s attr=%lu node=",
           removed ? "removed" : "created",
           (unsigned long)attribute_id);
    model_log_node_id_compact(node_id);
    putchar('\n');
    fflush(stdout);
    ua_port_alloc_log_stats(removed ? "MonitoredItem removed" : "MonitoredItem created");
}

const char *model_notification_type_name(UA_ApplicationNotificationType type)
{
    switch (type)
    {
    case UA_APPLICATIONNOTIFICATIONTYPE_LIFECYCLE_STARTED:
        return "lifecycle-started";
    case UA_APPLICATIONNOTIFICATIONTYPE_LIFECYCLE_SHUTDOWN:
        return "lifecycle-shutdown";
    case UA_APPLICATIONNOTIFICATIONTYPE_LIFECYCLE_STOPPING:
        return "lifecycle-stopping";
    case UA_APPLICATIONNOTIFICATIONTYPE_LIFECYCLE_STOPPED:
        return "lifecycle-stopped";
    case UA_APPLICATIONNOTIFICATIONTYPE_SECURECHANNEL_OPENED:
        return "securechannel-opened";
    case UA_APPLICATIONNOTIFICATIONTYPE_SECURECHANNEL_CLOSED:
        return "securechannel-closed";
    case UA_APPLICATIONNOTIFICATIONTYPE_SESSION_CREATED:
        return "session-created";
    case UA_APPLICATIONNOTIFICATIONTYPE_SESSION_ACTIVATED:
        return "session-activated";
    case UA_APPLICATIONNOTIFICATIONTYPE_SESSION_DEACTIVATED:
        return "session-deactivated";
    case UA_APPLICATIONNOTIFICATIONTYPE_SESSION_CLOSED:
        return "session-closed";
    case UA_APPLICATIONNOTIFICATIONTYPE_SERVICE_BEGIN:
        return "service-begin";
    case UA_APPLICATIONNOTIFICATIONTYPE_SERVICE_ASYNC:
        return "service-async";
    case UA_APPLICATIONNOTIFICATIONTYPE_SERVICE_END:
        return "service-end";
    case UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_CREATED:
        return "subscription-created";
    case UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_MODIFIED:
        return "subscription-modified";
    case UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_PUBLISHINGMODE:
        return "subscription-publishing-mode";
    case UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_TRANSFERRED:
        return "subscription-transferred";
    case UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_DELETED:
        return "subscription-deleted";
    case UA_APPLICATIONNOTIFICATIONTYPE_MONITOREDITEM_CREATED:
        return "monitoreditem-created";
    case UA_APPLICATIONNOTIFICATIONTYPE_MONITOREDITEM_MODIFIED:
        return "monitoreditem-modified";
    case UA_APPLICATIONNOTIFICATIONTYPE_MONITOREDITEM_MONITORINGMODE:
        return "monitoreditem-monitoring-mode";
    case UA_APPLICATIONNOTIFICATIONTYPE_MONITOREDITEM_DELETE:
        return "monitoreditem-delete";
    default:
        return "unknown";
    }
}

void model_server_notification_callback(UA_Server *server,
                                               UA_ApplicationNotificationType type,
                                               const UA_KeyValueMap payload)
{
    const UA_UInt32 *request_id = NULL;
    const UA_UInt32 *subscription_id = NULL;
    const UA_UInt32 *monitored_item_id = NULL;
    const UA_NodeId *service_type = NULL;
    (void)server;

    printf("[OPC UA]\tNotification: %s (%u)\n",
           model_notification_type_name(type),
           (unsigned)type);
    printf("[OPC UA]\tNotification payload entries=%u\n", (unsigned)payload.mapSize);

    request_id = (const UA_UInt32 *)UA_KeyValueMap_getScalar(&payload,
                                                             UA_QUALIFIEDNAME(0, "request-id"),
                                                             &UA_TYPES[UA_TYPES_UINT32]);
    if (request_id != NULL)
    {
        printf("[OPC UA]\tNotification request-id=%lu\n", (unsigned long)*request_id);
    }

    service_type = (const UA_NodeId *)UA_KeyValueMap_getScalar(&payload,
                                                               UA_QUALIFIEDNAME(0, "service-type"),
                                                               &UA_TYPES[UA_TYPES_NODEID]);
    if (service_type != NULL)
    {
        printf("[OPC UA]\tNotification service-type=");
        model_log_node_id_compact(service_type);
        putchar('\n');
    }

    subscription_id = (const UA_UInt32 *)UA_KeyValueMap_getScalar(&payload,
                                                                  UA_QUALIFIEDNAME(0, "subscription-id"),
                                                                  &UA_TYPES[UA_TYPES_UINT32]);
    if (subscription_id != NULL)
    {
        printf("[OPC UA]\tNotification subscription-id=%lu\n",
               (unsigned long)*subscription_id);
    }

    monitored_item_id = (const UA_UInt32 *)UA_KeyValueMap_getScalar(&payload,
                                                                    UA_QUALIFIEDNAME(0, "monitoreditem-id"),
                                                                    &UA_TYPES[UA_TYPES_UINT32]);
    if (monitored_item_id != NULL)
    {
        printf("[OPC UA]\tNotification monitoreditem-id=%lu\n",
               (unsigned long)*monitored_item_id);
    }

    fflush(stdout);

    switch (type)
    {
    case UA_APPLICATIONNOTIFICATIONTYPE_SERVICE_BEGIN:
        ua_port_alloc_log_stats("Service begin");
        break;
    case UA_APPLICATIONNOTIFICATIONTYPE_SERVICE_END:
        ua_port_alloc_log_stats("Service end");
        break;
    case UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_CREATED:
        ua_port_alloc_log_stats("Subscription created");
        break;
    case UA_APPLICATIONNOTIFICATIONTYPE_MONITOREDITEM_CREATED:
        ua_port_alloc_log_stats("MonitoredItem notification created");
        break;
    default:
        break;
    }
}

void model_set_boolean_method_output(size_t output_size, UA_Variant *output, UA_Boolean result);
void model_set_string_method_output(size_t output_size, UA_Variant *output, const char *result);
void model_set_bytestring_method_output(size_t output_size,
                                               UA_Variant *output,
                                               const UA_ByteString *result);
void model_set_default_variant_value(UA_VariableAttributes *attributes, UA_NodeId data_type);
const UA_ByteString *model_session_get_username_bytes(const void *session_context);
bool model_copy_session_username(const void *session_context, char *buffer, size_t buffer_size);
void model_copy_string_safe(char *buffer, size_t buffer_size, const char *text);
void model_emit_client_certificate_expiry_notice(UA_Server *server,
                                                        const UA_ByteString *remote_certificate,
                                                        void *session_context);
bool model_is_admin_only_method(const UA_NodeId *method_id);
bool model_string_changed(const char *lhs, const char *rhs);
UA_StatusCode model_set_amb_maintenance_state(UA_Server *server,
                                                       const UA_NodeId condition_id,
                                                       const char *state_name,
                                                       UA_UInt32 state_node_id);
UA_StatusCode model_condition_write_common_fields(UA_Server *server,
                                                         const UA_NodeId condition_id,
                                                         UA_UInt16 severity,
                                                         UA_StatusCode quality,
                                                         const char *message);
const char *model_maintenance_state_text(maintenance_service_state_t state);
UA_StatusCode model_sync_maintenance_condition(UA_Server *server,
                                                       const maintenance_service_snapshot_t *snapshot);
UA_StatusCode model_read_maintenance_value(
    UA_Server *server, const UA_NodeId *session_id, void *session_context,
    const UA_NodeId *node_id, void *node_context,
    UA_Boolean include_source_timestamp, const UA_NumericRange *range,
    UA_DataValue *value);

bool model_session_is_secure(const void *session_context)
{
    const model_session_context_t *context = (const model_session_context_t *)session_context;
    return context != NULL && context->secure_channel;
}

bool model_nodeid_string_has_prefix(const UA_NodeId *node_id, const char *prefix)
{
    const size_t prefix_length = strlen(prefix);

    return node_id != NULL &&
           prefix != NULL &&
           node_id->namespaceIndex == 1u &&
           node_id->identifierType == UA_NODEIDTYPE_STRING &&
           node_id->identifier.string.data != NULL &&
           node_id->identifier.string.length >= prefix_length &&
           memcmp(node_id->identifier.string.data, prefix, prefix_length) == 0;
}

bool model_session_is_anonymous(const void *session_context)
{
    const UA_ByteString *username = model_session_get_username_bytes(session_context);
    return username == NULL || username->data == NULL || username->length == 0u;
}

bool model_node_is_anonymous_hidden(const UA_NodeId *node_id)
{
    return model_nodeid_string_has_prefix(node_id, "Device.Maintenance") ||
           model_nodeid_string_has_prefix(node_id, "Device.Identification") ||
           model_nodeid_string_has_prefix(node_id, "Device.Security");
}

bool model_node_is_maintenance_method(const UA_NodeId *method_id)
{
    return model_nodeid_string_has_prefix(method_id, "Device.Maintenance");
}

UA_Byte model_strip_anonymous_write_access(UA_Byte access_level)
{
    return access_level &
           (UA_Byte)~(UA_ACCESSLEVELMASK_WRITE |
                      UA_ACCESSLEVELMASK_HISTORYWRITE |
                      UA_ACCESSLEVELMASK_STATUSWRITE |
                      UA_ACCESSLEVELMASK_TIMESTAMPWRITE);
}

const UA_ByteString *model_session_get_username_bytes(const void *session_context)
{
    const model_session_context_t *context = (const model_session_context_t *)session_context;

    if (context == NULL || context->base_context == NULL)
    {
        return NULL;
    }

    return (const UA_ByteString *)context->base_context;
}

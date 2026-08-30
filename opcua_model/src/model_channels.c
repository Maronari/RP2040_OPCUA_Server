/**
 * @file
 * @brief Config-diff/reboot-required helpers, certificate/trust-list expiry
 *        classification, fixed-NodeId accessor functions, and PubSub
 *        topic/client-id string builders.
 *
 * Responsibilities:
 * - Decide whether a config change requires a full server rebuild/reboot
 *   (`model_config_requires_server_reboot`,
 *   `model_address_space_snapshot_requires_server_restart`,
 *   `model_is_reboot_required`) against service-owned RUNNING/STAGED state;
 *   only the controlled-restart helper ignores STAGED state. This file owns
 *   no configuration state itself.
 * - Classify server-certificate and trust-list expiry into the
 *   `model_certificate_expiry_state_t` buckets consumed by
 *   `model_maintenance.c`'s runtime-event polling.
 * - Provide the fixed (`"Device"`, `"Device.Network"`, ... ) NodeId accessor
 *   functions consumed by `model_address_space.c` / `model_events.c` and the
 *   PubSub topic/client-id string builders consumed by `model_pubsub.c`.
 *
 * Note on file naming (see `docs/code/modules/opcua_model.md`): despite the
 * name, this file does not build channel nodes — that happens in
 * `model_address_space.c`. It holds config-diff, expiry, NodeId-accessor,
 * and PubSub string-building helpers instead.
 *
 * Ownership: all functions here are stateless helpers over caller-supplied
 * data or service snapshots, except `model_pubsub_reset_runtime` /
 * `model_store_last_event`, which mutate fields of the single
 * `opcua_model_context_t` (`g_model_pubsub`, `g_model_pubsub_event_mirror`,
 * `g_last_event_state`).
 *
 * Concurrency: every function here runs on `OpcUaTask` only (called from
 * `model_core_task`'s poll loop, address-space construction, or PubSub
 * setup); none is reentrant-safe from another task.
 */
#include "opcua_model_internal.h"

bool model_byte_array_changed(const uint8_t *lhs, const uint8_t *rhs, size_t size)
{
    if (lhs == NULL || rhs == NULL)
    {
        return lhs != rhs;
    }

    return memcmp(lhs, rhs, size) != 0;
}

/* Implementation note: after the scalar/string/byte-array field comparisons,
 * this walks all PLATFORM_CHANNEL_LIMIT channel slots (not just populated
 * ones) through model_channel_requires_server_rebuild, since an unpopulated
 * slot becoming populated (or vice versa) also changes the address space. */
bool model_config_requires_server_reboot(const device_profile_config_t *lhs,
                                                const device_profile_config_t *rhs)
{
    if (lhs == NULL || rhs == NULL)
    {
        return lhs != rhs;
    }

    if (lhs->opcua_port != rhs->opcua_port ||
        lhs->enable_mqtt_pubsub != rhs->enable_mqtt_pubsub ||
        lhs->mqtt_pubsub_use_json != rhs->mqtt_pubsub_use_json ||
        lhs->enable_alarm_ack_method != rhs->enable_alarm_ack_method ||
        lhs->enable_heap_diagnostics != rhs->enable_heap_diagnostics ||
        lhs->enable_task_diagnostics != rhs->enable_task_diagnostics ||
        model_string_changed(lhs->device_name, rhs->device_name) ||
        model_string_changed(lhs->opcua_hostname, rhs->opcua_hostname) ||
        model_string_changed(lhs->ntp_server, rhs->ntp_server) ||
        model_string_changed(lhs->mqtt_broker_url, rhs->mqtt_broker_url) ||
        model_string_changed(lhs->mqtt_topic_root, rhs->mqtt_topic_root) ||
        model_string_changed(lhs->product_instance_uri, rhs->product_instance_uri) ||
        model_byte_array_changed(lhs->mac, rhs->mac, sizeof(lhs->mac)) ||
        lhs->network_mode != rhs->network_mode ||
        model_byte_array_changed(lhs->static_ip, rhs->static_ip, sizeof(lhs->static_ip)) ||
        model_byte_array_changed(lhs->static_netmask, rhs->static_netmask, sizeof(lhs->static_netmask)) ||
        model_byte_array_changed(lhs->static_gateway, rhs->static_gateway, sizeof(lhs->static_gateway)) ||
        model_byte_array_changed(lhs->static_dns, rhs->static_dns, sizeof(lhs->static_dns)))
    {
        return true;
    }

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        if (model_channel_requires_server_rebuild(&lhs->channels[index], &rhs->channels[index]))
        {
            return true;
        }
    }

    return false;
}

bool model_address_space_snapshot_requires_server_restart(void)
{
    const device_profile_config_t *running = config_service_get();

    if (!g_model_address_space_config_snapshot_valid || running == NULL)
    {
        return false;
    }

    return model_config_requires_server_reboot(&g_model_address_space_config_snapshot, running);
}

/* TEMPORARY v0.5.0 diagnostic instrumentation (2026-08-30): logs which of
 * model_is_reboot_required()'s two independent terms is true, and which
 * specific field differs, when investigating a hardware-observed defect
 * where rebootRequired does not clear after the first controlled restart
 * of a boot cycle. Remove before shipping; see
 * docs/releases/v0.5.0-hardware-verification.md's 2026-08-30 addendum. */
static void model_debug_log_config_diff(const char *label,
                                        const device_profile_config_t *lhs,
                                        const device_profile_config_t *rhs)
{
    if (lhs == NULL || rhs == NULL)
    {
        printf("[DIAG-REBOOT-REQ] %s: lhs=%p rhs=%p (NULL)\n", label, (const void *)lhs, (const void *)rhs);
        fflush(stdout);
        return;
    }

    printf("[DIAG-REBOOT-REQ] %s: port=%d mqtt=%d mqttJson=%d alarmAck=%d heapDiag=%d taskDiag=%d "
           "devName=%d hostname=%d ntp=%d mqttUrl=%d mqttTopic=%d prodUri=%d mac=%d netMode=%d "
           "ip=%d mask=%d gw=%d dns=%d\n",
           label,
           (int)(lhs->opcua_port != rhs->opcua_port),
           (int)(lhs->enable_mqtt_pubsub != rhs->enable_mqtt_pubsub),
           (int)(lhs->mqtt_pubsub_use_json != rhs->mqtt_pubsub_use_json),
           (int)(lhs->enable_alarm_ack_method != rhs->enable_alarm_ack_method),
           (int)(lhs->enable_heap_diagnostics != rhs->enable_heap_diagnostics),
           (int)(lhs->enable_task_diagnostics != rhs->enable_task_diagnostics),
           (int)model_string_changed(lhs->device_name, rhs->device_name),
           (int)model_string_changed(lhs->opcua_hostname, rhs->opcua_hostname),
           (int)model_string_changed(lhs->ntp_server, rhs->ntp_server),
           (int)model_string_changed(lhs->mqtt_broker_url, rhs->mqtt_broker_url),
           (int)model_string_changed(lhs->mqtt_topic_root, rhs->mqtt_topic_root),
           (int)model_string_changed(lhs->product_instance_uri, rhs->product_instance_uri),
           (int)model_byte_array_changed(lhs->mac, rhs->mac, sizeof(lhs->mac)),
           (int)(lhs->network_mode != rhs->network_mode),
           (int)model_byte_array_changed(lhs->static_ip, rhs->static_ip, sizeof(lhs->static_ip)),
           (int)model_byte_array_changed(lhs->static_netmask, rhs->static_netmask, sizeof(lhs->static_netmask)),
           (int)model_byte_array_changed(lhs->static_gateway, rhs->static_gateway, sizeof(lhs->static_gateway)),
           (int)model_byte_array_changed(lhs->static_dns, rhs->static_dns, sizeof(lhs->static_dns)));
    fflush(stdout);

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        if (model_channel_requires_server_rebuild(&lhs->channels[index], &rhs->channels[index]))
        {
            printf("[DIAG-REBOOT-REQ] %s: channel[%u] differs\n", label, (unsigned)index);
            fflush(stdout);
        }
    }
}

bool model_is_reboot_required(void)
{
    const device_profile_config_t *running = config_service_get();
    const device_profile_config_t *staged = config_service_get_staged();
    bool snapshot_differs;
    bool staged_differs;

    if (!g_model_address_space_config_snapshot_valid || running == NULL || staged == NULL)
    {
        return false;
    }

    snapshot_differs = model_address_space_snapshot_requires_server_restart();
    staged_differs = model_config_requires_server_reboot(running, staged);

    if (snapshot_differs || staged_differs)
    {
        printf("[DIAG-REBOOT-REQ] snapshot_differs=%d staged_differs=%d\n",
               (int)snapshot_differs, (int)staged_differs);
        fflush(stdout);
        if (snapshot_differs)
        {
            model_debug_log_config_diff("snapshot-vs-running", &g_model_address_space_config_snapshot, running);
        }
        if (staged_differs)
        {
            model_debug_log_config_diff("staged-vs-running", staged, running);
        }
    }

    return snapshot_differs || staged_differs;
}

const char *model_security_mode_to_string(void)
{
    return security_service_is_commissioning_mode() ? "Commissioning" : "Strict";
}

model_certificate_expiry_state_t model_get_certificate_expiry_state(uint32_t *out_days_until_expiry,
                                                                           bool *out_time_check_performed)
{
    time_service_status_t time_status = {0};
    uint32_t not_after_epoch_s = 0u;
    uint32_t remaining_days = 0u;

    if (out_days_until_expiry != NULL)
    {
        *out_days_until_expiry = 0u;
    }

    if (out_time_check_performed != NULL)
    {
        *out_time_check_performed = false;
    }

    if (!security_service_has_device_identity())
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_MISSING;
    }

    if (!security_service_get_device_certificate_not_after(&not_after_epoch_s) || not_after_epoch_s == 0u)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN;
    }

    time_service_get_status(&time_status);
    if (time_status.quality == SERVICE_QUALITY_BAD)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN;
    }

    if (out_time_check_performed != NULL)
    {
        *out_time_check_performed = true;
    }

    if (time_status.unix_time_s >= not_after_epoch_s)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED;
    }

    /* Round up: any partial day remaining before expiry still counts as a
     * whole day, so the notice does not read "0 days" while still valid. */
    remaining_days = (not_after_epoch_s - time_status.unix_time_s + 86399u) / 86400u;

    if (out_days_until_expiry != NULL)
    {
        *out_days_until_expiry = remaining_days;
    }

    if (remaining_days <= MODEL_CERTIFICATE_EXPIRY_HIGH_WARN_DAYS)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH;
    }

    if (remaining_days <= MODEL_CERTIFICATE_EXPIRY_WARN_DAYS)
    {
        return MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM;
    }

    return MODEL_CERTIFICATE_EXPIRY_STATE_VALID;
}

const char *model_certificate_expiry_state_to_string(model_certificate_expiry_state_t state)
{
    switch (state)
    {
    case MODEL_CERTIFICATE_EXPIRY_STATE_MISSING:
        return "missing";
    case MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN:
        return "unknown";
    case MODEL_CERTIFICATE_EXPIRY_STATE_VALID:
        return "valid";
    case MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM:
        return "expires_soon_medium";
    case MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH:
        return "expires_soon_high";
    case MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED:
        return "expired";
    default:
        return "unknown";
    }
}

bool model_certificate_expiry_requires_attention(model_certificate_expiry_state_t state)
{
    return state == MODEL_CERTIFICATE_EXPIRY_STATE_MISSING ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH ||
           state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED;
}

bool model_format_certificate_expiry_notice(model_certificate_expiry_state_t state,
                                                   uint32_t days_until_expiry,
                                                   char *message,
                                                   size_t message_size,
                                                   UA_UInt16 *out_severity)
{
    if (message == NULL || message_size == 0u || out_severity == NULL)
    {
        return false;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_MISSING)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 900u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 350u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 900u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 750u;
        return true;
    }

    if (state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(state));
        *out_severity = 550u;
        return true;
    }

    (void)state;
    (void)days_until_expiry;
    return false;
}

/* Implementation note: does not call model_certificate_expiry_requires_attention
 * explicitly. It relies on model_format_certificate_expiry_notice's switch
 * being exhaustive over exactly the states requires_attention() considers
 * attention-worthy (MISSING/UNKNOWN/EXPIRES_SOON_/EXPIRED) and falling
 * through to `return false` for VALID (its only omitted state), so the two
 * functions stay behaviorally equivalent without a duplicated attention
 * check. If a new model_certificate_expiry_state_t value is ever added, both
 * this switch and model_certificate_expiry_requires_attention must be kept
 * in sync by hand. */
bool model_build_certificate_expiry_notice(model_certificate_expiry_state_t state,
                                                  uint32_t days_until_expiry,
                                                  char *message,
                                                  size_t message_size,
                                                  UA_UInt16 *out_severity)
{
    if (message == NULL || message_size == 0u || out_severity == NULL)
    {
        return false;
    }

    return model_format_certificate_expiry_notice(state,
                                                  days_until_expiry,
                                                  message,
                                                  message_size,
                                                  out_severity);
}

bool model_trust_expiry_requires_attention(const security_trust_expiry_service_report_t *report,
                                                  bool commissioning_mode)
{
    if (report == NULL)
    {
        return false;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EMPTY)
    {
        return !commissioning_mode;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_UNKNOWN ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRED ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_PARSE_FAILED ||
        report->state == SECURITY_TRUST_EXPIRY_STATE_INTERNAL_ERROR)
    {
        return true;
    }

    return false;
}

bool model_build_trust_expiry_notice(const security_trust_expiry_service_report_t *report,
                                            bool commissioning_mode,
                                            char *message,
                                            size_t message_size,
                                            UA_UInt16 *out_severity)
{
    if (report == NULL || message == NULL || message_size == 0u || out_severity == NULL)
    {
        return false;
    }

    if (!model_trust_expiry_requires_attention(report, commissioning_mode))
    {
        return false;
    }

    model_copy_string_safe(message,
                           message_size,
                           security_trust_expiry_service_state_to_string(report->state));

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EMPTY)
    {
        *out_severity = 850u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_UNKNOWN)
    {
        *out_severity = 350u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON)
    {
        *out_severity = 550u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        *out_severity = 750u;
        return true;
    }

    if (report->state == SECURITY_TRUST_EXPIRY_STATE_EXPIRED)
    {
        *out_severity = 880u;
        return true;
    }

    *out_severity = 820u;
    return true;
}

bool model_trust_expiry_report_changed(const security_trust_expiry_service_report_t *lhs,
                                              const security_trust_expiry_service_report_t *rhs)
{
    if (lhs == NULL || rhs == NULL)
    {
        return false;
    }

    return lhs->state != rhs->state ||
           lhs->have_trust_list != rhs->have_trust_list ||
           lhs->parse_ok != rhs->parse_ok ||
           lhs->time_check_performed != rhs->time_check_performed ||
           lhs->trusted_client_count != rhs->trusted_client_count ||
           lhs->trusted_client_expiring_soon_count != rhs->trusted_client_expiring_soon_count ||
           lhs->trusted_client_expiring_high_count != rhs->trusted_client_expiring_high_count ||
           lhs->trusted_client_expired_count != rhs->trusted_client_expired_count;
}

const char *model_security_commissioning_state_to_string(void)
{
    security_state_service_snapshot_t snapshot = {0};

    if (!security_state_service_get_snapshot(&snapshot))
    {
        return security_service_is_commissioning_mode() ? "commissioning_open" : "strict";
    }

    if (snapshot.pending_identity_generation > snapshot.active_identity_generation ||
        snapshot.pending_trust_generation > snapshot.active_trust_generation)
    {
        return "rotation_pending";
    }

    return snapshot.commissioning_mode ? "commissioning_open" : "strict";
}

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
UA_NodeId model_device_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device");
}

UA_NodeId model_network_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Network");
}

UA_NodeId model_temperature_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)DEVICE_CHANNEL_NODE_TEMPERATURE_1);
}

UA_NodeId model_maintenance_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Maintenance");
}

UA_NodeId model_configuration_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Configuration");
}

UA_NodeId model_security_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.Security");
}

UA_NodeId model_event_type_node_id(const char *node_id)
{
    return UA_NODEID_STRING(1, (char *)(node_id != NULL ? node_id : MODEL_EVENT_TYPE_DEVICE));
}

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
UA_NodeId model_condition_type_node_id(const char *node_id)
{
    return UA_NODEID_STRING(1, (char *)(node_id != NULL ? node_id : MODEL_CONDITION_TYPE_SECURITY_FAULT));
}

UA_NodeId model_security_condition_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)MODEL_CONDITION_SECURITY_FAULT);
}

UA_NodeId model_device_health_alarms_node_id(void)
{
    return UA_NODEID_STRING(1, (char *)"Device.DeviceHealthAlarms");
}

UA_NodeId model_device_health_condition_node_id(device_health_state_t state)
{
    switch (state)
    {
    case DEVICE_HEALTH_FAILURE:
        return UA_NODEID_STRING(1, (char *)MODEL_CONDITION_DEVICE_HEALTH_FAILURE);
    case DEVICE_HEALTH_CHECK_FUNCTION:
        return UA_NODEID_STRING(1, (char *)MODEL_CONDITION_DEVICE_HEALTH_CHECK_FUNCTION);
    case DEVICE_HEALTH_OFF_SPEC:
        return UA_NODEID_STRING(1, (char *)MODEL_CONDITION_DEVICE_HEALTH_OFF_SPEC);
    case DEVICE_HEALTH_MAINTENANCE_REQUIRED:
        return UA_NODEID_STRING(1, (char *)MODEL_CONDITION_DEVICE_HEALTH_MAINTENANCE_REQUIRED);
    default:
        return UA_NODEID_NULL;
    }
}
#endif

UA_StatusCode model_add_object_type(UA_Server *server,
                                           const char *node_id,
                                           const char *browse_name,
                                           const UA_NodeId parent_type_id)
{
    UA_ObjectTypeAttributes attributes = UA_ObjectTypeAttributes_default;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.isAbstract = false;

    return UA_Server_addObjectTypeNode(server,
                                       UA_NODEID_STRING(1, (char *)node_id),
                                       parent_type_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
                                       UA_QUALIFIEDNAME(1, (char *)browse_name),
                                       attributes,
                                       NULL,
                                       NULL);
}

void model_copy_string_safe(char *buffer, size_t buffer_size, const char *text)
{
    size_t length = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (text != NULL)
    {
        length = strlen(text);
        if (length >= buffer_size)
        {
            length = buffer_size - 1u;
        }
        if (length > 0u)
        {
            memcpy(buffer, text, length);
        }
    }

    buffer[length] = '\0';
}

void model_event_id_to_hex(const UA_ByteString *event_id, char *buffer, size_t buffer_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t offset = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    buffer[0] = '\0';
    if (event_id == NULL || event_id->data == NULL || event_id->length == 0u)
    {
        return;
    }

    for (size_t index = 0u; index < event_id->length && (offset + 2u) < buffer_size; ++index)
    {
        buffer[offset++] = hex[(event_id->data[index] >> 4u) & 0x0Fu];
        buffer[offset++] = hex[event_id->data[index] & 0x0Fu];
    }

    buffer[offset] = '\0';
}

void model_store_last_event(model_event_last_kind_t kind,
                                   const char *event_id_hex,
                                   const char *message)
{
    model_copy_string_safe(g_last_event_state.last_event_id,
                           sizeof(g_last_event_state.last_event_id),
                           event_id_hex);

    switch (kind)
    {
    case MODEL_EVENT_LAST_KIND_CONFIG:
        model_copy_string_safe(g_last_event_state.last_config_change,
                               sizeof(g_last_event_state.last_config_change),
                               message);
        break;
    case MODEL_EVENT_LAST_KIND_NETWORK:
        model_copy_string_safe(g_last_event_state.last_network_change,
                               sizeof(g_last_event_state.last_network_change),
                               message);
        break;
    case MODEL_EVENT_LAST_KIND_FAULT:
        model_copy_string_safe(g_last_event_state.last_fault,
                               sizeof(g_last_event_state.last_fault),
                               message);
        break;
    case MODEL_EVENT_LAST_KIND_GENERIC:
    default:
        break;
    }
}

#ifdef UA_ENABLE_PUBSUB
void model_pubsub_reset_runtime(void)
{
    UA_NodeId_clear(&g_model_pubsub.connection_id);
    UA_NodeId_clear(&g_model_pubsub.event_writer_group_id);
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        UA_NodeId_clear(&g_model_pubsub.telemetry_writer_group_ids[index]);
    }

    memset(&g_model_pubsub, 0, sizeof(g_model_pubsub));
    memset(&g_model_pubsub_event_mirror, 0, sizeof(g_model_pubsub_event_mirror));
}

bool model_pubsub_requested(void)
{
    return g_model_config.enable_mqtt_pubsub &&
           g_model_config.mqtt_broker_url != NULL &&
           g_model_config.mqtt_broker_url[0] != '\0' &&
           g_model_config.mqtt_topic_root != NULL &&
           g_model_config.mqtt_topic_root[0] != '\0';
}

const char *model_pubsub_component_type_to_string(UA_PubSubComponentType type)
{
    switch (type)
    {
    case UA_PUBSUBCOMPONENT_CONNECTION:
        return "Connection";
    case UA_PUBSUBCOMPONENT_WRITERGROUP:
        return "WriterGroup";
    case UA_PUBSUBCOMPONENT_DATASETWRITER:
        return "DataSetWriter";
    case UA_PUBSUBCOMPONENT_READERGROUP:
        return "ReaderGroup";
    case UA_PUBSUBCOMPONENT_DATASETREADER:
        return "DataSetReader";
    case UA_PUBSUBCOMPONENT_PUBLISHEDDATASET:
        return "PublishedDataSet";
    case UA_PUBSUBCOMPONENT_SUBSCRIBEDDDATASET:
        return "SubscribedDataSet";
    default:
        return "Component";
    }
}

const char *model_pubsub_state_to_string(UA_PubSubState state)
{
    switch (state)
    {
    case UA_PUBSUBSTATE_DISABLED:
        return "Disabled";
    case UA_PUBSUBSTATE_PAUSED:
        return "Paused";
    case UA_PUBSUBSTATE_OPERATIONAL:
        return "Operational";
    case UA_PUBSUBSTATE_ERROR:
        return "Error";
    case UA_PUBSUBSTATE_PREOPERATIONAL:
        return "PreOperational";
    default:
        return "Unknown";
    }
}

#endif /* UA_ENABLE_PUBSUB */
#endif /* UA_ENABLE_SUBSCRIPTIONS_EVENTS */

/* Implementation note: string-identifier NodeIds are copied directly from
 * their raw bytes (fast path, no allocation); numeric/GUID/opaque NodeIds
 * fall back to UA_NodeId_print (heap-allocates `rendered`, always cleared
 * via UA_String_clear before return) to get the `ns=<i>;i=<id>`-style text. */
void model_node_id_to_text(const UA_NodeId *node_id, char *buffer, size_t buffer_size)
{
    UA_String rendered = UA_STRING_NULL;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    buffer[0] = '\0';
    if (node_id == NULL)
    {
        return;
    }

    if (node_id->identifierType == UA_NODEIDTYPE_STRING &&
        node_id->identifier.string.data != NULL &&
        node_id->identifier.string.length > 0u)
    {
        size_t length = node_id->identifier.string.length;
        if (length >= buffer_size)
        {
            length = buffer_size - 1u;
        }

        memcpy(buffer, node_id->identifier.string.data, length);
        buffer[length] = '\0';
        return;
    }

    if (UA_NodeId_print(node_id, &rendered) == UA_STATUSCODE_GOOD &&
        rendered.data != NULL &&
        rendered.length > 0u)
    {
        size_t length = rendered.length;
        if (length >= buffer_size)
        {
            length = buffer_size - 1u;
        }

        memcpy(buffer, rendered.data, length);
        buffer[length] = '\0';
    }

    UA_String_clear(&rendered);
}

void model_pubsub_extract_tail(const char *text, char *buffer, size_t buffer_size)
{
    const char *tail = text;
    const char *last_dot = NULL;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (text != NULL)
    {
        last_dot = strrchr(text, '.');
        if (last_dot != NULL && last_dot[1] != '\0')
        {
            tail = last_dot + 1;
        }
    }

    model_copy_string_safe(buffer, buffer_size, tail);
}

bool model_pubsub_build_client_id(char *buffer, size_t buffer_size)
{
    const device_profile_config_t *device_config = config_service_get();
    const char *base = NULL;
    int written = 0;

    if (buffer == NULL || buffer_size == 0u)
    {
        return false;
    }

    base = (g_model_config.hostname != NULL && g_model_config.hostname[0] != '\0')
               ? g_model_config.hostname
               : ((device_config != NULL && device_config->opcua_hostname[0] != '\0')
                      ? device_config->opcua_hostname
                      : "rp2-opcua");

    written = snprintf(buffer, buffer_size, "rp2-%s", base);
    return written > 0 && (size_t)written < buffer_size;
}

bool model_pubsub_build_topic(char *buffer,
                                     size_t buffer_size,
                                     const char *root,
                                     const char *suffix,
                                     const char *leaf)
{
    int written = 0;

    if (buffer == NULL || buffer_size == 0u || root == NULL || suffix == NULL)
    {
        return false;
    }

    written = snprintf(buffer,
                       buffer_size,
                       (leaf != NULL && leaf[0] != '\0') ? "%s/%s/%s" : "%s/%s",
                       root,
                       suffix,
                       leaf != NULL ? leaf : "");
    return written > 0 && (size_t)written < buffer_size;
}

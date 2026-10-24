/**
 * @file
 * @brief `UA_AccessControl` plugin implementation: the login callback and
 *        the six `model_*_wrapper` functions open62541 invokes per-session
 *        for session activation/close, method executability, node access
 *        level, and browse visibility; plus the session/role helper
 *        functions and the centralized role-policy tables they build on.
 *
 * Responsibilities:
 * - Wrap open62541's default `UA_AccessControl` callbacks (saved by
 *   `model_configure_secure_access_control` / `model_core_task` into
 *   `g_default_*`): call the saved default first, then layer role-based and
 *   anonymous-session restrictions on top.
 * - Own the per-session rejected-maintenance-call rate limiter
 *   (`model_maintenance_rejected_audit_admit`) and the audit-log helpers
 *   built on `security_service_audit_log`.
 * - Provide the two centralized access-policy tables
 *   (`model_required_role_for_method`, `model_required_role_for_write_kind`)
 *   that every method callback and the generic write dispatch consult.
 * - Provide channel-snapshot/NodeId-building helpers
 *   (`model_copy_channel_by_id`, `model_build_channel_node_id`, ...) used
 *   while constructing and reading the per-channel address-space nodes.
 *
 * Ownership: `model_session_context_t` instances are allocated by
 * `model_activate_session_secure_wrapper` and freed by
 * `model_close_session_secure_wrapper`; nothing else in this file owns
 * long-lived state. The maintenance-reject rate limiter fields live inside
 * that same per-session struct plus one global bucket in
 * `opcua_model_context_t` (`g_model_maintenance_reject_*`).
 *
 * Concurrency: the `model_*_wrapper` functions and every method-callback
 * helper run on `OpcUaTask` inside open62541's synchronous service
 * dispatch. `model_maintenance_rejected_audit_admit` is the one exception —
 * it is reachable indirectly from `security_service`'s audit callback,
 * which is not guaranteed to run on `OpcUaTask`, so its counters are
 * updated under `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`.
 */
#include "opcua_model_internal.h"

/* Only this v0.5.2 surface is restricted to service/admin.  Do not broaden
 * the test to Device.Diagnostics as a whole: FreeHeap, the original PSRAM
 * members and the legacy Task mapping have compatibility-preserved access
 * behavior. String NodeIds are not NUL terminated by open62541. */
bool model_node_is_t2_resource_diagnostic(const UA_NodeId *node_id)
{
    static const char heap_prefix[] = "Device.Diagnostics.Heap";
    static const char psram_lwip_prefix[] = "Device.Diagnostics.PSRAM.LwIP";
    static const char tasks_prefix[] = "Device.Diagnostics.Tasks";
    static const char opcua_prefix[] = "Device.Diagnostics.OpcUa";
    static const char *const exact_ids[] = {
        "Device.Diagnostics.SnapshotSequence",
        "Device.Diagnostics.SnapshotSequenceWrapCount",
        "Device.Diagnostics.SnapshotTimestamp",
        "Device.Diagnostics.SnapshotAgeMs",
        "Device.Diagnostics.TaskCount",
        "Device.Diagnostics.TaskListTruncated",
        "Device.Diagnostics.TaskNameConflict",
        "Device.Diagnostics.TaskModelIncomplete",
        "Device.Diagnostics.PSRAM.PeakUsedBytes",
        "Device.Diagnostics.PSRAM.RequestedBytesInUse",
        "Device.Diagnostics.PSRAM.PeakRequestedBytesInUse",
        "Device.Diagnostics.PSRAM.SuccessfulAllocations",
        "Device.Diagnostics.PSRAM.SuccessfulAllocationsWrapCount",
        "Device.Diagnostics.PSRAM.SuccessfulFrees",
        "Device.Diagnostics.PSRAM.SuccessfulFreesWrapCount",
        "Device.Diagnostics.PSRAM.FailedAllocations",
        "Device.Diagnostics.PSRAM.FailedAllocationsWrapCount",
        "Device.Diagnostics.PSRAM.LockTimeouts",
        "Device.Diagnostics.PSRAM.LockTimeoutsWrapCount",
        "Device.Diagnostics.PSRAM.InvalidContextFailures",
        "Device.Diagnostics.PSRAM.InvalidContextFailuresWrapCount",
        "Device.Diagnostics.PSRAM.LastFailedRequestedBytes",
        "Device.Diagnostics.PSRAM.LastFailureReason",
        "Device.Diagnostics.PSRAM.LastFailureReasonText",
        "Device.Diagnostics.PSRAM.LastFailureTimestamp",
        "Device.Diagnostics.PSRAM.LastFailureUptimeMs"};
    const UA_String *identifier;
    const char *const subtree_prefixes[] = {
        heap_prefix, psram_lwip_prefix, tasks_prefix, opcua_prefix};

    if (node_id == NULL || node_id->namespaceIndex != 1u ||
        node_id->identifierType != UA_NODEIDTYPE_STRING)
    {
        return false;
    }
    identifier = &node_id->identifier.string;
    for (size_t index = 0u; index < sizeof(exact_ids) / sizeof(exact_ids[0]); ++index)
    {
        const size_t length = strlen(exact_ids[index]);
        if (identifier->length == length &&
            memcmp(identifier->data, exact_ids[index], length) == 0)
        {
            return true;
        }
    }
    for (size_t index = 0u; index < sizeof(subtree_prefixes) / sizeof(subtree_prefixes[0]); ++index)
    {
        const size_t length = strlen(subtree_prefixes[index]);
        if (identifier->length >= length &&
            memcmp(identifier->data, subtree_prefixes[index], length) == 0 &&
            (identifier->length == length || identifier->data[length] == '.'))
        {
            return true;
        }
    }
    return false;
}

static bool model_session_may_access_t2_resource_diagnostic(void *session_context)
{
    return model_session_has_role(session_context, SECURITY_ROLE_SERVICE);
}

bool model_copy_session_username(const void *session_context, char *buffer, size_t buffer_size)
{
    const UA_ByteString *username = model_session_get_username_bytes(session_context);

    if (username == NULL || buffer == NULL || buffer_size == 0u || username->length >= buffer_size)
    {
        return false;
    }

    memcpy(buffer, username->data, username->length);
    buffer[username->length] = '\0';
    return true;
}

/**
 * @brief Heap-allocates a `UA_ByteString` copy of @p user_name, for storage
 *        as a `model_session_context_t::base_context` after a successful
 *        username/password login.
 * @param user_name Login username as delivered by open62541; may be NULL.
 * @return Newly allocated `UA_ByteString*` (caller/`model_free_session_username`
 *         owns it), or NULL on invalid input or allocation failure.
 * @note Not declared in `opcua_model_internal.h`; only called from
 *       `model_login_callback` in this file. Runs on `OpcUaTask`.
 */
UA_ByteString *model_allocate_session_username(const UA_String *user_name)
{
    UA_ByteString *stored_username = NULL;

    if (user_name == NULL)
    {
        return NULL;
    }

    stored_username = (UA_ByteString *)UA_calloc(1u, sizeof(UA_ByteString));
    if (stored_username == NULL)
    {
        return NULL;
    }

    if (UA_ByteString_allocBuffer(stored_username, user_name->length) != UA_STATUSCODE_GOOD)
    {
        UA_free(stored_username);
        return NULL;
    }

    memcpy(stored_username->data, user_name->data, user_name->length);
    return stored_username;
}

void model_free_session_username(void *base_context)
{
    UA_ByteString *stored_username = (UA_ByteString *)base_context;

    if (stored_username == NULL)
    {
        return;
    }

    UA_ByteString_clear(stored_username);
    UA_free(stored_username);
}

bool model_session_has_role(const void *session_context, security_user_role_t required_role)
{
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    security_user_role_t current_role = SECURITY_ROLE_VIEWER;

    if (!model_copy_session_username(session_context, username, sizeof(username)))
    {
        return false;
    }

    if (!security_service_get_role(username, &current_role))
    {
        return false;
    }

    return current_role >= required_role;
}

bool model_session_get_role(const void *session_context, security_user_role_t *out_role)
{
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];

    if (out_role == NULL)
    {
        return false;
    }

    if (!model_copy_session_username(session_context, username, sizeof(username)))
    {
        return false;
    }

    return security_service_get_role(username, out_role);
}

const char *model_session_role_to_string(const void *session_context)
{
    security_user_role_t role = SECURITY_ROLE_VIEWER;

    if (model_session_is_anonymous(session_context))
    {
        return "Anonymous";
    }

    if (!model_session_get_role(session_context, &role))
    {
        return "Unknown";
    }

    return security_service_role_to_string(role);
}

const char *model_session_actor_name(void *session_context, char *buffer, size_t buffer_size)
{
    if (model_copy_session_username(session_context, buffer, buffer_size))
    {
        return buffer;
    }

    return model_session_is_anonymous(session_context) ? "anonymous" : "system";
}

void model_audit_session_event(void *session_context,
                                      const char *event,
                                      const char *subject,
                                      bool success,
                                      const char *details)
{
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];

    security_service_audit_log(event,
                               model_session_actor_name(session_context, actor, sizeof(actor)),
                               subject,
                               success,
                               details);
}

/* Rejected maintenance calls are useful operational evidence but must not let
 * one client consume the fixed audit-event queue.  The limiter is intentionally
 * adapter-owned: it is volatile, bounded by the existing session table, and is
 * cleared when a session closes.
 *
 * Implementation note: two independent token buckets gate admission — a
 * per-session bucket (refilled 1 token/interval, burst
 * MODEL_MAINTENANCE_REJECT_SESSION_BURST) and one global bucket shared by
 * all sessions (refilled 8 tokens/interval, burst
 * MODEL_MAINTENANCE_REJECT_GLOBAL_BURST). A call is only admitted (and thus
 * audited) if both buckets have a token; otherwise it increments the
 * session's `maintenance_reject_suppressed` counter (saturating at
 * MODEL_MAINTENANCE_REJECT_SUPPRESSION_MAX) so the next admitted audit entry
 * can report how many rejections were coalesced. */
bool model_maintenance_rejected_audit_admit(void *session_context,
                                                   UA_StatusCode reason,
                                                   uint16_t *out_suppressed,
                                                   UA_StatusCode *out_last_reason)
{
    model_session_context_t *session = (model_session_context_t *)session_context;
    const TickType_t now = xTaskGetTickCount();
    TickType_t interval = pdMS_TO_TICKS(1000u);
    bool admitted = false;

    if (out_suppressed != NULL)
    {
        *out_suppressed = 0u;
    }
    if (out_last_reason != NULL)
    {
        *out_last_reason = reason;
    }
    if (session == NULL)
    {
        return false;
    }
    if (interval == 0u)
    {
        interval = 1u;
    }

    taskENTER_CRITICAL();
    if (!session->maintenance_reject_limiter_initialized)
    {
        session->maintenance_reject_limiter_initialized = true;
        session->maintenance_reject_last_refill = now;
        session->maintenance_reject_tokens = MODEL_MAINTENANCE_REJECT_SESSION_BURST;
    }
    if (!g_model_maintenance_reject_limiter_initialized)
    {
        g_model_maintenance_reject_limiter_initialized = true;
        g_model_maintenance_reject_last_refill = now;
        g_model_maintenance_reject_tokens = MODEL_MAINTENANCE_REJECT_GLOBAL_BURST;
    }
    {
        const TickType_t elapsed = now - session->maintenance_reject_last_refill;
        const uint32_t periods = (uint32_t)(elapsed / interval);
        if (periods > 0u)
        {
            const uint32_t replenished = session->maintenance_reject_tokens + periods;
            session->maintenance_reject_tokens = (uint8_t)(replenished > MODEL_MAINTENANCE_REJECT_SESSION_BURST
                ? MODEL_MAINTENANCE_REJECT_SESSION_BURST : replenished);
            session->maintenance_reject_last_refill += (TickType_t)(periods * interval);
        }
    }
    {
        const TickType_t elapsed = now - g_model_maintenance_reject_last_refill;
        const uint32_t periods = (uint32_t)(elapsed / interval);
        if (periods > 0u)
        {
            const uint32_t replenished = g_model_maintenance_reject_tokens + periods * 8u;
            g_model_maintenance_reject_tokens = (uint8_t)(replenished > MODEL_MAINTENANCE_REJECT_GLOBAL_BURST
                ? MODEL_MAINTENANCE_REJECT_GLOBAL_BURST : replenished);
            g_model_maintenance_reject_last_refill += (TickType_t)(periods * interval);
        }
    }
    if (session->maintenance_reject_tokens > 0u && g_model_maintenance_reject_tokens > 0u)
    {
        --session->maintenance_reject_tokens;
        --g_model_maintenance_reject_tokens;
        admitted = true;
        if (out_suppressed != NULL)
        {
            *out_suppressed = session->maintenance_reject_suppressed;
        }
        if (out_last_reason != NULL)
        {
            *out_last_reason = session->maintenance_reject_last_reason;
        }
        session->maintenance_reject_suppressed = 0u;
    }
    else
    {
        if (session->maintenance_reject_suppressed < MODEL_MAINTENANCE_REJECT_SUPPRESSION_MAX)
        {
            ++session->maintenance_reject_suppressed;
        }
        session->maintenance_reject_last_reason = reason;
    }
    taskEXIT_CRITICAL();
    return admitted;
}

void model_audit_maintenance_rejection(void *session_context,
                                              const char *method,
                                              UA_StatusCode reason)
{
    char details[96];
    uint16_t suppressed = 0u;
    UA_StatusCode last_reason = reason;

    if (!model_maintenance_rejected_audit_admit(session_context, reason,
                                                &suppressed, &last_reason))
    {
        return;
    }
    if (suppressed > 0u)
    {
        (void)snprintf(details, sizeof(details),
                       "rejected=0x%08x; suppressed=%u; last=0x%08x",
                       (unsigned)reason, (unsigned)suppressed,
                       (unsigned)last_reason);
    }
    else
    {
        (void)snprintf(details, sizeof(details), "rejected=0x%08x",
                       (unsigned)reason);
    }
    model_audit_session_event(session_context, "maintenance-call", method,
                              false, details);
}

void model_audit_maintenance_accept(void *session_context,
                                           const char *method)
{
    model_audit_session_event(session_context, "maintenance-call", method,
                              true, "accepted");
}

void model_audit_maintenance_state_change(void *session_context,
                                                  const char *method,
                                                  const maintenance_service_snapshot_t *snapshot)
{
    char details[96];

    if (snapshot == NULL)
    {
        return;
    }
    (void)snprintf(details, sizeof(details), "revision=%lu; state=%s",
                   (unsigned long)snapshot->revision,
                   model_maintenance_state_text(snapshot->state));
    model_audit_session_event(session_context, "maintenance-state", method,
                              true, details);
}

UA_StatusCode model_require_session_role(void *session_context,
                                                security_user_role_t required_role,
                                                size_t output_size,
                                                UA_Variant *output)
{
    security_user_role_t current_role = SECURITY_ROLE_VIEWER;

#if !(RP2_BENCHMARK_PROFILE_ENABLED && RP2_BENCHMARK_SECURITY_NONE)
    if (required_role > SECURITY_ROLE_VIEWER && !model_session_is_secure(session_context))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADSECURITYMODEINSUFFICIENT;
    }
#endif

    if (!model_session_get_role(session_context, &current_role) || current_role < required_role)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_require_admin_session(void *session_context,
                                                 size_t output_size,
                                                 UA_Variant *output)
{
    return model_require_session_role(session_context,
                                      SECURITY_ROLE_ADMIN,
                                      output_size,
                                      output);
}

/* Implementation note: order matters. This checks, in sequence, (1) the four
 * specific maintenance-workflow method NodeIds (Service role), (2) the fixed
 * admin-only set (model_is_admin_only_method), (3) any Device.Security
 * method (Admin), (4) a small set of specific Admin-adjacent
 * Device.Maintenance.* methods that are Operator-level, then (5) falls back
 * to Service for any other Device.Maintenance method and Viewer otherwise.
 * A method NodeId under Device.Maintenance therefore never reaches the
 * final Viewer default unless it is also excluded by every earlier check. */
security_user_role_t model_required_role_for_method(const UA_NodeId *method_id)
{
    const UA_NodeId plan_maintenance =
        UA_NODEID_STRING(1, (char *)MODEL_METHOD_PLAN_MAINTENANCE);
    const UA_NodeId reschedule_maintenance =
        UA_NODEID_STRING(1, (char *)MODEL_METHOD_RESCHEDULE_MAINTENANCE);
    const UA_NodeId start_maintenance =
        UA_NODEID_STRING(1, (char *)MODEL_METHOD_START_MAINTENANCE);
    const UA_NodeId finish_maintenance =
        UA_NODEID_STRING(1, (char *)MODEL_METHOD_FINISH_MAINTENANCE);

    if (method_id == NULL)
    {
        return SECURITY_ROLE_ADMIN;
    }

    if (UA_NodeId_equal(method_id, &plan_maintenance) ||
        UA_NodeId_equal(method_id, &reschedule_maintenance) ||
        UA_NodeId_equal(method_id, &start_maintenance) ||
        UA_NodeId_equal(method_id, &finish_maintenance))
    {
        return SECURITY_ROLE_SERVICE;
    }

    if (model_is_admin_only_method(method_id))
    {
        return SECURITY_ROLE_ADMIN;
    }

    if (model_nodeid_string_has_prefix(method_id, "Device.Security"))
    {
        return SECURITY_ROLE_ADMIN;
    }

    if (model_nodeid_string_has_prefix(method_id, "Device.Maintenance.AcknowledgeAllAlarms") ||
        model_nodeid_string_has_prefix(method_id, "Device.Maintenance.ApplyConfig") ||
        model_nodeid_string_has_prefix(method_id, "Device.Maintenance.SaveConfig") ||
        model_nodeid_string_has_prefix(method_id, MODEL_METHOD_SET_MQTT_PUBSUB_ENABLED))
    {
        return SECURITY_ROLE_OPERATOR;
    }

    if (model_node_is_maintenance_method(method_id))
    {
        return SECURITY_ROLE_SERVICE;
    }

    return SECURITY_ROLE_VIEWER;
}

security_user_role_t model_required_role_for_write_kind(model_node_kind_t kind)
{
    switch (kind)
    {
    case MODEL_NODE_CHANNEL_VALUE:
    case MODEL_NODE_IDENT_ASSET_ID:
    case MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS:
    case MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS:
    case MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES:
    case MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS:
    case MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS:
    case MODEL_NODE_CONFIG_TEMP_OFFSET:
    case MODEL_NODE_CONFIG_TEMP_SCALE:
    case MODEL_NODE_CONFIG_HIGH_ALARM:
    case MODEL_NODE_CONFIG_HIGH_HIGH_ALARM:
        return SECURITY_ROLE_OPERATOR;
    default:
        return SECURITY_ROLE_ADMIN;
    }
}

UA_StatusCode model_require_method_role(void *session_context,
                                               const UA_NodeId *method_id,
                                               size_t output_size,
                                               UA_Variant *output)
{
    return model_require_session_role(session_context,
                                      model_required_role_for_method(method_id),
                                      output_size,
                                      output);
}

bool model_is_admin_only_method(const UA_NodeId *method_id)
{
    const UA_NodeId add_user = UA_NODEID_STRING(1, (char *)MODEL_METHOD_ADD_USER);
    const UA_NodeId change_password = UA_NODEID_STRING(1, (char *)MODEL_METHOD_CHANGE_PASSWORD);
    const UA_NodeId set_role = UA_NODEID_STRING(1, (char *)MODEL_METHOD_SET_ROLE);
    const UA_NodeId disable_user = UA_NODEID_STRING(1, (char *)MODEL_METHOD_DISABLE_USER);
    const UA_NodeId delete_user = UA_NODEID_STRING(1, (char *)MODEL_METHOD_DELETE_USER);
    const UA_NodeId reset_maintenance_journal =
        UA_NODEID_STRING(1, (char *)MODEL_METHOD_RESET_MAINTENANCE_JOURNAL);

    return method_id != NULL &&
           (UA_NodeId_equal(method_id, &add_user) ||
            UA_NodeId_equal(method_id, &change_password) ||
            UA_NodeId_equal(method_id, &set_role) ||
            UA_NodeId_equal(method_id, &disable_user) ||
            UA_NodeId_equal(method_id, &delete_user) ||
            UA_NodeId_equal(method_id, &reset_maintenance_journal));
}

bool model_copy_input_string(const UA_Variant *value, char *buffer, size_t buffer_size)
{
    const UA_String *input_string = NULL;

    if (value == NULL || buffer == NULL || buffer_size == 0u ||
        !UA_Variant_hasScalarType(value, &UA_TYPES[UA_TYPES_STRING]))
    {
        return false;
    }

    input_string = (const UA_String *)value->data;
    if (input_string == NULL || input_string->length >= buffer_size)
    {
        return false;
    }

    memcpy(buffer, input_string->data, input_string->length);
    buffer[input_string->length] = '\0';
    return true;
}

bool model_get_input_ua_string(const UA_Variant *value, const UA_String **out_string)
{
    if (value == NULL || out_string == NULL ||
        !UA_Variant_hasScalarType(value, &UA_TYPES[UA_TYPES_STRING]) ||
        value->data == NULL)
    {
        return false;
    }

    *out_string = (const UA_String *)value->data;
    return true;
}

bool model_get_input_ua_bytestring(const UA_Variant *value, const UA_ByteString **out_bytes)
{
    if (value == NULL || out_bytes == NULL ||
        !UA_Variant_hasScalarType(value, &UA_TYPES[UA_TYPES_BYTESTRING]) ||
        value->data == NULL)
    {
        return false;
    }

    *out_bytes = (const UA_ByteString *)value->data;
    return true;
}

bool model_has_config_text(const char *value)
{
    return value != NULL && value[0] != '\0';
}

void model_format_mac_string(const uint8_t mac[6], char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u || mac == NULL)
    {
        return;
    }

    (void)snprintf(buffer,
                   buffer_size,
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0],
                   mac[1],
                   mac[2],
                   mac[3],
                   mac[4],
                   mac[5]);
    buffer[buffer_size - 1u] = '\0';
}

bool model_parse_ipv4_string(const char *text, uint8_t out_ipv4[4])
{
    unsigned int octets[4] = {0};

    if (text == NULL || out_ipv4 == NULL ||
        sscanf(text, "%u.%u.%u.%u", &octets[0], &octets[1], &octets[2], &octets[3]) != 4)
    {
        return false;
    }

    for (size_t index = 0u; index < 4u; ++index)
    {
        if (octets[index] > 255u)
        {
            return false;
        }

        out_ipv4[index] = (uint8_t)octets[index];
    }

    return true;
}

bool model_parse_mac_string(const char *text, uint8_t out_mac[6])
{
    unsigned int octets[6] = {0};

    if (text == NULL || out_mac == NULL)
    {
        return false;
    }

    if (sscanf(text,
               "%x:%x:%x:%x:%x:%x",
               &octets[0],
               &octets[1],
               &octets[2],
               &octets[3],
               &octets[4],
               &octets[5]) != 6 &&
        sscanf(text,
               "%x-%x-%x-%x-%x-%x",
               &octets[0],
               &octets[1],
               &octets[2],
               &octets[3],
               &octets[4],
               &octets[5]) != 6)
    {
        return false;
    }

    for (size_t index = 0u; index < 6u; ++index)
    {
        if (octets[index] > 255u)
        {
            return false;
        }

        out_mac[index] = (uint8_t)octets[index];
    }

    return true;
}

bool model_identity_is_complete(const device_profile_config_t *config)
{
    return config != NULL &&
           model_has_config_text(config->device_name) &&
           model_has_config_text(config->opcua_hostname) &&
           model_has_config_text(config->product_instance_uri) &&
           model_has_config_text(config->serial_number) &&
           model_has_config_text(config->asset_id) &&
           model_has_config_text(config->hw_revision) &&
           model_has_config_text(config->fw_revision) &&
           model_has_config_text(config->location) &&
           model_has_config_text(config->asset_class) &&
           config->manufacture_date_utc > 0 &&
           config->calibration_date_utc > 0;
}

bool model_network_config_is_complete(const device_profile_config_t *config)
{
    if (config == NULL || config->opcua_port == 0u || !model_has_config_text(config->opcua_hostname))
    {
        return false;
    }

    if (config->network_mode == NETWORK_MODE_DHCP)
    {
        return true;
    }

    return !(config->static_ip[0] == 0u && config->static_ip[1] == 0u &&
             config->static_ip[2] == 0u && config->static_ip[3] == 0u);
}

bool model_has_trust_anchors(void)
{
    security_service_blob_view_t trust_list = {0};
    return security_service_get_trust_list(&trust_list) && trust_list.length > 0u;
}

bool model_is_commissioned(void)
{
    const device_profile_config_t *config = config_service_get();

    return model_identity_is_complete(config) &&
           model_network_config_is_complete(config) &&
           model_has_trust_anchors() &&
           !security_service_is_commissioning_mode();
}

UA_StatusCode model_login_callback(const UA_String *user_name,
                                          const UA_ByteString *password,
                                          size_t username_password_login_size,
                                          const UA_UsernamePasswordLogin *username_password_login,
                                          void **session_context,
                                          void *login_context)
{
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    security_user_role_t role = SECURITY_ROLE_VIEWER;
    uint32_t retry_after_ms = 0u;
    security_auth_result_t auth_result;
    (void)username_password_login_size;
    (void)username_password_login;
    (void)session_context;
    (void)login_context;

    if (user_name == NULL || password == NULL || user_name->length >= sizeof(username))
    {
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    memcpy(username, user_name->data, user_name->length);
    username[user_name->length] = '\0';

    auth_result = security_service_authenticate_user(username,
                                                     password->data,
                                                     password->length,
                                                     &role,
                                                     &retry_after_ms);
    if (auth_result != SECURITY_AUTH_RESULT_SUCCESS)
    {
        if (auth_result == SECURITY_AUTH_RESULT_LOCKED_OUT)
        {
            printf("[OPC UA]\tLogin rejected for user '%s': locked retry_after_ms=%u\n",
                   username,
                   (unsigned)retry_after_ms);
        }
        else
        {
            printf("[OPC UA]\tLogin rejected for user '%s'\n", username);
        }
        fflush(stdout);
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    printf("[OPC UA]\tLogin accepted for user '%s' as %s\n",
           username,
           security_service_role_to_string(role));
    fflush(stdout);

    if (session_context != NULL)
    {
        *session_context = model_allocate_session_username(user_name);
        if (*session_context == NULL)
        {
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_activate_session_secure_wrapper(UA_Server *server,
                                                           UA_AccessControl *ac,
                                                           const UA_EndpointDescription *endpoint_description,
                                                           const UA_ByteString *secure_channel_remote_certificate,
                                                           const UA_NodeId *session_id,
                                                           const UA_ExtensionObject *user_identity_token,
                                                           void **session_context)
{
    model_session_context_t *wrapped_context = NULL;
    void *base_context = NULL;
    UA_StatusCode status;

    if (g_default_activate_session == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    status = g_default_activate_session(server,
                                        ac,
                                        endpoint_description,
                                        secure_channel_remote_certificate,
                                        session_id,
                                        user_identity_token,
                                        &base_context);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tActivateSession rejected: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        return status;
    }

    wrapped_context = (model_session_context_t *)UA_calloc(1u, sizeof(model_session_context_t));
    if (wrapped_context == NULL)
    {
        if (base_context != NULL && g_default_close_session != NULL)
        {
            g_default_close_session(server, ac, session_id, base_context);
        }
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    wrapped_context->base_context = base_context;
    wrapped_context->secure_channel = endpoint_description != NULL &&
                                      endpoint_description->securityMode != UA_MESSAGESECURITYMODE_NONE &&
                                      !UA_ByteString_equal(&endpoint_description->securityPolicyUri,
                                                           &UA_SECURITY_POLICY_NONE_URI);
    *session_context = wrapped_context;
    if (wrapped_context->secure_channel)
    {
        model_emit_client_certificate_expiry_notice(server,
                                                    secure_channel_remote_certificate,
                                                    wrapped_context);
    }
    ua_port_alloc_log_stats("ActivateSession completed");
    return UA_STATUSCODE_GOOD;
}

void model_close_session_secure_wrapper(UA_Server *server,
                                               UA_AccessControl *ac,
                                               const UA_NodeId *session_id,
                                               void *session_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    void *base_context = wrapped_context != NULL ? wrapped_context->base_context : NULL;

    /* HistoryReadRaw points are session-owned.  Drop them before either the
     * wrapped or default access-control context can be released, preventing
     * a later session from inheriting a recycled context address. */
    model_history_release_session_points(session_context);

    if (g_default_close_session != NULL)
    {
        g_default_close_session(server, ac, session_id, base_context);
    }

    model_free_session_username(base_context);
    UA_free(wrapped_context);
    ua_port_alloc_log_stats("CloseSession completed");
}

UA_Boolean model_get_user_executable_wrapper(UA_Server *server,
                                                    UA_AccessControl *ac,
                                                    const UA_NodeId *session_id,
                                                    void *session_context,
                                                    const UA_NodeId *method_id,
                                                    void *method_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Boolean base_allowed =
        g_default_get_user_executable != NULL
            ? g_default_get_user_executable(server,
                                            ac,
                                            session_id,
                                            wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                            method_id,
                                            method_context)
            : true;

    if (!base_allowed)
    {
        return false;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_maintenance_method(method_id))
    {
        return false;
    }

    return model_session_has_role(session_context, model_required_role_for_method(method_id));
}

UA_Boolean model_get_user_executable_on_object_wrapper(UA_Server *server,
                                                              UA_AccessControl *ac,
                                                              const UA_NodeId *session_id,
                                                              void *session_context,
                                                              const UA_NodeId *method_id,
                                                              void *method_context,
                                                              const UA_NodeId *object_id,
                                                              void *object_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Boolean base_allowed =
        g_default_get_user_executable_on_object != NULL
            ? g_default_get_user_executable_on_object(server,
                                                      ac,
                                                      session_id,
                                                      wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                                      method_id,
                                                      method_context,
                                                      object_id,
                                                      object_context)
            : true;

    if (!base_allowed)
    {
        return false;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_maintenance_method(method_id))
    {
        return false;
    }

    return model_session_has_role(session_context, model_required_role_for_method(method_id));
}

/* Implementation note: beyond the header's anonymous-session summary, this
 * also strips write access from MODEL_NODE_CHANNEL_VALUE nodes for any
 * session (including authenticated ones) that is not over a secure
 * SecureChannel (model_session_is_secure) — writable channel values must
 * not be settable over SecurityPolicy#None. Role sufficiency
 * (model_required_role_for_write_kind) is re-checked here as defense in
 * depth even though the generic write dispatch (model_write_value) checks
 * it again before actually writing. */
UA_Byte model_get_user_access_level_wrapper(UA_Server *server,
                                                   UA_AccessControl *ac,
                                                   const UA_NodeId *session_id,
                                                   void *session_context,
                                                   const UA_NodeId *node_id,
                                                   void *node_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Byte base_access =
        g_default_get_user_access_level != NULL
            ? g_default_get_user_access_level(server,
                                              ac,
                                              session_id,
                                              wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                              node_id,
                                              node_context)
            : 0u;

    if (model_node_is_t2_resource_diagnostic(node_id) &&
        !model_session_may_access_t2_resource_diagnostic(session_context))
    {
        return 0u;
    }

    if (model_session_is_anonymous(session_context))
    {
        if (model_node_is_anonymous_hidden(node_id))
        {
            return 0u;
        }

        return model_strip_anonymous_write_access(base_access);
    }

#if !(RP2_BENCHMARK_PROFILE_ENABLED && RP2_BENCHMARK_SECURITY_NONE)
    if (node_context != NULL &&
        (((const model_node_context_t *)node_context)->kind == MODEL_NODE_CHANNEL_VALUE &&
         !model_session_is_secure(session_context)))
    {
        return model_strip_anonymous_write_access(base_access);
    }
#endif

    if (node_context != NULL &&
        !model_session_has_role(session_context,
                                model_required_role_for_write_kind(((const model_node_context_t *)node_context)->kind)))
    {
        return model_strip_anonymous_write_access(base_access);
    }

    return base_access;
}

UA_Boolean model_allow_browse_node_wrapper(UA_Server *server,
                                                  UA_AccessControl *ac,
                                                  const UA_NodeId *session_id,
                                                  void *session_context,
                                                  const UA_NodeId *node_id,
                                                  void *node_context)
{
    model_session_context_t *wrapped_context = (model_session_context_t *)session_context;
    const UA_Boolean base_allowed =
        g_default_allow_browse_node != NULL
            ? g_default_allow_browse_node(server,
                                          ac,
                                          session_id,
                                          wrapped_context != NULL ? wrapped_context->base_context : NULL,
                                          node_id,
                                          node_context)
            : true;

    if (!base_allowed)
    {
        return false;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_anonymous_hidden(node_id))
    {
        return false;
    }

    if (model_node_is_t2_resource_diagnostic(node_id) &&
        !model_session_may_access_t2_resource_diagnostic(session_context))
    {
        return false;
    }

    return true;
}

UA_StatusCode model_quality_to_status(service_quality_t quality)
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

const char *model_quality_to_string(service_quality_t quality)
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

const char *model_alarm_state_to_string(telemetry_alarm_state_t alarm_state)
{
    switch (alarm_state)
    {
    case TELEMETRY_ALARM_STATE_NORMAL:
        return "Normal";
    case TELEMETRY_ALARM_STATE_WARNING:
        return "Warning";
    case TELEMETRY_ALARM_STATE_CRITICAL:
        return "Critical";
    case TELEMETRY_ALARM_STATE_INVALID:
    default:
        return "Invalid";
    }
}

bool model_copy_channel_by_id(const char *channel_id, telemetry_channel_snapshot_t *out_snapshot)
{
    if (channel_id == NULL || out_snapshot == NULL)
    {
        return false;
    }

    return channel_manager_copy_snapshot_by_id(channel_id, out_snapshot);
}

bool model_copy_channel_from_context(const model_node_context_t *context,
                                     telemetry_channel_snapshot_t *out_snapshot)
{
    return context != NULL &&
           context->channel_id != NULL &&
           model_copy_channel_by_id(context->channel_id, out_snapshot);
}

bool model_copy_channel_settings(const char *channel_id, telemetry_channel_settings_t *out_settings)
{
    telemetry_channel_snapshot_t snapshot = {0};

    if (channel_id == NULL || out_settings == NULL || !model_copy_channel_by_id(channel_id, &snapshot))
    {
        return false;
    }

    *out_settings = snapshot.channel.settings;
    return true;
}

UA_NodeId model_channel_value_type_to_data_type(telemetry_channel_value_type_t value_type)
{
    switch (value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        return UA_TYPES[UA_TYPES_DOUBLE].typeId;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        return UA_TYPES[UA_TYPES_INT32].typeId;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        return UA_TYPES[UA_TYPES_UINT32].typeId;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        return UA_TYPES[UA_TYPES_INT64].typeId;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        return UA_TYPES[UA_TYPES_UINT64].typeId;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return UA_TYPES[UA_TYPES_BOOLEAN].typeId;
    case TELEMETRY_CHANNEL_VALUE_STRING:
        return UA_TYPES[UA_TYPES_STRING].typeId;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        return UA_TYPES[UA_TYPES_FLOAT].typeId;
    }
}

/* Implementation note: the channel id is hex-encoded (2 chars/byte) rather
 * than embedded as raw text, so any channel id value is safe to use as an
 * OPC UA NodeId path segment (no '.', '/', or non-ASCII bytes can break the
 * "Device.Channels.c_<hex>[.<field>]" path or collide with a sibling
 * literal), at the cost of an unreadable NodeId string. */
bool model_build_channel_node_id(const telemetry_channel_t *channel,
                                        const char *field_name,
                                        char *buffer,
                                        size_t buffer_size)
{
    static const char hex[] = "0123456789ABCDEF";
    const char *id;
    size_t id_length;
    size_t offset = 0u;

    if (channel == NULL || channel->metadata.id == NULL || buffer == NULL || buffer_size == 0u)
    {
        return false;
    }

    id = channel->metadata.id;
    id_length = 0u;
    while (id_length < TELEMETRY_CHANNEL_ID_MAX_LEN && id[id_length] != '\0')
    {
        ++id_length;
    }
    if (id_length == 0u || id_length >= TELEMETRY_CHANNEL_ID_MAX_LEN)
    {
        return false;
    }

    offset = (size_t)snprintf(buffer, buffer_size, "Device.Channels.c_");
    if (offset >= buffer_size)
    {
        return false;
    }
    for (size_t index = 0u; index < id_length; ++index)
    {
        const unsigned char byte = (unsigned char)id[index];
        if (offset + 2u >= buffer_size)
        {
            return false;
        }
        buffer[offset++] = hex[(byte >> 4u) & 0x0fu];
        buffer[offset++] = hex[byte & 0x0fu];
    }
    if (field_name != NULL && field_name[0] != '\0')
    {
        const size_t field_length = strlen(field_name);
        if (offset + 1u + field_length >= buffer_size)
        {
            return false;
        }
        buffer[offset++] = '.';
        memcpy(&buffer[offset], field_name, field_length);
        offset += field_length;
    }
    buffer[offset] = '\0';
    return true;
}

bool model_build_legacy_channel_node_id(const telemetry_channel_t *channel,
                                        const char *field_name,
                                        char *buffer,
                                        size_t buffer_size)
{
    const int written = (channel != NULL && channel->metadata.name != NULL &&
                         field_name != NULL && field_name[0] != '\0')
                            ? snprintf(buffer, buffer_size, "Device.Channels.%s.%s",
                                       channel->metadata.name, field_name)
                            : (channel != NULL && channel->metadata.name != NULL)
                                  ? snprintf(buffer, buffer_size, "Device.Channels.%s",
                                             channel->metadata.name)
                                  : -1;
    return written > 0 && (size_t)written < buffer_size;
}

bool model_channel_requires_server_rebuild(const config_channel_definition_t *lhs,
                                           const config_channel_definition_t *rhs)
{
    const uint32_t model_flags = TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                                 TELEMETRY_CHANNEL_FLAG_WRITABLE;

    if (lhs == NULL || rhs == NULL)
    {
        return lhs != rhs;
    }

    /* The channel hierarchy is intentionally materialized only at server
     * startup.  Compare precisely the persisted fields copied into its static
     * generic/legacy representation: topology/identity (in_use, id, name),
     * typed Value and metadata (kind, value_type, source, description, unit,
     * range), generic properties (sample/publish periods and model flags).
     * Do not compare padding or runtime-only acquisition/alarm settings. */
    return lhs->in_use != rhs->in_use ||
           lhs->kind != rhs->kind ||
           lhs->value_type != rhs->value_type ||
           lhs->driver_id != rhs->driver_id ||
           lhs->source_index != rhs->source_index ||
           lhs->source_flags != rhs->source_flags ||
           (lhs->has_minimum != 0u) != (rhs->has_minimum != 0u) ||
           (lhs->has_maximum != 0u) != (rhs->has_maximum != 0u) ||
           ((lhs->has_minimum != 0u) && (lhs->has_maximum != 0u) &&
            (rhs->has_minimum != 0u) && (rhs->has_maximum != 0u) &&
            (lhs->minimum != rhs->minimum || lhs->maximum != rhs->maximum)) ||
           (lhs->flags & model_flags) != (rhs->flags & model_flags) ||
           model_string_changed(lhs->id, rhs->id) ||
           model_string_changed(lhs->name, rhs->name) ||
           model_string_changed(lhs->description, rhs->description) ||
           model_string_changed(lhs->unit, rhs->unit) ||
           lhs->settings.sample_period_ms != rhs->settings.sample_period_ms ||
           lhs->settings.publish_period_ms != rhs->settings.publish_period_ms;
}

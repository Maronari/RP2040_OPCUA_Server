/**
 * @file
 * @brief Generic `UA_DataSource.write` dispatch (`model_write_value`) plus
 *        the configuration/identity/reboot/diagnostics `UA_MethodCallback`
 *        implementations registered under `Device.Maintenance`.
 *
 * Responsibilities:
 * - `model_write_value` is installed as the `write` callback on every
 *   writable variable node; it re-checks role/security-mode, then delegates
 *   each node kind to exactly one service call
 *   (`channel_manager_write_value`, `config_service_set_asset_id`,
 *   `channel_manager_update_settings`,
 *   `config_service_set_timezone_offset_minutes`) — it never mutates
 *   service-owned state directly.
 * - The `Device.Maintenance.*` method callbacks in this file cover the
 *   deferred-action group (Reboot, BlinkIdentify, FactoryReset,
 *   AcknowledgeAllAlarms, SyncTimeNow) and the staged-configuration group
 *   (ApplyConfig, SaveConfig, SetDeviceIdentity, SetLocationPaths,
 *   SetNetworkConfig, SetMqttPubSubEnabled, GetChannelConfig, ListDrivers,
 *   GetConfigState).
 *
 * Ownership: this file owns no persistent state; `model_schedule_reboot` /
 * `model_schedule_identify_blink` write into the single
 * `opcua_model_context_t::pending_actions` (`g_pending_actions`), consumed
 * by `model_core_task`'s poll loop. `model_get_config_state` borrows the
 * one shared PSRAM scratch buffer (`g_model_maintenance_export_payload`).
 *
 * Concurrency: every function here runs on `OpcUaTask` inside open62541's
 * synchronous Write/Call service dispatch; none is reentrant across tasks.
 */
#include "opcua_model_internal.h"

/**
 * @brief Maps a `channel_manager_write_result_t` to the matching
 *        `UA_StatusCode`, used by `model_write_value`'s
 *        `MODEL_NODE_CHANNEL_VALUE` case to translate the channel-manager
 *        write outcome into an OPC UA status.
 * @note File-local; not declared in `opcua_model_internal.h`.
 */
static UA_StatusCode model_channel_write_result_to_status(channel_manager_write_result_t result)
{
    switch (result)
    {
    case CHANNEL_MANAGER_WRITE_RESULT_OK: return UA_STATUSCODE_GOOD;
    case CHANNEL_MANAGER_WRITE_RESULT_NOT_FOUND: return UA_STATUSCODE_BADNODEIDUNKNOWN;
    case CHANNEL_MANAGER_WRITE_RESULT_DISABLED:
    case CHANNEL_MANAGER_WRITE_RESULT_STALE:
    case CHANNEL_MANAGER_WRITE_RESULT_INVALID_LIFECYCLE: return UA_STATUSCODE_BADINVALIDSTATE;
    case CHANNEL_MANAGER_WRITE_RESULT_READ_ONLY: return UA_STATUSCODE_BADNOTWRITABLE;
    case CHANNEL_MANAGER_WRITE_RESULT_TYPE_MISMATCH: return UA_STATUSCODE_BADTYPEMISMATCH;
    case CHANNEL_MANAGER_WRITE_RESULT_UNSUPPORTED: return UA_STATUSCODE_BADNOTSUPPORTED;
    case CHANNEL_MANAGER_WRITE_RESULT_TIMEOUT: return UA_STATUSCODE_BADTIMEOUT;
    case CHANNEL_MANAGER_WRITE_RESULT_HARDWARE_FAILURE: return UA_STATUSCODE_BADDEVICEFAILURE;
    case CHANNEL_MANAGER_WRITE_RESULT_RESOURCE_FAILURE: return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    default: return UA_STATUSCODE_BADINTERNALERROR;
    }
}

/**
 * @brief Converts an incoming `UA_Variant` scalar into a
 *        `telemetry_channel_value_t` of exactly @p type, for writes to
 *        `MODEL_NODE_CHANNEL_VALUE` nodes.
 * @param variant Write value from open62541; must hold a scalar exactly
 *        matching @p type's expected `UA_TYPES` entry (no implicit numeric
 *        conversion between e.g. Int32 and Float).
 * @param type Channel's declared `telemetry_channel_value_type_t`, from the
 *        channel snapshot (`model_write_value` looks this up first).
 * @param out_value Zeroed and populated on success.
 * @return `UA_STATUSCODE_GOOD` on success; `UA_STATUSCODE_BADTYPEMISMATCH`
 *         if @p variant's type does not match; `UA_STATUSCODE_BADOUTOFRANGE`
 *         if a string value does not fit `TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN`;
 *         `UA_STATUSCODE_BADNODATA` for a NULL/empty @p variant.
 * @note File-local; not declared in `opcua_model_internal.h`.
 */
static UA_StatusCode model_copy_channel_write_variant(const UA_Variant *variant,
                                                       telemetry_channel_value_type_t type,
                                                       telemetry_channel_value_t *out_value)
{
    const UA_String *string_value;

    if (variant == NULL || out_value == NULL || variant->data == NULL)
    {
        return UA_STATUSCODE_BADNODATA;
    }
    memset(out_value, 0, sizeof(*out_value));
    switch (type)
    {
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_FLOAT])) return UA_STATUSCODE_BADTYPEMISMATCH;
        out_value->f32 = *(const UA_Float *)variant->data;
        return UA_STATUSCODE_GOOD;
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_DOUBLE])) return UA_STATUSCODE_BADTYPEMISMATCH;
        out_value->f64 = *(const UA_Double *)variant->data;
        return UA_STATUSCODE_GOOD;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_INT32])) return UA_STATUSCODE_BADTYPEMISMATCH;
        out_value->i32 = *(const UA_Int32 *)variant->data;
        return UA_STATUSCODE_GOOD;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_UINT32])) return UA_STATUSCODE_BADTYPEMISMATCH;
        out_value->u32 = *(const UA_UInt32 *)variant->data;
        return UA_STATUSCODE_GOOD;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_INT64])) return UA_STATUSCODE_BADTYPEMISMATCH;
        out_value->i64 = *(const UA_Int64 *)variant->data;
        return UA_STATUSCODE_GOOD;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_UINT64])) return UA_STATUSCODE_BADTYPEMISMATCH;
        out_value->u64 = *(const UA_UInt64 *)variant->data;
        return UA_STATUSCODE_GOOD;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_BOOLEAN])) return UA_STATUSCODE_BADTYPEMISMATCH;
        out_value->boolean = *(const UA_Boolean *)variant->data;
        return UA_STATUSCODE_GOOD;
    case TELEMETRY_CHANNEL_VALUE_STRING:
        if (!UA_Variant_hasScalarType(variant, &UA_TYPES[UA_TYPES_STRING])) return UA_STATUSCODE_BADTYPEMISMATCH;
        string_value = (const UA_String *)variant->data;
        if (string_value->length >= TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN ||
            (string_value->length > 0u && string_value->data == NULL)) return UA_STATUSCODE_BADOUTOFRANGE;
        if (string_value->length > 0u) memcpy(out_value->string, string_value->data, string_value->length);
        out_value->string[string_value->length] = '\0';
        return UA_STATUSCODE_GOOD;
    default:
        return UA_STATUSCODE_BADTYPEMISMATCH;
    }
}

UA_StatusCode model_write_value(UA_Server *server,
                                       const UA_NodeId *session_id,
                                       void *session_context,
                                       const UA_NodeId *node_id,
                                       void *node_context,
                                       const UA_NumericRange *range,
                                       const UA_DataValue *value)
{
    model_node_context_t *context = (model_node_context_t *)node_context;
    telemetry_channel_settings_t channel_settings = {0};
    const char *settings_channel_id = NULL;
    bool save_device_config = false;
    (void)session_id;
    (void)session_context;
    (void)node_id;

    if (context == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    if (range != NULL)
    {
        return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }

    if (value == NULL || !value->hasValue)
    {
        return UA_STATUSCODE_BADNODATA;
    }

    if (model_session_is_anonymous(session_context))
    {
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    if (!model_session_has_role(session_context, model_required_role_for_write_kind(context->kind)))
    {
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }

    /* Implementation note: the legacy fixed Device.Configuration.* nodes
     * (Sampling/Publish period, TempOffset/Scale, High/HighHigh alarm) carry
     * no channel_id of their own — they always resolve to the single legacy
     * temperature channel (DEVICE_CHANNEL_ID_TEMPERATURE_1), matching the
     * pre-v0.4.2 single-channel model these nodes predate. */
    if (context->channel_id != NULL)
    {
        settings_channel_id = context->channel_id;
    }
    else if (context->kind == MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS ||
             context->kind == MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS ||
             context->kind == MODEL_NODE_CONFIG_TEMP_OFFSET ||
             context->kind == MODEL_NODE_CONFIG_TEMP_SCALE ||
             context->kind == MODEL_NODE_CONFIG_HIGH_ALARM ||
             context->kind == MODEL_NODE_CONFIG_HIGH_HIGH_ALARM)
    {
        settings_channel_id = DEVICE_CHANNEL_ID_TEMPERATURE_1;
    }

    switch (context->kind)
    {
    case MODEL_NODE_CHANNEL_VALUE:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        telemetry_channel_value_t channel_value;
        UA_StatusCode conversion_status;

        if (!model_session_is_secure(session_context))
        {
            return UA_STATUSCODE_BADSECURITYMODEINSUFFICIENT;
        }
        if (!model_copy_channel_from_context(context, &snapshot))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        conversion_status = model_copy_channel_write_variant(&value->value,
                                                             snapshot.channel.metadata.value_type,
                                                             &channel_value);
        if (conversion_status != UA_STATUSCODE_GOOD)
        {
            return conversion_status;
        }
        return model_channel_write_result_to_status(
            channel_manager_write_value(context->channel_id,
                                        snapshot.channel.metadata.value_type,
                                        &channel_value,
                                        NULL));
    }
    case MODEL_NODE_IDENT_ASSET_ID:
    {
        char asset_id[CONFIG_SERVICE_ASSET_ID_MAX_LEN] = {0};

        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_STRING]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (!model_copy_input_string(&value->value, asset_id, sizeof(asset_id)))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        if (!config_service_set_asset_id(asset_id))
        {
            return UA_STATUSCODE_BADINTERNALERROR;
        }
        {
            const UA_StatusCode alias_status = model_update_asset_id_alias(server, asset_id);
            if (alias_status != UA_STATUSCODE_GOOD)
            {
                printf("[OPC UA]\tFailed to update AssetId alias: 0x%x (%s)\n",
                       alias_status,
                       UA_StatusCode_name(alias_status));
                fflush(stdout);
            }
        }
        break;
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS:
    case MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_UINT32]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.sample_period_ms = *(const UA_UInt32 *)value->value.data;
        if (channel_settings.sample_period_ms == 0u ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_UINT32]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.history_interval_ms = *(const UA_UInt32 *)value->value.data;
        if (!channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_UINT32]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.publish_period_ms = *(const UA_UInt32 *)value->value.data;
        if (channel_settings.publish_period_ms == 0u ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_INT16]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (!config_service_set_timezone_offset_minutes(*(const UA_Int16 *)value->value.data))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        save_device_config = true;
        break;
    case MODEL_NODE_CONFIG_TEMP_OFFSET:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.offset = *(const UA_Float *)value->value.data;
        if (!channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_TEMP_SCALE:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.scale = *(const UA_Float *)value->value.data;
        if (channel_settings.scale == 0.0 ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_HIGH_ALARM:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.warning_threshold = *(const UA_Float *)value->value.data;
        if (channel_settings.warning_threshold > channel_settings.critical_threshold ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    case MODEL_NODE_CONFIG_HIGH_HIGH_ALARM:
        if (!UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_FLOAT]))
        {
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
        if (settings_channel_id == NULL || !model_copy_channel_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        }
        channel_settings.critical_threshold = *(const UA_Float *)value->value.data;
        if (channel_settings.critical_threshold < channel_settings.warning_threshold ||
            !channel_manager_update_settings(settings_channel_id, &channel_settings))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        break;
    default:
        return UA_STATUSCODE_BADNOTWRITABLE;
    }

    if (save_device_config)
    {
        (void)config_service_save();
    }
    return UA_STATUSCODE_GOOD;
}

void model_set_boolean_method_output(size_t output_size, UA_Variant *output, UA_Boolean result)
{
    if (output_size > 0)
    {
        UA_Variant_setScalarCopy(&output[0], &result, &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
}

void model_schedule_reboot(uint32_t delay_ms)
{
    const TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms > 0u ? delay_ms : 1u);

    g_pending_actions.reboot_pending = true;
    g_pending_actions.reboot_due_tick = xTaskGetTickCount() + delay_ticks;
}

void model_schedule_identify_blink(uint8_t blink_count, uint32_t period_ms)
{
    TickType_t delay_ticks;

    if (blink_count == 0u)
    {
        g_pending_actions.identify_blink_active = false;
        g_pending_actions.identify_toggles_remaining = 0u;
        g_pending_actions.identify_led_on = false;
        led_driver_set(false);
        return;
    }

    delay_ticks = pdMS_TO_TICKS(period_ms > 0u ? period_ms : 1u);
    if (delay_ticks == 0)
    {
        delay_ticks = 1;
    }

    g_pending_actions.identify_blink_active = true;
    g_pending_actions.identify_next_tick = xTaskGetTickCount();
    g_pending_actions.identify_toggles_remaining = (uint8_t)(blink_count * 2u);
    g_pending_actions.identify_led_on = false;
}

UA_StatusCode model_acknowledge_alarms(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = true;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    alarm_service_acknowledge_all();
    model_set_boolean_method_output(output_size, output, result);

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_request_reboot(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = true;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    /* Leave a bounded delivery window for the queued success AuditEvent and
     * its Publish response before the transport is intentionally torn down. */
    model_schedule_reboot(1500u);
    printf("[OPC UA]\tReboot requested\n");
    fflush(stdout);
    model_audit_session_event(session_context, "reboot", "Reboot", true, "reboot scheduled");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     "Reboot requested",
                                     450u,
                                     "Reboot",
                                     0u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_factory_reset(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = false;
    const UA_String *scope_string = NULL;
    char scope_text[48] = {0};
    config_reset_scope_t scope = CONFIG_RESET_SCOPE_ALL_KEEP_IDENTITY;
    const char *details = "factory reset failed";
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size > 1u || (input_size == 1u && (input == NULL || !model_get_input_ua_string(&input[0], &scope_string))))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (scope_string != NULL)
    {
        if (scope_string->length >= sizeof(scope_text))
        {
            model_set_boolean_method_output(output_size, output, false);
            return UA_STATUSCODE_BADINVALIDARGUMENT;
        }

        memcpy(scope_text, scope_string->data, scope_string->length);
        scope_text[scope_string->length] = '\0';
    }

    if (!config_reset_service_parse_scope(scope_text, &scope))
    {
        details = "unsupported factory reset scope";
    }
    else if (!config_reset_service_execute(scope))
    {
        details = "factory reset execution failed";
    }
    else
    {
        result = true;
        details = "factory reset scheduled";
        model_schedule_reboot(250u);
    }

    printf("[OPC UA]\tFactoryReset %s scope=%s\n",
           result == UA_TRUE ? "requested" : "failed",
           config_reset_service_scope_to_string(scope));
    fflush(stdout);

    model_audit_session_event(session_context,
                              "factory-reset",
                              "device",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result == UA_TRUE ? "Factory reset requested" : "Factory reset failed",
                                     result == UA_TRUE ? 600u : 800u,
                                     "FactoryReset",
                                     result == UA_TRUE ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_sync_time_now(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    result = time_service_request_sync_now();
    printf("[OPC UA]\tTime sync request %s\n", result ? "accepted" : "rejected");
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_blink_identify(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (led_driver_is_initialized())
    {
        model_schedule_identify_blink(5u, 200u);
        result = true;
        printf("[OPC UA]\tIdentify blink requested\n");
        fflush(stdout);
    }

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_apply_config(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    result = config_service_apply();
    if (result)
    {
        status = model_refresh_amb_locations(server, config_service_get());
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tLocation model refresh failed: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            result = false;
        }
    }
    printf("[OPC UA]\tApplyConfig %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_apply_config_preset(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = false;
    char preset_name[CONFIG_SERVICE_TEXT_MAX_LEN] = {0};
    const config_preset_catalog_entry_t *preset = NULL;
    const char *details = "unknown configuration preset";
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || input == NULL || !model_copy_input_string(&input[0], preset_name, sizeof(preset_name)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    preset = config_presets_get_by_name(preset_name);
    if (preset == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    /* config_presets_apply() resolves the preset onto the currently running
     * configuration and feeds it straight into config_service_replace_and_save()
     * -- the same validate -> persist staged -> stop -> activate -> verify ->
     * commit/rollback transaction every other candidate configuration goes
     * through. A preset always replaces the channel set, so it always trips
     * model_config_requires_server_reboot() and the commit-step restart
     * (T6) already rebuilds the full address space -- no separate
     * model_refresh_amb_locations() call is needed here. */
    if (config_presets_apply(preset->preset_id))
    {
        result = true;
        details = "preset applied";
    }
    else
    {
        details = "preset failed validation or activation";
    }

    printf("[OPC UA]\tApplyConfigPreset %s preset=%s\n",
           result == UA_TRUE ? "succeeded" : "failed",
           preset_name);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "config-preset-apply",
                              preset_name,
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_config_changed_event(server,
                                        result == UA_TRUE ? "Configuration preset applied" : "Configuration preset apply failed",
                                        result == UA_TRUE ? 300u : 700u,
                                        "Configuration",
                                        preset_name,
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_save_config(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    result = config_service_save();
    printf("[OPC UA]\tSaveConfig %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_audit_session_event(session_context,
                              "config-save",
                              "device-configuration",
                              result,
                              result ? "configuration saved" : "configuration save failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_config_changed_event(server,
                                        result ? "Configuration saved" : "Configuration save failed",
                                        result ? 300u : 700u,
                                        "Configuration",
                                        "*",
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_mqtt_pubsub_enabled(UA_Server *server,
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
    UA_StatusCode status;
    UA_Boolean result = false;
    UA_Boolean enabled = false;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_BOOLEAN]) ||
        input[0].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    enabled = *(const UA_Boolean *)input[0].data;
    result = config_service_stage_set_mqtt_pubsub_enabled(enabled == UA_TRUE);

    printf("[OPC UA]\tSetMqttPubSubEnabled %s (%s in staged config after SaveConfig + Reboot)\n",
           result ? "accepted" : "failed",
           enabled == UA_TRUE ? "enable" : "disable");
    fflush(stdout);

    model_audit_session_event(session_context,
                              "mqtt-pubsub-enable",
                              "device-messaging",
                              result,
                              enabled == UA_TRUE ? "mqtt pubsub enabled in runtime config" : "mqtt pubsub disabled in runtime config");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_config_changed_event(server,
                                        enabled == UA_TRUE ? "MQTT PubSub enabled in runtime configuration" : "MQTT PubSub disabled in runtime configuration",
                                        result ? 250u : 700u,
                                        "Messaging",
                                        "EnableMqttPubSub",
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_device_identity(UA_Server *server,
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
    static const size_t expected_input_size = 10u;
    UA_StatusCode status;
    UA_Boolean result = false;
    device_identity_config_t identity = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != expected_input_size || input == NULL ||
        !model_copy_input_string(&input[0], identity.device_name, sizeof(identity.device_name)) ||
        !model_copy_input_string(&input[1], identity.product_instance_uri, sizeof(identity.product_instance_uri)) ||
        !model_copy_input_string(&input[2], identity.serial_number, sizeof(identity.serial_number)) ||
        !model_copy_input_string(&input[3], identity.asset_id, sizeof(identity.asset_id)) ||
        !model_copy_input_string(&input[4], identity.hw_revision, sizeof(identity.hw_revision)) ||
        !model_copy_input_string(&input[5], identity.fw_revision, sizeof(identity.fw_revision)) ||
        !UA_Variant_hasScalarType(&input[6], &UA_TYPES[UA_TYPES_DATETIME]) || input[6].data == NULL ||
        !UA_Variant_hasScalarType(&input[7], &UA_TYPES[UA_TYPES_DATETIME]) || input[7].data == NULL ||
        !model_copy_input_string(&input[8], identity.location, sizeof(identity.location)) ||
        !model_copy_input_string(&input[9], identity.asset_class, sizeof(identity.asset_class)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    identity.manufacture_date_utc = (int64_t)*(const UA_DateTime *)input[6].data;
    identity.calibration_date_utc = (int64_t)*(const UA_DateTime *)input[7].data;
    result = config_service_stage_set_identity(&identity);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_location_paths(UA_Server *server,
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
    char hierarchical_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN] = {0};
    char operational_path[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN] = {0};
    UA_StatusCode status;
    UA_Boolean result = false;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 2u || input == NULL ||
        !model_copy_input_string(&input[0],
                                 hierarchical_path,
                                 sizeof(hierarchical_path)) ||
        !model_copy_input_string(&input[1],
                                 operational_path,
                                 sizeof(operational_path)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = config_service_stage_set_location_paths(hierarchical_path,
                                                     operational_path);
    printf("[OPC UA]\tSetLocationPaths %s hierarchical='%s' operational='%s'\n",
           result ? "accepted" : "rejected",
           hierarchical_path,
           operational_path);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "location-config",
                              "device-location",
                              result,
                              result ? "location paths staged" : "invalid location paths");
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_network_config(UA_Server *server,
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
    /* Implementation note: the 9 input arguments are strictly positional
     * (useDhcp, mac, hostname, opcuaPort, ntpServer, ipv4, netmask, gateway,
     * dns) with no argument names checked here — the UA_Argument list that
     * defines this contract to clients is built where this method node is
     * registered in model_address_space.c. Reordering the checks below
     * without also updating that argument list would silently desync the
     * two. */
    static const size_t expected_input_size = 9u;
    UA_StatusCode status;
    UA_Boolean result = false;
    device_network_config_t network = {0};
    UA_Boolean use_dhcp = false;
    UA_UInt32 opcua_port = 0u;
    char mac_address[18] = {0};
    char ipv4[16] = {0};
    char netmask[16] = {0};
    char gateway[16] = {0};
    char dns[16] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != expected_input_size || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_BOOLEAN]) || input[0].data == NULL ||
        !model_copy_input_string(&input[1], mac_address, sizeof(mac_address)) ||
        !model_copy_input_string(&input[2], network.opcua_hostname, sizeof(network.opcua_hostname)) ||
        !UA_Variant_hasScalarType(&input[3], &UA_TYPES[UA_TYPES_UINT32]) || input[3].data == NULL ||
        !model_copy_input_string(&input[4], network.ntp_server, sizeof(network.ntp_server)) ||
        !model_copy_input_string(&input[5], ipv4, sizeof(ipv4)) ||
        !model_copy_input_string(&input[6], netmask, sizeof(netmask)) ||
        !model_copy_input_string(&input[7], gateway, sizeof(gateway)) ||
        !model_copy_input_string(&input[8], dns, sizeof(dns)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    use_dhcp = *(const UA_Boolean *)input[0].data;
    opcua_port = *(const UA_UInt32 *)input[3].data;
    network.use_dhcp = use_dhcp == UA_TRUE;

    if (opcua_port > UINT16_MAX ||
        !model_parse_mac_string(mac_address, network.mac) ||
        !model_parse_ipv4_string(ipv4, network.static_ip) ||
        !model_parse_ipv4_string(netmask, network.static_netmask) ||
        !model_parse_ipv4_string(gateway, network.static_gateway) ||
        !model_parse_ipv4_string(dns, network.static_dns))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    network.opcua_port = (uint16_t)opcua_port;
    result = config_service_stage_set_network(&network);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

void model_format_channel_config_json(size_t slot_index,
                                             const config_channel_definition_t *channel,
                                             char *buffer,
                                             size_t buffer_size)
{
    const driver_catalog_entry_t *driver = NULL;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (channel == NULL || channel->in_use == 0u)
    {
        (void)snprintf(buffer,
                       buffer_size,
                       "{\"slot\":%u,\"inUse\":false}",
                       (unsigned)slot_index);
        return;
    }

    driver = driver_catalog_get_by_id(channel->driver_id);
    (void)snprintf(buffer,
                   buffer_size,
                   "{\"slot\":%u,\"inUse\":true,\"enabled\":%s,\"driverId\":%u,\"driver\":\"%s\",\"sourceIndex\":%u,\"sourceFlags\":%u,\"id\":\"%s\",\"name\":\"%s\",\"unit\":\"%s\",\"samplePeriodMs\":%u,\"publishPeriodMs\":%u,\"historyIntervalMs\":%u,\"scale\":%.6f,\"offset\":%.6f,\"warningThreshold\":%.6f,\"criticalThreshold\":%.6f}",
                   (unsigned)slot_index,
                   channel->settings.enabled ? "true" : "false",
                   (unsigned)channel->driver_id,
                   driver != NULL && driver->name != NULL ? driver->name : "unknown",
                   (unsigned)channel->source_index,
                   (unsigned)channel->source_flags,
                   channel->id,
                   channel->name,
                   channel->unit,
                   (unsigned)channel->settings.sample_period_ms,
                   (unsigned)channel->settings.publish_period_ms,
                   (unsigned)channel->settings.history_interval_ms,
                   channel->settings.scale,
                   channel->settings.offset,
                   channel->settings.warning_threshold,
                   channel->settings.critical_threshold);
}

void model_format_driver_catalog_json(char *buffer, size_t buffer_size)
{
    size_t offset = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "[");
    for (size_t index = 0u; index < driver_catalog_count() && offset < buffer_size; ++index)
    {
        const driver_catalog_entry_t *driver = driver_catalog_get_by_index(index);
        const int written = snprintf(buffer + offset,
                                     buffer_size - offset,
                                     "%s{\"id\":%u,\"name\":\"%s\",\"analog\":%s,\"defaultUnit\":\"%s\"}",
                                     index > 0u ? "," : "",
                                     driver != NULL ? (unsigned)driver->driver_id : 0u,
                                     driver != NULL && driver->name != NULL ? driver->name : "unknown",
                                     driver != NULL && driver->analog ? "true" : "false",
                                     driver != NULL && driver->default_unit != NULL ? driver->default_unit : "");

        if (written < 0)
        {
            break;
        }

        offset += (size_t)written;
    }

    if (offset < buffer_size)
    {
        (void)snprintf(buffer + offset, buffer_size - offset, "]");
    }
    else
    {
        buffer[buffer_size - 1u] = '\0';
    }
}

UA_StatusCode model_get_channel_config(UA_Server *server,
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
    UA_StatusCode status;
    UA_UInt32 slot_index = 0u;
    config_channel_definition_t channel = {0};
    char payload[512] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL)
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    slot_index = *(const UA_UInt32 *)input[0].data;
    if (slot_index >= PLATFORM_CHANNEL_LIMIT ||
        !config_service_stage_copy_channel((size_t)slot_index, &channel))
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    model_format_channel_config_json((size_t)slot_index, &channel, payload, sizeof(payload));
    if (output_size == 0u || output == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    {
        UA_String string_value = UA_STRING(payload);
        status = UA_Variant_setScalarCopy(&output[0], &string_value, &UA_TYPES[UA_TYPES_STRING]);
    }
    return status;
}

void model_format_config_preset_catalog_json(char *buffer, size_t buffer_size)
{
    size_t offset = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "[");
    for (size_t index = 0u; index < config_presets_count() && offset < buffer_size; ++index)
    {
        const config_preset_catalog_entry_t *preset = config_presets_get_by_index(index);
        const int written = snprintf(buffer + offset,
                                     buffer_size - offset,
                                     "%s{\"id\":%u,\"name\":\"%s\",\"description\":\"%s\"}",
                                     index > 0u ? "," : "",
                                     preset != NULL ? (unsigned)preset->preset_id : 0u,
                                     preset != NULL && preset->name != NULL ? preset->name : "unknown",
                                     preset != NULL && preset->description != NULL ? preset->description : "");

        if (written < 0)
        {
            break;
        }

        offset += (size_t)written;
    }

    if (offset < buffer_size)
    {
        (void)snprintf(buffer + offset, buffer_size - offset, "]");
    }
    else
    {
        buffer[buffer_size - 1u] = '\0';
    }
}

UA_StatusCode model_list_config_presets(UA_Server *server,
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
    UA_StatusCode status;
    char payload[512] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    model_format_config_preset_catalog_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

/** @brief Appends the comma-separated names of every
 *         `TELEMETRY_DRIVER_CAPABILITY_*` bit set in @p capabilities as a
 *         JSON string array, e.g. `["start","stop","read"]`. */
static void model_append_driver_capability_names_json(uint32_t capabilities, char *buffer, size_t buffer_size, size_t *offset)
{
    static const struct
    {
        uint32_t bit;
        const char *name;
    } known[] = {
        {TELEMETRY_DRIVER_CAPABILITY_START, "start"},
        {TELEMETRY_DRIVER_CAPABILITY_STOP, "stop"},
        {TELEMETRY_DRIVER_CAPABILITY_READ, "read"},
        {TELEMETRY_DRIVER_CAPABILITY_WRITE, "write"},
        {TELEMETRY_DRIVER_CAPABILITY_CONFIGURE, "configure"},
        {TELEMETRY_DRIVER_CAPABILITY_SELF_TEST, "self_test"},
    };
    bool first = true;

    *offset += (size_t)snprintf(buffer + *offset, buffer_size - *offset, "[");
    for (size_t index = 0u; index < sizeof(known) / sizeof(known[0]) && *offset < buffer_size; ++index)
    {
        int written;

        if ((capabilities & known[index].bit) == 0u)
        {
            continue;
        }
        written = snprintf(buffer + *offset, buffer_size - *offset, "%s\"%s\"", first ? "" : ",", known[index].name);
        if (written < 0)
        {
            break;
        }
        *offset += (size_t)written;
        first = false;
    }
    if (*offset < buffer_size)
    {
        *offset += (size_t)snprintf(buffer + *offset, buffer_size - *offset, "]");
    }
}

/**
 * @brief Maps a driver_catalog_resource_type_t to its JSON wire name.
 *
 * Must be extended whenever a new resource type is added to
 * services/driver_catalog.h -- static/tools/test_driver_catalog_opcua_contract.py
 * fails closed if this mapping falls behind that enum (see v0.5.1 T13,
 * which found and fixed this mapping silently reporting every v0.5.1
 * industrial driver's resource claims as "none" before that regression
 * guard existed).
 */
static const char *model_driver_resource_type_to_string(driver_catalog_resource_type_t type)
{
    switch (type)
    {
    case DRIVER_CATALOG_RESOURCE_ADC_INPUT: return "adc_input";
    case DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL: return "adc_peripheral";
    case DRIVER_CATALOG_RESOURCE_GPIO: return "gpio";
    case DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT: return "spi_chip_select";
    case DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL: return "spi_peripheral";
    case DRIVER_CATALOG_RESOURCE_AD5422_CHANNEL: return "ad5422_channel";
    case DRIVER_CATALOG_RESOURCE_MAX22190_CHANNEL: return "max22190_channel";
    case DRIVER_CATALOG_RESOURCE_IPS2050_CHANNEL: return "ips2050_channel";
    case DRIVER_CATALOG_RESOURCE_UIC_CHANNEL: return "uic_channel";
    case DRIVER_CATALOG_RESOURCE_NONE:
    default: return "none";
    }
}

void model_format_driver_compatibility_json(const driver_catalog_entry_t *driver, char *buffer, size_t buffer_size)
{
    driver_catalog_resource_claim_t claims[DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL];
    size_t claim_count = 0u;
    bool have_claims = false;
    size_t offset;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }
    if (driver == NULL)
    {
        (void)snprintf(buffer, buffer_size, "{}");
        return;
    }

    /* Resource claim shape is fixed per driver_id, not per configured
     * channel (driver_catalog_get_resource_claims()); source_index 0 /
     * source_flags 0 is a representative probe only -- it is not a claim
     * about any actually-configured channel. */
    have_claims = driver_catalog_get_resource_claims(driver->driver_id, 0u, 0u, claims,
                                                      sizeof(claims) / sizeof(claims[0]), &claim_count);

    offset = (size_t)snprintf(buffer, buffer_size,
                              "{\"id\":%u,\"name\":\"%s\",\"analog\":%s,\"defaultUnit\":\"%s\",\"capabilities\":",
                              (unsigned)driver->driver_id,
                              driver->name != NULL ? driver->name : "unknown",
                              driver->analog ? "true" : "false",
                              driver->default_unit != NULL ? driver->default_unit : "");
    if (offset >= buffer_size)
    {
        buffer[buffer_size - 1u] = '\0';
        return;
    }

    model_append_driver_capability_names_json(driver->capabilities, buffer, buffer_size, &offset);

    if (offset < buffer_size)
    {
        offset += (size_t)snprintf(buffer + offset, buffer_size - offset, ",\"resources\":[");
    }
    for (size_t index = 0u; have_claims && index < claim_count && offset < buffer_size; ++index)
    {
        const int written = snprintf(buffer + offset,
                                     buffer_size - offset,
                                     "%s{\"type\":\"%s\",\"index\":%u,\"shared\":%s}",
                                     index > 0u ? "," : "",
                                     model_driver_resource_type_to_string(claims[index].type),
                                     (unsigned)claims[index].index,
                                     claims[index].shared ? "true" : "false");

        if (written < 0)
        {
            break;
        }
        offset += (size_t)written;
    }
    if (offset < buffer_size)
    {
        (void)snprintf(buffer + offset, buffer_size - offset, "]}");
    }
    else
    {
        buffer[buffer_size - 1u] = '\0';
    }
}

UA_StatusCode model_list_drivers(UA_Server *server,
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
    UA_StatusCode status;
    char payload[768] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    model_format_driver_catalog_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_get_config_state(UA_Server *server,
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
    UA_StatusCode status;
    char *const payload = g_model_maintenance_export_payload;
    const bool reboot_required = model_is_reboot_required();
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    /* The model owns one bounded PSRAM scratch buffer. MaxNodesPerMethodCall
     * is one, so the server encodes this persistent view before any method
     * callback can reuse the payload. */
    if (payload == NULL)
    {
        return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    }
    if (!config_export_service_write_state_json(payload,
                                                MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY,
                                                reboot_required))
    {
        return UA_STATUSCODE_BADENCODINGLIMITSEXCEEDED;
    }

    if (output_size == 0u || output == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return model_set_maintenance_export_method_output(output_size,
                                                      output,
                                                      strlen(payload));
}

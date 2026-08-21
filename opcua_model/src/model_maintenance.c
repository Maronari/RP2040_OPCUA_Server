#include "opcua_model_internal.h"

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)

UA_StatusCode model_initialize_amb_maintenance_condition(
    UA_Server *server, const UA_NodeId condition_id)
{
    maintenance_service_snapshot_t snapshot = {0};
    UA_DateTime planned_date = 0;
    UA_Double estimated_downtime = 0.0;
    UA_Int32 maintenance_method = 0;
    UA_Boolean configuration_changed = false;
    UA_NodeId condition_class = UA_NODEID_NUMERIC(
        g_model_amb_namespace_index, UA_AMBID_SERVICING_CONDITION_CLASS_TYPE);
    UA_LocalizedText condition_class_name =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)"Servicing");
    UA_StatusCode status;

    status = maintenance_service_get_snapshot(&snapshot);
    if (status != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    }
    if (snapshot.has_activity)
    {
        planned_date = (UA_DateTime)snapshot.activity.planned_date_utc_100ns;
        estimated_downtime = snapshot.activity.estimated_downtime_ms;
        maintenance_method = (UA_Int32)snapshot.activity.method;
        configuration_changed = snapshot.activity.configuration_changed ? UA_TRUE : UA_FALSE;
    }

    status = model_register_amb_name_node_id_type(server);
    if (status != UA_STATUSCODE_GOOD) return status;

    g_model_amb_maintenance_method_type = UA_TYPES[UA_TYPES_INT32];
    g_model_amb_maintenance_method_type.typeId = UA_NODEID_NUMERIC(
        g_model_amb_namespace_index, UA_AMBID_MAINTENANCE_METHOD_ENUM);
    g_model_amb_maintenance_method_type.typeKind = UA_DATATYPEKIND_ENUM;

    status = model_add_amb_maintenance_property(
        server, condition_id,
        "Device.DeviceHealthAlarms.MaintenanceRequired.PlannedDate",
        "PlannedDate", UA_NODEID_NUMERIC(0, UA_NS0ID_UTCTIME),
        &planned_date, &UA_TYPES[UA_TYPES_DATETIME]);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_add_amb_name_node_id_property(
        server, condition_id, MODEL_NODE_MAINTENANCE_SUPPLIER,
        "MaintenanceSupplier", UA_FALSE);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_add_amb_name_node_id_property(
        server, condition_id, MODEL_NODE_MAINTENANCE_PERSONNEL,
        "QualificationOfPersonnel", UA_FALSE);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_add_amb_name_node_id_property(
        server, condition_id, MODEL_NODE_MAINTENANCE_REPLACED,
        "PartsOfAssetReplaced", UA_TRUE);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_add_amb_name_node_id_property(
        server, condition_id, MODEL_NODE_MAINTENANCE_SERVICED,
        "PartsOfAssetServiced", UA_TRUE);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_add_amb_maintenance_property(
        server, condition_id,
        "Device.DeviceHealthAlarms.MaintenanceRequired.EstimatedDowntime",
        "EstimatedDowntime", UA_NODEID_NUMERIC(0, UA_NS0ID_DURATION),
        &estimated_downtime, &UA_TYPES[UA_TYPES_DOUBLE]);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_add_amb_maintenance_property(
        server, condition_id,
        "Device.DeviceHealthAlarms.MaintenanceRequired.MaintenanceMethod",
        "MaintenanceMethod",
        UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                          UA_AMBID_MAINTENANCE_METHOD_ENUM),
        &maintenance_method, &g_model_amb_maintenance_method_type);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_add_amb_maintenance_property(
        server, condition_id,
        "Device.DeviceHealthAlarms.MaintenanceRequired.ConfigurationChanged",
        "ConfigurationChanged", UA_NODEID_NUMERIC(0, UA_NS0ID_BOOLEAN),
        &configuration_changed, &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(status != UA_STATUSCODE_GOOD) return status;

    status = model_condition_set_field_scalar(
        server, condition_id, "ConditionClassId", &condition_class,
        &UA_TYPES[UA_TYPES_NODEID]);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_condition_set_field_scalar(
        server, condition_id, "ConditionClassName", &condition_class_name,
        &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    if(status != UA_STATUSCODE_GOOD) return status;
    status = model_set_amb_maintenance_state(
        server, condition_id, model_maintenance_state_text(snapshot.state),
        model_maintenance_state_node_id(snapshot.state));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    status = model_install_maintenance_condition_value_sources(server, condition_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    /* The service has already completed bounded recovery before model
     * construction. This deliberately writes no event. */
    g_model_maintenance_condition_available = true;
    status = model_sync_maintenance_condition(server, &snapshot);
    if (status != UA_STATUSCODE_GOOD)
    {
        g_model_maintenance_condition_available = false;
    }
    return status;
}

UA_StatusCode model_condition_write_common_fields(UA_Server *server,
                                                         const UA_NodeId condition_id,
                                                         UA_UInt16 severity,
                                                         UA_StatusCode quality,
                                                         const char *message)
{
    UA_DateTime now = UA_DateTime_now();
    UA_StatusCode status;

    status = model_condition_set_field_scalar(server,
                                              condition_id,
                                              "Time",
                                              &now,
                                              &UA_TYPES[UA_TYPES_DATETIME]);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "ReceiveTime",
                                                  &now,
                                                  &UA_TYPES[UA_TYPES_DATETIME]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Severity",
                                                  &severity,
                                                  &UA_TYPES[UA_TYPES_UINT16]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Quality",
                                                  &quality,
                                                  &UA_TYPES[UA_TYPES_STATUSCODE]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_message(server, condition_id, message);
    }

    return status;
}

UA_StatusCode model_condition_initialize_inactive(UA_Server *server,
                                                         const UA_NodeId condition_id,
                                                         const char *message)
{
    UA_Boolean retain = false;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 100u,
                                                 UA_STATUSCODE_GOOD,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", true);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "ActiveState", false);
    }

    return status;
}

UA_StatusCode model_condition_activate(UA_Server *server,
                                              const UA_NodeId condition_id,
                                              UA_UInt16 severity,
                                              UA_StatusCode quality,
                                              const char *message,
                                              bool reset_ack)
{
    UA_Boolean retain = true;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 severity,
                                                 quality,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD && reset_ack)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", false);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "ActiveState", true);
    }

    return status;
}

UA_StatusCode model_condition_update_active(UA_Server *server,
                                                   const UA_NodeId condition_id,
                                                   const UA_NodeId source_id,
                                                   UA_UInt16 severity,
                                                   UA_StatusCode quality,
                                                   const char *message,
                                                   bool reset_ack)
{
    UA_Boolean retain = true;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 severity,
                                                 quality,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD && reset_ack)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", false);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_triggerConditionEvent(server, condition_id, source_id, NULL);
    }

    return status;
}

UA_StatusCode model_condition_deactivate(UA_Server *server,
                                                const UA_NodeId condition_id,
                                                const UA_NodeId source_id,
                                                const char *message)
{
    UA_Boolean retain = false;
    UA_StatusCode status;

    status = model_condition_write_common_fields(server,
                                                 condition_id,
                                                 100u,
                                                 UA_STATUSCODE_GOOD,
                                                 message);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_field_scalar(server,
                                                  condition_id,
                                                  "Retain",
                                                  &retain,
                                                  &UA_TYPES[UA_TYPES_BOOLEAN]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "AckedState", true);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_condition_set_two_state_id(server, condition_id, "ActiveState", false);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_triggerConditionEvent(server, condition_id, source_id, NULL);
    }

    return status;
}

/* Recovery and live commits update condition state silently.  Condition value
 * sources supply the state text/Id, message, severity, timestamps and AMB
 * properties directly from the same service snapshot; only A&C lifecycle
 * fields without a value source need an explicit write here.  Do not trigger a
 * condition event: recovery never replays events and the v0.3.2 transition
 * event remains the single live workflow notification. */
UA_StatusCode model_sync_maintenance_condition(
    UA_Server *server, const maintenance_service_snapshot_t *snapshot)
{
#if defined(UA_ENABLE_SUBSCRIPTIONS_EVENTS) && \
    defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    const UA_NodeId condition_id =
        UA_NODEID_STRING(1, (char *)MODEL_CONDITION_DEVICE_HEALTH_MAINTENANCE_REQUIRED);
    const UA_Boolean active = model_maintenance_condition_is_active(snapshot) ? UA_TRUE : UA_FALSE;
    UA_StatusCode status;

    if (server == NULL || snapshot == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    if (!g_model_maintenance_condition_available)
    {
        return UA_STATUSCODE_GOOD;
    }
    status = UA_STATUSCODE_GOOD;
    if (active == UA_FALSE)
    {
        /* A cleared condition cannot remain unacknowledged solely because the
         * process rebooted; this is state reconciliation, not an Ack action. */
        status = model_condition_set_two_state_id(server, condition_id,
                                                  "AckedState", true);
    }
    return status;
#else
    (void)server;
    (void)snapshot;
    return UA_STATUSCODE_GOOD;
#endif
}

UA_StatusCode model_create_condition_instance(UA_Server *server,
                                                     const char *condition_node_id,
                                                     const char *browse_name,
                                                     const UA_NodeId condition_type_id,
                                                     const UA_NodeId source_id,
                                                     const char *normal_message)
{
    UA_NodeId created_id = UA_NODEID_NULL;
    UA_StatusCode status;

    status = UA_Server_createCondition(server,
                                       UA_NODEID_STRING(1, (char *)condition_node_id),
                                       condition_type_id,
                                       UA_QUALIFIEDNAME(1, (char *)browse_name),
                                       source_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                       &created_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return model_condition_initialize_inactive(server, created_id, normal_message);
}

UA_StatusCode model_create_device_health_condition(UA_Server *server,
                                                           device_health_state_t state,
                                                           const char *browse_name,
                                                           UA_UInt32 condition_type_id)
{
    UA_NodeId condition_id = model_device_health_condition_node_id(state);
    UA_NodeId created_id = UA_NODEID_NULL;
    UA_StatusCode status;

    status = UA_Server_createCondition(
        server,
        condition_id,
        UA_NODEID_NUMERIC(g_model_di_namespace_index, condition_type_id),
        UA_QUALIFIEDNAME(g_model_di_namespace_index, (char *)browse_name),
        model_device_node_id(),
        UA_NODEID_NULL,
        &created_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_condition_initialize_inactive(server,
                                                  created_id,
                                                  "Device health category inactive");
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    if (state == DEVICE_HEALTH_MAINTENANCE_REQUIRED)
    {
        status = model_initialize_amb_maintenance_condition(server, created_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    return UA_Server_addReference(server,
                                  model_device_health_alarms_node_id(),
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                  UA_NODEID2EXPANDEDNODEID(created_id),
                                  true);
}

UA_StatusCode model_build_alarm_conditions(UA_Server *server)
{
    UA_NodeClass condition_type_class = UA_NODECLASS_UNSPECIFIED;
    UA_NodeClass security_parent_class = UA_NODECLASS_UNSPECIFIED;
    UA_NodeClass device_health_parent_class = UA_NODECLASS_UNSPECIFIED;
    UA_StatusCode status;

    status = UA_Server_readNodeClass(server,
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_CONDITIONTYPE),
                                     &condition_type_class);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tA&C disabled: ConditionType is not present in namespace zero\n");
        fflush(stdout);
        g_model_alarm_conditions_available = false;
        return UA_STATUSCODE_GOOD;
    }

    status = UA_Server_readNodeClass(server,
                                     UA_NODEID_NUMERIC(0, UA_NS0ID_SYSTEMOFFNORMALALARMTYPE),
                                     &security_parent_class);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tA&C disabled: SystemOffNormalAlarmType is not present in namespace zero\n");
        fflush(stdout);
        g_model_alarm_conditions_available = false;
        return UA_STATUSCODE_GOOD;
    }

    status = UA_Server_readNodeClass(
        server,
        UA_NODEID_NUMERIC(g_model_di_namespace_index, UA_DIID_DEVICEHEALTHDIAGNOSTICALARMTYPE),
        &device_health_parent_class);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tA&C disabled: DI DeviceHealth alarm types are unavailable\n");
        fflush(stdout);
        g_model_alarm_conditions_available = false;
        return UA_STATUSCODE_GOOD;
    }

    status = model_enable_condition_source(server,
                                           model_device_node_id(),
                                           model_security_node_id());
    if (status != UA_STATUSCODE_GOOD)
    {
        g_model_alarm_conditions_available = false;
        return status;
    }

    status = model_add_condition_type(server,
                                      MODEL_CONDITION_TYPE_SECURITY_FAULT,
                                      "SecurityFaultConditionType",
                                      UA_NODEID_NUMERIC(0, UA_NS0ID_SYSTEMOFFNORMALALARMTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_create_condition_instance(server,
                                             MODEL_CONDITION_SECURITY_FAULT,
                                             "SecurityFaultCondition",
                                             model_condition_type_node_id(MODEL_CONDITION_TYPE_SECURITY_FAULT),
                                             model_security_node_id(),
                                             "Security state normal");
    if (status != UA_STATUSCODE_GOOD)
    {
        g_model_alarm_conditions_available = false;
        return status;
    }

    status = model_create_device_health_condition(server,
                                                   DEVICE_HEALTH_FAILURE,
                                                   "Failure",
                                                   UA_DIID_FAILUREALARMTYPE);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_create_device_health_condition(server,
                                                       DEVICE_HEALTH_CHECK_FUNCTION,
                                                       "CheckFunction",
                                                       UA_DIID_CHECKFUNCTIONALARMTYPE);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_create_device_health_condition(server,
                                                       DEVICE_HEALTH_OFF_SPEC,
                                                       "OffSpec",
                                                       UA_DIID_OFFSPECALARMTYPE);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_create_device_health_condition(
            server,
            DEVICE_HEALTH_MAINTENANCE_REQUIRED,
            "MaintenanceRequired",
            UA_DIID_MAINTENANCEREQUIREDALARMTYPE);
    }

    g_model_alarm_conditions_available = status == UA_STATUSCODE_GOOD;
    return status;
}

model_security_condition_reason_t model_evaluate_security_condition(char *message,
                                                                          size_t message_size,
                                                                          UA_UInt16 *out_severity,
                                                                          UA_StatusCode *out_quality)
{
    const bool commissioning_mode = security_service_is_commissioning_mode();
    const uint32_t trusted_client_count = security_service_get_trusted_client_count();
    const uint32_t failed_login_count = security_service_get_failed_login_count();
    uint32_t days_until_expiry = 0u;
    bool time_check_performed = false;
    const model_certificate_expiry_state_t expiry_state =
        model_get_certificate_expiry_state(&days_until_expiry, &time_check_performed);

    (void)time_check_performed;

    if (expiry_state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED)
    {
        model_copy_string_safe(message, message_size, "Device certificate expired");
        *out_severity = 900u;
        *out_quality = UA_STATUSCODE_BADCERTIFICATETIMEINVALID;
        return MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRED;
    }

    if (expiry_state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        snprintf(message,
                 message_size,
                 "Device certificate expires soon (%" PRIu32 " days remaining)",
                 days_until_expiry);
        *out_severity = 750u;
        *out_quality = UA_STATUSCODE_UNCERTAIN;
        return MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRES_SOON_HIGH;
    }

    if (expiry_state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM)
    {
        snprintf(message,
                 message_size,
                 "Device certificate approaching expiry (%" PRIu32 " days remaining)",
                 days_until_expiry);
        *out_severity = 550u;
        *out_quality = UA_STATUSCODE_UNCERTAIN;
        return MODEL_SECURITY_CONDITION_REASON_CERT_EXPIRES_SOON_MEDIUM;
    }

    if (!commissioning_mode && trusted_client_count == 0u)
    {
        model_copy_string_safe(message, message_size, "Strict mode active with an empty trust list");
        *out_severity = 850u;
        *out_quality = UA_STATUSCODE_BADSECURITYCHECKSFAILED;
        return MODEL_SECURITY_CONDITION_REASON_TRUST_LIST_EMPTY;
    }

    if (failed_login_count > 0u)
    {
        snprintf(message,
                 message_size,
                 "Authentication failures detected (%" PRIu32 ")",
                 failed_login_count);
        *out_severity = 650u;
        *out_quality = UA_STATUSCODE_BADUSERACCESSDENIED;
        return MODEL_SECURITY_CONDITION_REASON_AUTH_FAILURES;
    }

    if (commissioning_mode)
    {
        model_copy_string_safe(message, message_size, "Commissioning mode is active");
        *out_severity = 350u;
        *out_quality = UA_STATUSCODE_UNCERTAIN;
        return MODEL_SECURITY_CONDITION_REASON_COMMISSIONING;
    }

    model_copy_string_safe(message, message_size, "Security state normal");
    *out_severity = 100u;
    *out_quality = UA_STATUSCODE_GOOD;
    return MODEL_SECURITY_CONDITION_REASON_NONE;
}

void model_sync_condition(UA_Server *server,
                                 const UA_NodeId condition_id,
                                 const UA_NodeId source_id,
                                 bool *was_active,
                                 uint32_t *was_reason,
                                 bool *was_enabled,
                                 TickType_t *next_enable_poll,
                                 uint32_t reason,
                                 UA_UInt16 severity,
                                 UA_StatusCode quality,
                                 const char *active_message,
                                 const char *normal_message)
{
    UA_StatusCode status = UA_STATUSCODE_GOOD;
    bool force_reconcile = false;
    const TickType_t now = xTaskGetTickCount();
    TickType_t enable_poll_interval = pdMS_TO_TICKS(MODEL_CONDITION_ENABLE_POLL_MS);

    if (server == NULL || was_active == NULL || was_reason == NULL || was_enabled == NULL ||
        next_enable_poll == NULL)
    {
        return;
    }

    if (enable_poll_interval == 0u)
    {
        enable_poll_interval = 1u;
    }

    /* A disabled condition does not necessarily reject a service-state update:
     * an unchanged normal state has no trigger to fail. Poll the standard
     * EnabledState at a fixed rate so re-enable always causes exactly one
     * service-derived reconciliation. */
    if ((TickType_t)(now - *next_enable_poll) <
        ((TickType_t)1u << ((sizeof(TickType_t) * 8u) - 1u)))
    {
        bool enabled = false;

        status = model_condition_read_enabled(server, condition_id, &enabled);
        *next_enable_poll = now + enable_poll_interval;
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tCondition enable-state read failed: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            return;
        }
        if (!enabled)
        {
            *was_enabled = false;
            *was_active = reason != 0u;
            *was_reason = reason;
            return;
        }

        force_reconcile = !*was_enabled;
        *was_enabled = true;
    }
    else if (!*was_enabled)
    {
        return;
    }

    if (reason != 0u)
    {
        if (force_reconcile || !*was_active)
        {
            status = model_condition_activate(server,
                                              condition_id,
                                              severity,
                                              quality,
                                              active_message,
                                              true);
        }
        else if (*was_reason != reason)
        {
            status = model_condition_update_active(server,
                                                   condition_id,
                                                   source_id,
                                                   severity,
                                                   quality,
                                                   active_message,
                                                   true);
        }

        if (status == UA_STATUSCODE_GOOD)
        {
            *was_active = true;
            *was_reason = reason;
        }
    }
    else if (force_reconcile || *was_active)
    {
        status = model_condition_deactivate(server,
                                            condition_id,
                                            source_id,
                                            normal_message);
        if (status == UA_STATUSCODE_GOOD)
        {
            *was_active = false;
            *was_reason = 0u;
        }
    }

    if (status == UA_STATUSCODE_BADCONDITIONALREADYDISABLED)
    {
        /* Condition fields have already been reconciled. Do not retry an
         * event while a client keeps the condition disabled. */
        *was_enabled = false;
        *was_active = reason != 0u;
        *was_reason = reason;
        *next_enable_poll = now + enable_poll_interval;
        return;
    }

    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tCondition sync failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }
}

void model_process_device_health_conditions(UA_Server *server,
                                                   model_event_runtime_state_t *state)
{
    device_health_report_t report = {0};

    device_health_service_get_report(&report);

    for (uint32_t raw_state = DEVICE_HEALTH_FAILURE;
         raw_state <= DEVICE_HEALTH_MAINTENANCE_REQUIRED;
         ++raw_state)
    {
        const device_health_state_t category = (device_health_state_t)raw_state;
        const uint32_t category_reasons =
            device_health_service_reasons_for_state(&report, category);
        const bool active = category_reasons != 0u;
        device_health_report_t category_report = report;
        char message[192] = {0};
        UA_UInt16 severity = 100u;
        UA_StatusCode quality = UA_STATUSCODE_GOOD;

        /* v0.3.2 assigns this existing AMB condition to the maintenance
         * service. Its fields and transition events must never be overwritten
         * by the periodic DeviceHealth condition synchronizer. */
        if (category == DEVICE_HEALTH_MAINTENANCE_REQUIRED)
        {
            continue;
        }

        category_report.reasons = category_reasons;
        device_health_service_format_summary(&category_report, message, sizeof(message));

        if (active)
        {
            switch (category)
            {
            case DEVICE_HEALTH_FAILURE:
                severity = 700u;
                quality = UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
                break;
            case DEVICE_HEALTH_CHECK_FUNCTION:
                severity = 350u;
                quality = UA_STATUSCODE_UNCERTAIN;
                break;
            case DEVICE_HEALTH_OFF_SPEC:
                severity = 450u;
                quality = UA_STATUSCODE_UNCERTAIN;
                break;
            case DEVICE_HEALTH_MAINTENANCE_REQUIRED:
                severity = 350u;
                quality = UA_STATUSCODE_GOOD;
                break;
            default:
                break;
            }
        }

        model_sync_condition(server,
                             model_device_health_condition_node_id(category),
                             model_device_node_id(),
                             &state->device_health_condition_active[raw_state],
                             &state->device_health_condition_reason[raw_state],
                             &state->device_health_condition_enabled[raw_state],
                             &state->device_health_condition_next_enable_poll[raw_state],
                             category_reasons,
                             severity,
                             quality,
                             message,
                             "Device health category inactive");
    }
}

void model_process_alarm_conditions(UA_Server *server, model_event_runtime_state_t *state)
{
    char message[192];
    UA_UInt16 severity = 100u;
    UA_StatusCode quality = UA_STATUSCODE_GOOD;
    model_security_condition_reason_t security_reason;

    if (server == NULL || state == NULL)
    {
        return;
    }

    if (!g_model_alarm_conditions_available)
    {
        return;
    }

    security_reason = model_evaluate_security_condition(message,
                                                        sizeof(message),
                                                        &severity,
                                                        &quality);
    model_sync_condition(server,
                         model_security_condition_node_id(),
                         model_security_node_id(),
                         &state->security_condition_active,
                         &state->security_condition_reason,
                         &state->security_condition_enabled,
                         &state->security_condition_next_enable_poll,
                         (uint32_t)security_reason,
                         severity,
                         quality,
                         message,
                         "Security state normal");

    model_process_device_health_conditions(server, state);
    model_process_channel_alarm_conditions(server);
}
#endif

UA_StatusCode model_set_event_bool_field(UA_KeyValueMap *event_fields,
                                                const char *browse_name,
                                                UA_Boolean value)
{
    char field_key[MODEL_EVENT_FIELD_PATH_CAPACITY];
    UA_StatusCode status = model_make_event_field_key(browse_name, field_key);

    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(0, field_key),
                                    &value,
                                    &UA_TYPES[UA_TYPES_BOOLEAN]);
}

UA_StatusCode model_set_event_uint32_field(UA_KeyValueMap *event_fields,
                                                  const char *browse_name,
                                                  UA_UInt32 value)
{
    char field_key[MODEL_EVENT_FIELD_PATH_CAPACITY];
    UA_StatusCode status = model_make_event_field_key(browse_name, field_key);

    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(0, field_key),
                                    &value,
                                    &UA_TYPES[UA_TYPES_UINT32]);
}

UA_StatusCode model_set_event_float_field(UA_KeyValueMap *event_fields,
                                                 const char *browse_name,
                                                 UA_Float value)
{
    char field_key[MODEL_EVENT_FIELD_PATH_CAPACITY];
    UA_StatusCode status = model_make_event_field_key(browse_name, field_key);

    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(0, field_key),
                                    &value,
                                    &UA_TYPES[UA_TYPES_FLOAT]);
}

UA_StatusCode model_write_config_event_fields(UA_KeyValueMap *event_fields,
                                                     const void *fields_context)
{
    const model_config_event_fields_t *fields = (const model_config_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "ChangedSection", fields->changed_section);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "ChangedKey", fields->changed_key);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Actor", fields->actor);
    }
    return status;
}

UA_StatusCode model_write_network_event_fields(UA_KeyValueMap *event_fields,
                                                      const void *fields_context)
{
    const model_network_event_fields_t *fields = (const model_network_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "OldIp", fields->old_ip);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "NewIp", fields->new_ip);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_bool_field(event_fields, "DhcpEnabled", fields->dhcp_enabled ? UA_TRUE : UA_FALSE);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_bool_field(event_fields, "LinkUp", fields->link_up ? UA_TRUE : UA_FALSE);
    }
    return status;
}

UA_StatusCode model_write_sensor_fault_event_fields(UA_KeyValueMap *event_fields,
                                                           const void *fields_context)
{
    const model_sensor_fault_event_fields_t *fields = (const model_sensor_fault_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "SensorId", fields->sensor_id);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_uint32_field(event_fields, "FaultCode", fields->fault_code);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Quality", fields->quality);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_float_field(event_fields, "MeasuredValue", fields->measured_value);
    }
    return status;
}

UA_StatusCode model_write_maintenance_event_fields(UA_KeyValueMap *event_fields,
                                                          const void *fields_context)
{
    const model_maintenance_event_fields_t *fields = (const model_maintenance_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "Operation", fields->operation);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_uint32_field(event_fields, "ResultCode", fields->result_code);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Actor", fields->actor);
    }
    return status;
}

UA_StatusCode model_write_firmware_event_fields(UA_KeyValueMap *event_fields,
                                                       const void *fields_context)
{
    const model_firmware_event_fields_t *fields = (const model_firmware_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "Version", fields->version);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "BuildTime", fields->build_time);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "GitHash", fields->git_hash);
    }
    return status;
}

UA_StatusCode model_write_audit_event_fields(UA_KeyValueMap *event_fields,
                                                    const void *fields_context)
{
    const model_audit_event_fields_t *fields = (const model_audit_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "Actor", fields->actor);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Subject", fields->subject);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_bool_field(event_fields, "Result", fields->result ? UA_TRUE : UA_FALSE);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "Details", fields->details);
    }
    return status;
}

UA_StatusCode model_write_audit_config_event_fields(UA_KeyValueMap *event_fields,
                                                           const void *fields_context)
{
    const model_audit_config_event_fields_t *fields = (const model_audit_config_event_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_set_event_string_field(event_fields, "ChangedSection", fields->changed_section);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_set_event_string_field(event_fields, "ChangedKey", fields->changed_key);
    }
    return status;
}

UA_StatusCode model_write_audit_security_event_fields(UA_KeyValueMap *event_fields,
                                                             const void *fields_context)
{
    const model_audit_security_event_fields_t *fields = (const model_audit_security_event_fields_t *)fields_context;
    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    return model_set_event_string_field(event_fields, "Operation", fields->operation);
}

UA_StatusCode model_write_audit_config_emit_fields(UA_KeyValueMap *event_fields,
                                                          const void *fields_context)
{
    const model_audit_config_emit_fields_t *fields = (const model_audit_config_emit_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_write_audit_event_fields(event_fields, &fields->audit);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_audit_config_event_fields(event_fields, &fields->config);
    }
    return status;
}

UA_StatusCode model_write_audit_security_emit_fields(UA_KeyValueMap *event_fields,
                                                            const void *fields_context)
{
    const model_audit_security_emit_fields_t *fields = (const model_audit_security_emit_fields_t *)fields_context;
    UA_StatusCode status;

    if (fields == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_write_audit_event_fields(event_fields, &fields->audit);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_audit_security_event_fields(event_fields, &fields->security);
    }
    return status;
}

UA_StatusCode model_emit_event_ex(UA_Server *server,
                                         const UA_NodeId event_type_id,
                                         const UA_NodeId source_node_id,
                                         const char *source_name,
                                         const char *message,
                                         UA_UInt16 severity,
                                         model_event_fields_writer_t fields_writer,
                                         const void *fields_context,
                                         model_event_last_kind_t last_kind)
{
    UA_KeyValueMap event_fields = {0};
    UA_DateTime event_time = UA_DateTime_now();
    UA_String source_name_string =
        UA_STRING((char *)(source_name != NULL ? source_name : ""));
    UA_LocalizedText event_message =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)(message != NULL ? message : ""));
    UA_ByteString event_id = UA_BYTESTRING_NULL;
    char event_id_hex[33];
    UA_StatusCode status;

    if (server == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    status = UA_KeyValueMap_setScalar(&event_fields,
                                      UA_QUALIFIEDNAME(0, (char *)"Time"),
                                      &event_time,
                                      &UA_TYPES[UA_TYPES_DATETIME]);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(&event_fields,
                                          UA_QUALIFIEDNAME(0, (char *)"ReceiveTime"),
                                          &event_time,
                                          &UA_TYPES[UA_TYPES_DATETIME]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(&event_fields,
                                          UA_QUALIFIEDNAME(0, (char *)"SourceNode"),
                                          &source_node_id,
                                          &UA_TYPES[UA_TYPES_NODEID]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(&event_fields,
                                          UA_QUALIFIEDNAME(0, (char *)"SourceName"),
                                          &source_name_string,
                                          &UA_TYPES[UA_TYPES_STRING]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = fields_writer != NULL ? fields_writer(&event_fields, fields_context)
                                       : UA_STATUSCODE_GOOD;
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_createEvent(server,
                                       source_node_id,
                                       event_type_id,
                                       severity,
                                       event_message,
                                       &event_fields,
                                       NULL,
                                       &event_id);
    }

    if (status == UA_STATUSCODE_GOOD)
    {
        model_event_id_to_hex(&event_id, event_id_hex, sizeof(event_id_hex));
        model_store_last_event(last_kind, event_id_hex, message);
#ifdef UA_ENABLE_PUBSUB
        model_pubsub_update_event_mirror(&event_type_id,
                                         source_name,
                                         message,
                                         severity,
                                         event_time,
                                         &event_id);
        model_pubsub_trigger_event_publish(server);
#endif
    }

    UA_KeyValueMap_clear(&event_fields);
    UA_ByteString_clear(&event_id);

    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tEvent emit failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }

    return status;
}

UA_StatusCode model_emit_event(UA_Server *server,
                                      const UA_NodeId event_type_id,
                                      const UA_NodeId source_node_id,
                                      const char *source_name,
                                      const char *message,
                                      UA_UInt16 severity)
{
    return model_emit_event_ex(server,
                               event_type_id,
                               source_node_id,
                               source_name,
                               message,
                               severity,
                               NULL,
                               NULL,
                               MODEL_EVENT_LAST_KIND_GENERIC);
}

void model_emit_network_runtime_event(UA_Server *server,
                                             const char *message,
                                             UA_UInt16 severity,
                                             const char *old_ip,
                                             const char *new_ip,
                                             bool dhcp_enabled,
                                             bool link_up)
{
    const model_network_event_fields_t fields = {
        old_ip,
        new_ip,
        dhcp_enabled,
        link_up};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_NETWORK),
                              model_network_node_id(),
                              "Device.Network",
                              message,
                              severity,
                              model_write_network_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_NETWORK);
}

void model_emit_sensor_runtime_event(UA_Server *server,
                                            const char *message,
                                            UA_UInt16 severity,
                                            uint32_t fault_code,
                                            const char *quality,
                                            float measured_value)
{
    const model_sensor_fault_event_fields_t fields = {
        "Temperature1",
        fault_code,
        quality,
        measured_value};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_SENSOR_FAULT),
                              model_temperature_node_id(),
                              DEVICE_CHANNEL_NODE_TEMPERATURE_1,
                              message,
                              severity,
                              model_write_sensor_fault_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_FAULT);
}

UA_StatusCode model_emit_maintenance_event(UA_Server *server,
                                         const char *message,
                                         UA_UInt16 severity,
                                         const char *operation,
                                         uint32_t result_code,
                                         const char *actor)
{
    const model_maintenance_event_fields_t fields = {operation, result_code, actor};
    return model_emit_event_ex(server,
                               model_event_type_node_id(MODEL_EVENT_TYPE_MAINTENANCE),
                               model_maintenance_node_id(),
                               "Device.Maintenance",
                               message,
                               severity,
                               model_write_maintenance_event_fields,
                               &fields,
                               MODEL_EVENT_LAST_KIND_GENERIC);
}

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
int64_t model_x509_days_from_civil(int32_t year, uint32_t month, uint32_t day)
{
    const int32_t adjusted_year = year - (month <= 2u ? 1 : 0);
    const int32_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const uint32_t year_of_era = (uint32_t)(adjusted_year - era * 400);
    const uint32_t shifted_month = (month > 2u) ? (month - 3u) : (month + 9u);
    const uint32_t day_of_year = (153u * shifted_month + 2u) / 5u + day - 1u;
    const uint32_t day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;

    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

bool model_x509_time_to_epoch(const mbedtls_x509_time *time, uint32_t *out_epoch_s)
{
    int64_t days;
    int64_t seconds;

    if (time == NULL || out_epoch_s == NULL ||
        time->year < 1970 ||
        time->mon < 1 || time->mon > 12 ||
        time->day < 1 || time->day > 31 ||
        time->hour < 0 || time->hour > 23 ||
        time->min < 0 || time->min > 59 ||
        time->sec < 0 || time->sec > 60)
    {
        return false;
    }

    days = model_x509_days_from_civil(time->year, (uint32_t)time->mon, (uint32_t)time->day);
    if (days < 0)
    {
        return false;
    }

    seconds = days * 86400 +
              (int64_t)time->hour * 3600 +
              (int64_t)time->min * 60 +
              (int64_t)time->sec;
    if (seconds < 0 || (uint64_t)seconds > UINT32_MAX)
    {
        return false;
    }

    *out_epoch_s = (uint32_t)seconds;
    return true;
}

bool model_remote_certificate_thumbprint(const UA_ByteString *remote_certificate,
                                                char *buffer,
                                                size_t buffer_size)
{
    unsigned char digest[20];
    static const char hex[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', '\0'};

    if (remote_certificate == NULL || remote_certificate->data == NULL || remote_certificate->length == 0u ||
        buffer == NULL || buffer_size < (SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u))
    {
        return false;
    }

    if (mbedtls_sha1(remote_certificate->data, remote_certificate->length, digest) != 0)
    {
        return false;
    }

    for (size_t i = 0u; i < sizeof(digest); ++i)
    {
        buffer[i * 2u] = hex[(digest[i] >> 4u) & 0x0Fu];
        buffer[i * 2u + 1u] = hex[digest[i] & 0x0Fu];
    }
    buffer[SECURITY_SERVICE_CERT_THUMBPRINT_LEN] = '\0';
    return true;
}

bool model_get_remote_certificate_expiry_info(const UA_ByteString *remote_certificate,
                                                     model_remote_certificate_expiry_info_t *out_info)
{
    mbedtls_x509_crt certificate;
    time_service_status_t time_status = {0};

    if (remote_certificate == NULL || remote_certificate->data == NULL || remote_certificate->length == 0u ||
        out_info == NULL)
    {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_UNKNOWN;
    (void)model_remote_certificate_thumbprint(remote_certificate,
                                              out_info->thumbprint,
                                              sizeof(out_info->thumbprint));

    mbedtls_x509_crt_init(&certificate);
    if (mbedtls_x509_crt_parse(&certificate, remote_certificate->data, remote_certificate->length) != 0)
    {
        mbedtls_x509_crt_free(&certificate);
        return false;
    }

    if (!model_x509_time_to_epoch(&certificate.valid_to, &out_info->not_after_epoch_s) ||
        out_info->not_after_epoch_s == 0u)
    {
        mbedtls_x509_crt_free(&certificate);
        return true;
    }

    time_service_get_status(&time_status);
    if (time_status.quality == SERVICE_QUALITY_BAD)
    {
        mbedtls_x509_crt_free(&certificate);
        return true;
    }

    out_info->time_check_performed = true;
    if (time_status.unix_time_s >= out_info->not_after_epoch_s)
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED;
        mbedtls_x509_crt_free(&certificate);
        return true;
    }

    out_info->days_until_expiry = (out_info->not_after_epoch_s - time_status.unix_time_s + 86399u) / 86400u;
    if (out_info->days_until_expiry <= MODEL_CERTIFICATE_EXPIRY_HIGH_WARN_DAYS)
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH;
    }
    else if (out_info->days_until_expiry <= MODEL_CERTIFICATE_EXPIRY_WARN_DAYS)
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM;
    }
    else
    {
        out_info->state = MODEL_CERTIFICATE_EXPIRY_STATE_VALID;
    }

    mbedtls_x509_crt_free(&certificate);
    return true;
}

void model_emit_client_certificate_expiry_notice(UA_Server *server,
                                                        const UA_ByteString *remote_certificate,
                                                        void *session_context)
{
    model_remote_certificate_expiry_info_t info = {0};
    char message[224];
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN] = {0};
    const char *actor_name;
    UA_UInt16 severity = 0u;

    if (server == NULL || !model_get_remote_certificate_expiry_info(remote_certificate, &info) ||
        info.state == MODEL_CERTIFICATE_EXPIRY_STATE_VALID ||
        info.state == MODEL_CERTIFICATE_EXPIRY_STATE_MISSING)
    {
        return;
    }

    actor_name = model_session_actor_name(session_context, actor, sizeof(actor));

    if (info.state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRED)
    {
        severity = 900u;
    }
    else if (info.state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_HIGH)
    {
        severity = 750u;
    }
    else if (info.state == MODEL_CERTIFICATE_EXPIRY_STATE_EXPIRES_SOON_MEDIUM)
    {
        severity = 550u;
    }
    else
    {
        severity = 350u;
    }

    model_copy_string_safe(message, sizeof(message), model_certificate_expiry_state_to_string(info.state));

    model_emit_maintenance_event(server,
                                 message,
                                 severity,
                                 model_certificate_expiry_state_to_string(info.state),
                                 (uint32_t)info.state,
                                 actor_name);
    printf("[OPC UA]\tClientCertificateExpiry: %s\n", message);
    fflush(stdout);
}
#endif

void model_emit_config_changed_event(UA_Server *server,
                                            const char *message,
                                            UA_UInt16 severity,
                                            const char *changed_section,
                                            const char *changed_key,
                                            const char *actor)
{
    const model_config_event_fields_t fields = {changed_section, changed_key, actor};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_CONFIG_CHANGED),
                              model_configuration_node_id(),
                              "Device.Configuration",
                              message,
                              severity,
                              model_write_config_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_CONFIG);
}

void model_emit_firmware_event(UA_Server *server,
                                      const char *message,
                                      UA_UInt16 severity)
{
    const device_profile_config_t *config = config_service_get();
    const model_firmware_event_fields_t fields = {
        config != NULL ? config->fw_revision : "",
        config != NULL ? config->build_time : "",
        config != NULL ? config->git_hash : ""};
    (void)model_emit_event_ex(server,
                              model_event_type_node_id(MODEL_EVENT_TYPE_FIRMWARE),
                              model_device_node_id(),
                              "Device",
                              message,
                              severity,
                              model_write_firmware_event_fields,
                              &fields,
                              MODEL_EVENT_LAST_KIND_GENERIC);
}

void model_process_pending_audit_events(UA_Server *server)
{
    model_pending_audit_event_t pending = {{0}};

    while (model_pop_pending_audit_event(&pending))
    {
        model_audit_event_fields_t audit_fields = {pending.actor,
                                                   pending.subject,
                                                   pending.details,
                                                   pending.success};
        UA_NodeId event_type_id = model_event_type_node_id(MODEL_EVENT_TYPE_AUDIT_SECURITY_CHANGE);
        UA_NodeId source_node_id = model_security_node_id();
        const char *source_name = "Device.Security";
        char message[192];
        model_event_fields_writer_t writer = model_write_audit_event_fields;
        const void *writer_context = &audit_fields;

        if (strcmp(pending.event, "login") == 0)
        {
            event_type_id = model_event_type_node_id(
                pending.success ? MODEL_EVENT_TYPE_AUDIT_LOGIN_SUCCESS : MODEL_EVENT_TYPE_AUDIT_LOGIN_FAILURE);
            snprintf(message,
                     sizeof(message),
                     "Login %s for %s",
                     pending.success ? "succeeded" : "failed",
                     pending.actor[0] != '\0' ? pending.actor : "unknown");
        }
        else if (strcmp(pending.event, "config-save") == 0)
        {
            const model_audit_config_emit_fields_t config_fields = {
                {pending.actor, pending.subject, pending.details, pending.success},
                {"Configuration", "*"}};
            event_type_id = model_event_type_node_id(MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE);
            source_node_id = model_configuration_node_id();
            source_name = "Device.Configuration";
            writer = model_write_audit_config_emit_fields;
            writer_context = &config_fields;
            snprintf(message,
                     sizeof(message),
                     "Configuration %s",
                     pending.success ? "saved" : "save failed");
            (void)model_emit_event_ex(server,
                                      event_type_id,
                                      source_node_id,
                                      source_name,
                                      message,
                                      pending.success ? 250u : 700u,
                                      writer,
                                      writer_context,
                                      MODEL_EVENT_LAST_KIND_GENERIC);
            continue;
        }
        else
        {
            const model_audit_security_emit_fields_t security_fields = {
                {pending.actor, pending.subject, pending.details, pending.success},
                {pending.event}};
            if (strcmp(pending.event, "reboot") == 0 || strcmp(pending.event, "factory-reset") == 0)
            {
                source_node_id = model_maintenance_node_id();
                source_name = "Device.Maintenance";
            }
            writer = model_write_audit_security_emit_fields;
            writer_context = &security_fields;
            snprintf(message,
                     sizeof(message),
                     "%s %s",
                     pending.event[0] != '\0' ? pending.event : "audit",
                     pending.success ? "succeeded" : "failed");
            (void)model_emit_event_ex(server,
                                      event_type_id,
                                      source_node_id,
                                      source_name,
                                      message,
                                      pending.success ? 250u : 700u,
                                      writer,
                                      writer_context,
                                      MODEL_EVENT_LAST_KIND_GENERIC);
            continue;
        }

        (void)model_emit_event_ex(server,
                                  event_type_id,
                                  source_node_id,
                                  source_name,
                                  message,
                                  pending.success ? 250u : 700u,
                                  writer,
                                  writer_context,
                                  MODEL_EVENT_LAST_KIND_GENERIC);
    }
}

void model_event_runtime_state_init(model_event_runtime_state_t *state)
{
    if (state == NULL)
    {
        return;
    }

    memset(state, 0, sizeof(*state));
    network_manager_get_status(&state->network_status);
    sensor_manager_get_snapshot(&state->sensor_snapshot);
#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    state->security_condition_active = false;
    state->security_condition_reason = 0u;
    state->security_condition_enabled = true;
    state->security_condition_next_enable_poll = 0u;
    for (size_t index = 0u; index < 5u; ++index)
    {
        state->device_health_condition_active[index] = false;
        state->device_health_condition_reason[index] = 0u;
        state->device_health_condition_enabled[index] = true;
        state->device_health_condition_next_enable_poll[index] = 0u;
    }
#endif
    state->initialized = true;
}

void model_process_certificate_expiry_runtime(UA_Server *server,
                                                     model_event_runtime_state_t *state,
                                                     char *message,
                                                     size_t message_size)
{
    model_certificate_expiry_state_t current_state;
    uint32_t days_until_expiry = 0u;
    UA_UInt16 severity = 0u;
    bool current_attention;
    bool previous_attention;

    if (server == NULL || state == NULL || message == NULL || message_size == 0u)
    {
        return;
    }

    current_state = model_get_certificate_expiry_state(&days_until_expiry, NULL);
    current_attention = model_certificate_expiry_requires_attention(current_state);
    previous_attention = state->server_certificate_expiry_seen &&
                         model_certificate_expiry_requires_attention(state->server_certificate_expiry_state);

    if (!state->server_certificate_expiry_seen)
    {
        state->server_certificate_expiry_seen = true;
        state->server_certificate_expiry_state = current_state;
        if (current_attention)
        {
            if (model_build_certificate_expiry_notice(current_state,
                                                      days_until_expiry,
                                                      message,
                                                      message_size,
                                                      &severity))
            {
                model_emit_maintenance_event(server,
                                             message,
                                             severity,
                                             model_certificate_expiry_state_to_string(current_state),
                                             (uint32_t)current_state,
                                             model_security_commissioning_state_to_string());
                printf("[OPC UA]\tCertificateExpiry: %s\n", message);
                fflush(stdout);
            }
        }
        return;
    }

    if (state->server_certificate_expiry_state == current_state)
    {
        return;
    }

    state->server_certificate_expiry_state = current_state;
    if (current_attention &&
        model_build_certificate_expiry_notice(current_state,
                                              days_until_expiry,
                                              message,
                                              message_size,
                                              &severity))
    {
        model_emit_maintenance_event(server,
                                     message,
                                     severity,
                                     model_certificate_expiry_state_to_string(current_state),
                                     (uint32_t)current_state,
                                     model_security_commissioning_state_to_string());
        printf("[OPC UA]\tCertificateExpiry: %s\n", message);
        fflush(stdout);
    }
    else if (previous_attention)
    {
        model_copy_string_safe(message, message_size, model_certificate_expiry_state_to_string(current_state));
        model_emit_maintenance_event(server,
                                     message,
                                     200u,
                                     model_certificate_expiry_state_to_string(current_state),
                                     (uint32_t)current_state,
                                     model_security_commissioning_state_to_string());
        printf("[OPC UA]\tCertificateExpiry: %s\n", message);
        fflush(stdout);
    }
}

void model_process_trust_expiry_runtime(UA_Server *server,
                                               model_event_runtime_state_t *state,
                                               char *message,
                                               size_t message_size)
{
    security_trust_expiry_service_report_t report = {0};
    UA_UInt16 severity = 0u;
    const bool commissioning_mode = security_service_is_commissioning_mode();
    bool current_attention;
    bool previous_attention;

    if (server == NULL || state == NULL || message == NULL || message_size == 0u)
    {
        return;
    }

    if (!security_trust_expiry_service_get_report(&report))
    {
        return;
    }

    current_attention = model_trust_expiry_requires_attention(&report, commissioning_mode);
    previous_attention = state->trust_expiry_seen &&
                         model_trust_expiry_requires_attention(&state->trust_expiry_report,
                                                               state->trust_expiry_commissioning_mode);

    if (!state->trust_expiry_seen)
    {
        state->trust_expiry_seen = true;
        state->trust_expiry_report = report;
        state->trust_expiry_commissioning_mode = commissioning_mode;
        if (current_attention &&
            model_build_trust_expiry_notice(&report,
                                            commissioning_mode,
                                            message,
                                            message_size,
                                            &severity))
        {
            model_emit_maintenance_event(server,
                                         message,
                                         severity,
                                         security_trust_expiry_service_state_to_string(report.state),
                                         (uint32_t)report.state,
                                         model_security_commissioning_state_to_string());
            printf("[OPC UA]\tTrustExpiry: %s\n", message);
            fflush(stdout);
        }
        return;
    }

    if (!model_trust_expiry_report_changed(&state->trust_expiry_report, &report) &&
        state->trust_expiry_commissioning_mode == commissioning_mode)
    {
        return;
    }

    state->trust_expiry_report = report;
    state->trust_expiry_commissioning_mode = commissioning_mode;

    if (current_attention &&
        model_build_trust_expiry_notice(&report,
                                        commissioning_mode,
                                        message,
                                        message_size,
                                        &severity))
    {
        model_emit_maintenance_event(server,
                                     message,
                                     severity,
                                     security_trust_expiry_service_state_to_string(report.state),
                                     (uint32_t)report.state,
                                     model_security_commissioning_state_to_string());
        printf("[OPC UA]\tTrustExpiry: %s\n", message);
        fflush(stdout);
    }
    else if (previous_attention)
    {
        model_copy_string_safe(message,
                               message_size,
                               security_trust_expiry_service_state_to_string(report.state));
        model_emit_maintenance_event(server,
                                     message,
                                     200u,
                                     security_trust_expiry_service_state_to_string(report.state),
                                     (uint32_t)report.state,
                                     model_security_commissioning_state_to_string());
        printf("[OPC UA]\tTrustExpiry: %s\n", message);
        fflush(stdout);
    }
}

/* v0.4.3: guards the one-shot per-channel alarm condition instance build.
 * Kept file-static rather than added to model_event_runtime_state_t so this
 * change stays confined to model_maintenance.c/model_events.c and does not
 * touch shared struct layouts also used by history's model_history.c. This
 * must run after model_build_address_space() has created the channel
 * objects, which model_build_alarm_conditions() (called from
 * model_build_address_space() itself, before the channel-node loop runs)
 * cannot guarantee -- so it is deferred to the first runtime-event poll,
 * which model_core_lifecycle.c only reaches after address-space build
 * (including channel nodes) has fully completed. */
static bool g_model_channel_alarm_conditions_built;
static TickType_t g_model_channel_alarm_conditions_retry_at;
static UA_StatusCode g_model_channel_alarm_conditions_last_failure;

void model_process_runtime_events(UA_Server *server, model_event_runtime_state_t *state)
{
    network_manager_status_t current_network = {0};
    sensor_manager_snapshot_t current_sensor = {0};
    char message[160];

    if (server == NULL || state == NULL)
    {
        return;
    }

    if (!state->initialized)
    {
        model_event_runtime_state_init(state);
        return;
    }

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    if (!g_model_channel_alarm_conditions_built &&
        (g_model_channel_alarm_conditions_retry_at == 0u ||
         (TickType_t)(xTaskGetTickCount() - g_model_channel_alarm_conditions_retry_at) <
             (TickType_t)(~(TickType_t)0u / 2u)))
    {
        const UA_StatusCode condition_status = model_build_channel_alarm_conditions(server);
        TickType_t retry_delay = pdMS_TO_TICKS(1000u);

        if (condition_status == UA_STATUSCODE_GOOD)
        {
            /* Set only after every eligible channel has a fully initialized
             * condition instance and acknowledgement callback. */
            g_model_channel_alarm_conditions_built = true;
            g_model_channel_alarm_conditions_last_failure = UA_STATUSCODE_GOOD;
            g_model_channel_alarm_conditions_retry_at = 0u;
        }
        else
        {
            if (condition_status != g_model_channel_alarm_conditions_last_failure)
            {
                /* Retain the deterministic fault for diagnostics without
                 * repeatedly filling the bounded log/event paths. */
                g_model_channel_alarm_conditions_last_failure = condition_status;
            }
            if (retry_delay == 0u)
            {
                retry_delay = 1u;
            }
            g_model_channel_alarm_conditions_retry_at = xTaskGetTickCount() + retry_delay;
        }
    }
#endif

    network_manager_get_status(&current_network);
    sensor_manager_get_snapshot(&current_sensor);

    if (model_string_changed(state->network_status.ip_mode, current_network.ip_mode))
    {
        snprintf(message,
                 sizeof(message),
                 "Network mode changed: %s -> %s",
                 state->network_status.ip_mode,
                 current_network.ip_mode);
        model_emit_network_runtime_event(server,
                                         message,
                                         400u,
                                         state->network_status.ipv4_address,
                                         current_network.ipv4_address,
                                         strcmp(current_network.ip_mode, "DHCP") == 0,
                                         current_network.link_up);
    }

    if (model_string_changed(state->network_status.ipv4_address, current_network.ipv4_address))
    {
        snprintf(message,
                 sizeof(message),
                 "IPv4 changed: %s -> %s",
                 state->network_status.ipv4_address,
                 current_network.ipv4_address);
        model_emit_network_runtime_event(server,
                                         message,
                                         350u,
                                         state->network_status.ipv4_address,
                                         current_network.ipv4_address,
                                         strcmp(current_network.ip_mode, "DHCP") == 0,
                                         current_network.link_up);
    }

    if (state->network_status.link_up != current_network.link_up)
    {
        snprintf(message,
                 sizeof(message),
                 "Link state changed: %s",
                 current_network.link_up ? "up" : "down");
        model_emit_network_runtime_event(server,
                                         message,
                                         current_network.link_up ? 250u : 700u,
                                         state->network_status.ipv4_address,
                                         current_network.ipv4_address,
                                         strcmp(current_network.ip_mode, "DHCP") == 0,
                                         current_network.link_up);
    }

    if (current_sensor.quality != state->sensor_snapshot.quality)
    {
        snprintf(message,
                 sizeof(message),
                 "Sensor quality changed: %s -> %s",
                 model_quality_to_string(state->sensor_snapshot.quality),
                 model_quality_to_string(current_sensor.quality));
        model_emit_sensor_runtime_event(server,
                                        message,
                                        current_sensor.quality == SERVICE_QUALITY_GOOD ? 250u : 800u,
                                        current_sensor.quality == SERVICE_QUALITY_GOOD ? 0u : 1001u,
                                        model_quality_to_string(current_sensor.quality),
                                        current_sensor.temperature_c);
    }

    /* v0.4.3: the singleton Temperature1-only alarm transition event
     * previously emitted here is superseded by the generalized per-channel
     * A&C condition instances below (model_process_channel_alarm_conditions,
     * called from model_process_alarm_conditions), which cover every
     * alarm-eligible channel -- including the legacy Temperature1 channel --
     * and emit their own standard condition transition events via
     * UA_Server_triggerConditionEvent through the same live
     * Events/Audit bridge, consistent with how the pre-existing
     * Security/DeviceHealth conditions already work without a redundant
     * parallel custom event. */

    model_process_certificate_expiry_runtime(server, state, message, sizeof(message));
    model_process_trust_expiry_runtime(server, state, message, sizeof(message));

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    model_process_alarm_conditions(server, state);
#endif
    model_process_pending_audit_events(server);

    state->network_status = current_network;
    state->sensor_snapshot = current_sensor;
}
void model_apply_now_source_timestamp(UA_Boolean include_source_timestamp, UA_DataValue *value)
{
    if (!include_source_timestamp)
    {
        return;
    }

    value->hasSourceTimestamp = true;
    value->sourceTimestamp = UA_DateTime_now();
}

void model_apply_epoch_source_timestamp(UA_Boolean include_source_timestamp, uint32_t epoch_seconds, UA_DataValue *value)
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

bool model_source_timestamp_to_datetime(uint64_t source_timestamp_unix_us,
                                        UA_DateTime *out_datetime)
{
    const uint64_t max_source_timestamp_unix_us =
        (uint64_t)((UA_INT64_MAX - UA_DATETIME_UNIX_EPOCH) / 10LL);

    if (out_datetime == NULL ||
        source_timestamp_unix_us == TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID ||
        source_timestamp_unix_us > max_source_timestamp_unix_us)
    {
        return false;
    }

    *out_datetime = UA_DATETIME_UNIX_EPOCH + (UA_DateTime)(source_timestamp_unix_us * 10u);
    return true;
}

void model_apply_channel_source_timestamp(UA_Boolean include_source_timestamp,
                                          uint64_t source_timestamp_unix_us,
                                          UA_DataValue *value)
{
    UA_DateTime timestamp = 0;

    if (include_source_timestamp && value != NULL &&
        model_source_timestamp_to_datetime(source_timestamp_unix_us, &timestamp))
    {
        value->hasSourceTimestamp = true;
        value->sourceTimestamp = timestamp;
    }
}

UA_StatusCode model_set_uint32_value(UA_DataValue *value, UA_UInt32 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_UINT32]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_int32_value(UA_DataValue *value, UA_Int32 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_INT32]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_float_value(UA_DataValue *value, UA_Float data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_FLOAT]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_double_value(UA_DataValue *value, UA_Double data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_DOUBLE]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_boolean_value(UA_DataValue *value, UA_Boolean data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_BOOLEAN]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_uint64_value(UA_DataValue *value, UA_UInt64 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_UINT64]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_int64_value(UA_DataValue *value, UA_Int64 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_INT64]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_int16_value(UA_DataValue *value, UA_Int16 data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_INT16]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_string_value(UA_DataValue *value, const char *data, UA_StatusCode status)
{
    UA_String string_value = UA_STRING((char *)(data != NULL ? data : ""));
    UA_Variant_setScalarCopy(&value->value, &string_value, &UA_TYPES[UA_TYPES_STRING]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_maintenance_result_status(maintenance_service_result_t result)
{
    switch (result) {
    case MAINTENANCE_SERVICE_RESULT_OK: return UA_STATUSCODE_GOOD;
    case MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED:
    case MAINTENANCE_SERVICE_RESULT_BUSY:
    case MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE:
        return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    case MAINTENANCE_SERVICE_RESULT_INVALID_STATE: return UA_STATUSCODE_BADSTATENOTACTIVE;
    case MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE: return UA_STATUSCODE_BADOUTOFRANGE;
    case MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT: return UA_STATUSCODE_BADINVALIDARGUMENT;
    default: return UA_STATUSCODE_BADINTERNALERROR;
    }
}

UA_StatusCode model_prepare_maintenance_event(
    model_prepared_maintenance_event_t *event, const char *message,
    const char *operation, const char *actor)
{
    UA_StatusCode status;

    if (event == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    memset(event, 0, sizeof(*event));
    event->event_time = UA_DateTime_now();
    event->source_name = UA_STRING((char *)"Device.Maintenance");
    event->message = UA_LOCALIZEDTEXT(
        (char *)"en-US", (char *)(message != NULL ? message : ""));
    event->maintenance_fields.operation = operation;
    event->maintenance_fields.result_code = 0u;
    event->maintenance_fields.actor = actor;

    status = UA_KeyValueMap_setScalar(&event->fields,
                                      UA_QUALIFIEDNAME(0, (char *)"Time"),
                                      &event->event_time,
                                      &UA_TYPES[UA_TYPES_DATETIME]);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(
            &event->fields, UA_QUALIFIEDNAME(0, (char *)"ReceiveTime"),
            &event->event_time, &UA_TYPES[UA_TYPES_DATETIME]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        const UA_NodeId source = model_maintenance_node_id();
        status = UA_KeyValueMap_setScalar(
            &event->fields, UA_QUALIFIEDNAME(0, (char *)"SourceNode"),
            &source, &UA_TYPES[UA_TYPES_NODEID]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_KeyValueMap_setScalar(
            &event->fields, UA_QUALIFIEDNAME(0, (char *)"SourceName"),
            &event->source_name, &UA_TYPES[UA_TYPES_STRING]);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_maintenance_event_fields(
            &event->fields, &event->maintenance_fields);
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        UA_KeyValueMap_clear(&event->fields);
    }
    return status;
}

UA_StatusCode model_publish_prepared_maintenance_event(
    UA_Server *server, model_prepared_maintenance_event_t *event)
{
    UA_ByteString event_id = UA_BYTESTRING_NULL;
    char event_id_hex[33];
    const UA_NodeId source = model_maintenance_node_id();
    const UA_NodeId type = model_event_type_node_id(MODEL_EVENT_TYPE_MAINTENANCE);
    UA_StatusCode status;

    if (server == NULL || event == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    status = UA_Server_createEvent(server, source, type, 350u, event->message,
                                   &event->fields, NULL, &event_id);
    if (status == UA_STATUSCODE_GOOD)
    {
        model_event_id_to_hex(&event_id, event_id_hex, sizeof(event_id_hex));
        model_store_last_event(MODEL_EVENT_LAST_KIND_GENERIC, event_id_hex,
                               (const char *)event->message.text.data);
#ifdef UA_ENABLE_PUBSUB
        model_pubsub_update_event_mirror(&type, "Device.Maintenance",
                                         (const char *)event->message.text.data,
                                         350u, event->event_time, &event_id);
        model_pubsub_trigger_event_publish(server);
#endif
    }
    UA_ByteString_clear(&event_id);
    return status;
}

UA_StatusCode model_maintenance_mutations_available(void)
{
    maintenance_service_event_publication_fault_t fault = {0};
    maintenance_service_result_t result =
        maintenance_service_get_event_publication_fault(&fault);

    if (g_model_maintenance_faulted ||
        result != MAINTENANCE_SERVICE_RESULT_OK || fault.active)
    {
        return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    }
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_commit_maintenance_transaction(UA_Server *server,
                                                            const maintenance_service_transaction_t *transaction,
                                                            const char *operation,
                                                            void *session_context)
{
    maintenance_service_snapshot_t prepared = {0};
    maintenance_service_snapshot_t committed = {0};
    model_prepared_maintenance_event_t event = {0};
    maintenance_service_result_t service_status;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    if (server == NULL || transaction == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    service_status = maintenance_service_get_prepared_snapshot(transaction, &prepared);
    if (service_status != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return model_maintenance_result_status(service_status);
    }
    if (transaction->transition != MAINTENANCE_SERVICE_TRANSITION_NONE)
    {
        status = model_prepare_maintenance_event(
            &event,
            prepared.activity.result[0] != '\0' ? prepared.activity.result : operation,
            operation, prepared.activity.actor);
        if (status != UA_STATUSCODE_GOOD)
        {
            (void)maintenance_service_abort(transaction);
            return status;
        }
    }

    service_status = maintenance_service_apply(transaction);
    if (service_status != MAINTENANCE_SERVICE_RESULT_OK)
    {
        UA_KeyValueMap_clear(&event.fields);
        (void)maintenance_service_abort(transaction);
        return model_maintenance_result_status(service_status);
    }
    service_status = maintenance_service_commit(transaction, &committed);
    if (service_status != MAINTENANCE_SERVICE_RESULT_OK)
    {
        UA_KeyValueMap_clear(&event.fields);
        (void)maintenance_service_abort(transaction);
        return model_maintenance_result_status(service_status);
    }

    status = model_sync_maintenance_condition(server, &committed);
    if (status != UA_STATUSCODE_GOOD)
    {
        /* The durable/service snapshot cannot be rolled back. Fail closed on
         * later mutations if the northbound condition can no longer be kept
         * coherent with it. */
        if (maintenance_service_mark_event_publication_fault(
                committed.revision, transaction->operation) !=
            MAINTENANCE_SERVICE_RESULT_OK)
        {
            g_model_maintenance_faulted = true;
        }
        return status;
    }

    if (transaction->transition != MAINTENANCE_SERVICE_TRANSITION_NONE)
    {
        status = model_publish_prepared_maintenance_event(server, &event);
        UA_KeyValueMap_clear(&event.fields);
        if (status != UA_STATUSCODE_GOOD)
        {
            /* The public event API is the firmware boundary. The committed
             * snapshot remains authoritative even when that API reports an
             * error; no rollback or duplicate publication is permitted. */
            if (maintenance_service_mark_event_publication_fault(
                    committed.revision, transaction->operation) !=
                MAINTENANCE_SERVICE_RESULT_OK)
            {
                g_model_maintenance_faulted = true;
            }
            return status;
        }
    }
    model_audit_maintenance_accept(session_context, operation);
    if (transaction->transition != MAINTENANCE_SERVICE_TRANSITION_NONE)
    {
        model_audit_maintenance_state_change(session_context, operation,
                                             &committed);
    }
    return UA_STATUSCODE_GOOD;
}

bool model_copy_maintenance_string(const UA_Variant *value, char *out, size_t capacity)
{
    return model_copy_input_string(value, out, capacity) && out[0] != '\0';
}

bool model_copy_maintenance_parts(const UA_Variant *value,
    char out[MAINTENANCE_SERVICE_MAX_PARTS][MAINTENANCE_SERVICE_PART_TEXT_CAPACITY], uint8_t *count)
{
    size_t index;
    UA_String *items;
    if (value == NULL || count == NULL || !UA_Variant_hasArrayType(value, &UA_TYPES[UA_TYPES_STRING]) || value->arrayLength > MAINTENANCE_SERVICE_MAX_PARTS)
        return false;
    items = (UA_String *)value->data;
    for (index = 0u; index < value->arrayLength; ++index) {
        if (items[index].length == 0u || items[index].length >= MAINTENANCE_SERVICE_PART_TEXT_CAPACITY)
            return false;
        memcpy(out[index], items[index].data, items[index].length);
        out[index][items[index].length] = '\0';
    }
    *count = (uint8_t)value->arrayLength;
    return true;
}

UA_StatusCode model_plan_maintenance(UA_Server *server, const UA_NodeId *sid, void *ctx,
 const UA_NodeId *mid, void *mc, const UA_NodeId *oid, void *oc, size_t n, const UA_Variant *in, size_t no, UA_Variant *out)
{
    maintenance_service_plan_request_t request = {0};
    maintenance_service_transaction_t tx = {0};
    UA_DateTime date;
    UA_Double downtime;
    UA_Int32 method;
    UA_StatusCode status;
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
    (void)sid; (void)mc; (void)oid; (void)oc;

    status = model_maintenance_mutations_available();
    if (status == UA_STATUSCODE_GOOD &&
        (n != 6u || !UA_Variant_hasScalarType(&in[1], &UA_TYPES[UA_TYPES_DATETIME]) ||
         !UA_Variant_hasScalarType(&in[2], &UA_TYPES[UA_TYPES_DOUBLE]) ||
         !UA_Variant_hasScalarType(&in[3], &UA_TYPES[UA_TYPES_INT32]) ||
         !model_copy_maintenance_string(&in[0], request.operation_id, sizeof(request.operation_id)) ||
         !model_copy_maintenance_string(&in[4], request.supplier, sizeof(request.supplier)) ||
         !model_copy_maintenance_string(&in[5], request.personnel, sizeof(request.personnel))))
        status = UA_STATUSCODE_BADINVALIDARGUMENT;
    if (status == UA_STATUSCODE_GOOD)
    {
        date = *(UA_DateTime *)in[1].data;
        downtime = *(UA_Double *)in[2].data;
        method = *(UA_Int32 *)in[3].data;
        if (date <= 0 || !isfinite(downtime) || downtime <= 0.0 ||
            downtime > MAINTENANCE_SERVICE_MAX_DOWNTIME_MS || method < 0 || method > 1)
            status = UA_STATUSCODE_BADOUTOFRANGE;
    }
    if (status == UA_STATUSCODE_GOOD)
        status = model_require_method_role(ctx, mid, no, out);
    if (status == UA_STATUSCODE_GOOD && !model_copy_session_username(ctx, actor, sizeof(actor)))
        status = UA_STATUSCODE_BADUSERACCESSDENIED;
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "PlanMaintenance", status);
        return status;
    }
    request.command_time_utc_100ns = (int64_t)UA_DateTime_now();
    request.planned_date_utc_100ns = date;
    request.estimated_downtime_ms = downtime;
    request.method = (maintenance_service_method_t)method;
    model_copy_string_safe(request.actor, sizeof(request.actor), actor);
    status = model_maintenance_result_status(maintenance_service_prepare_plan(&request, &tx));
    if (status == UA_STATUSCODE_GOOD)
        status = model_commit_maintenance_transaction(server, &tx, "PlanMaintenance", ctx);
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "PlanMaintenance", status);
        return status;
    }
    model_set_boolean_method_output(no, out, true);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_start_maintenance(UA_Server *server, const UA_NodeId *sid, void *ctx,
 const UA_NodeId *mid, void *mc, const UA_NodeId *oid, void *oc, size_t n, const UA_Variant *in, size_t no, UA_Variant *out)
{
    maintenance_service_start_request_t request = {0};
    maintenance_service_transaction_t tx = {0};
    UA_StatusCode status;
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
    (void)sid; (void)mc; (void)oid; (void)oc; (void)in;
    status = model_maintenance_mutations_available();
    if (status == UA_STATUSCODE_GOOD && n != 0u)
        status = UA_STATUSCODE_BADINVALIDARGUMENT;
    if (status == UA_STATUSCODE_GOOD)
        status = model_require_method_role(ctx, mid, no, out);
    if (status == UA_STATUSCODE_GOOD && !model_copy_session_username(ctx, actor, sizeof(actor)))
        status = UA_STATUSCODE_BADUSERACCESSDENIED;
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "StartMaintenance", status);
        return status;
    }
    request.command_time_utc_100ns = (int64_t)UA_DateTime_now();
    model_copy_string_safe(request.actor, sizeof(request.actor), actor);
    status = model_maintenance_result_status(maintenance_service_prepare_start(&request, &tx));
    if (status == UA_STATUSCODE_GOOD)
        status = model_commit_maintenance_transaction(server, &tx, "StartMaintenance", ctx);
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "StartMaintenance", status);
        return status;
    }
    model_set_boolean_method_output(no, out, true);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_reschedule_maintenance(UA_Server *server,const UA_NodeId *sid,void *ctx,const UA_NodeId *mid,void *mc,const UA_NodeId *oid,void *oc,size_t n,const UA_Variant *in,size_t no,UA_Variant *out) {
    maintenance_service_reschedule_request_t request = {0};
    maintenance_service_transaction_t tx = {0};
    UA_DateTime date;
    UA_Double downtime;
    UA_Int32 method;
    UA_StatusCode status;
    (void)sid; (void)mc; (void)oid; (void)oc;
    status = model_maintenance_mutations_available();
    if (status == UA_STATUSCODE_GOOD &&
        (n != 5u || !UA_Variant_hasScalarType(&in[0], &UA_TYPES[UA_TYPES_DATETIME]) ||
         !UA_Variant_hasScalarType(&in[1], &UA_TYPES[UA_TYPES_DOUBLE]) ||
         !UA_Variant_hasScalarType(&in[2], &UA_TYPES[UA_TYPES_INT32]) ||
         !model_copy_maintenance_string(&in[3], request.supplier, sizeof(request.supplier)) ||
         !model_copy_maintenance_string(&in[4], request.personnel, sizeof(request.personnel))))
        status = UA_STATUSCODE_BADINVALIDARGUMENT;
    if (status == UA_STATUSCODE_GOOD)
    {
        date = *(UA_DateTime *)in[0].data;
        downtime = *(UA_Double *)in[1].data;
        method = *(UA_Int32 *)in[2].data;
        if (date <= 0 || !isfinite(downtime) || downtime <= 0.0 ||
            downtime > MAINTENANCE_SERVICE_MAX_DOWNTIME_MS || method < 0 || method > 1)
            status = UA_STATUSCODE_BADOUTOFRANGE;
    }
    if (status == UA_STATUSCODE_GOOD)
        status = model_require_method_role(ctx, mid, no, out);
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "RescheduleMaintenance", status);
        return status;
    }
    request.command_time_utc_100ns = (int64_t)UA_DateTime_now();
    request.planned_date_utc_100ns = date;
    request.estimated_downtime_ms = downtime;
    request.method = (maintenance_service_method_t)method;
    status = model_maintenance_result_status(maintenance_service_prepare_reschedule(&request, &tx));
    if (status == UA_STATUSCODE_GOOD)
        status = model_commit_maintenance_transaction(server, &tx, "RescheduleMaintenance", ctx);
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "RescheduleMaintenance", status);
        return status;
    }
    model_set_boolean_method_output(no, out, true);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_finish_maintenance(UA_Server *server,const UA_NodeId *sid,void *ctx,const UA_NodeId *mid,void *mc,const UA_NodeId *oid,void *oc,size_t n,const UA_Variant *in,size_t no,UA_Variant *out) {
    maintenance_service_finish_request_t request = {0};
    maintenance_service_transaction_t tx = {0};
    UA_StatusCode status;
    char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
    (void)sid; (void)mc; (void)oid; (void)oc;
    status = model_maintenance_mutations_available();
    if (status == UA_STATUSCODE_GOOD &&
        (n != 4u || !model_copy_maintenance_parts(&in[0], request.parts_serviced,
                                                   &request.parts_serviced_count) ||
         !model_copy_maintenance_parts(&in[1], request.parts_replaced,
                                       &request.parts_replaced_count) ||
         !UA_Variant_hasScalarType(&in[2], &UA_TYPES[UA_TYPES_BOOLEAN]) ||
         !model_copy_maintenance_string(&in[3], request.result, sizeof(request.result))))
        status = UA_STATUSCODE_BADINVALIDARGUMENT;
    if (status == UA_STATUSCODE_GOOD)
        status = model_require_method_role(ctx, mid, no, out);
    if (status == UA_STATUSCODE_GOOD && !model_copy_session_username(ctx, actor, sizeof(actor)))
        status = UA_STATUSCODE_BADUSERACCESSDENIED;
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "FinishMaintenance", status);
        return status;
    }
    request.command_time_utc_100ns = (int64_t)UA_DateTime_now();
    request.configuration_changed = *(UA_Boolean *)in[2].data == UA_TRUE;
    model_copy_string_safe(request.actor, sizeof(request.actor), actor);
    status = model_maintenance_result_status(maintenance_service_prepare_finish(&request, &tx));
    if (status == UA_STATUSCODE_GOOD)
        status = model_commit_maintenance_transaction(server, &tx, "FinishMaintenance", ctx);
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(ctx, "FinishMaintenance", status);
        return status;
    }
    model_set_boolean_method_output(no, out, true);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_reset_maintenance_journal(
    UA_Server *server, const UA_NodeId *session_id, void *session_context,
    const UA_NodeId *method_id, void *method_context,
    const UA_NodeId *object_id, void *object_context, size_t input_size,
    const UA_Variant *input, size_t output_size, UA_Variant *output)
{
    maintenance_service_reset_outcome_t outcome = MAINTENANCE_SERVICE_RESET_NO_CHANGE;
    maintenance_service_snapshot_t snapshot = {0};
    UA_StatusCode status;

    (void)session_id; (void)method_context; (void)object_id; (void)object_context;
    (void)input;
    if (input_size != 0u)
    {
        status = UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    else
    {
        status = model_require_method_role(session_context, method_id,
                                           output_size, output);
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(session_context,
                                          "ResetMaintenanceJournal", status);
        return status;
    }
    status = model_maintenance_result_status(
        maintenance_service_reset_journal_ex(&outcome));
    if (status != UA_STATUSCODE_GOOD)
    {
        model_audit_maintenance_rejection(session_context,
                                          "ResetMaintenanceJournal", status);
        return status;
    }
    status = model_maintenance_result_status(
        maintenance_service_get_snapshot(&snapshot));
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_sync_maintenance_condition(server, &snapshot);
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        /* A committed reset cannot be reversed; do not publish a success
         * audit when readback/reconciliation could not complete. */
        model_audit_maintenance_rejection(session_context,
                                          "ResetMaintenanceJournal", status);
        return status;
    }
    if (outcome == MAINTENANCE_SERVICE_RESET_APPLIED)
    {
        taskENTER_CRITICAL();
        memset(g_model_maintenance_export_continuations, 0,
               sizeof(g_model_maintenance_export_continuations));
        taskEXIT_CRITICAL();
        /* This is the sole successful Reset audit.  Publishing both a
         * maintenance-call and maintenance-reset audit would violate the
         * v0.3.3 one-event reset cardinality once the source is reachable. */
        model_audit_session_event(session_context, "maintenance-reset",
                                  "ResetMaintenanceJournal", true,
                                  "logical reset committed");
    }
    model_set_boolean_method_output(output_size, output, true);
    return UA_STATUSCODE_GOOD;
}

bool model_export_append(size_t *offset, const char *format, ...)
{
    va_list args;
    int written;

    if (offset == NULL || g_model_maintenance_export_payload == NULL ||
        *offset >= MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY)
    {
        return false;
    }
    va_start(args, format);
    written = vsnprintf(&g_model_maintenance_export_payload[*offset],
                        MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY - *offset,
                        format, args);
    va_end(args);
    if (written < 0 ||
        (size_t)written >= MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY - *offset)
    {
        return false;
    }
    *offset += (size_t)written;
    return true;
}

bool model_export_append_json_string(size_t *offset, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    if (!model_export_append(offset, "\""))
        return false;
    while (*cursor != '\0')
    {
        if (*cursor == '"' || *cursor == '\\')
        {
            if (!model_export_append(offset, "\\%c", *cursor)) return false;
        }
        else if (*cursor < 0x20u)
        {
            if (!model_export_append(offset, "\\u%04x", (unsigned)*cursor)) return false;
        }
        else if (!model_export_append(offset, "%c", *cursor))
        {
            return false;
        }
        ++cursor;
    }
    return model_export_append(offset, "\"");
}

bool model_export_append_parts(
    size_t *offset,
    const char parts[MAINTENANCE_SERVICE_MAX_PARTS][MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
    uint8_t count)
{
    uint8_t index;

    if (parts == NULL || count > MAINTENANCE_SERVICE_MAX_PARTS ||
        !model_export_append(offset, "["))
    {
        return false;
    }
    for (index = 0u; index < count; ++index)
    {
        if ((index > 0u && !model_export_append(offset, ",")) ||
            !model_export_append_json_string(offset, parts[index]))
        {
            return false;
        }
    }
    return model_export_append(offset, "]");
}

bool model_parse_maintenance_export_token(const UA_Variant *input,
                                                  uint32_t *out_token)
{
    const UA_String *text;
    uint32_t value = 0u;
    size_t index;

    if (input == NULL || out_token == NULL ||
        !UA_Variant_hasScalarType(input, &UA_TYPES[UA_TYPES_STRING]) || input->data == NULL)
        return false;
    text = (const UA_String *)input->data;
    if (text->length != 8u)
        return false;
    for (index = 0u; index < text->length; ++index)
    {
        const uint8_t c = text->data[index];
        uint8_t digit;
        if (c >= '0' && c <= '9') digit = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint8_t)(c - 'a' + 10u);
        else if (c >= 'A' && c <= 'F') digit = (uint8_t)(c - 'A' + 10u);
        else return false;
        value = (value << 4u) | digit;
    }
    *out_token = value;
    return value != 0u;
}

/* Every rejected export path goes through this one volatile audit boundary.
 * A failed page cannot safely retain its cursor because a retry might otherwise
 * expose an incompletely constructed response after reset/reclaim. */
UA_StatusCode model_export_maintenance_failure(
    void *session_context, model_maintenance_export_continuation_t *continuation,
    UA_StatusCode status)
{
    if (continuation != NULL)
    {
        taskENTER_CRITICAL();
        continuation->active = false;
        taskEXIT_CRITICAL();
    }
    model_audit_maintenance_rejection(session_context,
                                      "ExportMaintenanceJournal", status);
    return status;
}

UA_StatusCode model_export_maintenance_journal(
    UA_Server *server, const UA_NodeId *session_id, void *session_context,
    const UA_NodeId *method_id, void *method_context,
    const UA_NodeId *object_id, void *object_context, size_t input_size,
    const UA_Variant *input, size_t output_size, UA_Variant *output)
{
    maintenance_service_export_record_t records[MAINTENANCE_SERVICE_EXPORT_PAGE_MAX] = {{0}};
    maintenance_service_export_cursor_t cursor = {0};
    model_maintenance_export_continuation_t *continuation = NULL;
    maintenance_service_result_t service_result;
    UA_StatusCode status;
    uint32_t token = 0u;
    uint8_t count = 0u;
    bool more = false;
    size_t offset = 0u;
    size_t index;
    TickType_t expiry;

    (void)server; (void)session_id; (void)method_context; (void)object_id; (void)object_context;
    if (input_size != 1u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_STRING]))
        status = UA_STATUSCODE_BADINVALIDARGUMENT;
    else
        status = model_require_method_role(session_context, method_id, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return model_export_maintenance_failure(session_context, NULL, status);
    }
    {
        const UA_String *requested = (const UA_String *)input[0].data;
        if (requested == NULL)
            return model_export_maintenance_failure(
                session_context, NULL, UA_STATUSCODE_BADINVALIDARGUMENT);
        if (requested->length == 0u)
        {
            service_result = maintenance_service_export_begin(&cursor);
            status = model_maintenance_result_status(service_result);
            if (status != UA_STATUSCODE_GOOD)
                return model_export_maintenance_failure(session_context, NULL, status);
            taskENTER_CRITICAL();
            for (index = 0u; index < MODEL_MAINTENANCE_EXPORT_CONTINUATION_CAPACITY; ++index)
                if (!g_model_maintenance_export_continuations[index].active) { continuation = &g_model_maintenance_export_continuations[index]; break; }
            if (continuation != NULL)
            {
                token = ++g_model_maintenance_export_next_token;
                if (token == 0u) token = ++g_model_maintenance_export_next_token;
                continuation->active = true;
                continuation->token = token;
                continuation->cursor = cursor;
                continuation->expires_at = xTaskGetTickCount() +
                                           pdMS_TO_TICKS(MODEL_MAINTENANCE_EXPORT_EXPIRY_MS);
            }
            taskEXIT_CRITICAL();
            if (continuation == NULL)
                return model_export_maintenance_failure(
                    session_context, NULL, UA_STATUSCODE_BADNOCONTINUATIONPOINTS);
        }
        else
        {
            if (!model_parse_maintenance_export_token(&input[0], &token))
                return model_export_maintenance_failure(
                    session_context, NULL, UA_STATUSCODE_BADCONTINUATIONPOINTINVALID);
            taskENTER_CRITICAL();
            for (index = 0u; index < MODEL_MAINTENANCE_EXPORT_CONTINUATION_CAPACITY; ++index)
                if (g_model_maintenance_export_continuations[index].active &&
                    g_model_maintenance_export_continuations[index].token == token) { continuation = &g_model_maintenance_export_continuations[index]; break; }
            expiry = xTaskGetTickCount();
            if (continuation != NULL &&
                (TickType_t)(expiry - continuation->expires_at) <
                    (TickType_t)(1u << ((sizeof(TickType_t) * 8u) - 1u)))
            {
                continuation->active = false;
                continuation = NULL;
            }
            taskEXIT_CRITICAL();
            if (continuation == NULL)
                return model_export_maintenance_failure(
                    session_context, NULL, UA_STATUSCODE_BADCONTINUATIONPOINTINVALID);
        }
    }
    service_result = maintenance_service_export_page(&continuation->cursor, records, &count, &more);
    status = model_maintenance_result_status(service_result);
    if (status != UA_STATUSCODE_GOOD)
    {
        status = service_result == MAINTENANCE_SERVICE_RESULT_INVALID_STATE
                     ? UA_STATUSCODE_BADCONTINUATIONPOINTINVALID : status;
        return model_export_maintenance_failure(session_context, continuation, status);
    }
    if (g_model_maintenance_export_payload == NULL)
    {
        return model_export_maintenance_failure(
            session_context, continuation, UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    }
    if (!model_export_append(&offset, "{\"records\":["))
        return model_export_maintenance_failure(
            session_context, continuation, UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    for (index = 0u; index < count; ++index)
    {
        const maintenance_service_export_record_t *record = &records[index];
        if (index > 0u && !model_export_append(&offset, ","))
            return model_export_maintenance_failure(
                session_context, continuation, UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
        if (!model_export_append(&offset, "{\"epoch\":%llu,\"sequence\":%llu,\"operation\":%u,\"transition\":%u,\"state\":%u,\"revision\":%lu,\"committedAt\":%lld,\"plannedDate\":%lld,\"estimatedDowntime\":%.17g,\"method\":%u,\"configurationChanged\":%s,\"operationId\":",
                                 (unsigned long long)record->epoch, (unsigned long long)record->sequence,
                                 (unsigned)record->operation, (unsigned)record->transition,
                                 (unsigned)record->snapshot.state, (unsigned long)record->snapshot.revision,
                                 (long long)record->snapshot.committed_at_utc_100ns,
                                 (long long)record->snapshot.activity.planned_date_utc_100ns,
                                 record->snapshot.activity.estimated_downtime_ms,
                                 (unsigned)record->snapshot.activity.method,
                                 record->snapshot.activity.configuration_changed ? "true" : "false") ||
            !model_export_append_json_string(&offset, record->snapshot.activity.operation_id) ||
            !model_export_append(&offset, ",\"supplier\":") || !model_export_append_json_string(&offset, record->snapshot.activity.supplier) ||
            !model_export_append(&offset, ",\"personnel\":") || !model_export_append_json_string(&offset, record->snapshot.activity.personnel) ||
            !model_export_append(&offset, ",\"actor\":") || !model_export_append_json_string(&offset, record->snapshot.activity.actor) ||
            !model_export_append(&offset, ",\"result\":") || !model_export_append_json_string(&offset, record->snapshot.activity.result) ||
            !model_export_append(&offset, ",\"partsServicedCount\":%u,\"partsServiced\":",
                                 (unsigned)record->snapshot.activity.parts_serviced_count) ||
            !model_export_append_parts(&offset, record->snapshot.activity.parts_serviced,
                                       record->snapshot.activity.parts_serviced_count) ||
            !model_export_append(&offset, ",\"partsReplacedCount\":%u,\"partsReplaced\":",
                                 (unsigned)record->snapshot.activity.parts_replaced_count) ||
            !model_export_append_parts(&offset, record->snapshot.activity.parts_replaced,
                                       record->snapshot.activity.parts_replaced_count) ||
            !model_export_append(&offset, "}"))
            return model_export_maintenance_failure(
                session_context, continuation, UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    }
    if (more)
    {
        continuation->expires_at = xTaskGetTickCount() +
                                   pdMS_TO_TICKS(MODEL_MAINTENANCE_EXPORT_EXPIRY_MS);
        if (!model_export_append(&offset, "],\"continuationToken\":\"%08lx\",\"more\":true}", (unsigned long)token))
            return model_export_maintenance_failure(
                session_context, continuation, UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    }
    else
    {
        continuation->active = false;
        if (!model_export_append(&offset, "],\"continuationToken\":\"\",\"more\":false}"))
            return model_export_maintenance_failure(
                session_context, continuation, UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    }
    status = model_set_maintenance_export_method_output(output_size, output, offset);
    if (status != UA_STATUSCODE_GOOD)
        return model_export_maintenance_failure(session_context, continuation, status);
    model_audit_maintenance_accept(session_context, "ExportMaintenanceJournal");
    return UA_STATUSCODE_GOOD;
}

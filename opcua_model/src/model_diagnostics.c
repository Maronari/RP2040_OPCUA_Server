#include "opcua_model_internal.h"

UA_StatusCode model_set_role(UA_Server *server,
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
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    char role_text[16];
    security_user_role_t role = SECURITY_ROLE_VIEWER;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 2u ||
        !model_copy_input_string(&input[0], username, sizeof(username)) ||
        !model_copy_input_string(&input[1], role_text, sizeof(role_text)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!security_service_role_from_string(role_text, &role))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    result = security_service_set_role(username, role);
    printf("[OPC UA]\tSetRole %s for %s -> %s\n",
           result ? "succeeded" : "failed",
           username,
           security_service_role_to_string(role));
    fflush(stdout);
    model_audit_session_event(session_context,
                              "role-change",
                              username,
                              result,
                              security_service_role_to_string(role));
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_disable_user(UA_Server *server,
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
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || !model_copy_input_string(&input[0], username, sizeof(username)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_disable_user(username);
    printf("[OPC UA]\tDisableUser %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "user-disable",
                              username,
                              result,
                              result ? "user disabled" : "disable failed");
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_delete_user(UA_Server *server,
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
    char username[SECURITY_SERVICE_USERNAME_MAX_LEN];
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status = model_require_admin_session(session_context, output_size, output);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (input_size != 1u || !model_copy_input_string(&input[0], username, sizeof(username)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_delete_user(username);
    printf("[OPC UA]\tDeleteUser %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "user-delete",
                              username,
                              result,
                              result ? "user deleted" : "delete failed");
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_add_object(UA_Server *server,
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

UA_StatusCode model_add_folder(UA_Server *server,
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

UA_StatusCode model_add_amb_folder(UA_Server *server,
                                         const char *node_id,
                                         const char *browse_name,
                                         const char *description,
                                         UA_NodeId parent_node_id)
{
    UA_ObjectAttributes attributes = UA_ObjectAttributes_default;
    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);

    return UA_Server_addObjectNode(
        server,
        UA_NODEID_STRING(1, (char *)node_id),
        parent_node_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)browse_name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
        attributes,
        NULL,
        NULL);
}

UA_StatusCode model_add_static_organized_variable(UA_Server *server,
                                                         const char *node_id,
                                                         const char *browse_name,
                                                         const char *description,
                                                         UA_NodeId parent_node_id,
                                                         const void *value,
                                                         const UA_DataType *data_type)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_StatusCode status;

    if (value == NULL || data_type == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attributes.dataType = data_type->typeId;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    status = UA_Variant_setScalarCopy(&attributes.value, value, data_type);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = UA_Server_addVariableNode(
        server,
        UA_NODEID_STRING(1, (char *)node_id),
        parent_node_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, (char *)browse_name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
        attributes,
        NULL,
        NULL);
    UA_Variant_clear(&attributes.value);
    return status;
}

UA_StatusCode model_build_amb_capabilities_and_requirements(UA_Server *server,
                                                                   UA_NodeId device_id)
{
    static const struct
    {
        const char *node_id;
        const char *browse_name;
        const char *description;
    } capabilities[] = {
        {"Device.Capabilities.TemperatureMeasurement", "TemperatureMeasurement",
         "Measures and exposes temperature telemetry"},
        {"Device.Capabilities.FirmwareUpdate", "FirmwareUpdate",
         "Supports staged firmware update with integrity verification"},
        {"Device.Capabilities.ConfigurationManagement", "ConfigurationManagement",
         "Supports persistent configuration import, export and reset"},
        {"Device.Capabilities.CertificateAndTrustManagement", "CertificateAndTrustManagement",
         "Supports certificate rotation and persistent trust-list management"},
        {"Device.Capabilities.AssetDiscovery", "AssetDiscovery",
         "Supports OPC UA AMB discovery by ProductInstanceUri and AssetId"},
        {"Device.Capabilities.StructuredLocation", "StructuredLocation",
         "Supports AMB hierarchical and operational asset locations"},
        {"Device.Capabilities.SecureCommunication", "SecureCommunication",
         "Supports encrypted OPC UA SecureChannels"},
#ifdef UA_ENABLE_SUBSCRIPTIONS
        {"Device.Capabilities.Subscriptions", "Subscriptions",
         "Supports OPC UA subscriptions and monitored items"},
#endif
#ifdef UA_ENABLE_HISTORIZING
        {"Device.Capabilities.Historizing", "Historizing",
         "Supports historical data access"},
#endif
#if defined(UA_ENABLE_SUBSCRIPTIONS_EVENTS) && defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
        {"Device.Capabilities.AlarmsAndConditions", "AlarmsAndConditions",
         "Supports OPC UA Alarms and Conditions"},
#endif
#ifdef UA_ENABLE_PUBSUB
        {"Device.Capabilities.PubSub", "PubSub",
         "Supports OPC UA PubSub"},
#endif
#ifdef UA_ENABLE_MQTT
        {"Device.Capabilities.MQTTTransport", "MQTTTransport",
         "Supports MQTT transport for OPC UA PubSub"},
#endif
    };
    static const struct
    {
        const char *node_id;
        const char *browse_name;
        const char *description;
        const char *value;
    } requirements[] = {
        {"Device.Requirements.NetworkTransport", "NetworkTransport",
         "Network transport required for remote OPC UA services", "Ethernet/IPv4"},
        {"Device.Requirements.TrustedTimeSource", "TrustedTimeSource",
         "UTC source required for certificate validation and audit timestamps",
         "NTP or equivalent UTC source"},
        {"Device.Requirements.AdministrativeSecurityMode", "AdministrativeSecurityMode",
         "Required OPC UA MessageSecurityMode for administrative operations",
         "SignAndEncrypt"},
    };
    const UA_Boolean supported = true;
    const UA_UInt32 max_telemetry_channels = PLATFORM_CHANNEL_LIMIT;
    UA_NodeId capabilities_id = UA_NODEID_STRING(1, (char *)"Device.Capabilities");
    UA_NodeId requirements_id = UA_NODEID_STRING(1, (char *)"Device.Requirements");
    UA_StatusCode status;

    status = model_add_amb_folder(server,
                                  "Device.Capabilities",
                                  "Capabilities",
                                  "Standard AMB entry point for asset capabilities",
                                  device_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    status = model_add_amb_folder(server,
                                  "Device.Requirements",
                                  "Requirements",
                                  "Standard AMB entry point for asset requirements",
                                  device_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    for (size_t index = 0u; index < sizeof(capabilities) / sizeof(capabilities[0]); ++index)
    {
        status = model_add_static_organized_variable(server,
                                                     capabilities[index].node_id,
                                                     capabilities[index].browse_name,
                                                     capabilities[index].description,
                                                     capabilities_id,
                                                     &supported,
                                                     &UA_TYPES[UA_TYPES_BOOLEAN]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }
    status = model_add_static_organized_variable(
        server,
        "Device.Capabilities.MaxTelemetryChannels",
        "MaxTelemetryChannels",
        "Maximum number of persistently configured telemetry channels",
        capabilities_id,
        &max_telemetry_channels,
        &UA_TYPES[UA_TYPES_UINT32]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    for (size_t index = 0u; index < sizeof(requirements) / sizeof(requirements[0]); ++index)
    {
        UA_String requirement_value = UA_STRING((char *)requirements[index].value);
        status = model_add_static_organized_variable(server,
                                                     requirements[index].node_id,
                                                     requirements[index].browse_name,
                                                     requirements[index].description,
                                                     requirements_id,
                                                     &requirement_value,
                                                     &UA_TYPES[UA_TYPES_STRING]);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    printf("[OPC UA]\tAMB Capabilities and Requirements ready\n");
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}

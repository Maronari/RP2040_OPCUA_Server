#include "opcua_model_internal.h"

UA_StatusCode model_write_asset_alias_node_version(UA_Server *server,
                                                          UA_UInt32 node_id)
{
    char revision_text[16];
    UA_String revision;
    UA_Variant value;

    (void)snprintf(revision_text, sizeof(revision_text), "%lu",
                   (unsigned long)config_service_get_running_revision());
    revision = UA_STRING(revision_text);
    UA_Variant_init(&value);
    UA_Variant_setScalar(&value, &revision, &UA_TYPES[UA_TYPES_STRING]);
    return UA_Server_writeValue(server,
                                UA_NODEID_NUMERIC(g_model_amb_namespace_index, node_id),
                                value);
}

UA_StatusCode model_write_alias_last_change(UA_Server *server)
{
    const UA_Int64 unix_time = UA_DateTime_toUnixTime(UA_DateTime_now());
    const UA_Int64 seconds_since_2000 = unix_time - 946684800LL;
    UA_UInt32 next_value = 0u;
    UA_Variant value;

    if (seconds_since_2000 > 0LL)
    {
        next_value = seconds_since_2000 > (UA_Int64)UINT32_MAX
                         ? UINT32_MAX
                         : (UA_UInt32)seconds_since_2000;
        if (next_value <= g_model_alias_last_change &&
            g_model_alias_last_change < UINT32_MAX)
        {
            next_value = g_model_alias_last_change + 1u;
        }
    }
    g_model_alias_last_change = next_value;
    UA_Variant_init(&value);
    UA_Variant_setScalar(&value, &g_model_alias_last_change,
                         &UA_TYPES[UA_TYPES_UINT32]);
    return UA_Server_writeValue(server,
                                UA_NODEID_NUMERIC(0, UA_NS0ID_ALIASES_LASTCHANGE),
                                value);
}

UA_StatusCode model_update_asset_id_alias(UA_Server *server,
                                                 const char *asset_id)
{
    const char *alias_name = asset_id != NULL && asset_id[0] != '\0'
                                 ? asset_id
                                 : MODEL_ASSET_ALIAS_UNASSIGNED;
    UA_NodeId alias_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_ASSET_ALIAS_ASSET_ID);
    UA_QualifiedName browse_name =
        UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)alias_name);
    UA_LocalizedText display_name =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)alias_name);
    UA_StatusCode status;

    status = UA_Server_writeBrowseName(server, alias_id, browse_name);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    status = UA_Server_writeDisplayName(server, alias_id, display_name);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    status = model_write_asset_alias_node_version(server,
                                                  UA_AMBID_ASSET_ID_NODE_VERSION);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    return model_write_alias_last_change(server);
}

UA_StatusCode model_add_asset_alias(UA_Server *server,
                                           const char *node_id,
                                           const char *alias_name,
                                           UA_UInt32 category_id,
                                           UA_NodeId device_id)
{
    UA_ObjectAttributes attributes = UA_ObjectAttributes_default;
    UA_NodeId alias_id = UA_NODEID_STRING(1, (char *)node_id);
    UA_StatusCode status;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)alias_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                               (char *)"AMB asset discovery alias");
    status = UA_Server_addObjectNode(
        server,
        alias_id,
        UA_NODEID_NUMERIC(g_model_amb_namespace_index, category_id),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)alias_name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ALIASNAMETYPE),
        attributes,
        NULL,
        NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return UA_Server_addReference(server,
                                  alias_id,
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_ALIASFOR),
                                  UA_EXPANDEDNODEID_NODEID(device_id),
                                  true);
}

UA_StatusCode model_build_asset_discovery(UA_Server *server,
                                                 UA_NodeId device_id,
                                                 const device_profile_config_t *config)
{
    const char *asset_id;
    size_t namespace_index = 0u;
    UA_StatusCode status;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    status = UA_Server_getNamespaceByName(server,
                                          UA_STRING((char *)UA_AMB_NAMESPACE_URI),
                                          &namespace_index);
    if (status != UA_STATUSCODE_GOOD || namespace_index > UINT16_MAX)
    {
        return status != UA_STATUSCODE_GOOD ? status : UA_STATUSCODE_BADOUTOFRANGE;
    }
    g_model_amb_namespace_index = (UA_UInt16)namespace_index;

    status = UA_Server_setMethodNodeCallback(
        server,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ALIASES_FINDALIAS),
        model_find_asset_alias);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_setMethodNodeCallback(
            server,
            UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                              UA_AMBID_ASSETS_FIND_ALIAS),
            model_find_asset_alias);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_setMethodNodeCallback(
            server,
            UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                              UA_AMBID_PRODUCT_INSTANCE_URI_FIND_ALIAS),
            model_find_asset_alias);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = UA_Server_setMethodNodeCallback(
            server,
            UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                              UA_AMBID_ASSET_ID_FIND_ALIAS),
            model_find_asset_alias);
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_asset_alias(server,
                                   MODEL_NODE_ASSET_ALIAS_PRODUCT_INSTANCE_URI,
                                   config->product_instance_uri,
                                   UA_AMBID_ASSETS_BY_PRODUCT_INSTANCE_URI,
                                   device_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    asset_id = config->asset_id[0] != '\0' ? config->asset_id : MODEL_ASSET_ALIAS_UNASSIGNED;
    status = model_add_asset_alias(server,
                                   MODEL_NODE_ASSET_ALIAS_ASSET_ID,
                                   asset_id,
                                   UA_AMBID_ASSETS_BY_ASSET_ID,
                                   device_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_write_asset_alias_node_version(
        server, UA_AMBID_PRODUCT_INSTANCE_URI_NODE_VERSION);
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_asset_alias_node_version(
            server, UA_AMBID_ASSET_ID_NODE_VERSION);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_alias_last_change(server);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tAMB Asset Discovery ready, namespace=%u\n",
               (unsigned)g_model_amb_namespace_index);
        fflush(stdout);
    }
    return status;
}

UA_StatusCode model_build_device_health_nodes(UA_Server *server,
                                                     UA_NodeId device_id)
{
    UA_VariableAttributes health_attributes = UA_VariableAttributes_default;
    UA_ObjectAttributes alarms_attributes = UA_ObjectAttributes_default;
    UA_DataSource data_source = {0};
    UA_NodeId health_id = UA_NODEID_NULL;
    size_t namespace_index = 0u;
    UA_StatusCode status;

    status = UA_Server_getNamespaceByName(server,
                                          UA_STRING((char *)UA_DI_NAMESPACE_URI),
                                          &namespace_index);
    if (status != UA_STATUSCODE_GOOD || namespace_index > UINT16_MAX)
    {
        return status != UA_STATUSCODE_GOOD ? status : UA_STATUSCODE_BADOUTOFRANGE;
    }
    g_model_di_namespace_index = (UA_UInt16)namespace_index;

    status = UA_Server_addReference(
        server,
        device_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASINTERFACE),
        UA_EXPANDEDNODEID_NUMERIC(g_model_di_namespace_index, UA_DIID_IDEVICEHEALTHTYPE),
        true);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    health_attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"DeviceHealth");
    health_attributes.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                                      (char *)"Overall device health state");
    health_attributes.dataType = UA_NODEID_NUMERIC(g_model_di_namespace_index,
                                                    UA_DIID_DEVICEHEALTHENUMERATION);
    health_attributes.valueRank = UA_VALUERANK_SCALAR;
    health_attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
    health_attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    data_source.read = model_read_value;

    status = UA_Server_addVariableNode(
        server,
        UA_NODEID_STRING(1, (char *)"Device.DeviceHealth"),
        device_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(g_model_di_namespace_index, (char *)"DeviceHealth"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
        health_attributes,
        &g_fixed_node_contexts[FIXED_CTX_DEVICE_HEALTH],
        &health_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    status = UA_Server_setVariableNode_dataSource(server, health_id, data_source);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    alarms_attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US",
                                                      (char *)"DeviceHealthAlarms");
    alarms_attributes.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                                      (char *)"Device health alarm conditions");
    status = UA_Server_addObjectNode(
        server,
        UA_NODEID_STRING(1, (char *)"Device.DeviceHealthAlarms"),
        device_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(g_model_di_namespace_index, (char *)"DeviceHealthAlarms"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
        alarms_attributes,
        NULL,
        NULL);
    if (status == UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tDeviceHealth model ready, DI namespace=%u\n",
               (unsigned)g_model_di_namespace_index);
        fflush(stdout);
    }
    return status;
}

UA_StatusCode model_add_readonly_variable_ex(UA_Server *server,
                                                    const char *node_id,
                                                    const char *display_name,
                                                    UA_NodeId parent_node_id,
                                                    UA_NodeId data_type,
                                                    model_node_context_t *context,
                                                    UA_Byte access_level,
                                                    UA_Boolean historizing)
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
    attributes.accessLevel = access_level;
    attributes.userAccessLevel = access_level;
    attributes.historizing = historizing;
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

UA_StatusCode model_add_readonly_variable(UA_Server *server,
                                                 const char *node_id,
                                                 const char *display_name,
                                                 UA_NodeId parent_node_id,
                                                 UA_NodeId data_type,
                                                 model_node_context_t *context)
{
    return model_add_readonly_variable_ex(server,
                                          node_id,
                                          display_name,
                                          parent_node_id,
                                          data_type,
                                          context,
                                          UA_ACCESSLEVELMASK_READ,
                                          false);
}

UA_StatusCode model_add_writable_variable(UA_Server *server,
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
    data_source.write = model_write_value;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)display_name);
    attributes.dataType = data_type;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
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

void model_fill_string_argument(UA_Argument *argument,
                                       const char *name,
                                       const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

void model_fill_bytestring_argument(UA_Argument *argument,
                                           const char *name,
                                           const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_BYTESTRING].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

void model_fill_datetime_argument(UA_Argument *argument,
                                         const char *name,
                                         const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_DATETIME].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

void model_fill_uint32_argument(UA_Argument *argument,
                                       const char *name,
                                       const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_UINT32].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

void model_fill_double_argument(UA_Argument *argument,
                                       const char *name,
                                       const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

void model_fill_boolean_argument(UA_Argument *argument,
                                        const char *name,
                                        const char *description)
{
    UA_Argument_init(argument);
    argument->name = UA_STRING((char *)(name != NULL ? name : ""));
    argument->description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)(description != NULL ? description : ""));
    argument->dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
    argument->valueRank = UA_VALUERANK_SCALAR;
}

void model_set_string_method_output(size_t output_size, UA_Variant *output, const char *result)
{
    if (output_size > 0)
    {
        UA_String string_value = UA_STRING((char *)(result != NULL ? result : ""));
        UA_Variant_setScalarCopy(&output[0], &string_value, &UA_TYPES[UA_TYPES_STRING]);
    }
}

void model_set_bytestring_method_output(size_t output_size,
                                               UA_Variant *output,
                                               const UA_ByteString *result)
{
    if (output_size > 0)
    {
        UA_ByteString bytestring_value = UA_BYTESTRING_NULL;

        if (result != NULL)
        {
            bytestring_value = *result;
        }

        UA_Variant_setScalarCopy(&output[0], &bytestring_value, &UA_TYPES[UA_TYPES_BYTESTRING]);
    }
}

UA_StatusCode model_add_method_node_with_boolean_result(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id,
                                                               const char *node_id,
                                                               const char *method_name,
                                                               const char *description,
                                                               UA_MethodCallback callback,
                                                               size_t input_size,
                                                               UA_Argument *input_arguments)
{
    UA_MethodAttributes attributes = UA_MethodAttributes_default;
    UA_Argument output_argument;

    UA_Argument_init(&output_argument);
    output_argument.name = UA_STRING((char *)"Success");
    output_argument.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"True when the method was accepted");
    output_argument.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
    output_argument.valueRank = UA_VALUERANK_SCALAR;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)method_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attributes.executable = true;
    attributes.userExecutable = true;

    return UA_Server_addMethodNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   maintenance_folder_id,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(1, (char *)method_name),
                                   attributes,
                                   callback,
                                   input_size,
                                   input_arguments,
                                   1,
                                   &output_argument,
                                   NULL,
                                   NULL);
}

UA_StatusCode model_add_method_node_with_string_result(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id,
                                                              const char *node_id,
                                                              const char *method_name,
                                                              const char *description,
                                                              UA_MethodCallback callback,
                                                              size_t input_size,
                                                              UA_Argument *input_arguments)
{
    UA_MethodAttributes attributes = UA_MethodAttributes_default;
    UA_Argument output_argument;

    UA_Argument_init(&output_argument);
    output_argument.name = UA_STRING((char *)"Result");
    output_argument.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"Method result payload as JSON");
    output_argument.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    output_argument.valueRank = UA_VALUERANK_SCALAR;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)method_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attributes.executable = true;
    attributes.userExecutable = true;

    return UA_Server_addMethodNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   maintenance_folder_id,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(1, (char *)method_name),
                                   attributes,
                                   callback,
                                   input_size,
                                   input_arguments,
                                   1,
                                   &output_argument,
                                    NULL,
                                    NULL);
}

UA_StatusCode model_add_method_node_with_bytestring_result(UA_Server *server,
                                                                  UA_NodeId maintenance_folder_id,
                                                                  const char *node_id,
                                                                  const char *method_name,
                                                                  const char *description,
                                                                  UA_MethodCallback callback,
                                                                  size_t input_size,
                                                                  UA_Argument *input_arguments)
{
    UA_MethodAttributes attributes = UA_MethodAttributes_default;
    UA_Argument output_argument;

    UA_Argument_init(&output_argument);
    output_argument.name = UA_STRING((char *)"Payload");
    output_argument.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"Method result payload as ByteString");
    output_argument.dataType = UA_TYPES[UA_TYPES_BYTESTRING].typeId;
    output_argument.valueRank = UA_VALUERANK_SCALAR;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)method_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attributes.executable = true;
    attributes.userExecutable = true;

    return UA_Server_addMethodNode(server,
                                   UA_NODEID_STRING(1, (char *)node_id),
                                   maintenance_folder_id,
                                   UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                   UA_QUALIFIEDNAME(1, (char *)method_name),
                                   attributes,
                                   callback,
                                   input_size,
                                   input_arguments,
                                   1,
                                   &output_argument,
                                   NULL,
                                   NULL);
}

UA_StatusCode model_add_boolean_method_node(UA_Server *server,
                                                   UA_NodeId maintenance_folder_id,
                                                   const char *node_id,
                                                   const char *method_name,
                                                   const char *description,
                                                   UA_MethodCallback callback)
{
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     node_id,
                                                     method_name,
                                                     description,
                                                     callback,
                                                     0u,
                                                     NULL);
}

UA_StatusCode model_add_alarm_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.AcknowledgeAllAlarms",
                                         "AcknowledgeAllAlarms",
                                         "Acknowledges all active alarms",
                                         model_acknowledge_alarms);
}

UA_StatusCode model_add_reboot_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.Reboot",
                                         "Reboot",
                                         "Schedules a software reboot",
                                         model_request_reboot);
}

UA_StatusCode model_add_factory_reset_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_string_argument(&input_argument,
                               "Scope",
                               "Reset scope. Supported value in this build: all_keep_identity. Empty string also maps to that default.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     "Device.Maintenance.FactoryReset",
                                                     "FactoryReset",
                                                     "Erases persisted configuration slots for the requested scope and reboots the device.",
                                                     model_factory_reset,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_sync_time_now_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.SyncTimeNow",
                                         "SyncTimeNow",
                                         "Forces immediate SNTP resynchronization",
                                         model_sync_time_now);
}

UA_StatusCode model_add_blink_identify_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.BlinkIdentify",
                                         "BlinkIdentify",
                                         "Blinks the onboard identify LED",
                                         model_blink_identify);
}

UA_StatusCode model_add_apply_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.ApplyConfig",
                                         "ApplyConfig",
                                         "Applies the current runtime configuration",
                                         model_apply_config);
}

UA_StatusCode model_add_save_config_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.SaveConfig",
                                         "SaveConfig",
                                         "Persists the current configuration to flash",
                                         model_save_config);
}

UA_StatusCode model_add_set_mqtt_pubsub_enabled_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_boolean_argument(&input_argument,
                                "Enabled",
                                "True enables MQTT PubSub in configuration; requires SaveConfig and Reboot to take effect");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_MQTT_PUBSUB_ENABLED,
                                                     "SetMqttPubSubEnabled",
                                                     "Updates the MQTT PubSub enable flag in runtime configuration",
                                                     model_set_mqtt_pubsub_enabled,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_set_device_identity_method_node(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[10];

    model_fill_string_argument(&input_arguments[0], "DeviceName", "Human-readable device name");
    model_fill_string_argument(&input_arguments[1], "ProductInstanceUri", "Stable production URI");
    model_fill_string_argument(&input_arguments[2], "SerialNumber", "Production serial number");
    model_fill_string_argument(&input_arguments[3], "AssetId", "Plant asset identifier");
    model_fill_string_argument(&input_arguments[4], "HardwareRevision", "Hardware revision");
    model_fill_string_argument(&input_arguments[5], "SoftwareRevision", "Software revision");
    model_fill_datetime_argument(&input_arguments[6], "ManufactureDate", "Manufacturing timestamp in UTC");
    model_fill_datetime_argument(&input_arguments[7], "CalibrationDate", "Calibration timestamp in UTC");
    model_fill_string_argument(&input_arguments[8], "Location", "Site, line and position string");
    model_fill_string_argument(&input_arguments[9], "AssetClass", "Asset classification");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_DEVICE_IDENTITY,
                                                     "SetDeviceIdentity",
                                                     "Stages production identity fields.",
                                                     model_set_device_identity,
                                                     10u,
                                                     input_arguments);
}

UA_StatusCode model_add_set_location_paths_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[2];

    model_fill_string_argument(
        &input_arguments[0],
        "HierarchicalLocationPath",
        "Physical hierarchy separated by '/', for example Factory/Workshop/Cabinet");
    model_fill_string_argument(
        &input_arguments[1],
        "OperationalLocationPath",
        "Operational hierarchy separated by '/', for example Line/ProcessUnit");

    return model_add_method_node_with_boolean_result(
        server,
        maintenance_folder_id,
        MODEL_METHOD_SET_LOCATION_PATHS,
        "SetLocationPaths",
        "Stages AMB hierarchical and operational location paths",
        model_set_location_paths,
        2u,
        input_arguments);
}

UA_StatusCode model_add_maintenance_workflow_methods(UA_Server *server, UA_NodeId maintenance_id)
{
    UA_Argument plan[6], reschedule[5], finish[4], export_input;
    UA_UInt32 finish_array_dimension = 0u;
    UA_StatusCode status;
    model_fill_string_argument(&plan[0], "OperationId", "Bounded maintenance operation identifier");
    model_fill_datetime_argument(&plan[1], "PlannedDate", "UTC planned date");
    model_fill_double_argument(&plan[2], "EstimatedDowntime", "Estimated downtime in milliseconds");
    model_fill_uint32_argument(&plan[3], "MaintenanceMethod", "AMB MaintenanceMethodEnum: Local=0, Remote=1"); plan[3].dataType=UA_TYPES[UA_TYPES_INT32].typeId;
    model_fill_string_argument(&plan[4], "MaintenanceSupplier", "Maintenance supplier"); model_fill_string_argument(&plan[5], "QualificationOfPersonnel", "Personnel qualification");
    status=model_add_method_node_with_boolean_result(server,maintenance_id,MODEL_METHOD_PLAN_MAINTENANCE,"PlanMaintenance","Plans a finished maintenance activity",model_plan_maintenance,6u,plan); if(status!=UA_STATUSCODE_GOOD)return status;
    memcpy(reschedule,plan+1,sizeof(reschedule)); status=model_add_method_node_with_boolean_result(server,maintenance_id,MODEL_METHOD_RESCHEDULE_MAINTENANCE,"RescheduleMaintenance","Updates planned maintenance without a transition",model_reschedule_maintenance,5u,reschedule); if(status!=UA_STATUSCODE_GOOD)return status;
    status=model_add_boolean_method_node(server,maintenance_id,MODEL_METHOD_START_MAINTENANCE,"StartMaintenance","Starts planned maintenance",model_start_maintenance); if(status!=UA_STATUSCODE_GOOD)return status;
    model_fill_string_argument(&finish[0],"PartsOfAssetServiced","Bounded array of serviced parts");finish[0].valueRank=1;finish[0].arrayDimensions=&finish_array_dimension;finish[0].arrayDimensionsSize=1u;
    model_fill_string_argument(&finish[1],"PartsOfAssetReplaced","Bounded array of replaced parts");finish[1].valueRank=1;finish[1].arrayDimensions=&finish_array_dimension;finish[1].arrayDimensionsSize=1u;
    model_fill_boolean_argument(&finish[2],"ConfigurationChanged","Configuration changed during maintenance");model_fill_string_argument(&finish[3],"Result","Maintenance result");
    status = model_add_method_node_with_boolean_result(server,maintenance_id,MODEL_METHOD_FINISH_MAINTENANCE,"FinishMaintenance","Finishes executing maintenance",model_finish_maintenance,4u,finish);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    model_fill_string_argument(&export_input, "ContinuationToken",
                               "Empty to begin; returned token expires after 30 seconds");
    status = model_add_method_node_with_string_result(
        server, maintenance_id, MODEL_METHOD_EXPORT_MAINTENANCE_JOURNAL,
        "ExportMaintenanceJournal",
        "Exports at most four validated logical journal records per page",
        model_export_maintenance_journal, 1u, &export_input);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    return model_add_boolean_method_node(
        server, maintenance_id, MODEL_METHOD_RESET_MAINTENANCE_JOURNAL,
        "ResetMaintenanceJournal",
        "Admin-only logical journal reset; preserves workflow state and revision",
        model_reset_maintenance_journal);
}

UA_StatusCode model_add_set_network_config_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[9];

    model_fill_boolean_argument(&input_arguments[0], "UseDhcp", "True selects DHCP, false selects static IPv4");
    model_fill_string_argument(&input_arguments[1], "MacAddress", "MAC address in AA:BB:CC:DD:EE:FF format");
    model_fill_string_argument(&input_arguments[2], "Hostname", "Hostname used in the endpoint URL");
    model_fill_uint32_argument(&input_arguments[3], "OpcUaPort", "TCP port for the OPC UA endpoint");
    model_fill_string_argument(&input_arguments[4], "NtpServer", "NTP server hostname");
    model_fill_string_argument(&input_arguments[5], "IPv4", "Static IPv4 address");
    model_fill_string_argument(&input_arguments[6], "Netmask", "Static IPv4 netmask");
    model_fill_string_argument(&input_arguments[7], "Gateway", "Static IPv4 gateway");
    model_fill_string_argument(&input_arguments[8], "Dns", "Static DNS server IPv4 address");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_NETWORK_CONFIG,
                                                     "SetNetworkConfig",
                                                     "Stages network and endpoint settings.",
                                                     model_set_network_config,
                                                     9u,
                                                     input_arguments);
}

UA_StatusCode model_add_set_channel_config_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[13];

    model_fill_uint32_argument(&input_arguments[0], "SlotIndex", "Configuration slot index in the staged channel table");
    model_fill_boolean_argument(&input_arguments[1], "InUse", "True keeps the slot active; false clears it");
    model_fill_boolean_argument(&input_arguments[2], "Enabled", "True enables acquisition for the slot once applied");
    model_fill_uint32_argument(&input_arguments[3], "DriverId", "Built-in driver id: 1 internal_temp, 2 adc_voltage, 3 gpio_input");
    model_fill_uint32_argument(&input_arguments[4], "SourceIndex", "Driver-specific source index, for example ADC channel or GPIO number");
    model_fill_uint32_argument(&input_arguments[5], "SourceFlags", "Driver-specific flags such as invert or pull configuration");
    model_fill_uint32_argument(&input_arguments[6], "SamplePeriodMs", "Sampling period in milliseconds");
    model_fill_uint32_argument(&input_arguments[7], "PublishPeriodMs", "Publish period in milliseconds");
    model_fill_uint32_argument(&input_arguments[8], "HistoryIntervalMs", "Historian record interval in milliseconds");
    model_fill_double_argument(&input_arguments[9], "Scale", "Linear scale multiplier");
    model_fill_double_argument(&input_arguments[10], "Offset", "Linear offset added after scale");
    model_fill_double_argument(&input_arguments[11], "WarningThreshold", "Warning threshold for analog channels");
    model_fill_double_argument(&input_arguments[12], "CriticalThreshold", "Critical threshold for analog channels");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_CHANNEL_CONFIG,
                                                     "SetChannelConfig",
                                                     "Stages one channel slot. ApplyConfig activates runtime sampling changes; SaveConfig persists them; reboot refreshes OPC UA channel objects after topology changes.",
                                                     model_set_channel_config,
                                                     13u,
                                                     input_arguments);
}

UA_StatusCode model_add_get_channel_config_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_uint32_argument(&input_argument, "SlotIndex", "Configuration slot index in the staged channel table");
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_CHANNEL_CONFIG,
                                                    "GetChannelConfig",
                                                    "Returns one staged channel configuration as JSON.",
                                                    model_get_channel_config,
                                                    1u,
                                                    &input_argument);
}

UA_StatusCode model_add_clear_channel_config_method_node(UA_Server *server,
                                                                UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_uint32_argument(&input_argument, "SlotIndex", "Configuration slot index to clear from the staged channel table");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_CLEAR_CHANNEL_CONFIG,
                                                     "ClearChannelConfig",
                                                     "Clears one staged channel configuration slot.",
                                                     model_clear_channel_config,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_list_drivers_method_node(UA_Server *server,
                                                        UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_LIST_DRIVERS,
                                                    "ListDrivers",
                                                    "Returns the built-in driver catalog as JSON.",
                                                    model_list_drivers,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_get_config_state_method_node(UA_Server *server,
                                                            UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_CONFIG_STATE,
                                                    "GetConfigState",
                                                    "Returns running and staged configuration state as JSON.",
                                                    model_get_config_state,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_export_config_method_node(UA_Server *server,
                                                         UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_EXPORT_CONFIG,
                                                    "ExportConfig",
                                                    "Returns the current effective device configuration as JSON.",
                                                    model_export_config,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_begin_config_import_method_node(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "PayloadSizeBytes", "Total configuration payload size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "PayloadCrc32", "Expected CRC32 of the configuration payload");
    model_fill_uint32_argument(&input_arguments[2], "SchemaVersion", "Configuration schema version declared by the client payload");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_CONFIG_IMPORT,
                                                     "BeginConfigImport",
                                                     "Starts staged configuration upload into a temporary import buffer.",
                                                     model_begin_config_import,
                                                     3u,
                                                     input_arguments);
}

UA_StatusCode model_add_write_config_chunk_method_node(UA_Server *server,
                                                              UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next configuration payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_CONFIG_CHUNK,
                                                     "WriteConfigChunk",
                                                     "Appends the next sequential configuration payload chunk to the staged import buffer.",
                                                     model_write_config_chunk,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_abort_config_import_method_node(UA_Server *server,
                                                               UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_CONFIG_IMPORT,
                                         "AbortConfigImport",
                                         "Aborts the staged configuration upload session and frees the import buffer.",
                                         model_abort_config_import);
}

UA_StatusCode model_add_validate_imported_config_method_node(UA_Server *server,
                                                                    UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_VALIDATE_IMPORTED_CONFIG,
                                                    "ValidateImportedConfig",
                                                    "Validates the currently staged imported configuration payload and returns JSON diagnostics.",
                                                    model_validate_imported_config,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_apply_imported_config_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_APPLY_IMPORTED_CONFIG,
                                         "ApplyImportedConfig",
                                         "Validates, persists, and applies the currently staged imported configuration payload.",
                                         model_apply_imported_config);
}

UA_StatusCode model_add_get_trust_state_method_node(UA_Server *server,
                                                           UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_TRUST_STATE,
                                                    "GetTrustState",
                                                    "Returns persistent trust-list and staged import state as JSON.",
                                                    model_get_trust_state,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_export_trust_list_method_node(UA_Server *server,
                                                             UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_bytestring_result(server,
                                                        maintenance_folder_id,
                                                        MODEL_METHOD_EXPORT_TRUST_LIST,
                                                        "ExportTrustList",
                                                        "Returns the currently active persisted trust-list payload as a raw ByteString.",
                                                        model_export_trust_list,
                                                        0u,
                                                        NULL);
}

UA_StatusCode model_add_begin_trust_list_import_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "PayloadSizeBytes", "Total trust-list payload size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "PayloadCrc32", "Expected CRC32 of the trust-list payload");
    model_fill_uint32_argument(&input_arguments[2], "SchemaVersion", "Trust-list payload schema version declared by the client");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_TRUST_LIST_IMPORT,
                                                     "BeginTrustListImport",
                                                     "Starts staged trust-list upload into a temporary import buffer.",
                                                     model_begin_trust_list_import,
                                                     3u,
                                                     input_arguments);
}

UA_StatusCode model_add_write_trust_list_chunk_method_node(UA_Server *server,
                                                                  UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next trust-list payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_TRUST_LIST_CHUNK,
                                                     "WriteTrustListChunk",
                                                     "Appends the next sequential trust-list payload chunk to the staged import buffer.",
                                                     model_write_trust_list_chunk,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_abort_trust_list_import_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_TRUST_LIST_IMPORT,
                                         "AbortTrustListImport",
                                         "Aborts the staged trust-list upload session and frees the import buffer.",
                                         model_abort_trust_list_import);
}

UA_StatusCode model_add_validate_imported_trust_list_method_node(UA_Server *server,
                                                                        UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_VALIDATE_IMPORTED_TRUST_LIST,
                                                    "ValidateImportedTrustList",
                                                    "Validates the currently staged trust-list payload and returns JSON diagnostics.",
                                                    model_validate_imported_trust_list,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_apply_imported_trust_list_method_node(UA_Server *server,
                                                                     UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_APPLY_IMPORTED_TRUST_LIST,
                                         "ApplyImportedTrustList",
                                         "Validates, persists, and activates the currently staged trust-list payload.",
                                         model_apply_imported_trust_list);
}

UA_StatusCode model_add_get_certificate_state_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_CERTIFICATE_STATE,
                                                    "GetCertificateState",
                                                    "Returns active certificate metadata and staged identity import state as JSON.",
                                                    model_get_certificate_state,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_export_certificate_metadata_method_node(UA_Server *server,
                                                                       UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_EXPORT_CERTIFICATE_METADATA,
                                                    "ExportCertificateMetadata",
                                                    "Returns the currently active device certificate metadata as JSON.",
                                                    model_export_certificate_metadata,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_begin_identity_rotation_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "PayloadSizeBytes", "Total identity rotation package size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "PayloadCrc32", "Expected CRC32 of the identity rotation package");
    model_fill_uint32_argument(&input_arguments[2], "SchemaVersion", "Identity rotation package schema version declared by the client");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_IDENTITY_ROTATION,
                                                     "BeginIdentityRotation",
                                                     "Starts staged upload of a new device certificate and private key package.",
                                                     model_begin_identity_rotation,
                                                     3u,
                                                     input_arguments);
}

UA_StatusCode model_add_write_identity_rotation_chunk_method_node(UA_Server *server,
                                                                         UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next identity rotation payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_IDENTITY_ROTATION_CHUNK,
                                                     "WriteIdentityRotationChunk",
                                                     "Appends the next sequential identity rotation payload chunk to the staged import buffer.",
                                                     model_write_identity_rotation_chunk,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_abort_identity_rotation_method_node(UA_Server *server,
                                                                   UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_IDENTITY_ROTATION,
                                         "AbortIdentityRotation",
                                         "Aborts the staged identity rotation upload session and frees the import buffer.",
                                         model_abort_identity_rotation);
}

UA_StatusCode model_add_validate_staged_identity_method_node(UA_Server *server,
                                                                    UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_VALIDATE_STAGED_IDENTITY,
                                                    "ValidateStagedIdentity",
                                                    "Validates the currently staged identity rotation package and returns JSON diagnostics.",
                                                    model_validate_staged_identity,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_apply_staged_identity_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_APPLY_STAGED_IDENTITY,
                                         "ApplyStagedIdentity",
                                         "Validates and persists the currently staged identity rotation package for activation after reboot.",
                                         model_apply_staged_identity);
}

UA_StatusCode model_add_get_firmware_update_status_method_node(UA_Server *server,
                                                                      UA_NodeId maintenance_folder_id)
{
    return model_add_method_node_with_string_result(server,
                                                    maintenance_folder_id,
                                                    MODEL_METHOD_GET_FIRMWARE_UPDATE_STATUS,
                                                    "GetFirmwareUpdateStatus",
                                                    "Returns OTA staging session status and persistent OTA state as JSON.",
                                                    model_get_firmware_update_status,
                                                    0u,
                                                    NULL);
}

UA_StatusCode model_add_begin_firmware_update_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_uint32_argument(&input_arguments[0], "ImageSizeBytes", "Total OTA payload size in bytes");
    model_fill_uint32_argument(&input_arguments[1], "ImageCrc32", "Expected CRC32 of the OTA payload");
    model_fill_uint32_argument(&input_arguments[2], "VersionNumber", "Monotonic firmware version number for OTA state tracking");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_BEGIN_FIRMWARE_UPDATE,
                                                     "BeginFirmwareUpdate",
                                                     "Starts staging a new firmware image into Bank B and resets OTA state to IDLE for the new session.",
                                                     model_begin_firmware_update,
                                                     3u,
                                                     input_arguments);
}

UA_StatusCode model_add_write_firmware_chunk_method_node(UA_Server *server,
                                                                UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "Chunk",
                                   "Next firmware payload chunk. Maximum accepted chunk is 4096 bytes and data must be sent sequentially.");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_WRITE_FIRMWARE_CHUNK,
                                                     "WriteFirmwareChunk",
                                                     "Appends the next sequential firmware payload chunk to Bank B staging storage.",
                                                     model_write_firmware_chunk,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_finalize_firmware_update_method_node(UA_Server *server,
                                                                    UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_FINALIZE_FIRMWARE_UPDATE,
                                         "FinalizeFirmwareUpdate",
                                         "Verifies the staged Bank B image by CRC32 and writes OTA state as STAGED_IN_BANK_B.",
                                         model_finalize_firmware_update);
}

UA_StatusCode model_add_abort_firmware_update_method_node(UA_Server *server,
                                                                 UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         MODEL_METHOD_ABORT_FIRMWARE_UPDATE,
                                         "AbortFirmwareUpdate",
                                         "Aborts the active staging session and resets OTA state back to IDLE.",
                                         model_abort_firmware_update);
}

UA_StatusCode model_add_self_test_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.SelfTest",
                                         "SelfTest",
                                         "Runs a basic service health check",
                                         model_self_test);
}

UA_StatusCode model_add_clear_diagnostics_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    return model_add_boolean_method_node(server,
                                         maintenance_folder_id,
                                         "Device.Maintenance.ClearDiagnostics",
                                         "ClearDiagnostics",
                                         "Clears software diagnostic fault state",
                                         model_clear_diagnostics);
}

UA_StatusCode model_add_set_trust_list_method_node(UA_Server *server, UA_NodeId security_folder_id)
{
    UA_Argument input_argument;

    model_fill_bytestring_argument(&input_argument,
                                   "TrustList",
                                   "Trusted client certificate or bundle as a raw ByteString. Empty clears the trust list.");
    return model_add_method_node_with_boolean_result(server,
                                                     security_folder_id,
                                                     MODEL_METHOD_SET_TRUST_LIST,
                                                     "SetTrustList",
                                                     "Stores trust anchors in persistent security storage.",
                                                     model_set_trust_list,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_enter_commissioning_method_node(UA_Server *server, UA_NodeId security_folder_id)
{
    return model_add_boolean_method_node(server,
                                         security_folder_id,
                                         MODEL_METHOD_ENTER_COMMISSIONING,
                                         "EnterCommissioningMode",
                                         "Temporarily enables commissioning mode with weakened certificate checks",
                                         model_enter_commissioning_mode);
}

UA_StatusCode model_add_exit_commissioning_method_node(UA_Server *server, UA_NodeId security_folder_id)
{
    return model_add_boolean_method_node(server,
                                         security_folder_id,
                                         MODEL_METHOD_EXIT_COMMISSIONING,
                                         "ExitCommissioningMode",
                                         "Disables commissioning mode and requires a provisioned trust list",
                                         model_exit_commissioning_mode);
}

UA_StatusCode model_add_add_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_string_argument(&input_arguments[0], "Username", "Name of the user to create");
    model_fill_string_argument(&input_arguments[1], "Password", "Initial password");
    model_fill_string_argument(&input_arguments[2], "Role", "One of Viewer, Operator, Service or Admin");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_ADD_USER,
                                                     "AddUser",
                                                     "Creates a new local user",
                                                     model_add_user,
                                                     3u,
                                                     input_arguments);
}

UA_StatusCode model_add_change_password_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[3];

    model_fill_string_argument(&input_arguments[0], "Username", "Name of the user");
    model_fill_string_argument(&input_arguments[1], "OldPassword", "Current password");
    model_fill_string_argument(&input_arguments[2], "NewPassword", "Replacement password");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_CHANGE_PASSWORD,
                                                     "ChangePassword",
                                                     "Changes an existing local user password",
                                                     model_change_password,
                                                     3u,
                                                     input_arguments);
}

UA_StatusCode model_add_set_role_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_arguments[2];

    model_fill_string_argument(&input_arguments[0], "Username", "Name of the user");
    model_fill_string_argument(&input_arguments[1], "Role", "One of Viewer, Operator, Service or Admin");

    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_SET_ROLE,
                                                     "SetRole",
                                                     "Updates the local role assignment of a user",
                                                     model_set_role,
                                                     2u,
                                                     input_arguments);
}

UA_StatusCode model_add_disable_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_string_argument(&input_argument, "Username", "Name of the user to disable");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_DISABLE_USER,
                                                     "DisableUser",
                                                     "Disables an existing local user",
                                                     model_disable_user,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_delete_user_method_node(UA_Server *server, UA_NodeId maintenance_folder_id)
{
    UA_Argument input_argument;

    model_fill_string_argument(&input_argument, "Username", "Name of the user to delete");
    return model_add_method_node_with_boolean_result(server,
                                                     maintenance_folder_id,
                                                     MODEL_METHOD_DELETE_USER,
                                                     "DeleteUser",
                                                     "Deletes an existing local user",
                                                     model_delete_user,
                                                     1u,
                                                     &input_argument);
}

UA_StatusCode model_add_task_stack_high_water_node(UA_Server *server,
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

model_node_context_t *model_prepare_channel_context(size_t channel_index,
                                                           size_t field_index,
                                                           model_node_kind_t kind,
                                                           const char *channel_id)
{
    model_node_context_t *context = NULL;

    if (channel_index >= CHANNEL_MANAGER_MAX_CHANNELS || field_index >= MODEL_CHANNEL_FIELD_CONTEXT_COUNT)
    {
        return NULL;
    }

    context = &g_channel_node_contexts[channel_index][field_index];
    context->kind = kind;
    context->channel_id = channel_id;
    context->task_name = NULL;
    return context;
}

UA_StatusCode model_add_channel_nodes(UA_Server *server,
                                             UA_NodeId channels_folder_id,
                                             size_t channel_index,
                                             const telemetry_channel_t *channel)
{
    char channel_node_id[128];
    char value_node_id[128];
    char status_node_id[128];
    char source_timestamp_node_id[128];
    char units_node_id[128];
    char update_period_node_id[128];
    char history_interval_node_id[128];
    char alarm_state_node_id[128];
    UA_NodeId channel_object_id = UA_NODEID_NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;
    UA_NodeId value_data_type;

    if (server == NULL || channel == NULL || channel->metadata.id == NULL || channel->metadata.name == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!model_build_channel_node_id(channel, NULL, channel_node_id, sizeof(channel_node_id)) ||
        !model_build_channel_node_id(channel, "Value", value_node_id, sizeof(value_node_id)) ||
        !model_build_channel_node_id(channel, "Status", status_node_id, sizeof(status_node_id)) ||
        !model_build_channel_node_id(channel, "SourceTimestamp", source_timestamp_node_id, sizeof(source_timestamp_node_id)) ||
        !model_build_channel_node_id(channel, "Units", units_node_id, sizeof(units_node_id)) ||
        !model_build_channel_node_id(channel, "UpdatePeriodMs", update_period_node_id, sizeof(update_period_node_id)) ||
        !model_build_channel_node_id(channel, "HistoryIntervalMs", history_interval_node_id, sizeof(history_interval_node_id)) ||
        !model_build_channel_node_id(channel, "AlarmState", alarm_state_node_id, sizeof(alarm_state_node_id)))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    channel_object_id = UA_NODEID_STRING(1, channel_node_id);
    value_data_type = model_channel_value_type_to_data_type(channel->metadata.value_type);

    status = model_add_object(server,
                              channel_node_id,
                              channel->metadata.name,
                              channel->metadata.name,
                              channels_folder_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable_ex(server,
                                            value_node_id,
                                            "Value",
                                            channel_object_id,
                                            value_data_type,
                                            model_prepare_channel_context(channel_index,
                                                                          0u,
                                                                          MODEL_NODE_PROCESS_TEMPERATURE_VALUE,
                                                                          channel->metadata.id),
                                            UA_ACCESSLEVELMASK_READ |
                                                (((channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) != 0u)
                                                     ? UA_ACCESSLEVELMASK_HISTORYREAD
                                                     : 0u),
                                            ((channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) != 0u));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         status_node_id,
                                         "Status",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       1u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_STATUS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         source_timestamp_node_id,
                                         "SourceTimestamp",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       2u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         units_node_id,
                                         "Units",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       3u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_UNITS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         update_period_node_id,
                                         "UpdatePeriodMs",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       4u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         history_interval_node_id,
                                         "HistoryIntervalMs",
                                         channel_object_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         model_prepare_channel_context(channel_index,
                                                                       5u,
                                                                       MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS,
                                                                       channel->metadata.id));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return model_add_readonly_variable(server,
                                       alarm_state_node_id,
                                       "AlarmState",
                                       channel_object_id,
                                       UA_TYPES[UA_TYPES_STRING].typeId,
                                       model_prepare_channel_context(channel_index,
                                                                     6u,
                                                                     MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE,
                                                                     channel->metadata.id));
}

UA_StatusCode model_build_address_space(UA_Server *server)
{
    const device_profile_config_t *device_config = config_service_get();
    const char *device_display_name = device_config->device_name[0] != '\0' ? device_config->device_name : "Device";
    UA_NodeId device_id = UA_NODEID_STRING(1, (char *)"Device");
    UA_NodeId identification_id = UA_NODEID_STRING(1, (char *)"Device.Identification");
    UA_NodeId channels_id = UA_NODEID_STRING(1, (char *)DEVICE_CHANNELS_NODE_ID);
    UA_NodeId diagnostics_id = UA_NODEID_STRING(1, (char *)"Device.Diagnostics");
    UA_NodeId psram_id = UA_NODEID_STRING(1, (char *)"Device.Diagnostics.PSRAM");
    UA_NodeId task_id = UA_NODEID_STRING(1, (char *)"Device.Diagnostics.Task");
    UA_NodeId network_id = UA_NODEID_STRING(1, (char *)"Device.Network");
    UA_NodeId time_id = UA_NODEID_STRING(1, (char *)"Device.Time");
    UA_NodeId security_id = UA_NODEID_STRING(1, (char *)"Device.Security");
    UA_NodeId configuration_id = UA_NODEID_STRING(1, (char *)"Device.Configuration");
    UA_NodeId maintenance_id = UA_NODEID_STRING(1, (char *)"Device.Maintenance");
    UA_NodeId maintenance_configuration_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_CONFIGURATION);
    UA_NodeId maintenance_firmware_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_FIRMWARE);
    UA_NodeId maintenance_security_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_SECURITY);
    UA_NodeId maintenance_diagnostics_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_DIAGNOSTICS);
    UA_NodeId maintenance_system_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_SYSTEM);
    UA_NodeId messaging_id = UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING);
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

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    status = UA_Server_writeEventNotifier(server,
                                          device_id,
                                          UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
#endif

    status = model_build_asset_discovery(server, device_id, device_config);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_build_amb_capabilities_and_requirements(server, device_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_build_amb_locations(server, device_config);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_build_device_health_nodes(server, device_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_di_functional_group(server,
                                           "Device.Identification",
                                           "Identification",
                                           device_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, DEVICE_CHANNELS_NODE_ID, "Channels", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, "Device.Diagnostics", "Diagnostics", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              "Device.Diagnostics.PSRAM",
                              "PSRAM",
                              diagnostics_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
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

    status = model_add_folder(server, "Device.Security", "Security", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
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

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_CONFIGURATION,
                              "Configuration",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_FIRMWARE,
                              "Firmware",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_SECURITY,
                              "Security",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_DIAGNOSTICS,
                              "Diagnostics",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server,
                              MODEL_NODE_MAINTENANCE_SYSTEM,
                              "System",
                              maintenance_id,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_folder(server, MODEL_NODE_MESSAGING, "Messaging", device_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

#if defined(UA_ENABLE_SUBSCRIPTIONS_EVENTS) && defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
    status = model_build_alarm_conditions(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
#endif

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_DEVICE,
                                  "DeviceEvent",
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_CONFIG_CHANGED,
                                  "ConfigChangedEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_NETWORK,
                                  "NetworkEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_SENSOR_FAULT,
                                  "SensorFaultEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_MAINTENANCE,
                                  "MaintenanceEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_FIRMWARE,
                                  "FirmwareEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT,
                                  "AuditEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_DEVICE));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_LOGIN_SUCCESS,
                                  "AuditLoginSuccessEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_LOGIN_FAILURE,
                                  "AuditLoginFailureEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE,
                                  "AuditConfigChangeEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_type(server,
                                  MODEL_EVENT_TYPE_AUDIT_SECURITY_CHANGE,
                                  "AuditSecurityChangeEvent",
                                  UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_event_property(server,
                                      UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_CONFIG_CHANGED),
                                      "Types.Events.ConfigChangedEvent.ChangedSection",
                                      "ChangedSection",
                                      UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_CONFIG_CHANGED),
                                       "Types.Events.ConfigChangedEvent.ChangedKey",
                                       "ChangedKey",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_CONFIG_CHANGED),
                                       "Types.Events.ConfigChangedEvent.Actor",
                                       "Actor",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.OldIp",
                                       "OldIp",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.NewIp",
                                       "NewIp",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.DhcpEnabled",
                                       "DhcpEnabled",
                                       UA_TYPES[UA_TYPES_BOOLEAN].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_NETWORK),
                                       "Types.Events.NetworkEvent.LinkUp",
                                       "LinkUp",
                                       UA_TYPES[UA_TYPES_BOOLEAN].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.SensorId",
                                       "SensorId",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.FaultCode",
                                       "FaultCode",
                                       UA_TYPES[UA_TYPES_UINT32].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.Quality",
                                       "Quality",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_SENSOR_FAULT),
                                       "Types.Events.SensorFaultEvent.MeasuredValue",
                                       "MeasuredValue",
                                       UA_TYPES[UA_TYPES_FLOAT].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_MAINTENANCE),
                                       "Types.Events.MaintenanceEvent.Operation",
                                       "Operation",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_MAINTENANCE),
                                       "Types.Events.MaintenanceEvent.ResultCode",
                                       "ResultCode",
                                       UA_TYPES[UA_TYPES_UINT32].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_MAINTENANCE),
                                       "Types.Events.MaintenanceEvent.Actor",
                                       "Actor",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_FIRMWARE),
                                       "Types.Events.FirmwareEvent.Version",
                                       "Version",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_FIRMWARE),
                                       "Types.Events.FirmwareEvent.BuildTime",
                                       "BuildTime",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_FIRMWARE),
                                       "Types.Events.FirmwareEvent.GitHash",
                                       "GitHash",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Actor",
                                       "Actor",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Subject",
                                       "Subject",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Result",
                                       "Result",
                                       UA_TYPES[UA_TYPES_BOOLEAN].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT),
                                       "Types.Events.AuditEvent.Details",
                                       "Details",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE),
                                       "Types.Events.AuditConfigChangeEvent.ChangedSection",
                                       "ChangedSection",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT_CONFIG_CHANGE),
                                       "Types.Events.AuditConfigChangeEvent.ChangedKey",
                                       "ChangedKey",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    status |= model_add_event_property(server,
                                       UA_NODEID_STRING(1, (char *)MODEL_EVENT_TYPE_AUDIT_SECURITY_CHANGE),
                                       "Types.Events.AuditSecurityChangeEvent.Operation",
                                       "Operation",
                                       UA_TYPES[UA_TYPES_STRING].typeId);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
#endif

    status = model_add_di_identification_property(
        server,
        "Device.Identification.Manufacturer",
        "Manufacturer",
        identification_id,
        UA_TYPES[UA_TYPES_LOCALIZEDTEXT].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_MANUFACTURER],
        false);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_di_identification_property(
        server,
        "Device.Identification.Model",
        "Model",
        identification_id,
        UA_TYPES[UA_TYPES_LOCALIZEDTEXT].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_MODEL],
        false);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_di_identification_property(
        server,
        "Device.Identification.SerialNumber",
        "SerialNumber",
        identification_id,
        UA_TYPES[UA_TYPES_STRING].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_SERIAL_NUMBER],
        false);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_di_identification_property(
        server,
        "Device.Identification.HardwareRevision",
        "HardwareRevision",
        identification_id,
        UA_TYPES[UA_TYPES_STRING].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_HW_REVISION],
        false);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_di_identification_property(
        server,
        "Device.Identification.SoftwareRevision",
        "SoftwareRevision",
        identification_id,
        UA_TYPES[UA_TYPES_STRING].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_FW_REVISION],
        false);
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

    status = model_add_di_identification_property(
        server,
        "Device.Identification.ProductInstanceUri",
        "ProductInstanceUri",
        identification_id,
        UA_TYPES[UA_TYPES_STRING].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_PRODUCT_INSTANCE_URI],
        false);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_di_identification_property(
        server,
        "Device.Identification.AssetId",
        "AssetId",
        identification_id,
        UA_TYPES[UA_TYPES_STRING].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_ASSET_ID],
        true);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_di_identification_property(
        server,
        "Device.Identification.RevisionCounter",
        "RevisionCounter",
        identification_id,
        UA_TYPES[UA_TYPES_INT32].typeId,
        &g_fixed_node_contexts[FIXED_CTX_IDENT_REVISION_COUNTER],
        false);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.MacAddress",
                                         "MacAddress",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MAC_ADDRESS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.ManufactureDate",
                                         "ManufactureDate",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_MANUFACTURE_DATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.CalibrationDate",
                                         "CalibrationDate",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_CALIBRATION_DATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.Location",
                                         "Location",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_LOCATION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Identification.AssetClass",
                                         "AssetClass",
                                         identification_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_IDENT_ASSET_CLASS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    for (size_t channel_index = 0u; channel_index < channel_manager_count(); ++channel_index)
    {
        telemetry_channel_t *channel = channel_manager_get_by_index(channel_index);

        if (channel == NULL)
        {
            continue;
        }

        status = model_add_channel_nodes(server, channels_id, channel_index, channel);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
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
                                         "Device.Diagnostics.PSRAM.Ready",
                                         "Ready",
                                         psram_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_PSRAM_READY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.PSRAM.TotalBytes",
                                         "TotalBytes",
                                         psram_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_PSRAM_TOTAL_BYTES]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.PSRAM.UsedBytes",
                                         "UsedBytes",
                                         psram_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_PSRAM_USED_BYTES]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.PSRAM.FreeBytes",
                                         "FreeBytes",
                                         psram_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_PSRAM_FREE_BYTES]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.PSRAM.LargestFreeBlockBytes",
                                         "LargestFreeBlockBytes",
                                         psram_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_PSRAM_LARGEST_FREE_BLOCK_BYTES]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.PSRAM.AllocatorMode",
                                         "AllocatorMode",
                                         psram_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_PSRAM_ALLOCATOR_MODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
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

    status = model_add_readonly_variable(server,
                                         "Device.Diagnostics.LastFault",
                                         "LastFault",
                                         diagnostics_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_DIAG_LAST_FAULT]);
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
                                         "Device.Network.LastNetworkChange",
                                         "LastNetworkChange",
                                         network_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_NETWORK_LAST_NETWORK_CHANGE]);
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

    status = model_add_writable_variable(server,
                                         "Device.Time.TimeZoneOffsetMinutes",
                                         "TimeZoneOffsetMinutes",
                                         time_id,
                                         UA_TYPES[UA_TYPES_INT16].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_TIME_TIMEZONE_OFFSET_MINUTES]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.SecurityMode",
                                         "SecurityMode",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_MODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CommissioningMode",
                                         "CommissioningMode",
                                         security_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_COMMISSIONING_MODE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.TrustListVersion",
                                         "TrustListVersion",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_TRUST_LIST_VERSION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.TrustedClientCount",
                                         "TrustedClientCount",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_TRUSTED_CLIENT_COUNT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.LastAuthFailure",
                                         "LastAuthFailure",
                                         security_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_LAST_AUTH_FAILURE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.FailedLoginCount",
                                         "FailedLoginCount",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_FAILED_LOGIN_COUNT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CertThumbprint",
                                         "CertThumbprint",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CERT_THUMBPRINT]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CertNotBefore",
                                         "CertNotBefore",
                                         security_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CERT_NOT_BEFORE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CertNotAfter",
                                         "CertNotAfter",
                                         security_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CERT_NOT_AFTER]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.DaysUntilExpiry",
                                         "DaysUntilExpiry",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_DAYS_UNTIL_EXPIRY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.ExpiryState",
                                         "ExpiryState",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_EXPIRY_STATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CommissioningState",
                                         "CommissioningState",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_COMMISSIONING_STATE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.ActiveIdentityGeneration",
                                         "ActiveIdentityGeneration",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_ACTIVE_IDENTITY_GENERATION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.ActiveTrustGeneration",
                                         "ActiveTrustGeneration",
                                         security_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_ACTIVE_TRUST_GENERATION]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.CurrentUserRole",
                                         "CurrentUserRole",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_CURRENT_USER_ROLE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Security.LastEventId",
                                         "LastEventId",
                                         security_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_SECURITY_LAST_EVENT_ID]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.LastConfigChange",
                                         "LastConfigChange",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_LAST_CONFIG_CHANGE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.RebootRequired",
                                         "RebootRequired",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_REBOOT_REQUIRED]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         "Device.Configuration.Commissioned",
                                         "Commissioned",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_BOOLEAN].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_COMMISSIONED]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.SamplingPeriodMs",
                                         "SamplingPeriodMs",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_SAMPLING_PERIOD_MS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.PublishPeriodMs",
                                         "PublishPeriodMs",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_PUBLISH_PERIOD_MS]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.TempOffset",
                                         "TempOffset",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_TEMP_OFFSET]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.TempScale",
                                         "TempScale",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_TEMP_SCALE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.HighAlarm",
                                         "HighAlarm",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_HIGH_ALARM]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_writable_variable(server,
                                         "Device.Configuration.HighHighAlarm",
                                         "HighHighAlarm",
                                         configuration_id,
                                         UA_TYPES[UA_TYPES_FLOAT].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_CONFIG_HIGH_HIGH_ALARM]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_SEQUENCE,
                                         "LastPubSubEventSequence",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_SEQUENCE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_ID,
                                         "LastPubSubEventId",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_ID]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_TYPE,
                                         "LastPubSubEventType",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_TYPE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_SOURCE,
                                         "LastPubSubEventSource",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_SOURCE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_MESSAGE,
                                         "LastPubSubEventMessage",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_STRING].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_MESSAGE]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_SEVERITY,
                                         "LastPubSubEventSeverity",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_UINT32].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_SEVERITY]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_add_readonly_variable(server,
                                         MODEL_NODE_MESSAGING_LAST_EVENT_TIME,
                                         "LastPubSubEventTime",
                                         messaging_id,
                                         UA_TYPES[UA_TYPES_DATETIME].typeId,
                                         &g_fixed_node_contexts[FIXED_CTX_MESSAGING_EVENT_TIME]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (g_model_config.enable_alarm_methods)
    {
        status = model_add_maintenance_workflow_methods(server, maintenance_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_alarm_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_reboot_method_node(server, maintenance_system_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_factory_reset_method_node(server, maintenance_system_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_sync_time_now_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_blink_identify_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_save_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_mqtt_pubsub_enabled_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_device_identity_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_location_paths_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_network_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_channel_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_channel_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_clear_channel_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_list_drivers_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_config_state_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_export_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_config_import_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_config_chunk_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_config_import_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_validate_imported_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_imported_config_method_node(server, maintenance_configuration_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_firmware_update_status_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_firmware_update_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_firmware_chunk_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_finalize_firmware_update_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_firmware_update_method_node(server, maintenance_firmware_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_self_test_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_clear_diagnostics_method_node(server, maintenance_diagnostics_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_trust_list_method_node(server, security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_enter_commissioning_method_node(server, security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_exit_commissioning_method_node(server, security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_trust_state_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_export_trust_list_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_trust_list_import_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_trust_list_chunk_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_trust_list_import_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_validate_imported_trust_list_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_imported_trust_list_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_get_certificate_state_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_export_certificate_metadata_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_begin_identity_rotation_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_write_identity_rotation_chunk_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_abort_identity_rotation_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_validate_staged_identity_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_apply_staged_identity_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
        status = model_add_add_user_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_change_password_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_set_role_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_disable_user_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_add_delete_user_method_node(server, maintenance_security_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
#endif
    }

    return UA_STATUSCODE_GOOD;
}


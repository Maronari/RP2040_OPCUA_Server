#include "opcua_model_internal.h"

bool model_pop_pending_audit_event(model_pending_audit_event_t *out_event)
{
    bool has_event = false;

    taskENTER_CRITICAL();
    if (g_pending_audit_event_count > 0u)
    {
        if (out_event != NULL)
        {
            *out_event = g_pending_audit_events[g_pending_audit_event_head];
        }
        g_pending_audit_event_head = (g_pending_audit_event_head + 1u) % MODEL_AUDIT_QUEUE_CAPACITY;
        --g_pending_audit_event_count;
        has_event = true;
    }
    taskEXIT_CRITICAL();

    return has_event;
}

void model_security_audit_callback(const char *event,
                                          const char *actor,
                                          const char *subject,
                                          bool success,
                                          const char *details,
                                          void *context)
{
    (void)context;
    model_queue_pending_audit_event(event, actor, subject, success, details);
}

bool model_string_changed(const char *lhs, const char *rhs)
{
    if (lhs == NULL)
    {
        lhs = "";
    }

    if (rhs == NULL)
    {
        rhs = "";
    }

    return strcmp(lhs, rhs) != 0;
}

UA_StatusCode model_add_event_type(UA_Server *server,
                                          const char *node_id,
                                          const char *browse_name,
                                          const UA_NodeId parent_type_id)
{
    return model_add_object_type(server, node_id, browse_name, parent_type_id);
}

UA_StatusCode model_add_event_property(UA_Server *server,
                                              const UA_NodeId event_type_id,
                                              const char *node_id,
                                              const char *browse_name,
                                              UA_NodeId data_type)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_NodeId property_id = UA_NODEID_NULL;
    UA_StatusCode status;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.dataType = data_type;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    model_set_default_variant_value(&attributes, data_type);

    status = UA_Server_addVariableNode(server,
                                       UA_NODEID_STRING(1, (char *)node_id),
                                       event_type_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
                                       UA_QUALIFIEDNAME(1, (char *)browse_name),
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE),
                                       attributes,
                                       NULL,
                                       &property_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    /* MINIMAL Namespace 0 does not instantiate the ModellingRule objects.
       Keep the custom event fields usable even when the Mandatory rule target
       is absent. */
    {
        UA_NodeClass modelling_rule_class = UA_NODECLASS_UNSPECIFIED;
        status = UA_Server_readNodeClass(server,
                                         UA_NODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_MANDATORY),
                                         &modelling_rule_class);
        if (status != UA_STATUSCODE_GOOD)
        {
            return UA_STATUSCODE_GOOD;
        }
    }

    return UA_Server_addReference(server,
                                  property_id,
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_HASMODELLINGRULE),
                                  UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_MANDATORY),
                                  true);
}

#define MODEL_EVENT_FIELD_PATH_CAPACITY 64u

/* open62541 indexes custom fields by UA_SimpleAttributeOperand_print(). For a
 * BaseEventType select with ns=1 BrowseName this is the namespace-zero map key
 * "/1:<BrowseName>", not QualifiedName(1, <BrowseName>). */
UA_StatusCode model_make_event_field_key(const char *browse_name,
                                                char field_key[MODEL_EVENT_FIELD_PATH_CAPACITY])
{
    int written;

    if (browse_name == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    written = snprintf(field_key, MODEL_EVENT_FIELD_PATH_CAPACITY, "/1:%s", browse_name);
    return written >= 0 && (size_t)written < MODEL_EVENT_FIELD_PATH_CAPACITY
               ? UA_STATUSCODE_GOOD
               : UA_STATUSCODE_BADENCODINGLIMITSEXCEEDED;
}

UA_StatusCode model_set_event_string_field(UA_KeyValueMap *event_fields,
                                                  const char *browse_name,
                                                  const char *value)
{
    char field_key[MODEL_EVENT_FIELD_PATH_CAPACITY];
    UA_String string_value = UA_STRING((char *)(value != NULL ? value : ""));
    UA_StatusCode status = model_make_event_field_key(browse_name, field_key);

    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    return UA_KeyValueMap_setScalar(event_fields,
                                    UA_QUALIFIEDNAME(0, field_key),
                                    &string_value,
                                    &UA_TYPES[UA_TYPES_STRING]);
}

#if defined(UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS)
UA_StatusCode model_enable_condition_source(UA_Server *server,
                                                   const UA_NodeId parent_id,
                                                   const UA_NodeId source_id)
{
    UA_ExpandedNodeId expanded_source = UA_EXPANDEDNODEID_NULL;
    UA_StatusCode status;

    status = UA_Server_writeEventNotifier(server,
                                          source_id,
                                          UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    expanded_source.nodeId = source_id;
    status = UA_Server_addReference(server,
                                    parent_id,
                                    UA_NODEID_NUMERIC(0, UA_NS0ID_HASNOTIFIER),
                                    expanded_source,
                                    true);
    if (status == UA_STATUSCODE_BADDUPLICATEREFERENCENOTALLOWED)
    {
        return UA_STATUSCODE_GOOD;
    }

    return status;
}

UA_StatusCode model_add_condition_type(UA_Server *server,
                                              const char *node_id,
                                              const char *browse_name,
                                              const UA_NodeId parent_type_id)
{
    return model_add_object_type(server, node_id, browse_name, parent_type_id);
}

UA_StatusCode model_condition_set_field_scalar(UA_Server *server,
                                                      const UA_NodeId condition_id,
                                                      const char *field_name,
                                                      const void *value,
                                                      const UA_DataType *type)
{
    UA_Variant variant;

    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, (void *)value, type);
    return UA_Server_setConditionField(server,
                                       condition_id,
                                       &variant,
                                       UA_QUALIFIEDNAME(0, (char *)field_name));
}

UA_StatusCode model_condition_set_message(UA_Server *server,
                                                 const UA_NodeId condition_id,
                                                 const char *message)
{
    UA_LocalizedText text =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)(message != NULL ? message : ""));
    UA_Variant variant;

    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, &text, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    return UA_Server_setConditionField(server,
                                       condition_id,
                                       &variant,
                                       UA_QUALIFIEDNAME(0, (char *)"Message"));
}

UA_StatusCode model_condition_set_two_state_id(UA_Server *server,
                                                      const UA_NodeId condition_id,
                                                      const char *field_name,
                                                      UA_Boolean state)
{
    UA_Variant variant;

    UA_Variant_init(&variant);
    UA_Variant_setScalar(&variant, &state, &UA_TYPES[UA_TYPES_BOOLEAN]);
    return UA_Server_setConditionVariableFieldProperty(server,
                                                       condition_id,
                                                       &variant,
                                                       UA_QUALIFIEDNAME(0, (char *)field_name),
                                                       UA_QUALIFIEDNAME(0, (char *)"Id"));
}

/* Condition Enable/Disable is a standard OPC UA client operation. The runtime
 * synchronizer must not publish repeatedly to a disabled condition, but must
 * resume service-owned reconciliation after it is enabled again. */
UA_StatusCode model_condition_read_enabled(UA_Server *server,
                                                  const UA_NodeId condition_id,
                                                  bool *out_enabled)
{
    UA_QualifiedName path[2] = {
        UA_QUALIFIEDNAME(0, (char *)"EnabledState"),
        UA_QUALIFIEDNAME(0, (char *)"Id")};
    UA_BrowsePathResult result;
    UA_Variant value;
    UA_StatusCode status;

    if (server == NULL || out_enabled == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = UA_Server_browseSimplifiedBrowsePath(server, condition_id, 2u, path);
    if (result.statusCode != UA_STATUSCODE_GOOD || result.targetsSize != 1u)
    {
        status = result.statusCode != UA_STATUSCODE_GOOD ? result.statusCode
                                                         : UA_STATUSCODE_BADNOTFOUND;
        UA_BrowsePathResult_clear(&result);
        return status;
    }

    UA_Variant_init(&value);
    status = UA_Server_readValue(server, result.targets[0].targetId.nodeId, &value);
    UA_BrowsePathResult_clear(&result);
    if (status != UA_STATUSCODE_GOOD)
    {
        UA_Variant_clear(&value);
        return status;
    }
    if (!UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BOOLEAN]))
    {
        UA_Variant_clear(&value);
        return UA_STATUSCODE_BADTYPEMISMATCH;
    }

    *out_enabled = (*(const UA_Boolean *)value.data) == UA_TRUE;
    UA_Variant_clear(&value);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_add_amb_maintenance_property(
    UA_Server *server, const UA_NodeId condition_id, const char *node_id,
    const char *browse_name, UA_NodeId data_type, const void *value,
    const UA_DataType *value_type)
{
    UA_VariableAttributes attr = UA_VariableAttributes_default;

    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attr.dataType = data_type;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    UA_Variant_setScalar(&attr.value, (void *)value, value_type);
    return UA_Server_addVariableNode(
        server, UA_NODEID_STRING(1, (char *)node_id), condition_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)browse_name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, NULL);
}

UA_StatusCode model_register_amb_name_node_id_type(UA_Server *server)
{
    UA_ServerConfig *config;

    if (server == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    if (g_model_amb_custom_types_registered)
    {
        return UA_STATUSCODE_GOOD;
    }

    memset(g_model_amb_name_node_id_members, 0,
           sizeof(g_model_amb_name_node_id_members));
#ifdef UA_ENABLE_TYPEDESCRIPTION
    g_model_amb_name_node_id_members[0].memberName = "Name";
    g_model_amb_name_node_id_members[1].memberName = "NodeId";
#endif
    g_model_amb_name_node_id_members[0].memberType = &UA_TYPES[UA_TYPES_LOCALIZEDTEXT];
    g_model_amb_name_node_id_members[0].padding =
        (UA_Byte)offsetof(model_amb_name_node_id_t, name);
    g_model_amb_name_node_id_members[1].memberType = &UA_TYPES[UA_TYPES_NODEID];
    g_model_amb_name_node_id_members[1].padding = (UA_Byte)(
        offsetof(model_amb_name_node_id_t, node_id) -
        offsetof(model_amb_name_node_id_t, name) - sizeof(UA_LocalizedText));

    memset(&g_model_amb_name_node_id_type, 0, sizeof(g_model_amb_name_node_id_type));
#ifdef UA_ENABLE_TYPEDESCRIPTION
    g_model_amb_name_node_id_type.typeName = "NameNodeIdDataType";
#endif
    g_model_amb_name_node_id_type.typeId =
        UA_NODEID_NUMERIC(g_model_amb_namespace_index, UA_AMBID_NAME_NODE_ID_DATA_TYPE);
    g_model_amb_name_node_id_type.binaryEncodingId =
        UA_NODEID_NUMERIC(g_model_amb_namespace_index, 5012u);
    g_model_amb_name_node_id_type.xmlEncodingId =
        UA_NODEID_NUMERIC(g_model_amb_namespace_index, 5013u);
    g_model_amb_name_node_id_type.memSize = sizeof(model_amb_name_node_id_t);
    g_model_amb_name_node_id_type.typeKind = UA_DATATYPEKIND_STRUCTURE;
    g_model_amb_name_node_id_type.pointerFree = false;
    g_model_amb_name_node_id_type.overlayable = false;
    g_model_amb_name_node_id_type.membersSize = 2u;
    g_model_amb_name_node_id_type.members = g_model_amb_name_node_id_members;

    memset(&g_model_amb_custom_types, 0, sizeof(g_model_amb_custom_types));
    g_model_amb_custom_types.typesSize = 1u;
    g_model_amb_custom_types.types = &g_model_amb_name_node_id_type;
    g_model_amb_custom_types.cleanup = false;
    config = UA_Server_getConfig(server);
    g_model_amb_custom_types.next = (UA_DataTypeArray *)config->customDataTypes;
    config->customDataTypes = &g_model_amb_custom_types;
    g_model_amb_custom_types_registered = true;
    return UA_STATUSCODE_GOOD;
}

void model_make_amb_name_node_id(model_amb_name_node_id_t *value,
                                        const char *text)
{
    if (value == NULL)
    {
        return;
    }
    UA_LocalizedText_init(&value->name);
    value->name = UA_LOCALIZEDTEXT((char *)"en-US", (char *)(text != NULL ? text : ""));
    value->node_id = UA_NODEID_NULL;
}

const char *model_maintenance_state_text(
    maintenance_service_state_t state)
{
    switch (state)
    {
    case MAINTENANCE_SERVICE_STATE_FINISHED:
        return "Finished";
    case MAINTENANCE_SERVICE_STATE_EXECUTING:
        return "Executing";
    case MAINTENANCE_SERVICE_STATE_PLANNED:
    default:
        return "Planned";
    }
}

UA_UInt32 model_maintenance_state_node_id(
    maintenance_service_state_t state)
{
    switch (state)
    {
    case MAINTENANCE_SERVICE_STATE_FINISHED:
        return UA_AMBID_MAINTENANCE_STATE_FINISHED;
    case MAINTENANCE_SERVICE_STATE_EXECUTING:
        return UA_AMBID_MAINTENANCE_STATE_EXECUTING;
    case MAINTENANCE_SERVICE_STATE_PLANNED:
    default:
        return UA_AMBID_MAINTENANCE_STATE_PLANNED;
    }
}

/* The MaintenanceRequired condition represents outstanding work only.  A
 * completed activity remains readable as the last service snapshot, but is no
 * longer active/retained.  These values deliberately depend on no local model
 * state so recovery and live reads expose the same condition semantics. */
bool model_maintenance_condition_is_active(
    const maintenance_service_snapshot_t *snapshot)
{
    return snapshot != NULL && snapshot->has_activity &&
           snapshot->state != MAINTENANCE_SERVICE_STATE_FINISHED;
}

UA_UInt16 model_maintenance_condition_severity(
    const maintenance_service_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->has_activity ||
        snapshot->state == MAINTENANCE_SERVICE_STATE_FINISHED)
    {
        return 100u;
    }
    return snapshot->state == MAINTENANCE_SERVICE_STATE_EXECUTING ? 500u : 350u;
}

const char *model_maintenance_condition_message(
    const maintenance_service_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->has_activity)
    {
        return "Maintenance finished";
    }
    if (snapshot->state == MAINTENANCE_SERVICE_STATE_FINISHED &&
        snapshot->activity.result[0] != '\0')
    {
        return snapshot->activity.result;
    }
    return snapshot->state == MAINTENANCE_SERVICE_STATE_EXECUTING
               ? "Maintenance executing"
               : (snapshot->state == MAINTENANCE_SERVICE_STATE_PLANNED
                      ? "Maintenance planned"
                      : "Maintenance finished");
}

UA_StatusCode model_maintenance_set_read_error(UA_DataValue *value,
                                                       UA_StatusCode status)
{
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_read_maintenance_value(
    UA_Server *server, const UA_NodeId *session_id, void *session_context,
    const UA_NodeId *node_id, void *node_context,
    UA_Boolean include_source_timestamp, const UA_NumericRange *range,
    UA_DataValue *value)
{
    const model_maintenance_value_context_t *context =
        (const model_maintenance_value_context_t *)node_context;
    maintenance_service_snapshot_t snapshot = {0};
    model_amb_name_node_id_t names[MAINTENANCE_SERVICE_MAX_PARTS];
    UA_StatusCode status;
    UA_LocalizedText text;
    UA_NodeId state_id;
    UA_DateTime planned_date;
    UA_Double downtime;
    UA_Int32 method;
    UA_Boolean configuration_changed;
    UA_Boolean active;
    UA_UInt16 severity;
    UA_StatusCode quality = UA_STATUSCODE_GOOD;
    uint8_t index;

    (void)server;
    (void)session_id;
    (void)session_context;
    (void)node_id;

    if (value == NULL || context == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    if (range != NULL)
    {
        return model_maintenance_set_read_error(value,
                                                UA_STATUSCODE_BADINDEXRANGEINVALID);
    }
    if (maintenance_service_get_snapshot(&snapshot) != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return model_maintenance_set_read_error(value,
                                                UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    }
    severity = model_maintenance_condition_severity(&snapshot);

    /* The service copies one active revision under its lock. All values below
     * are consequently derived from that committed revision only. */
    switch (context->kind)
    {
    case MODEL_MAINTENANCE_VALUE_CURRENT_STATE:
        text = UA_LOCALIZEDTEXT((char *)"en-US",
                                (char *)model_maintenance_state_text(snapshot.state));
        status = UA_Variant_setScalarCopy(&value->value, &text,
                                          &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
        break;
    case MODEL_MAINTENANCE_VALUE_CURRENT_STATE_ID:
        state_id = UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                                     model_maintenance_state_node_id(snapshot.state));
        status = UA_Variant_setScalarCopy(&value->value, &state_id,
                                          &UA_TYPES[UA_TYPES_NODEID]);
        break;
    case MODEL_MAINTENANCE_VALUE_PLANNED_DATE:
        planned_date = (UA_DateTime)snapshot.activity.planned_date_utc_100ns;
        status = UA_Variant_setScalarCopy(&value->value, &planned_date,
                                          &UA_TYPES[UA_TYPES_DATETIME]);
        break;
    case MODEL_MAINTENANCE_VALUE_ESTIMATED_DOWNTIME:
        downtime = snapshot.activity.estimated_downtime_ms;
        status = UA_Variant_setScalarCopy(&value->value, &downtime,
                                          &UA_TYPES[UA_TYPES_DOUBLE]);
        break;
    case MODEL_MAINTENANCE_VALUE_METHOD:
        method = (UA_Int32)snapshot.activity.method;
        status = UA_Variant_setScalarCopy(&value->value, &method,
                                          &g_model_amb_maintenance_method_type);
        break;
    case MODEL_MAINTENANCE_VALUE_SUPPLIER:
        model_make_amb_name_node_id(&names[0], snapshot.activity.supplier);
        status = UA_Variant_setScalarCopy(&value->value, &names[0],
                                          &g_model_amb_name_node_id_type);
        break;
    case MODEL_MAINTENANCE_VALUE_PERSONNEL:
        model_make_amb_name_node_id(&names[0], snapshot.activity.personnel);
        status = UA_Variant_setScalarCopy(&value->value, &names[0],
                                          &g_model_amb_name_node_id_type);
        break;
    case MODEL_MAINTENANCE_VALUE_PARTS_REPLACED:
        for (index = 0u; index < snapshot.activity.parts_replaced_count; ++index)
        {
            model_make_amb_name_node_id(&names[index],
                                        snapshot.activity.parts_replaced[index]);
        }
        status = UA_Variant_setArrayCopy(&value->value, names,
                                         snapshot.activity.parts_replaced_count,
                                         &g_model_amb_name_node_id_type);
        break;
    case MODEL_MAINTENANCE_VALUE_PARTS_SERVICED:
        for (index = 0u; index < snapshot.activity.parts_serviced_count; ++index)
        {
            model_make_amb_name_node_id(&names[index],
                                        snapshot.activity.parts_serviced[index]);
        }
        status = UA_Variant_setArrayCopy(&value->value, names,
                                         snapshot.activity.parts_serviced_count,
                                         &g_model_amb_name_node_id_type);
        break;
    case MODEL_MAINTENANCE_VALUE_CONFIGURATION_CHANGED:
        configuration_changed = snapshot.activity.configuration_changed ? UA_TRUE : UA_FALSE;
        status = UA_Variant_setScalarCopy(&value->value, &configuration_changed,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    case MODEL_MAINTENANCE_VALUE_MESSAGE:
        text = UA_LOCALIZEDTEXT(
            (char *)"en-US",
            (char *)model_maintenance_condition_message(&snapshot));
        status = UA_Variant_setScalarCopy(&value->value, &text,
                                          &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
        break;
    case MODEL_MAINTENANCE_VALUE_SEVERITY:
        status = UA_Variant_setScalarCopy(&value->value, &severity,
                                          &UA_TYPES[UA_TYPES_UINT16]);
        break;
    case MODEL_MAINTENANCE_VALUE_QUALITY:
        status = UA_Variant_setScalarCopy(&value->value, &quality,
                                          &UA_TYPES[UA_TYPES_STATUSCODE]);
        break;
    case MODEL_MAINTENANCE_VALUE_TIME:
    case MODEL_MAINTENANCE_VALUE_RECEIVE_TIME:
        planned_date = (UA_DateTime)snapshot.committed_at_utc_100ns;
        status = UA_Variant_setScalarCopy(&value->value, &planned_date,
                                          &UA_TYPES[UA_TYPES_DATETIME]);
        break;
    case MODEL_MAINTENANCE_VALUE_ACTIVE_STATE:
    case MODEL_MAINTENANCE_VALUE_RETAIN:
        active = model_maintenance_condition_is_active(&snapshot) ? UA_TRUE : UA_FALSE;
        status = UA_Variant_setScalarCopy(&value->value, &active,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    default:
        return model_maintenance_set_read_error(value, UA_STATUSCODE_BADNOTFOUND);
    }

    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    value->hasValue = true;
    value->hasStatus = true;
    value->status = UA_STATUSCODE_GOOD;
    (void)include_source_timestamp;
    return UA_STATUSCODE_GOOD;
}

/* Immutable callback descriptors are adapter metadata, not service state. */
static const model_maintenance_value_context_t g_model_maintenance_value_contexts[] = {
    {MODEL_MAINTENANCE_VALUE_CURRENT_STATE},
    {MODEL_MAINTENANCE_VALUE_CURRENT_STATE_ID},
    {MODEL_MAINTENANCE_VALUE_PLANNED_DATE},
    {MODEL_MAINTENANCE_VALUE_ESTIMATED_DOWNTIME},
    {MODEL_MAINTENANCE_VALUE_METHOD},
    {MODEL_MAINTENANCE_VALUE_SUPPLIER},
    {MODEL_MAINTENANCE_VALUE_PERSONNEL},
    {MODEL_MAINTENANCE_VALUE_PARTS_REPLACED},
    {MODEL_MAINTENANCE_VALUE_PARTS_SERVICED},
    {MODEL_MAINTENANCE_VALUE_CONFIGURATION_CHANGED},
    {MODEL_MAINTENANCE_VALUE_MESSAGE},
    {MODEL_MAINTENANCE_VALUE_SEVERITY},
    {MODEL_MAINTENANCE_VALUE_QUALITY},
    {MODEL_MAINTENANCE_VALUE_TIME},
    {MODEL_MAINTENANCE_VALUE_RECEIVE_TIME},
    {MODEL_MAINTENANCE_VALUE_ACTIVE_STATE},
    {MODEL_MAINTENANCE_VALUE_RETAIN}};

UA_StatusCode model_install_maintenance_value_source(
    UA_Server *server, UA_NodeId node_id, model_maintenance_value_kind_t kind)
{
    UA_CallbackValueSource source = {0};
    UA_StatusCode status;

    source.read = model_read_maintenance_value;
    source.write = NULL;
    status = UA_Server_setNodeContext(
        server, node_id, (void *)&g_model_maintenance_value_contexts[(size_t)kind]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }
    return UA_Server_setVariableNode_callbackValueSource(server, node_id, source);
}

UA_StatusCode model_install_maintenance_value_source_at_path(
    UA_Server *server, const UA_NodeId condition_id, size_t path_size,
    UA_QualifiedName *path, model_maintenance_value_kind_t kind)
{
    UA_BrowsePathResult result = UA_Server_browseSimplifiedBrowsePath(
        server, condition_id, path_size, path);
    UA_StatusCode status;

    if (result.statusCode != UA_STATUSCODE_GOOD || result.targetsSize != 1u)
    {
        status = result.statusCode != UA_STATUSCODE_GOOD
                     ? result.statusCode
                     : UA_STATUSCODE_BADNOTFOUND;
        UA_BrowsePathResult_clear(&result);
        return status;
    }
    status = model_install_maintenance_value_source(
        server, result.targets[0].targetId.nodeId, kind);
    UA_BrowsePathResult_clear(&result);
    return status;
}

UA_StatusCode model_install_maintenance_condition_value_sources(
    UA_Server *server, const UA_NodeId condition_id)
{
    UA_QualifiedName state_path[] = {
        UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)"MaintenanceState"),
        UA_QUALIFIEDNAME(0, (char *)"CurrentState")};
    UA_QualifiedName state_id_path[] = {
        UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)"MaintenanceState"),
        UA_QUALIFIEDNAME(0, (char *)"CurrentState"),
        UA_QUALIFIEDNAME(0, (char *)"Id")};
    UA_QualifiedName active_state_id_path[] = {
        UA_QUALIFIEDNAME(0, (char *)"ActiveState"),
        UA_QUALIFIEDNAME(0, (char *)"Id")};
    UA_QualifiedName retain_path[] = {UA_QUALIFIEDNAME(0, (char *)"Retain")};
    UA_QualifiedName message_path[] = {UA_QUALIFIEDNAME(0, (char *)"Message")};
    UA_QualifiedName severity_path[] = {UA_QUALIFIEDNAME(0, (char *)"Severity")};
    UA_QualifiedName quality_path[] = {UA_QUALIFIEDNAME(0, (char *)"Quality")};
    UA_QualifiedName time_path[] = {UA_QUALIFIEDNAME(0, (char *)"Time")};
    UA_QualifiedName receive_time_path[] = {UA_QUALIFIEDNAME(0, (char *)"ReceiveTime")};
    UA_StatusCode status;

    status = model_install_maintenance_value_source_at_path(
        server, condition_id, 2u, state_path, MODEL_MAINTENANCE_VALUE_CURRENT_STATE);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 3u, state_id_path,
            MODEL_MAINTENANCE_VALUE_CURRENT_STATE_ID);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 2u, active_state_id_path,
            MODEL_MAINTENANCE_VALUE_ACTIVE_STATE);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 1u, retain_path, MODEL_MAINTENANCE_VALUE_RETAIN);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)"Device.DeviceHealthAlarms.MaintenanceRequired.PlannedDate"),
            MODEL_MAINTENANCE_VALUE_PLANNED_DATE);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)"Device.DeviceHealthAlarms.MaintenanceRequired.EstimatedDowntime"),
            MODEL_MAINTENANCE_VALUE_ESTIMATED_DOWNTIME);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)"Device.DeviceHealthAlarms.MaintenanceRequired.MaintenanceMethod"),
            MODEL_MAINTENANCE_VALUE_METHOD);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_SUPPLIER),
            MODEL_MAINTENANCE_VALUE_SUPPLIER);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_PERSONNEL),
            MODEL_MAINTENANCE_VALUE_PERSONNEL);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_REPLACED),
            MODEL_MAINTENANCE_VALUE_PARTS_REPLACED);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)MODEL_NODE_MAINTENANCE_SERVICED),
            MODEL_MAINTENANCE_VALUE_PARTS_SERVICED);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source(
            server, UA_NODEID_STRING(1, (char *)"Device.DeviceHealthAlarms.MaintenanceRequired.ConfigurationChanged"),
            MODEL_MAINTENANCE_VALUE_CONFIGURATION_CHANGED);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 1u, message_path, MODEL_MAINTENANCE_VALUE_MESSAGE);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 1u, severity_path, MODEL_MAINTENANCE_VALUE_SEVERITY);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 1u, quality_path, MODEL_MAINTENANCE_VALUE_QUALITY);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 1u, time_path, MODEL_MAINTENANCE_VALUE_TIME);
    if (status == UA_STATUSCODE_GOOD)
        status = model_install_maintenance_value_source_at_path(
            server, condition_id, 1u, receive_time_path,
            MODEL_MAINTENANCE_VALUE_RECEIVE_TIME);
    return status;
}

UA_StatusCode model_add_amb_name_node_id_property(
    UA_Server *server, const UA_NodeId condition_id, const char *node_id,
    const char *browse_name, UA_Boolean is_array)
{
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    model_amb_name_node_id_t empty_value;
    UA_UInt32 array_dimension = 0u;

    model_make_amb_name_node_id(&empty_value, "");
    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attr.dataType = g_model_amb_name_node_id_type.typeId;
    attr.valueRank = is_array ? 1 : UA_VALUERANK_SCALAR;
    if (is_array)
    {
        attr.arrayDimensions = &array_dimension;
        attr.arrayDimensionsSize = 1u;
    }
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    if (is_array)
    {
        UA_Variant_setArrayCopy(&attr.value, NULL, 0u, &g_model_amb_name_node_id_type);
    }
    else
    {
        UA_Variant_setScalarCopy(&attr.value, &empty_value, &g_model_amb_name_node_id_type);
    }
    {
        UA_StatusCode status = UA_Server_addVariableNode(
            server, UA_NODEID_STRING(1, (char *)node_id), condition_id,
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
            UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)browse_name),
            UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, NULL);
        UA_Variant_clear(&attr.value);
        return status;
    }
}

UA_StatusCode model_set_amb_maintenance_state(
    UA_Server *server, const UA_NodeId condition_id, const char *state_name,
    UA_UInt32 state_node_id)
{
    UA_QualifiedName path[3] = {
        UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)"MaintenanceState"),
        UA_QUALIFIEDNAME(0, (char *)"CurrentState"),
        UA_QUALIFIEDNAME(0, (char *)"Id")
    };
    UA_BrowsePathResult current_state_result;
    UA_BrowsePathResult state_id_result;
    UA_LocalizedText state_text =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)state_name);
    UA_NodeId state_id = UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                                           state_node_id);
    UA_Variant value;
    UA_StatusCode status;

    current_state_result = UA_Server_browseSimplifiedBrowsePath(
        server, condition_id, 2u, path);
    if(current_state_result.statusCode != UA_STATUSCODE_GOOD ||
       current_state_result.targetsSize != 1u) {
        status = current_state_result.statusCode != UA_STATUSCODE_GOOD
                     ? current_state_result.statusCode
                     : UA_STATUSCODE_BADNOTFOUND;
        UA_BrowsePathResult_clear(&current_state_result);
        return status;
    }
    UA_Variant_init(&value);
    UA_Variant_setScalar(&value, &state_text, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    status = UA_Server_writeValue(
        server, current_state_result.targets[0].targetId.nodeId, value);
    UA_BrowsePathResult_clear(&current_state_result);
    if(status != UA_STATUSCODE_GOOD)
        return status;

    state_id_result = UA_Server_browseSimplifiedBrowsePath(
        server, condition_id, 3u, path);
    if(state_id_result.statusCode != UA_STATUSCODE_GOOD ||
       state_id_result.targetsSize != 1u) {
        status = state_id_result.statusCode != UA_STATUSCODE_GOOD
                     ? state_id_result.statusCode
                     : UA_STATUSCODE_BADNOTFOUND;
        UA_BrowsePathResult_clear(&state_id_result);
        return status;
    }
    UA_Variant_init(&value);
    UA_Variant_setScalar(&value, &state_id, &UA_TYPES[UA_TYPES_NODEID]);
    status = UA_Server_writeValue(
        server, state_id_result.targets[0].targetId.nodeId, value);
    UA_BrowsePathResult_clear(&state_id_result);
    return status;
}

/*
 * v0.4.3 per-channel alarm condition model (T6).
 *
 * NodeId grammar: one condition instance per alarm-eligible channel, derived
 * deterministically from the v0.4.2 channel NodeId grammar
 * (model_build_channel_node_id(), "Device.Channels.c_" + hex(channel id) +
 * "." + field) by using the sibling field name "AlarmCondition" -- e.g.
 * "Device.Channels.c_74656D7031.AlarmCondition" for channel id "temp1".
 * This can never collide with an existing channel field name because every
 * existing channel field ("Value", "SamplePeriodMs", ...) is a distinct
 * fixed string.  Both the condition instance and its ConditionSource (the
 * channel object itself, i.e. the same NodeId with no field suffix) live in
 * the channel namespace (g_model_channel_namespace_index), matching where
 * the channel objects themselves live, rather than the ns=1 "Device.*"
 * strings used by the pre-existing Security/DeviceHealth conditions.
 *
 * Condition type: "Types.Conditions.ChannelAlarmConditionType", a subtype of
 * the standard SystemOffNormalAlarmType (same base used by
 * SecurityFaultConditionType), giving every channel condition the standard
 * ActiveState/AckedState/Retain/Severity/Quality/Message fields plus the
 * standard Acknowledge Method for free.
 *
 * Quality-fault surfacing: the condition's standard Quality field mirrors
 * alarm_condition_status_t.quality_fault (GOOD/UNCERTAIN/BAD) and Severity
 * reflects whichever of {quality fault, process/discrete alarm} is more
 * urgent; the Message text explicitly names both when they are
 * simultaneously true.  This deliberately does not overwrite ActiveState:
 * ActiveState/Retain answer "does this condition need attention right now"
 * for either reason, while the separate service-level
 * alarm_condition_status_t (readable via alarm_service_get_condition_by_*)
 * keeps process_state and quality_fault fully separate for any richer
 * surfacing a future release chooses to add.
 */
#define MODEL_CONDITION_TYPE_CHANNEL_ALARM "Types.Conditions.ChannelAlarmConditionType"
#define MODEL_CHANNEL_ALARM_CONDITION_FIELD_NAME "AlarmCondition"
#define MODEL_CHANNEL_ALARM_SLOT_CAPACITY PLATFORM_CHANNEL_LIMIT

/*
 * Deliberately small: the condition/source NodeId strings are never cached
 * here.  They are cheap to rebuild on demand from channel_id via
 * channel_manager_copy_snapshot_by_id() + model_build_channel_node_id(), and
 * at up to PLATFORM_CHANNEL_LIMIT slots the two MODEL_CHANNEL_NODE_ID_MAX_LEN
 * (128 byte) buffers this struct used to carry per slot would have added
 * several KB of permanent internal SRAM .bss just to avoid a bounded, rare
 * (poll-cycle or Acknowledge-Method-frequency) registry lookup + string
 * build. */
typedef struct
{
    bool slot_used;
    char channel_id[TELEMETRY_CHANNEL_ID_MAX_LEN];
    bool condition_active;
    uint32_t condition_reason;
    bool condition_enabled;
    TickType_t next_enable_poll;
    bool last_acknowledged_seen;
} model_channel_alarm_slot_t;

static model_channel_alarm_slot_t g_model_channel_alarm_slots[MODEL_CHANNEL_ALARM_SLOT_CAPACITY];
static size_t g_model_channel_alarm_slot_count;

static model_channel_alarm_slot_t *model_find_channel_alarm_slot(const char *channel_id)
{
    if (channel_id == NULL || channel_id[0] == '\0')
    {
        return NULL;
    }
    for (size_t index = 0u; index < g_model_channel_alarm_slot_count; ++index)
    {
        if (g_model_channel_alarm_slots[index].slot_used &&
            strcmp(g_model_channel_alarm_slots[index].channel_id, channel_id) == 0)
        {
            return &g_model_channel_alarm_slots[index];
        }
    }
    return NULL;
}

/* Rebuilds the condition instance NodeId (and optionally its ConditionSource
 * NodeId) for a channel from its stable id, mirroring the strings computed
 * once at creation time in model_create_channel_alarm_condition() without
 * caching them permanently. */
static bool model_channel_alarm_node_ids(const char *channel_id,
                                         char *condition_buf,
                                         size_t condition_buf_size,
                                         char *source_buf,
                                         size_t source_buf_size)
{
    telemetry_channel_snapshot_t snapshot = {0};

    if (!channel_manager_copy_snapshot_by_id(channel_id, &snapshot))
    {
        return false;
    }
    if (condition_buf != NULL &&
        !model_build_channel_node_id(&snapshot.channel, MODEL_CHANNEL_ALARM_CONDITION_FIELD_NAME,
                                     condition_buf, condition_buf_size))
    {
        return false;
    }
    if (source_buf != NULL &&
        !model_build_channel_node_id(&snapshot.channel, NULL, source_buf, source_buf_size))
    {
        return false;
    }
    return true;
}

/* Called once when a client acknowledges a per-channel condition through the
 * standard OPC UA Acknowledge Method (open62541 invokes this internally on
 * the AckedState/Id transition to true).  Mirrors the acknowledgement back
 * into the owning alarm_service condition so service-level acknowledgement
 * state stays authoritative and independent per channel, per the v0.4.3
 * requirement. */
static UA_StatusCode model_channel_alarm_acked_callback(UA_Server *server, const UA_NodeId *condition)
{
    (void)server;
    if (condition == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }
    for (size_t index = 0u; index < g_model_channel_alarm_slot_count; ++index)
    {
        model_channel_alarm_slot_t *slot = &g_model_channel_alarm_slots[index];
        char condition_node_id_buf[MODEL_CHANNEL_NODE_ID_MAX_LEN];
        UA_NodeId candidate;

        if (!slot->slot_used ||
            !model_channel_alarm_node_ids(slot->channel_id, condition_node_id_buf,
                                          sizeof(condition_node_id_buf), NULL, 0u))
        {
            continue;
        }
        candidate = UA_NODEID_STRING(g_model_channel_namespace_index, condition_node_id_buf);
        if (UA_NodeId_equal(condition, &candidate))
        {
            (void)alarm_service_acknowledge_channel(slot->channel_id);
            slot->last_acknowledged_seen = true;
            break;
        }
    }
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode model_create_channel_alarm_condition(UA_Server *server,
                                                           const telemetry_channel_t *channel,
                                                           const UA_NodeId condition_type_id)
{
    model_channel_alarm_slot_t *slot;
    UA_NodeId created_id = UA_NODEID_NULL;
    UA_NodeId source_id;
    UA_StatusCode status;
    char condition_node_id_buf[MODEL_CHANNEL_NODE_ID_MAX_LEN];
    char source_node_id_buf[MODEL_CHANNEL_NODE_ID_MAX_LEN];

    if (g_model_channel_alarm_slot_count >= MODEL_CHANNEL_ALARM_SLOT_CAPACITY)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
    if (!model_build_channel_node_id(channel, MODEL_CHANNEL_ALARM_CONDITION_FIELD_NAME,
                                     condition_node_id_buf, sizeof(condition_node_id_buf)) ||
        !model_build_channel_node_id(channel, NULL, source_node_id_buf, sizeof(source_node_id_buf)))
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    source_id = UA_NODEID_STRING(g_model_channel_namespace_index, source_node_id_buf);
    status = UA_Server_createCondition(server,
                                       UA_NODEID_STRING(g_model_channel_namespace_index, condition_node_id_buf),
                                       condition_type_id,
                                       UA_QUALIFIEDNAME(g_model_channel_namespace_index,
                                                        (char *)MODEL_CHANNEL_ALARM_CONDITION_FIELD_NAME),
                                       source_id,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                                       &created_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_condition_initialize_inactive(server, created_id, "Channel alarm inactive");
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    /* Best-effort: the condition is already usable (reflecting service state
     * every poll cycle and accepting the standard Acknowledge Method) even
     * if this registration fails; only the ack-sync-back to alarm_service
     * would be delayed until the next full poll reconciliation. */
    (void)UA_Server_setConditionTwoStateVariableCallback(server,
                                                         created_id,
                                                         source_id,
                                                         false,
                                                         model_channel_alarm_acked_callback,
                                                         UA_ENTERING_ACKEDSTATE);

    slot = &g_model_channel_alarm_slots[g_model_channel_alarm_slot_count];
    memset(slot, 0, sizeof(*slot));
    (void)snprintf(slot->channel_id, sizeof(slot->channel_id), "%s",
                   channel->metadata.id != NULL ? channel->metadata.id : "");
    slot->slot_used = true;
    slot->condition_enabled = true;
    slot->last_acknowledged_seen = true;
    ++g_model_channel_alarm_slot_count;

    return UA_STATUSCODE_GOOD;
}

/* Builds one stable condition instance per alarm-eligible channel.  Must run
 * after the channel objects themselves exist in the address space; see the
 * one-shot call site in model_process_runtime_events() for why this cannot
 * be invoked from model_build_alarm_conditions() (which runs before channel
 * nodes are built). */
UA_StatusCode model_build_channel_alarm_conditions(UA_Server *server)
{
    UA_NodeId condition_type_id;
    size_t channel_count;
    UA_StatusCode status;

    if (server == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    if (!g_model_alarm_conditions_available)
    {
        return UA_STATUSCODE_GOOD;
    }

    status = model_add_condition_type(server,
                                      MODEL_CONDITION_TYPE_CHANNEL_ALARM,
                                      "ChannelAlarmConditionType",
                                      UA_NODEID_NUMERIC(0, UA_NS0ID_SYSTEMOFFNORMALALARMTYPE));
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tChannelAlarmConditionType create failed: 0x%x (%s)\n",
               status, UA_StatusCode_name(status));
        fflush(stdout);
        return status;
    }

    condition_type_id = model_condition_type_node_id(MODEL_CONDITION_TYPE_CHANNEL_ALARM);
    channel_count = channel_manager_count();
    if (channel_count > PLATFORM_CHANNEL_LIMIT)
    {
        channel_count = PLATFORM_CHANNEL_LIMIT;
    }

    for (size_t index = 0u; index < channel_count; ++index)
    {
        telemetry_channel_snapshot_t snapshot = {0};

        if (!channel_manager_copy_snapshot_by_index(index, &snapshot))
        {
            continue;
        }
        if (!alarm_service_value_type_is_eligible(snapshot.channel.metadata.value_type))
        {
            continue;
        }

        status = model_create_channel_alarm_condition(server, &snapshot.channel, condition_type_id);
        if (status != UA_STATUSCODE_GOOD)
        {
            /* One channel's condition failing to build must not block the
             * rest -- conditions are independent, unlike the transactional
             * v0.4.2 channel-node build. */
            printf("[OPC UA]\tChannel alarm condition create failed for %s: 0x%x (%s)\n",
                   snapshot.channel.metadata.id != NULL ? snapshot.channel.metadata.id : "?",
                   status, UA_StatusCode_name(status));
            fflush(stdout);
        }
    }

    return UA_STATUSCODE_GOOD;
}

static uint32_t model_channel_alarm_reason(const alarm_condition_status_t *condition)
{
    uint32_t reason = condition->is_discrete ? (condition->discrete_active ? 1u : 0u)
                                             : (uint32_t)condition->process_state;
    reason |= ((uint32_t)condition->quality_fault) << 8;
    return reason;
}

static void model_format_channel_alarm_message(const alarm_condition_status_t *condition,
                                               char *message,
                                               size_t message_size,
                                               UA_UInt16 *out_severity,
                                               UA_StatusCode *out_quality)
{
    const bool process_active = condition->is_discrete ? condition->discrete_active
                                                        : condition->process_state != ALARM_CONDITION_STATE_NORMAL;
    const char *process_text;
    UA_UInt16 severity = 100u;
    UA_StatusCode quality = UA_STATUSCODE_GOOD;

    if (condition->is_discrete)
    {
        process_text = condition->discrete_active ? "Discrete alarm active" : "Discrete alarm inactive";
        if (condition->discrete_active)
        {
            severity = 600u;
        }
    }
    else
    {
        switch (condition->process_state)
        {
        case ALARM_CONDITION_STATE_LOW:
            process_text = "Low alarm active";
            severity = 550u;
            break;
        case ALARM_CONDITION_STATE_HIGH:
            process_text = "High alarm active";
            severity = 650u;
            break;
        case ALARM_CONDITION_STATE_LOW_LOW:
            process_text = "Low-low alarm active";
            severity = 800u;
            break;
        case ALARM_CONDITION_STATE_HIGH_HIGH:
            process_text = "High-high alarm active";
            severity = 850u;
            break;
        case ALARM_CONDITION_STATE_NORMAL:
        default:
            process_text = "Channel alarm inactive";
            break;
        }
    }

    switch (condition->quality_fault)
    {
    case ALARM_QUALITY_FAULT_BAD:
        quality = UA_STATUSCODE_BADSENSORFAILURE;
        severity = severity > 800u ? severity : 800u;
        break;
    case ALARM_QUALITY_FAULT_UNCERTAIN:
        quality = UA_STATUSCODE_UNCERTAIN;
        severity = severity > 500u ? severity : 500u;
        break;
    case ALARM_QUALITY_FAULT_NONE:
    default:
        break;
    }

    if (condition->quality_fault != ALARM_QUALITY_FAULT_NONE && process_active)
    {
        snprintf(message, message_size, "%s (last good-quality state) while channel quality is %s",
                 process_text, condition->quality_fault == ALARM_QUALITY_FAULT_BAD ? "BAD" : "UNCERTAIN");
    }
    else if (condition->quality_fault != ALARM_QUALITY_FAULT_NONE)
    {
        snprintf(message, message_size, "Channel quality is %s (last known state: %s)",
                 condition->quality_fault == ALARM_QUALITY_FAULT_BAD ? "BAD" : "UNCERTAIN", process_text);
    }
    else
    {
        snprintf(message, message_size, "%s", process_text);
    }

    if (out_severity != NULL)
    {
        *out_severity = severity;
    }
    if (out_quality != NULL)
    {
        *out_quality = quality;
    }
}

/* Reflects alarm_service-owned per-channel condition state onto the A&C
 * condition instances built by model_build_channel_alarm_conditions() -- it
 * evaluates no threshold itself, only translates already-evaluated service
 * state into ActiveState/Severity/Quality/Message and standard condition
 * events, exactly like model_process_device_health_conditions() already
 * does for DeviceHealth.  Also reconciles AckedState from the service side
 * (e.g. after AcknowledgeAllAlarms) since a client can also acknowledge
 * through the standard per-condition Method, handled by
 * model_channel_alarm_acked_callback() above. */
void model_process_channel_alarm_conditions(UA_Server *server)
{
    size_t condition_count;

    if (server == NULL || !g_model_alarm_conditions_available)
    {
        return;
    }

    condition_count = alarm_service_get_condition_count();
    for (size_t index = 0u; index < condition_count; ++index)
    {
        alarm_condition_status_t condition = {0};
        model_channel_alarm_slot_t *slot;
        char message[192];
        char condition_node_id_buf[MODEL_CHANNEL_NODE_ID_MAX_LEN];
        char source_node_id_buf[MODEL_CHANNEL_NODE_ID_MAX_LEN];
        UA_UInt16 severity = 100u;
        UA_StatusCode quality = UA_STATUSCODE_GOOD;
        uint32_t reason;
        UA_NodeId condition_node_id;
        UA_NodeId source_node_id;

        if (!alarm_service_get_condition_by_index(index, &condition) || !condition.in_use)
        {
            continue;
        }

        slot = model_find_channel_alarm_slot(condition.channel_id);
        if (slot == NULL)
        {
            /* The channel became alarm-eligible after the one-shot
             * condition build ran (should not happen in v0.4.3, since
             * channels are fixed at startup); skip until a future release
             * adds dynamic condition creation. */
            continue;
        }
        if (!model_channel_alarm_node_ids(condition.channel_id, condition_node_id_buf,
                                          sizeof(condition_node_id_buf), source_node_id_buf,
                                          sizeof(source_node_id_buf)))
        {
            continue;
        }

        reason = model_channel_alarm_reason(&condition);
        model_format_channel_alarm_message(&condition, message, sizeof(message), &severity, &quality);

        condition_node_id = UA_NODEID_STRING(g_model_channel_namespace_index, condition_node_id_buf);
        source_node_id = UA_NODEID_STRING(g_model_channel_namespace_index, source_node_id_buf);

        model_sync_condition(server,
                             condition_node_id,
                             source_node_id,
                             &slot->condition_active,
                             &slot->condition_reason,
                             &slot->condition_enabled,
                             &slot->next_enable_poll,
                             reason,
                             severity,
                             quality,
                             message,
                             "Channel alarm inactive");

        if (condition.acknowledged != slot->last_acknowledged_seen)
        {
            (void)model_condition_set_two_state_id(server, condition_node_id, "AckedState",
                                                   condition.acknowledged ? true : false);
            slot->last_acknowledged_seen = condition.acknowledged;
        }
    }
}

#endif /* UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS */

/**
 * @file
 * @brief Builds the open62541 PubSub/MQTT address-space objects (Connection,
 *        WriterGroups, DataSetWriters, PublishedDataSets) and owns the
 *        adapter-side PubSub runtime state; also hosts the cross-task
 *        pending-audit-event queue (unrelated to PubSub, but the only other
 *        piece of state in this codebase written from a non-`OpcUaTask`
 *        context, hence grouped with the other lock-guarded state here).
 *
 * Responsibilities:
 * - `model_configure_pubsub_publishers()` is the single startup entry point
 *   that resets runtime state, registers the MQTT connection manager,
 *   builds one telemetry Writer chain per enabled channel, and builds the
 *   Events-mirror Writer chain.
 * - Mirrors the most recently emitted/published event into
 *   `g_model_pubsub_event_mirror` for the `Device.Messaging.LastPubSubEvent*`
 *   read-only nodes.
 *
 * Ownership:
 * `g_model_pubsub` / `g_model_pubsub_event_mirror` (both fields of
 * `opcua_model_context_t`) are adapter/model runtime state, not
 * `services/`-owned business state — see `opcua_model_context_t` in
 * `opcua_model_internal.h`.
 *
 * Concurrency:
 * PubSub setup/config functions run once on `OpcUaTask` during startup;
 * `model_pubsub_trigger_event_publish` runs on `OpcUaTask` per poll
 * iteration. `model_queue_pending_audit_event` is the one exception — it may
 * be called from `security_service`'s audit callback on a different task,
 * and is synchronized with `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`.
 */
#include "opcua_model_internal.h"

void model_pubsub_state_change_callback(UA_Server *server,
                                               const UA_NodeId id,
                                               UA_PubSubState state,
                                               UA_StatusCode status)
{
    UA_PubSubComponentType component_type = UA_PUBSUBCOMPONENT_CONNECTION;
    char id_text[64] = {0};
    const char *component_name = "Component";

    if (UA_Server_getPubSubComponentType(server, id, &component_type) == UA_STATUSCODE_GOOD)
    {
        component_name = model_pubsub_component_type_to_string(component_type);
    }

    model_node_id_to_text(&id, id_text, sizeof(id_text));
    printf("[OPC UA]\tPubSub %s %s -> %s (0x%x %s)\n",
           component_name,
           id_text[0] != '\0' ? id_text : "<id>",
           model_pubsub_state_to_string(state),
           status,
           UA_StatusCode_name(status));
    fflush(stdout);
}

UA_StatusCode model_pubsub_register_mqtt_manager(UA_ServerConfig *config)
{
    UA_ConnectionManager *mqtt_manager = NULL;

    if (config == NULL || config->eventLoop == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    mqtt_manager = UA_ConnectionManager_new_MQTT(UA_STRING("mqtt connection manager"));
    if (mqtt_manager == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    return config->eventLoop->registerEventSource(config->eventLoop, (UA_EventSource *)mqtt_manager);
}

void model_pubsub_update_event_mirror(const UA_NodeId *event_type_id,
                                             const char *source_name,
                                             const char *message,
                                             UA_UInt16 severity,
                                             UA_DateTime event_time,
                                             const UA_ByteString *event_id)
{
    char type_text[64] = {0};
    char event_id_hex[33] = {0};

    g_model_pubsub_event_mirror.sequence++;
    g_model_pubsub_event_mirror.time = event_time;
    g_model_pubsub_event_mirror.severity = (UA_UInt32)severity;

    model_node_id_to_text(event_type_id, type_text, sizeof(type_text));
    model_pubsub_extract_tail(type_text,
                              g_model_pubsub_event_mirror.event_type,
                              sizeof(g_model_pubsub_event_mirror.event_type));
    model_copy_string_safe(g_model_pubsub_event_mirror.source,
                           sizeof(g_model_pubsub_event_mirror.source),
                           source_name);
    model_copy_string_safe(g_model_pubsub_event_mirror.message,
                           sizeof(g_model_pubsub_event_mirror.message),
                           message);
    model_event_id_to_hex(event_id, event_id_hex, sizeof(event_id_hex));
    model_copy_string_safe(g_model_pubsub_event_mirror.event_id,
                           sizeof(g_model_pubsub_event_mirror.event_id),
                           event_id_hex);
}

void model_pubsub_trigger_event_publish(UA_Server *server)
{
    UA_StatusCode status;

    if (server == NULL || !g_model_pubsub.configured || UA_NodeId_isNull(&g_model_pubsub.event_writer_group_id))
    {
        return;
    }

    status = UA_Server_triggerWriterGroupPublish(server, g_model_pubsub.event_writer_group_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tPubSub event publish trigger failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }
}

UA_StatusCode model_pubsub_add_variable_field(UA_Server *server,
                                                     const UA_NodeId pds_id,
                                                     const UA_NodeId published_variable,
                                                     const char *alias)
{
    UA_DataSetFieldConfig field_config;
    UA_DataSetFieldResult result;

    memset(&field_config, 0, sizeof(field_config));
    field_config.dataSetFieldType = UA_PUBSUB_DATASETFIELD_VARIABLE;
    field_config.field.variable.fieldNameAlias = UA_STRING((char *)(alias != NULL ? alias : ""));
    field_config.field.variable.publishParameters.publishedVariable = published_variable;
    field_config.field.variable.publishParameters.attributeId = UA_ATTRIBUTEID_VALUE;
    field_config.field.variable.promotedField = false;

    result = UA_Server_addDataSetField(server, pds_id, &field_config, NULL);
    return result.result;
}

UA_StatusCode model_pubsub_add_mqtt_connection(UA_Server *server)
{
    UA_PubSubConnectionConfig connection_config;
    UA_NetworkAddressUrlDataType broker_address;
    UA_KeyValuePair connection_properties[1];
    UA_String mqtt_client_id;

    memset(&connection_config, 0, sizeof(connection_config));
    connection_config.name = UA_STRING((char *)MODEL_PUBSUB_CONNECTION_NAME);
    connection_config.transportProfileUri =
        UA_STRING((char *)(g_model_pubsub.use_json ? MODEL_PUBSUB_MQTT_PROFILE_JSON : MODEL_PUBSUB_MQTT_PROFILE_UADP));
    connection_config.publisherId.idType = UA_PUBLISHERIDTYPE_STRING;
    connection_config.publisherId.id.string = UA_STRING(g_model_pubsub.client_id);

    broker_address.networkInterface = UA_STRING_NULL;
    broker_address.url = UA_STRING(g_model_pubsub.broker_url);
    UA_Variant_setScalar(&connection_config.address,
                         &broker_address,
                         &UA_TYPES[UA_TYPES_NETWORKADDRESSURLDATATYPE]);

    memset(connection_properties, 0, sizeof(connection_properties));
    mqtt_client_id = UA_STRING(g_model_pubsub.client_id);
    connection_properties[0].key = UA_QUALIFIEDNAME(0, (char *)"mqttClientId");
    UA_Variant_setScalar(&connection_properties[0].value,
                         &mqtt_client_id,
                         &UA_TYPES[UA_TYPES_STRING]);
    connection_config.connectionProperties.map = connection_properties;
    connection_config.connectionProperties.mapSize = 1u;

    return UA_Server_addPubSubConnection(server, &connection_config, &g_model_pubsub.connection_id);
}

/* @note The JSON message-settings block is only compiled under
 * `UA_ENABLE_JSON_ENCODING`. If that macro is undefined while
 * `g_model_pubsub.use_json` is true at runtime (JSON was requested by
 * config), `writer_group_config.encodingMimeType`/`messageSettings` are left
 * at their `memset`-zeroed defaults instead of being set to JSON — the build
 * must enable JSON encoding for `mqtt_pubsub_use_json` to actually take
 * effect. */
UA_StatusCode model_pubsub_add_writer_group(UA_Server *server,
                                                   const char *name,
                                                   const char *topic,
                                                   UA_Duration publishing_interval,
                                                   UA_UInt16 writer_group_id,
                                                   UA_NodeId *out_writer_group_id)
{
    UA_WriterGroupConfig writer_group_config;
    UA_BrokerWriterGroupTransportDataType broker_transport;
    UA_ExtensionObject transport_settings;
    UA_ExtensionObject message_settings;
    UA_UadpWriterGroupMessageDataType uadp_message;
#ifdef UA_ENABLE_JSON_ENCODING
    UA_JsonWriterGroupMessageDataType json_message;
#endif
    UA_StatusCode status;

    memset(&writer_group_config, 0, sizeof(writer_group_config));
    writer_group_config.name = UA_STRING((char *)(name != NULL ? name : ""));
    writer_group_config.publishingInterval = publishing_interval;
    writer_group_config.writerGroupId = writer_group_id;

    if (g_model_pubsub.use_json)
    {
#ifdef UA_ENABLE_JSON_ENCODING
        memset(&json_message, 0, sizeof(json_message));
        json_message.networkMessageContentMask =
            (UA_JsonNetworkMessageContentMask)(UA_JSONNETWORKMESSAGECONTENTMASK_NETWORKMESSAGEHEADER |
                                               UA_JSONNETWORKMESSAGECONTENTMASK_DATASETMESSAGEHEADER |
                                               UA_JSONNETWORKMESSAGECONTENTMASK_SINGLEDATASETMESSAGE |
                                               UA_JSONNETWORKMESSAGECONTENTMASK_PUBLISHERID);
        memset(&message_settings, 0, sizeof(message_settings));
        message_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
        message_settings.content.decoded.type = &UA_TYPES[UA_TYPES_JSONWRITERGROUPMESSAGEDATATYPE];
        message_settings.content.decoded.data = &json_message;
        writer_group_config.encodingMimeType = UA_PUBSUB_ENCODING_JSON;
        writer_group_config.messageSettings = message_settings;
#endif
    }
    else
    {
        memset(&uadp_message, 0, sizeof(uadp_message));
        uadp_message.networkMessageContentMask =
            (UA_UadpNetworkMessageContentMask)(UA_UADPNETWORKMESSAGECONTENTMASK_PUBLISHERID |
                                               UA_UADPNETWORKMESSAGECONTENTMASK_GROUPHEADER |
                                               UA_UADPNETWORKMESSAGECONTENTMASK_WRITERGROUPID |
                                               UA_UADPNETWORKMESSAGECONTENTMASK_PAYLOADHEADER);
        memset(&message_settings, 0, sizeof(message_settings));
        message_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
        message_settings.content.decoded.type = &UA_TYPES[UA_TYPES_UADPWRITERGROUPMESSAGEDATATYPE];
        message_settings.content.decoded.data = &uadp_message;
        writer_group_config.encodingMimeType = UA_PUBSUB_ENCODING_UADP;
        writer_group_config.messageSettings = message_settings;
    }

    memset(&broker_transport, 0, sizeof(broker_transport));
    broker_transport.queueName = UA_STRING((char *)(topic != NULL ? topic : ""));
    broker_transport.resourceUri = UA_STRING_NULL;
    broker_transport.authenticationProfileUri = UA_STRING_NULL;
    broker_transport.requestedDeliveryGuarantee = UA_BROKERTRANSPORTQUALITYOFSERVICE_BESTEFFORT;

    memset(&transport_settings, 0, sizeof(transport_settings));
    transport_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
    transport_settings.content.decoded.type = &UA_TYPES[UA_TYPES_BROKERWRITERGROUPTRANSPORTDATATYPE];
    transport_settings.content.decoded.data = &broker_transport;
    writer_group_config.transportSettings = transport_settings;

    status = UA_Server_addWriterGroup(server,
                                      g_model_pubsub.connection_id,
                                      &writer_group_config,
                                      out_writer_group_id);
    return status;
}

/* @note Same `UA_ENABLE_JSON_ENCODING`-gated behavior as
 * model_pubsub_add_writer_group(): if that macro is undefined,
 * `writer_config.messageSettings` is never set for a JSON-requested writer
 * and is left at its `memset`-zeroed default. */
UA_StatusCode model_pubsub_add_data_set_writer(UA_Server *server,
                                                      const UA_NodeId writer_group_id,
                                                      const UA_NodeId pds_id,
                                                      const char *name,
                                                      const char *topic,
                                                      UA_UInt16 writer_id)
{
    UA_DataSetWriterConfig writer_config;
    UA_BrokerDataSetWriterTransportDataType broker_transport;
    UA_ExtensionObject transport_settings;
#ifdef UA_ENABLE_JSON_ENCODING
    UA_JsonDataSetWriterMessageDataType json_message;
    UA_ExtensionObject message_settings;
#endif

    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.name = UA_STRING((char *)(name != NULL ? name : ""));
    writer_config.dataSetWriterId = writer_id;
    writer_config.keyFrameCount = 1u;
    writer_config.dataSetName = UA_STRING((char *)(name != NULL ? name : ""));

    if (g_model_pubsub.use_json)
    {
#ifdef UA_ENABLE_JSON_ENCODING
        memset(&json_message, 0, sizeof(json_message));
        json_message.dataSetMessageContentMask =
            (UA_JsonDataSetMessageContentMask)(UA_JSONDATASETMESSAGECONTENTMASK_DATASETWRITERID |
                                               UA_JSONDATASETMESSAGECONTENTMASK_SEQUENCENUMBER |
                                               UA_JSONDATASETMESSAGECONTENTMASK_STATUS |
                                               UA_JSONDATASETMESSAGECONTENTMASK_TIMESTAMP);
        memset(&message_settings, 0, sizeof(message_settings));
        message_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
        message_settings.content.decoded.type = &UA_TYPES[UA_TYPES_JSONDATASETWRITERMESSAGEDATATYPE];
        message_settings.content.decoded.data = &json_message;
        writer_config.messageSettings = message_settings;
#endif
    }

    memset(&broker_transport, 0, sizeof(broker_transport));
    broker_transport.queueName = UA_STRING((char *)(topic != NULL ? topic : ""));
    broker_transport.resourceUri = UA_STRING_NULL;
    broker_transport.authenticationProfileUri = UA_STRING_NULL;
    broker_transport.metaDataQueueName = UA_STRING_NULL;
    broker_transport.metaDataUpdateTime = 0.0;
    broker_transport.requestedDeliveryGuarantee = UA_BROKERTRANSPORTQUALITYOFSERVICE_BESTEFFORT;

    memset(&transport_settings, 0, sizeof(transport_settings));
    transport_settings.encoding = UA_EXTENSIONOBJECT_DECODED;
    transport_settings.content.decoded.type = &UA_TYPES[UA_TYPES_BROKERDATASETWRITERTRANSPORTDATATYPE];
    transport_settings.content.decoded.data = &broker_transport;
    writer_config.transportSettings = transport_settings;

    return UA_Server_addDataSetWriter(server, writer_group_id, pds_id, &writer_config, NULL);
}

/* @note The four published fields deliberately span two different NodeId
 * naming/namespace schemes: "Value" is addressed via
 * model_build_channel_node_id() (the v0.4.2 generic per-channel NodeId,
 * `Device.Channels.c_<hex id>.Value`, in `g_model_channel_namespace_index`),
 * while "Status"/"SourceTimestamp"/"AlarmState" are addressed via
 * model_build_legacy_channel_node_id() (`Device.Channels.<name>.<field>`,
 * hardcoded to namespace 1) — both resolve per-channel NodeIds, just under
 * different address-space paths/namespaces for historical reasons. */
UA_StatusCode model_pubsub_add_telemetry_writer_for_channel(UA_Server *server,
                                                                   size_t channel_index,
                                                                   const telemetry_channel_t *channel)
{
    char pds_name[64];
    char writer_group_name[64];
    char data_set_writer_name[64];
    char topic[MODEL_PUBSUB_TOPIC_MAX_LEN];
    char value_node_id[128];
    char status_node_id[128];
    char source_timestamp_node_id[128];
    char alarm_state_node_id[128];
    UA_PublishedDataSetConfig pds_config;
    UA_AddPublishedDataSetResult pds_result;
    UA_NodeId pds_id = UA_NODEID_NULL;
    UA_NodeId writer_group_id = UA_NODEID_NULL;
    UA_StatusCode status;
    UA_Duration publishing_interval = 1000.0;

    if (server == NULL || channel == NULL || channel->metadata.name == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!channel->settings.enabled || (channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_ENABLED) == 0u)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (!model_pubsub_build_topic(topic,
                                  sizeof(topic),
                                  g_model_pubsub.topic_root,
                                  MODEL_PUBSUB_TELEMETRY_TOPIC_SUFFIX,
                                  channel->metadata.name))
    {
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    if (!model_build_channel_node_id(channel, "Value", value_node_id, sizeof(value_node_id)) ||
        !model_build_legacy_channel_node_id(channel, "Status", status_node_id, sizeof(status_node_id)) ||
        !model_build_legacy_channel_node_id(channel, "SourceTimestamp", source_timestamp_node_id,
                                            sizeof(source_timestamp_node_id)) ||
        !model_build_legacy_channel_node_id(channel, "AlarmState", alarm_state_node_id,
                                            sizeof(alarm_state_node_id)))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    snprintf(pds_name, sizeof(pds_name), "%s.%s", MODEL_PUBSUB_TELEMETRY_PDS_PREFIX, channel->metadata.name);
    snprintf(writer_group_name, sizeof(writer_group_name), "%s.%s", MODEL_PUBSUB_TELEMETRY_WG_PREFIX, channel->metadata.name);
    snprintf(data_set_writer_name, sizeof(data_set_writer_name), "%s.%s", MODEL_PUBSUB_TELEMETRY_DSW_PREFIX, channel->metadata.name);

    memset(&pds_config, 0, sizeof(pds_config));
    pds_config.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    pds_config.name = UA_STRING(pds_name);
    pds_result = UA_Server_addPublishedDataSet(server, &pds_config, &pds_id);
    if (pds_result.addResult != UA_STATUSCODE_GOOD)
    {
        return pds_result.addResult;
    }

    status = model_pubsub_add_variable_field(server,
                                             pds_id,
                                             UA_NODEID_STRING(g_model_channel_namespace_index, value_node_id),
                                             "Value");
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, status_node_id),
                                                 "Status");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, source_timestamp_node_id),
                                                 "SourceTimestamp");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, alarm_state_node_id),
                                                 "AlarmState");
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (channel->settings.publish_period_ms > 0u)
    {
        publishing_interval = (UA_Duration)channel->settings.publish_period_ms;
    }

    status = model_pubsub_add_writer_group(server,
                                           writer_group_name,
                                           topic,
                                           publishing_interval,
                                           (UA_UInt16)(100u + channel_index),
                                           &writer_group_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_pubsub_add_data_set_writer(server,
                                              writer_group_id,
                                              pds_id,
                                              data_set_writer_name,
                                              topic,
                                              (UA_UInt16)(1000u + channel_index));
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (channel_index < PLATFORM_CHANNEL_LIMIT)
    {
        g_model_pubsub.telemetry_writer_group_ids[channel_index] = writer_group_id;
        g_model_pubsub.telemetry_writer_group_count++;
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_pubsub_add_event_mirror_writer(UA_Server *server)
{
    char topic[MODEL_PUBSUB_TOPIC_MAX_LEN];
    UA_PublishedDataSetConfig pds_config;
    UA_AddPublishedDataSetResult pds_result;
    UA_NodeId pds_id = UA_NODEID_NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    if (!model_pubsub_build_topic(topic,
                                  sizeof(topic),
                                  g_model_pubsub.topic_root,
                                  MODEL_PUBSUB_EVENTS_TOPIC_SUFFIX,
                                  NULL))
    {
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    memset(&pds_config, 0, sizeof(pds_config));
    pds_config.publishedDataSetType = UA_PUBSUB_DATASET_PUBLISHEDITEMS;
    pds_config.name = UA_STRING((char *)MODEL_PUBSUB_EVENTS_PDS_NAME);
    pds_result = UA_Server_addPublishedDataSet(server, &pds_config, &pds_id);
    if (pds_result.addResult != UA_STATUSCODE_GOOD)
    {
        return pds_result.addResult;
    }

    status = model_pubsub_add_variable_field(server,
                                             pds_id,
                                             UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_SEQUENCE),
                                             "Sequence");
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_ID),
                                                 "EventId");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_TYPE),
                                                 "EventType");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_SOURCE),
                                                 "Source");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_MESSAGE),
                                                 "Message");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_SEVERITY),
                                                 "Severity");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_pubsub_add_variable_field(server,
                                                 pds_id,
                                                 UA_NODEID_STRING(1, (char *)MODEL_NODE_MESSAGING_LAST_EVENT_TIME),
                                                 "Time");
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_pubsub_add_writer_group(server,
                                           MODEL_PUBSUB_EVENTS_WG_NAME,
                                           topic,
                                           60000.0,
                                           500u,
                                           &g_model_pubsub.event_writer_group_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return model_pubsub_add_data_set_writer(server,
                                            g_model_pubsub.event_writer_group_id,
                                            pds_id,
                                            MODEL_PUBSUB_EVENTS_DSW_NAME,
                                            topic,
                                            5000u);
}

UA_StatusCode model_configure_pubsub_publishers(UA_Server *server, UA_ServerConfig *config)
{
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    if (server == NULL || config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    model_pubsub_reset_runtime();
    g_model_pubsub.enabled = model_pubsub_requested();
    if (!g_model_pubsub.enabled)
    {
        return UA_STATUSCODE_GOOD;
    }

    g_model_pubsub.use_json = g_model_config.mqtt_pubsub_use_json;
    model_copy_string_safe(g_model_pubsub.broker_url,
                           sizeof(g_model_pubsub.broker_url),
                           g_model_config.mqtt_broker_url);
    model_copy_string_safe(g_model_pubsub.topic_root,
                           sizeof(g_model_pubsub.topic_root),
                           g_model_config.mqtt_topic_root);
    if (!model_pubsub_build_client_id(g_model_pubsub.client_id, sizeof(g_model_pubsub.client_id)))
    {
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    config->pubSubConfig.stateChangeCallback = model_pubsub_state_change_callback;
    config->pubSubConfig.enableDeltaFrames = false;

    status = model_pubsub_add_mqtt_connection(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    for (size_t channel_index = 0u; channel_index < PLATFORM_CHANNEL_LIMIT; ++channel_index)
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;

        if (!channel_manager_copy_snapshot_by_index(channel_index, &snapshot))
        {
            continue;
        }

        status = model_pubsub_add_telemetry_writer_for_channel(server, channel_index, channel);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = model_pubsub_add_event_mirror_writer(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    g_model_pubsub.configured = true;
    printf("[OPC UA]\tPubSub configured: broker=%s, topicRoot=%s, telemetryWriters=%u, events=yes, encoding=%s\n",
           g_model_pubsub.broker_url,
           g_model_pubsub.topic_root,
           (unsigned)g_model_pubsub.telemetry_writer_group_count,
           g_model_pubsub.use_json ? "JSON" : "UADP");
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}

/* Fixed-capacity ring buffer (`MODEL_AUDIT_QUEUE_CAPACITY` slots): when full,
 * the oldest entry is overwritten (head advances, count stays at capacity)
 * rather than rejecting the new one, so a burst of audit events can never
 * block or fail the caller (which may be running on a non-`OpcUaTask`
 * thread) — it can only silently lose the oldest unconsumed entries. */
void model_queue_pending_audit_event(const char *event,
                                            const char *actor,
                                            const char *subject,
                                            bool success,
                                            const char *details)
{
    size_t slot;
    model_pending_audit_event_t *entry = NULL;

    taskENTER_CRITICAL();
    if (g_pending_audit_event_count == MODEL_AUDIT_QUEUE_CAPACITY)
    {
        g_pending_audit_event_head = (g_pending_audit_event_head + 1u) % MODEL_AUDIT_QUEUE_CAPACITY;
        --g_pending_audit_event_count;
    }

    slot = (g_pending_audit_event_head + g_pending_audit_event_count) % MODEL_AUDIT_QUEUE_CAPACITY;
    entry = &g_pending_audit_events[slot];
    memset(entry, 0, sizeof(*entry));
    model_copy_string_safe(entry->event, sizeof(entry->event), event);
    model_copy_string_safe(entry->actor, sizeof(entry->actor), actor);
    model_copy_string_safe(entry->subject, sizeof(entry->subject), subject);
    model_copy_string_safe(entry->details, sizeof(entry->details), details);
    entry->success = success;
    ++g_pending_audit_event_count;
    taskEXIT_CRITICAL();
}

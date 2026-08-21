#include "opcua_model_internal.h"

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
static const uint8_t g_bootstrap_device_certificate[] = TEST_SRV_CRT_RSA_SHA256_DER;
static const uint8_t g_bootstrap_device_private_key[] = TEST_SRV_KEY_RSA_PEM;
#endif

bool model_core_init(const opcua_model_config_t *config)
{
#ifdef UA_ENABLE_HISTORIZING
    bool maintenance_export_payload_allocated = false;
#endif

    if (config == NULL)
    {
        return false;
    }

    if (g_model_maintenance_export_payload == NULL)
    {
        if (!psram_allocator_is_ready())
        {
            printf("[OPC UA]\tMaintenance export scratch requires PSRAM\n");
            fflush(stdout);
            return false;
        }

        g_model_maintenance_export_payload =
            (char *)psram_malloc(MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY);
        if (g_model_maintenance_export_payload == NULL)
        {
            printf("[OPC UA]\tMaintenance export scratch allocation failed (%u bytes)\n",
                   (unsigned)MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY);
            fflush(stdout);
            return false;
        }

#ifdef UA_ENABLE_HISTORIZING
        maintenance_export_payload_allocated = true;
#endif
    }

#ifdef UA_ENABLE_HISTORIZING
    if (g_model_history_read_sample_buffer == NULL)
    {
        /* Bounded to the frozen per-call HistoryRead cap (v0.4.3), not the
         * per-channel retention capacity: a single HistoryReadRaw call never
         * returns more than HISTORY_SERVICE_MAX_RETURN_COUNT samples for one
         * node (see model_history.c), so this scratch buffer only needs to
         * hold that many, with the rest of a channel's retained window paged
         * through bounded continuation points. */
        g_model_history_read_sample_buffer = (history_service_channel_sample_t *)psram_malloc(
            HISTORY_SERVICE_MAX_RETURN_COUNT * sizeof(*g_model_history_read_sample_buffer));
        if (g_model_history_read_sample_buffer == NULL)
        {
            if (maintenance_export_payload_allocated)
            {
                psram_free(g_model_maintenance_export_payload);
                g_model_maintenance_export_payload = NULL;
            }

            return false;
        }
    }
#endif

    g_model_config = *config;
    g_pending_actions.reboot_pending = false;
    g_pending_actions.reboot_due_tick = 0;
    g_pending_actions.identify_blink_active = false;
    g_pending_actions.identify_next_tick = 0;
    g_pending_actions.identify_toggles_remaining = 0u;
    g_pending_actions.identify_led_on = false;
#ifdef UA_ENABLE_PUBSUB
    model_pubsub_reset_runtime();
#endif
    g_model_initialized = true;
    return true;
}

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
bool model_certificate_view_is_pem(const security_service_blob_view_t *certificate)
{
    static const char pem_prefix[] = "-----BEGIN CERTIFICATE-----";

    return certificate != NULL &&
           certificate->data != NULL &&
           certificate->length >= (sizeof(pem_prefix) - 1u) &&
           memcmp(certificate->data, pem_prefix, sizeof(pem_prefix) - 1u) == 0;
}

void model_clear_runtime_security_config(UA_ServerConfig *config)
{
    if (config == NULL)
    {
        return;
    }

    if (config->accessControl.clear != NULL)
    {
        config->accessControl.clear(&config->accessControl);
        memset(&config->accessControl, 0, sizeof(config->accessControl));
    }

    if (config->secureChannelPKI.clear != NULL)
    {
        config->secureChannelPKI.clear(&config->secureChannelPKI);
        memset(&config->secureChannelPKI, 0, sizeof(config->secureChannelPKI));
    }

    if (config->sessionPKI.clear != NULL)
    {
        config->sessionPKI.clear(&config->sessionPKI);
        memset(&config->sessionPKI, 0, sizeof(config->sessionPKI));
    }

    for (size_t i = 0u; i < config->endpointsSize; ++i)
    {
        UA_EndpointDescription_clear(&config->endpoints[i]);
    }
    UA_free(config->endpoints);
    config->endpoints = NULL;
    config->endpointsSize = 0u;

    for (size_t i = 0u; i < config->securityPoliciesSize; ++i)
    {
        config->securityPolicies[i].clear(&config->securityPolicies[i]);
    }
    UA_free(config->securityPolicies);
    config->securityPolicies = NULL;
    config->securityPoliciesSize = 0u;

}

void model_clear_server_urls(UA_ServerConfig *config)
{
    if (config == NULL)
    {
        return;
    }

    for (size_t i = 0u; i < config->serverUrlsSize; ++i)
    {
        UA_String_clear(&config->serverUrls[i]);
    }
    UA_free(config->serverUrls);
    config->serverUrls = NULL;
    config->serverUrlsSize = 0u;
}

UA_StatusCode model_configure_server_url(UA_ServerConfig *config,
                                                const char *hostname,
                                                uint16_t port)
{
    char endpoint_url[160];
    UA_String url;
    UA_StatusCode status;

    if (config == NULL || hostname == NULL || hostname[0] == '\0')
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    snprintf(endpoint_url, sizeof(endpoint_url), "opc.tcp://%s:%u/", hostname, (unsigned)port);
    url = UA_STRING(endpoint_url);

    model_clear_server_urls(config);
    config->serverUrls = (UA_String *)UA_Array_new(1u, &UA_TYPES[UA_TYPES_STRING]);
    if (config->serverUrls == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    status = UA_String_copy(&url, &config->serverUrls[0]);
    if (status != UA_STATUSCODE_GOOD)
    {
        model_clear_server_urls(config);
        return status;
    }

    config->serverUrlsSize = 1u;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_configure_application_uri(UA_ServerConfig *config)
{
    const device_profile_config_t *device_config = config_service_get();
    const char *configured_uri = MODEL_BOOTSTRAP_APPLICATION_URI;
    UA_String application_uri;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (device_config != NULL && device_config->product_instance_uri[0] != '\0')
    {
        configured_uri = device_config->product_instance_uri;
    }

    application_uri = UA_STRING((char *)configured_uri);

    UA_String_clear(&config->applicationDescription.applicationUri);
    return UA_String_copy(&application_uri, &config->applicationDescription.applicationUri);
}

UA_StatusCode model_seed_bootstrap_identity_if_needed(void)
{
    security_service_blob_view_t existing_certificate = {0};

    if (security_service_has_device_identity())
    {
        if (security_service_get_device_certificate(&existing_certificate) &&
            model_certificate_view_is_pem(&existing_certificate))
        {
            if (!security_service_set_device_identity(g_bootstrap_device_certificate,
                                                      sizeof(g_bootstrap_device_certificate),
                                                      g_bootstrap_device_private_key,
                                                      sizeof(g_bootstrap_device_private_key) - 1u))
            {
                return UA_STATUSCODE_BADINTERNALERROR;
            }

            printf("[OPC UA]\tMigrated legacy PEM bootstrap certificate to DER\n");
            fflush(stdout);
        }

        return UA_STATUSCODE_GOOD;
    }

    if (!security_service_seed_device_identity_if_missing(g_bootstrap_device_certificate,
                                                          sizeof(g_bootstrap_device_certificate),
                                                          g_bootstrap_device_private_key,
                                                          sizeof(g_bootstrap_device_private_key) - 1u))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    printf("[OPC UA]\tSeeded bootstrap device certificate into security store\n");
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_configure_secure_access_control(UA_ServerConfig *config)
{
    UA_UsernamePasswordLogin placeholder_login;
    UA_StatusCode status;
    const UA_String *username_policy_uri = &UA_SECURITY_POLICY_NONE_URI;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    placeholder_login.username = UA_STRING((char *)"bootstrap");
    placeholder_login.password = UA_STRING((char *)"bootstrap");

    status = UA_AccessControl_defaultWithLoginCallback(config,
                                                       true,
                                                       username_policy_uri,
                                                       1u,
                                                       &placeholder_login,
                                                       model_login_callback,
                                                       NULL);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    g_default_activate_session = config->accessControl.activateSession;
    g_default_close_session = config->accessControl.closeSession;
    g_default_get_user_executable = config->accessControl.getUserExecutable;
    g_default_get_user_executable_on_object = config->accessControl.getUserExecutableOnObject;
    g_default_get_user_access_level = config->accessControl.getUserAccessLevel;
    g_default_allow_browse_node = config->accessControl.allowBrowseNode;

    config->accessControl.activateSession = model_activate_session_secure_wrapper;
    config->accessControl.closeSession = model_close_session_secure_wrapper;
    config->accessControl.getUserExecutable = model_get_user_executable_wrapper;
    config->accessControl.getUserExecutableOnObject = model_get_user_executable_on_object_wrapper;
    config->accessControl.getUserAccessLevel = model_get_user_access_level_wrapper;
    config->accessControl.allowBrowseNode = model_allow_browse_node_wrapper;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_keep_only_none_and_signandencrypt_endpoints(UA_ServerConfig *config)
{
    UA_EndpointDescription *filtered = NULL;
    size_t keep_count = 0u;
    size_t out_index = 0u;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        const UA_EndpointDescription *endpoint = &config->endpoints[i];
        if (endpoint->securityMode == UA_MESSAGESECURITYMODE_NONE ||
            endpoint->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            ++keep_count;
        }
    }

    if (keep_count == 0u || keep_count == config->endpointsSize)
    {
        return UA_STATUSCODE_GOOD;
    }

    filtered = (UA_EndpointDescription *)UA_Array_new(keep_count,
                                                      &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
    if (filtered == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        const UA_EndpointDescription *endpoint = &config->endpoints[i];
        UA_StatusCode status;

        if (endpoint->securityMode != UA_MESSAGESECURITYMODE_NONE &&
            endpoint->securityMode != UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            continue;
        }

        status = UA_EndpointDescription_copy(endpoint, &filtered[out_index]);
        if (status != UA_STATUSCODE_GOOD)
        {
            UA_Array_delete(filtered, keep_count, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
            return status;
        }
        ++out_index;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        UA_EndpointDescription_clear(&config->endpoints[i]);
    }
    UA_free(config->endpoints);
    config->endpoints = filtered;
    config->endpointsSize = keep_count;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_keep_only_signandencrypt_endpoints(UA_ServerConfig *config)
{
    UA_EndpointDescription *filtered = NULL;
    size_t keep_count = 0u;
    size_t out_index = 0u;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        if (config->endpoints[i].securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            ++keep_count;
        }
    }

    if (keep_count == 0u || keep_count == config->endpointsSize)
    {
        return UA_STATUSCODE_GOOD;
    }

    filtered = (UA_EndpointDescription *)UA_Array_new(keep_count,
                                                      &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
    if (filtered == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        UA_StatusCode status;

        if (config->endpoints[i].securityMode != UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        {
            continue;
        }

        status = UA_EndpointDescription_copy(&config->endpoints[i], &filtered[out_index]);
        if (status != UA_STATUSCODE_GOOD)
        {
            UA_Array_delete(filtered, keep_count, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
            return status;
        }
        ++out_index;
    }

    for (size_t i = 0; i < config->endpointsSize; ++i)
    {
        UA_EndpointDescription_clear(&config->endpoints[i]);
    }
    UA_free(config->endpoints);
    config->endpoints = filtered;
    config->endpointsSize = keep_count;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_limit_insecure_endpoints_to_anonymous(UA_ServerConfig *config)
{
    size_t endpoint_index;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    for (endpoint_index = 0; endpoint_index < config->endpointsSize; ++endpoint_index)
    {
        UA_EndpointDescription *endpoint = &config->endpoints[endpoint_index];
        size_t token_index;
        UA_UserTokenPolicy *anonymous_only = NULL;
        UA_StatusCode status;

        if (endpoint->securityMode != UA_MESSAGESECURITYMODE_NONE)
        {
            continue;
        }

        for (token_index = 0; token_index < endpoint->userIdentityTokensSize; ++token_index)
        {
            if (endpoint->userIdentityTokens[token_index].tokenType == UA_USERTOKENTYPE_ANONYMOUS)
            {
                break;
            }
        }

        if (token_index >= endpoint->userIdentityTokensSize || endpoint->userIdentityTokensSize == 1u)
        {
            continue;
        }

        anonymous_only = (UA_UserTokenPolicy *)UA_Array_new(1u, &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
        if (anonymous_only == NULL)
        {
            return UA_STATUSCODE_BADOUTOFMEMORY;
        }

        status = UA_UserTokenPolicy_copy(&endpoint->userIdentityTokens[token_index], &anonymous_only[0]);
        if (status != UA_STATUSCODE_GOOD)
        {
            UA_Array_delete(anonymous_only, 1u, &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
            return status;
        }

        UA_Array_delete(endpoint->userIdentityTokens,
                        endpoint->userIdentityTokensSize,
                        &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
        endpoint->userIdentityTokens = anonymous_only;
        endpoint->userIdentityTokensSize = 1u;

        /* Keep the compatibility discovery endpoint strictly "None":
           some clients are sensitive to a non-empty serverCertificate
           on SecurityPolicy#None / MessageSecurityMode=None. */
        UA_ByteString_clear(&endpoint->serverCertificate);
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_configure_secure_server(UA_ServerConfig *config)
{
    security_service_blob_view_t certificate = {0};
    security_service_blob_view_t private_key = {0};
    security_service_blob_view_t trust_list = {0};
    const bool commissioning_mode = security_service_is_commissioning_mode();
    const bool have_trust_list = security_service_get_trust_list(&trust_list);
    UA_ByteString local_certificate = UA_BYTESTRING_NULL;
    UA_ByteString local_private_key = UA_BYTESTRING_NULL;
    UA_NodeId default_application_group =
        UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTAPPLICATIONGROUP);
    UA_NodeId default_user_token_group =
        UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTUSERTOKENGROUP);
    UA_StatusCode status;

    if (config == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    status = model_seed_bootstrap_identity_if_needed();
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (!security_service_get_device_certificate(&certificate) ||
        !security_service_get_device_private_key(&private_key))
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    local_certificate.data = (UA_Byte *)certificate.data;
    local_certificate.length = certificate.length;
    local_private_key.data = (UA_Byte *)private_key.data;
    local_private_key.length = private_key.length;

    model_clear_runtime_security_config(config);

    if (!commissioning_mode && !have_trust_list)
    {
        printf("[OPC UA]\tStrict security mode requires a provisioned trust list\n");
        fflush(stdout);
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    if (!commissioning_mode && have_trust_list)
    {
        status = security_certificate_group_service_configure(&config->secureChannelPKI,
                                                              &default_application_group,
                                                              config->logging);
        if (status == UA_STATUSCODE_GOOD)
        {
            status = security_certificate_group_service_configure(&config->sessionPKI,
                                                                  &default_user_token_group,
                                                                  config->logging);
        }

        if (status == UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tStrict mode enabled: using flash-backed trust list backend\n");
            fflush(stdout);
        }
    }
    else
    {
        UA_CertificateGroup_AcceptAll(&config->secureChannelPKI);
        UA_CertificateGroup_AcceptAll(&config->sessionPKI);
        status = UA_STATUSCODE_GOOD;
        printf("[OPC UA]\tCommissioning mode enabled: using AcceptAll client certificate verification\n");
        fflush(stdout);
    }

    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = UA_ServerConfig_addSecurityPolicyBasic256Sha256(config,
                                                             &local_certificate,
                                                             &local_private_key);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_configure_secure_access_control(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (commissioning_mode)
    {
        status = UA_ServerConfig_addSecurityPolicyNone(config, &local_certificate);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    status = UA_ServerConfig_addAllEndpoints(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    if (commissioning_mode)
    {
        status = model_keep_only_none_and_signandencrypt_endpoints(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        status = model_limit_insecure_endpoints_to_anonymous(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }
    else
    {
        status = model_keep_only_signandencrypt_endpoints(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }
    }

    printf("[OPC UA]\tSecurity mode: %s\n",
           commissioning_mode ? "Commissioning" : "Strict");
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}
#endif

void model_bind_open62541_allocators(void)
{
#ifdef UA_ENABLE_MALLOC_SINGLETON
    UA_mallocSingleton = ua_port_malloc;
    UA_callocSingleton = ua_port_calloc;
    UA_reallocSingleton = ua_port_realloc;
    UA_freeSingleton = ua_port_free;
    ua_port_alloc_log_stats("open62541 allocator bound");
#endif
}

void model_core_task(void *argument)
{
    UA_Server *server = NULL;
    UA_ServerConfig *config = NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;
    bool wait_for_network_logged = false;
    TickType_t next_psram_stats_tick = 0;
    model_event_runtime_state_t event_runtime_state = {0};
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
    model_bind_open62541_allocators();

    UA_ServerConfig server_config;
    memset(&server_config, 0, sizeof(server_config));
    config = &server_config;
    config->logging = &g_model_ua_logger;

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
        vTaskDelete(NULL);
    }

    log_opcua_heap("Network layer configured");

    status = model_configure_application_uri(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tFailed to configure ApplicationUri: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_ServerConfig_clear(config);
        vTaskDelete(NULL);
    }

    /* PSRAM profile: keep connection counts bounded, but allow a realistic
       number of monitored items now that open62541 long-lived allocations live
       outside the internal SRAM heap. */
    config->maxSecureChannels = 4;
    config->maxSessions = 4;
    config->maxSubscriptions = 4;
    config->maxSubscriptionsPerSession = 4;
    config->maxMonitoredItems = 128;
    config->maxMonitoredItemsPerSubscription = 64;
    config->maxMonitoredItemsPerCall = 32;
    config->maxNotificationsPerPublish = 32;
    config->enableRetransmissionQueue = false;
    config->queueSizeLimits.min = 1;
    config->queueSizeLimits.max = 8;
    config->maxPublishReqPerSession = 4;
    config->monitoredItemRegisterCallback = model_monitored_item_register_callback;
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    config->maxEventsPerNode = 8;
#endif
#ifdef UA_ENABLE_PUBSUB
    config->pubsubEnabled = model_pubsub_requested();
    config->pubSubConfig.enableDeltaFrames = false;
    if (config->pubsubEnabled)
    {
        config->pubSubConfig.stateChangeCallback = model_pubsub_state_change_callback;
        status = model_pubsub_register_mqtt_manager(config);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to register MQTT connection manager: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            UA_ServerConfig_clear(config);
            vTaskDelete(NULL);
        }
    }
#endif

    if (g_model_config.hostname != NULL && g_model_config.hostname[0] != '\0')
    {
        network_manager_status_t network_status = {0};
        const char *hostname_text = g_model_config.hostname;

        network_manager_get_status(&network_status);
        if (network_status.has_ipv4 && network_status.ipv4_address[0] != '\0')
        {
            hostname_text = network_status.ipv4_address;
        }

        status = model_configure_server_url(config, hostname_text, g_model_config.port);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to configure endpoint URL: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            UA_ServerConfig_clear(config);
            vTaskDelete(NULL);
        }

        printf("[OPC UA]\tUsing endpoint host %s\n", hostname_text);
        fflush(stdout);
    }

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
    status = model_configure_secure_server(config);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tSecure server configuration failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        UA_ServerConfig_clear(config);
        vTaskDelete(NULL);
    }
    log_opcua_heap("Secure policies and access control configured");
#else
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
        UA_ServerConfig_clear(config);
        vTaskDelete(NULL);
    }

    log_opcua_heap("Access control configured");
#endif

#ifdef UA_ENABLE_HISTORIZING
    if (config->historyDatabase.clear != NULL)
    {
        config->historyDatabase.clear(&config->historyDatabase);
    }
    memset(&config->historyDatabase, 0, sizeof(config->historyDatabase));
    config->historyDatabase.context = &g_history_backend_context;
    config->historyDatabase.clear = model_history_database_clear;
    config->historyDatabase.readRaw = model_history_read_raw;
    config->accessHistoryDataCapability = true;
    /* Advertise the actual enforced per-call cap (see model_history.c),
     * not the larger per-channel retention capacity: a client that reads
     * this capability and sets numValuesPerNode accordingly should never be
     * surprised by a continuation point on the first call. */
    config->maxReturnDataValues = HISTORY_SERVICE_MAX_RETURN_COUNT;
    log_opcua_heap("Historizing configured");
#endif

    printf("[OPC UA]\tCreating server with configured namespace\n");
    fflush(stdout);
    server = UA_Server_newWithConfig(config);
    if (server == NULL)
    {
        printf("[OPC UA]\tUA_Server_newWithConfig returned NULL\n");
        fflush(stdout);
        vTaskDelete(NULL);
    }

    log_opcua_heap("UA_Server_newWithConfig completed");
    ua_port_alloc_log_stats("UA_Server_newWithConfig completed");
    config = UA_Server_getConfig(server);
    model_context_set_server_handles(server, config);

    if (maintenance_service_init() != MAINTENANCE_SERVICE_RESULT_OK)
    {
        printf("[OPC UA]\tMaintenance service initialization failed\n");
        fflush(stdout);
        model_context_delete_server(server);
        vTaskDelete(NULL);
    }

    status = model_build_address_space(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tFailed to build address space: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        model_context_delete_server(server);
        vTaskDelete(NULL);
    }

    {
        const device_profile_config_t *running_config = config_service_get();

        if (running_config != NULL)
        {
            g_model_address_space_config_snapshot = *running_config;
            g_model_address_space_config_snapshot_valid = true;
        }
    }

    log_opcua_heap("Address space ready");
    ua_port_alloc_log_stats("Address space ready");

#ifdef UA_ENABLE_PUBSUB
    if (config->pubsubEnabled)
    {
        status = model_configure_pubsub_publishers(server, config);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to configure PubSub publishers: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            model_context_delete_server(server);
            vTaskDelete(NULL);
        }

        log_opcua_heap("PubSub configured");
    }
#endif

    printf("[OPC UA]\tStarting server on port %u\n", (unsigned)g_model_config.port);
    fflush(stdout);

    status = UA_Server_run_startup(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_Server_run_startup failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
        model_context_delete_server(server);
        vTaskDelete(NULL);
    }

    model_finalize_identity_generation_after_startup();

    log_opcua_heap("Server startup complete");
    ua_port_alloc_log_stats("Server startup complete");
    ua_port_alloc_log_stats("Runtime allocation mode");
    next_psram_stats_tick = xTaskGetTickCount() + pdMS_TO_TICKS(30000u);

#ifdef UA_ENABLE_PUBSUB
    if (g_model_pubsub.configured)
    {
        status = UA_Server_enableAllPubSubComponents(server);
        if (status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tFailed to enable PubSub components: 0x%x (%s)\n",
                   status,
                   UA_StatusCode_name(status));
            fflush(stdout);
            model_context_delete_server(server);
            vTaskDelete(NULL);
        }

        log_opcua_heap("PubSub enabled");
        ua_port_alloc_log_stats("PubSub enabled");
    }
#endif

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    security_service_set_audit_callback(model_security_audit_callback, NULL);
    model_event_runtime_state_init(&event_runtime_state);
    {
        char startup_message[128];
        const device_profile_config_t *device_config = config_service_get();

        snprintf(startup_message,
                 sizeof(startup_message),
                 "Firmware startup complete (%s)",
                 device_config != NULL ? device_config->fw_revision : "unknown");
        model_emit_firmware_event(server, startup_message, 200u);
    }
#endif

    while (1)
    {
        if (g_pending_actions.reboot_pending &&
            xTaskGetTickCount() >= g_pending_actions.reboot_due_tick)
        {
            break;
        }

        if (g_pending_actions.identify_blink_active &&
            xTaskGetTickCount() >= g_pending_actions.identify_next_tick)
        {
            g_pending_actions.identify_led_on = !g_pending_actions.identify_led_on;
            led_driver_set(g_pending_actions.identify_led_on);

            if (g_pending_actions.identify_toggles_remaining > 0u)
            {
                g_pending_actions.identify_toggles_remaining--;
            }

            if (g_pending_actions.identify_toggles_remaining == 0u)
            {
                g_pending_actions.identify_blink_active = false;
                g_pending_actions.identify_led_on = false;
                led_driver_set(false);
            }
            else
            {
                g_pending_actions.identify_next_tick = xTaskGetTickCount() + pdMS_TO_TICKS(200u);
            }
        }

        model_process_runtime_events(server, &event_runtime_state);

        if (xTaskGetTickCount() >= next_psram_stats_tick)
        {
            log_opcua_heap("Periodic runtime");
            ua_port_alloc_log_stats("Periodic runtime");
            next_psram_stats_tick = xTaskGetTickCount() + pdMS_TO_TICKS(30000u);
        }

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

    if (g_pending_actions.reboot_pending)
    {
        printf("[OPC UA]\tExecuting scheduled reboot\n");
        fflush(stdout);
        diag_service_reboot(10u);
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    status = UA_Server_run_shutdown(server);
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_Server_run_shutdown failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }

    model_context_delete_server(server);

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    security_service_set_audit_callback(NULL, NULL);
#endif

    if (g_model_maintenance_export_payload != NULL)
    {
        psram_free(g_model_maintenance_export_payload);
        g_model_maintenance_export_payload = NULL;
    }

#ifdef UA_ENABLE_HISTORIZING
    if (g_model_history_read_sample_buffer != NULL)
    {
        psram_free(g_model_history_read_sample_buffer);
        g_model_history_read_sample_buffer = NULL;
    }
#endif

    vTaskDelete(NULL);
}

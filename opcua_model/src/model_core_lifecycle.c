/**
 * @file
 * @brief Server lifecycle: `opcua_model_init`/`opcua_model_task` entry points,
 *        `UA_ServerConfig` construction (transport, security, historizing),
 *        and the `OpcUaTask` run loop (`model_core_task`).
 *
 * Responsibilities:
 * - `model_core_init` performs one-time PSRAM scratch allocation (bounded
 *   maintenance-export payload and HistoryRead sample buffer), seeds
 *   `opcua_model_context_t`'s config/pending-actions/PubSub-runtime fields,
 *   and registers `model_config_service_restart_callback` with
 *   `config_service` (v0.5.0 T6).
 * - `model_core_task` is the `OpcUaTask` entry point: waits for network
 *   readiness once, then loops building the `UA_ServerConfig` (buffers,
 *   security policies, access control, historizing), calling
 *   `UA_Server_newWithConfig`, building the address space, starting PubSub
 *   if enabled, and running the `UA_Server_run_iterate` poll loop -- until
 *   either a scheduled reboot fires (MCU reset, task never returns) or a
 *   controlled restart is armed (v0.5.0 T6: the server is torn down and the
 *   same loop body rebuilds it against the newly-committed configuration,
 *   without exiting the task).
 * - The `#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS` block builds the secure
 *   `UA_ServerConfig`: bootstrap-identity seeding, trust-list/AcceptAll PKI
 *   selection by commissioning mode, and endpoint filtering.
 *
 * Controlled restart (v0.5.0 T6): `config_service_run_commit_extensions()`
 * unconditionally invokes the registered restart callback after every
 * successful configuration commit; `model_config_service_restart_callback`
 * (below) compares the current address-space snapshot with the committed
 * RUNNING configuration and, if they differ in an address-space-relevant
 * field, arms
 * `g_pending_actions.restart_pending` rather than tearing the server down
 * inline -- see that function's doc comment for why inline teardown is
 * unsafe here. `model_core_task`'s run loop notices the flag the moment its
 * current `UA_Server_run_iterate()` call returns (before any further client
 * request can be served), gracefully shuts the old server down, and loops
 * back to rebuild -- reusing exactly the same construction code cold boot
 * uses, never a second implementation of it. Existing sessions/subscriptions
 * do not survive a restart (same reconnect contract as a full firmware
 * reboot, deliberately -- see docs/releases/v0.5.0.md scope decision 6);
 * clients must reconnect, after which they see the new configuration's
 * address space. A transaction that does not change anything
 * `model_config_requires_server_reboot()` cares about never arms the flag,
 * so the running server, its sessions, and its subscriptions are completely
 * untouched.
 *
 * Ownership: `model_core_init`/`model_core_task` are the only functions that
 * allocate/free `g_model_maintenance_export_payload` and
 * `g_model_history_read_sample_buffer` (PSRAM); every other field this file
 * touches lives in the single `opcua_model_context_t` (`g_model_config`,
 * `g_pending_actions`, `g_default_*` saved access-control callbacks).
 *
 * Concurrency: `model_core_init` runs on whichever task calls
 * `opcua_model_init` (see `app/basic_profile/main.c`) before `OpcUaTask` is
 * created -- except its one re-entrant call from within `model_core_task`
 * itself to rebuild `g_model_config` before a restart, which runs on
 * `OpcUaTask`, between two server lifetimes, with no other task touching
 * model state at that point. Everything else in this file — the entire
 * `UA_ServerConfig` build sequence and the run loop — executes exclusively
 * on `OpcUaTask`. This ordering is safety-critical: `model_core_task` must
 * observe `g_model_initialized` before touching any model state, and every
 * `UA_ServerConfig` step that fails calls `vTaskDelete(NULL)` rather than
 * continuing with a partially-configured server -- including on a
 * restart-triggered rebuild, where a construction failure is treated
 * identically to a cold-boot one (logged, then the task exits) rather than
 * left to hang or silently retry; see `model_core_task`'s restart handling.
 */
#include "opcua_model_internal.h"

#include <timers.h>

/**
 * @brief Fallback deadline for `UA_Server_run_shutdown()` during a
 *        controlled restart (v0.5.0 T6).
 *
 * Root cause (found on hardware 2026-08-31): open62541's
 * `UA_BinaryProtocolManager` network callback accepts and fully activates
 * new incoming connections on the listening socket even after the
 * component has been told to stop (no check of its own STOPPING state
 * before creating a new SecureChannel), while `UA_Server_run_shutdown()`'s
 * drain loop waits for the SecureChannel list to become empty. Any client
 * that reconnects during the drain window -- completely normal client
 * behavior, and exactly what happens if the original client (or any
 * monitoring/SCADA client) retries after the expected disconnect --
 * keeps that list non-empty forever, so the call never returns. This is
 * upstream open62541 behavior, present in both v1.5.3 and v1.5.7; not a
 * version-specific regression.
 *
 * `UA_Server_run_shutdown()` is a single blocking call on OpcUaTask, which
 * cannot time itself out of its own blocking call. `model_shutdown_deadline_timer_cb()`
 * runs on the independent Timer Service Task instead: armed for
 * `MODEL_SHUTDOWN_TIMEOUT_MS` right before the call, disarmed right after
 * it returns. If it fires, the call is presumed stuck in the livelock
 * above and `diag_service_reboot()` (the same hardware-watchdog-based hard
 * reset already used for `g_pending_actions.reboot_pending`) is used as a
 * fallback -- it works via `watchdog_reboot()`'s countdown in silicon, so
 * it fires regardless of what OpcUaTask is doing.
 */
#define MODEL_SHUTDOWN_TIMEOUT_MS 8000u

static void model_shutdown_deadline_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    printf("[OPC UA]\tUA_Server_run_shutdown did not complete within %u ms "
           "(a reconnecting client likely kept a SecureChannel open through "
           "the shutdown drain) -- falling back to a hard reboot\n",
           (unsigned)MODEL_SHUTDOWN_TIMEOUT_MS);
    fflush(stdout);
    diag_service_reboot(10u);
}

/** @brief Lazily-created, reused across every controlled restart. */
static TimerHandle_t model_get_shutdown_deadline_timer(void)
{
    static TimerHandle_t timer = NULL;
    if (timer == NULL)
    {
        timer = xTimerCreate("RestartDeadline",
                             pdMS_TO_TICKS(MODEL_SHUTDOWN_TIMEOUT_MS),
                             pdFALSE, NULL, model_shutdown_deadline_timer_cb);
        if (timer == NULL)
        {
            printf("[OPC UA]\tFailed to create restart-deadline timer; "
                   "a stuck UA_Server_run_shutdown() will hang indefinitely\n");
            fflush(stdout);
        }
    }
    return timer;
}

#ifdef UA_ENABLE_ENCRYPTION_MBEDTLS
/**
 * @brief Compiled-in, non-secret placeholder TLS server identity
 *        (`TEST_SRV_CRT_RSA_SHA256_DER`/`TEST_SRV_KEY_RSA_PEM`, from
 *        `test_certs.h`), used only by `model_seed_bootstrap_identity_if_needed`
 *        to give a freshly-flashed device *some* identity to start a secure
 *        server with before a real one is provisioned.
 * @note Same key material on every device running this firmware image; not
 *       a production-quality identity. Never used once
 *       `security_service_has_device_identity()` reports a real,
 *       non-bootstrap identity.
 */
static const uint8_t g_bootstrap_device_certificate[] = TEST_SRV_CRT_RSA_SHA256_DER;
/** @brief Private key half of `g_bootstrap_device_certificate`. Same
 *         non-production caveat applies. */
static const uint8_t g_bootstrap_device_private_key[] = TEST_SRV_KEY_RSA_PEM;
#endif

/**
 * @brief `config_service_restart_fn` registered with `config_service` (v0.5.0
 * T6): decides, via the address-space snapshot and the service-owned RUNNING
 * configuration, whether the active OPC UA namespace needs rebuilding, and
 * if so arms a deferred, in-place server restart.
 *
 * Cannot rebuild `UA_Server` synchronously from here. This function runs
 * inside `config_service_apply()`/`config_service_replace_and_save()`,
 * which — for every caller in this codebase today
 * (`model_apply_config`/`model_apply_imported_config` in
 * `model_configuration.c`, the replace-and-save path in `model_ota.c`,
 * `config_reset_service_execute` reached from `model_factory_reset`) —
 * executes from an OPC UA method callback: already nested inside the very
 * `UA_Server_run_iterate()` call that owns the live `server` pointer's call
 * stack (`model_core_task`'s run loop is the only thing that calls
 * `UA_Server_run_iterate`, on `OpcUaTask`). Deleting/rebuilding `UA_Server`
 * from here would free memory that outer call frame is still using once
 * this callback returns (e.g. `model_apply_config` itself dereferences
 * `server` again immediately after `config_service_apply()` returns, via
 * `model_refresh_amb_locations`). Instead this arms
 * `g_pending_actions.restart_pending`, mirroring the existing
 * `reboot_pending` deferral pattern (`model_schedule_reboot`);
 * `model_core_task`'s run loop notices it the moment the current
 * `UA_Server_run_iterate()` call returns — before any further client
 * request can be served — and performs the actual teardown/rebuild there.
 * Single-tasked execution is what keeps this equivalent, from a
 * client-observable standpoint, to doing it inline: no request can be
 * served against the old namespace in the gap.
 *
 * @param new_config The just-committed configuration.
 * @param previous_config The configuration @p new_config replaced.
 */
static void model_config_service_restart_callback(const device_profile_config_t *new_config,
                                                   const device_profile_config_t *previous_config)
{
    /* The callback parameters are useful to config_service's generic contract,
     * but the authoritative lifecycle comparison is the snapshot that built
     * the live namespace against the now-committed RUNNING configuration. This
     * also recovers deterministically if a prior restart notification was
     * missed and the snapshot was already stale before this commit. */
    bool requires_restart;
    (void)new_config;
    (void)previous_config;

    requires_restart = model_address_space_snapshot_requires_server_restart();
    if (!requires_restart)
    {
        return;
    }

    g_pending_actions.restart_pending = true;
}

/* Implementation note: the two PSRAM scratch buffers are allocated
 * independently and idempotently (skipped if already non-NULL, so repeated
 * init calls are safe), but only the history sample buffer's failure path
 * rolls back the maintenance-export payload allocation made earlier in the
 * same call — a partial-success state (payload allocated, history buffer
 * not) must never be left in the context on a false return. Also called a
 * second time, re-entrantly from model_core_task on OpcUaTask (v0.5.0 T6),
 * to reset pending-actions/PubSub-runtime state before a controlled
 * restart's rebuild -- both PSRAM allocations are no-ops by then. */
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
    g_pending_actions.restart_pending = false;
    g_pending_actions.identify_blink_active = false;
    g_pending_actions.identify_next_tick = 0;
    g_pending_actions.identify_toggles_remaining = 0u;
    g_pending_actions.identify_led_on = false;
#ifdef UA_ENABLE_PUBSUB
    model_pubsub_reset_runtime();
#endif
    g_model_initialized = true;
    /* Idempotent: re-registering the same pointer on every call (including
     * the T6 restart re-init below) is a harmless no-op. */
    config_service_set_restart_callback(model_config_service_restart_callback);
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

    /* No silent downgrade: if the device is already out of commissioning
     * mode but has no trust list, startup fails hard here rather than
     * falling back to commissioning/AcceptAll on its own. Leaving strict
     * mode requires an explicit, audited model_enter_commissioning_mode
     * call (model_security.c) — see docs/code/modules/opcua_model.md#access-control. */
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

/* Implementation note: this is the FreeRTOS `OpcUaTask` entry function
 * (installed via xTaskCreate in app/basic_profile/main.c, not in this file).
 * Every fatal step below follows the same fail-fast pattern: log the
 * open62541 UA_StatusCode, release whatever has been allocated so far
 * (UA_ServerConfig_clear(config) before UA_Server_newWithConfig succeeds,
 * model_context_delete_server(server) after), then vTaskDelete(NULL) — there
 * is no retry or degraded-mode path once server construction has begun. This
 * applies identically whether construction is cold boot's or a v0.5.0 T6
 * restart's rebuild -- see the `for (;;)` loop below: it is the same code
 * either way, so a rebuild failure is handled the same way a cold-boot
 * failure always has been (logged, task exits) rather than left undefined. */
void model_core_task(void *argument)
{
    UA_Server *server = NULL;
    UA_ServerConfig *config = NULL;
    UA_StatusCode status = UA_STATUSCODE_GOOD;
    bool wait_for_network_logged = false;
    /* v0.5.0 T6: true from the second and every later pass through the
     * `for (;;)` loop below, i.e. every controlled-restart rebuild; false
     * only for the initial cold-boot pass. */
    bool is_restart = false;
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

    /* v0.5.0 T6: everything from here through the run loop below is one
     * "server lifetime". Cold boot runs it once; a controlled restart
     * (g_pending_actions.restart_pending, armed by
     * model_config_service_restart_callback) loops back and runs the exact
     * same construction code again against the newly-committed
     * configuration, rather than a second, parallel implementation of it. */
    for (;;)
    {
    UA_ServerConfig server_config;
    memset(&server_config, 0, sizeof(server_config));
    config = &server_config;
    config->logging = &g_model_ua_logger;

    if (is_restart)
    {
        /* Rebuild g_model_config from the configuration that was just
         * committed -- app_init_task()'s one-time opcua_model_init() call
         * only ever reflected whatever was running at cold boot, so
         * hostname/port/PubSub adapter settings must be re-derived here or
         * a restart triggered by one of those fields changing would rebuild
         * the server with stale values. Reusing model_core_init() (rather
         * than poking g_model_config directly) also resets PubSub runtime
         * state and pending-actions, matching a real cold boot exactly. */
        const device_profile_config_t *restarted_profile = config_service_get();

        if (restarted_profile != NULL)
        {
            opcua_model_config_t restarted_config = {0};
            /* model_core_init() unconditionally zeroes every field of
             * g_pending_actions, including reboot_pending/reboot_due_tick --
             * correct for a cold boot, but this call is re-entrant during a
             * controlled restart (v0.5.0 T6) and a hard reboot can
             * legitimately be scheduled (model_schedule_reboot()) around the
             * same time a restart is armed (see the due-tick re-check below
             * this loop). Save and restore across the call so a real
             * scheduled reboot survives a restart's rebuild instead of being
             * silently cancelled. */
            const bool saved_reboot_pending = g_pending_actions.reboot_pending;
            const TickType_t saved_reboot_due_tick = g_pending_actions.reboot_due_tick;

            restarted_config.application_name = restarted_profile->device_name;
            restarted_config.hostname = restarted_profile->opcua_hostname;
            restarted_config.mqtt_broker_url = restarted_profile->mqtt_broker_url;
            restarted_config.mqtt_topic_root = restarted_profile->mqtt_topic_root;
            restarted_config.port = restarted_profile->opcua_port;
            restarted_config.enable_alarm_methods = restarted_profile->enable_alarm_ack_method;
            restarted_config.enable_heap_diagnostics = restarted_profile->enable_heap_diagnostics;
            restarted_config.enable_task_diagnostics = restarted_profile->enable_task_diagnostics;
            restarted_config.enable_mqtt_pubsub = restarted_profile->enable_mqtt_pubsub;
            restarted_config.mqtt_pubsub_use_json = restarted_profile->mqtt_pubsub_use_json;
            (void)model_core_init(&restarted_config);

            g_pending_actions.reboot_pending = saved_reboot_pending;
            g_pending_actions.reboot_due_tick = saved_reboot_due_tick;
        }
    }

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

    /* A configuration export can own almost 12 KiB until its CallResponse is
     * encoded. Bound the full CallRequest, not only each individual method,
     * so batched calls cannot multiply transient PSRAM without limit. */
    config->maxNodesPerMethodCall = OPCUA_MAX_METHODS_PER_CALL;

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

    /* Boot-only: maintenance_service owns its own independent Flash journal
     * with its own recovery scan (drivers/maintenance_journal_backend.c),
     * unrelated to the OPC UA namespace this loop rebuilds -- re-running its
     * boot-time recovery mid-uptime on every restart would be both wrong and
     * out of scope for a namespace-only rebuild. */
    if (!is_restart && maintenance_service_init() != MAINTENANCE_SERVICE_RESULT_OK)
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

        /* v0.5.0 T6: intentionally the same message/event on a restart's
         * rebuild as on cold boot (not a distinct string) -- this keeps
         * opcua_model's reviewed public-inventory string-literal set
         * (static/tools/verify_opcua_public_contract.py) byte-for-byte
         * unchanged; the printf lines a few lines below (already
         * unconditional, already inside this per-lifetime block) log
         * "Creating server..."/"Starting server..." on every pass, restart
         * included, which is enough operator-visible signal. */
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

        if (g_pending_actions.restart_pending)
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

        /* Fail closed if a committed RUNNING configuration has diverged from
         * the snapshot that built this server. The normal commit callback arms
         * restart_pending while nested in UA_Server_run_iterate(); this check
         * also covers a missed or late callback and runs before another client
         * request can be served against the stale namespace. Staged-only data
         * is deliberately ignored: it has not become the running model. */
        {
            const bool fail_closed = model_address_space_snapshot_requires_server_restart();
            if (fail_closed)
            {
                g_pending_actions.restart_pending = true;
                break;
            }
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

    if (g_pending_actions.reboot_pending &&
        xTaskGetTickCount() >= g_pending_actions.reboot_due_tick)
    {
        /* Re-check the due-tick here, not just `reboot_pending` -- the loop
         * above can also break on `restart_pending` (v0.5.0 T6) while a
         * reboot is scheduled but not yet due (e.g. model_factory_reset()
         * arms both: config_service_apply()'s restart callback fires
         * synchronously, then model_schedule_reboot() gives the stack a
         * short grace period to flush the method's own response before the
         * MCU resets). Without this check, that early, unrelated break would
         * be misread as "the reboot is due" and fire the hard reset
         * immediately, cutting the grace period short and dropping the
         * response. Falling through instead lets the controlled-restart path
         * below run first; reboot_pending/reboot_due_tick are preserved
         * across it (see the is_restart branch above) and this check fires
         * for real on a later pass once the tick is actually reached. */

        /* Hard MCU reset: the whole address space, PSRAM scratch, and this
         * task itself disappear with it, so there is nothing to gracefully
         * shut down first -- unlike the controlled-restart path below, which
         * keeps the firmware running and must tear the server down cleanly
         * before rebuilding. */
        printf("[OPC UA]\tExecuting scheduled reboot\n");
        fflush(stdout);
        diag_service_reboot(10u);
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Reached for a controlled restart (v0.5.0 T6) -- existing
     * sessions/subscriptions are lost here and clients must reconnect; once
     * the loop below rebuilds, a reconnecting client sees the
     * newly-committed configuration's address space. Not reached for a
     * normal exit today (the only two break conditions above are
     * reboot_pending, handled above, and restart_pending, handled below). */
    {
        /* See model_shutdown_deadline_timer_cb()'s doc comment: a hard
         * fallback for UA_Server_run_shutdown()'s open62541-internal
         * reconnect livelock. */
        TimerHandle_t shutdown_deadline = model_get_shutdown_deadline_timer();
        if (shutdown_deadline != NULL)
        {
            xTimerStart(shutdown_deadline, 0);
        }

        status = UA_Server_run_shutdown(server);

        if (shutdown_deadline != NULL)
        {
            xTimerStop(shutdown_deadline, 0);
        }
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tUA_Server_run_shutdown failed: 0x%x (%s)\n",
               status,
               UA_StatusCode_name(status));
        fflush(stdout);
    }

    model_context_delete_server(server);
    server = NULL;

#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    security_service_set_audit_callback(NULL, NULL);
#endif

    if (g_pending_actions.restart_pending)
    {
        /* v0.5.0 T6: no new printf string here -- see the startup-message
         * comment above the run loop for why: this keeps opcua_model's
         * reviewed public-inventory string-literal set byte-for-byte
         * unchanged. The "Creating server..."/"Starting server..." lines
         * below, already unconditional and already inside this per-lifetime
         * block, print again on this pass and are the operator-visible
         * signal that a rebuild happened. */
        g_pending_actions.restart_pending = false;
        is_restart = true;
        continue;
    }

    break;
    }

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

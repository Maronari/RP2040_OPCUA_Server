/**
 * @file
 * @brief `Device.Maintenance.Security.*` method callbacks and JSON-status
 *        formatters bridging `services/security_service`,
 *        `services/security_identity_import_service`,
 *        `services/security_trust_import_service`,
 *        `services/security_state_service`, and
 *        `services/security_certificate_group_service` into open62541.
 *
 * Responsibilities:
 * - Device-identity rotation (certificate/private key) staged chunked
 *   upload: Begin/Write/Abort/ValidateStaged/ApplyStaged, delegating to
 *   `security_identity_import_service`.
 * - Trust-list staged chunked import (Begin/Write/Abort/Validate/Apply,
 *   delegating to `security_trust_import_service`) plus one-shot export
 *   and state/metadata JSON reporting.
 * - `model_refresh_runtime_trust_backends()`: re-points the live
 *   `UA_ServerConfig` PKI stores at whatever
 *   `security_certificate_group_service` currently manages, called after
 *   any trust-list mutation so it takes effect without a reboot.
 *
 * Ownership:
 * No certificate/key/trust-list bytes are held here; each service owns its
 * own staging buffer. This file only validates method arguments, formats
 * JSON status reports, and translates service result codes to
 * `UA_StatusCode`/boolean method outputs — see the Begin/Write/Abort/
 * Validate/Apply pattern documented per-family in `opcua_model_internal.h`.
 *
 * Concurrency:
 * All callbacks run synchronously on `OpcUaTask` inside
 * `UA_Server_run_iterate`.
 *
 * @note This adapter never invents certificate/crypto semantics: expiry
 *       classification, key matching, and trust validation are entirely
 *       `security_*` service responsibilities; this file only formats their
 *       results.
 */
#include "opcua_model_internal.h"

/* MaxNodesPerMethodCall is one (see `OPCUA_MAX_METHODS_PER_CALL`), so the
 * shared `g_model_maintenance_export_payload` scratch buffer written here is
 * guaranteed to be fully encoded into the response before another method
 * callback (on this or any other method) can reuse it; there is no
 * synchronization on the buffer itself because of that single-call
 * guarantee, not because it is otherwise safe to share across calls. */
UA_StatusCode model_export_config(UA_Server *server,
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

    if (payload == NULL)
    {
        return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    }
    /* MaxNodesPerMethodCall is one, so the server encodes the persistent
     * response view before another method callback can reuse this scratch. */
    if (!config_export_service_write_running_json(payload,
                                                  MODEL_MAINTENANCE_EXPORT_PAYLOAD_CAPACITY))
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

UA_StatusCode model_refresh_runtime_trust_backends(UA_Server *server)
{
    UA_ServerConfig *config;
    UA_StatusCode refresh_status = UA_STATUSCODE_GOOD;

    if (server == NULL)
    {
        return UA_STATUSCODE_GOOD;
    }

    config = UA_Server_getConfig(server);
    if (config == NULL)
    {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    if (security_certificate_group_service_manages(&config->secureChannelPKI))
    {
        refresh_status = security_certificate_group_service_refresh(&config->secureChannelPKI);
    }

    if (refresh_status == UA_STATUSCODE_GOOD &&
        security_certificate_group_service_manages(&config->sessionPKI))
    {
        refresh_status = security_certificate_group_service_refresh(&config->sessionPKI);
    }

    return refresh_status;
}

void model_finalize_identity_generation_after_startup(void)
{
    security_state_service_snapshot_t snapshot = {0};
    security_service_blob_view_t certificate = {0};
    security_service_blob_view_t private_key = {0};
    bool have_identity = false;
    uint32_t next_active_generation = 0u;
    uint32_t next_pending_generation = 0u;

    if (!security_state_service_get_snapshot(&snapshot))
    {
        return;
    }

    have_identity = security_service_get_device_certificate(&certificate) &&
                    security_service_get_device_private_key(&private_key) &&
                    certificate.length > 0u &&
                    private_key.length > 0u;

    next_active_generation = snapshot.active_identity_generation;
    next_pending_generation = snapshot.pending_identity_generation;

    if (snapshot.pending_identity_generation > snapshot.active_identity_generation)
    {
        next_active_generation = snapshot.pending_identity_generation;
        next_pending_generation = 0u;
    }
    else if (snapshot.active_identity_generation == 0u &&
             snapshot.pending_identity_generation == 0u &&
             have_identity)
    {
        next_active_generation = 1u;
    }

    if (next_active_generation == snapshot.active_identity_generation &&
        next_pending_generation == snapshot.pending_identity_generation)
    {
        return;
    }

    if (security_state_service_set_identity_generations(next_active_generation,
                                                        next_pending_generation))
    {
        printf("[OPC UA]\tIdentity generation active=%lu pending=%lu\n",
               (unsigned long)next_active_generation,
               (unsigned long)next_pending_generation);
        fflush(stdout);
    }
}

void model_write_identity_import_report_json(char *buffer,
                                                    size_t buffer_size,
                                                    const security_identity_import_service_report_t *report)
{
    if (buffer == NULL || buffer_size == 0u || report == NULL)
    {
        return;
    }

    snprintf(buffer,
             buffer_size,
             "{\"sessionActive\":%s,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"valid\":%s,\"timeCheckPerformed\":%s,\"rebootRequired\":%s,\"payloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"certificateSizeBytes\":%lu,\"privateKeySizeBytes\":%lu,\"notBeforeEpochS\":%lu,\"notAfterEpochS\":%lu,\"result\":\"%s\"}",
             report->session_active ? "true" : "false",
             report->payload_complete ? "true" : "false",
             report->payload_crc32_matches ? "true" : "false",
             report->valid ? "true" : "false",
             report->time_check_performed ? "true" : "false",
             report->reboot_required ? "true" : "false",
             (unsigned long)report->payload_size,
             (unsigned long)report->expected_payload_crc32,
             (unsigned long)report->schema_version,
             (unsigned long)report->certificate_size,
             (unsigned long)report->private_key_size,
             (unsigned long)report->not_before_epoch_s,
             (unsigned long)report->not_after_epoch_s,
             security_identity_import_service_result_to_string(report->result));
}

void model_write_certificate_metadata_json(char *buffer, size_t buffer_size)
{
    security_service_blob_view_t certificate = {0};
    security_service_blob_view_t private_key = {0};
    security_state_service_snapshot_t snapshot = {0};
    char thumbprint[SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u] = {0};
    const device_profile_config_t *config = config_service_get();
    const bool have_identity =
        security_service_get_device_certificate(&certificate) &&
        security_service_get_device_private_key(&private_key) &&
        certificate.length > 0u &&
        private_key.length > 0u;
    uint32_t not_before_epoch_s = 0u;
    uint32_t not_after_epoch_s = 0u;
    uint32_t days_until_expiry = 0u;
    bool time_check_performed = false;
    bool expiry_known = false;
    const model_certificate_expiry_state_t expiry_state =
        model_get_certificate_expiry_state(&days_until_expiry, &time_check_performed);

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    if (have_identity)
    {
        (void)security_service_get_device_certificate_thumbprint(thumbprint, sizeof(thumbprint));
        (void)security_service_get_device_certificate_not_before(&not_before_epoch_s);
        if (security_service_get_device_certificate_not_after(&not_after_epoch_s))
        {
            expiry_known = true;
        }
    }

    (void)security_state_service_get_snapshot(&snapshot);
    snprintf(buffer,
             buffer_size,
             "{\"haveDeviceIdentity\":%s,\"certificateSizeBytes\":%lu,\"privateKeySizeBytes\":%lu,\"applicationUri\":\"%s\",\"thumbprint\":\"%s\",\"notBeforeEpochS\":%lu,\"notAfterEpochS\":%lu,\"expiryKnown\":%s,\"timeCheckPerformed\":%s,\"daysUntilExpiry\":%lu,\"expiryState\":\"%s\",\"commissioningState\":\"%s\",\"activeIdentityGeneration\":%lu,\"pendingIdentityGeneration\":%lu,\"activeTrustGeneration\":%lu,\"pendingTrustGeneration\":%lu,\"rebootRequired\":%s}",
             have_identity ? "true" : "false",
             (unsigned long)certificate.length,
             (unsigned long)private_key.length,
             (config != NULL) ? config->product_instance_uri : "",
             thumbprint,
             (unsigned long)not_before_epoch_s,
             (unsigned long)not_after_epoch_s,
             expiry_known ? "true" : "false",
             time_check_performed ? "true" : "false",
             (unsigned long)days_until_expiry,
             model_certificate_expiry_state_to_string(expiry_state),
             model_security_commissioning_state_to_string(),
             (unsigned long)snapshot.active_identity_generation,
             (unsigned long)snapshot.pending_identity_generation,
             (unsigned long)snapshot.active_trust_generation,
             (unsigned long)snapshot.pending_trust_generation,
             snapshot.pending_identity_generation > snapshot.active_identity_generation ? "true" : "false");
}

void model_write_certificate_state_json(char *buffer, size_t buffer_size)
{
    security_identity_import_service_status_t import_status = {0};
    security_identity_import_service_report_t report = {0};
    char metadata[768] = {0};
    const char *validation_result = "NONE";
    bool validation_valid = false;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    model_write_certificate_metadata_json(metadata, sizeof(metadata));
    security_identity_import_service_get_status(&import_status);
    if (import_status.session_active)
    {
        (void)security_identity_import_service_validate_staged(&report);
        validation_result = security_identity_import_service_result_to_string(report.result);
        validation_valid = report.valid;
    }

    snprintf(buffer,
             buffer_size,
             "{\"metadata\":%s,\"maxChunkBytes\":%u,\"import\":{\"sessionActive\":%s,\"expectedPayloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"bytesWritten\":%lu,\"remainingBytes\":%lu,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"lastResult\":\"%s\"},\"stagedValidation\":{\"available\":%s,\"valid\":%s,\"certificateSizeBytes\":%lu,\"privateKeySizeBytes\":%lu,\"notBeforeEpochS\":%lu,\"notAfterEpochS\":%lu,\"timeCheckPerformed\":%s,\"result\":\"%s\"}}",
             metadata,
             (unsigned)SECURITY_IDENTITY_IMPORT_SERVICE_MAX_CHUNK_BYTES,
             import_status.session_active ? "true" : "false",
             (unsigned long)import_status.expected_payload_size,
             (unsigned long)import_status.expected_payload_crc32,
             (unsigned long)import_status.schema_version,
             (unsigned long)import_status.bytes_written,
             (unsigned long)import_status.remaining_bytes,
             import_status.payload_complete ? "true" : "false",
             import_status.payload_crc32_matches ? "true" : "false",
             security_identity_import_service_result_to_string(import_status.last_result),
             import_status.session_active ? "true" : "false",
             validation_valid ? "true" : "false",
             (unsigned long)report.certificate_size,
             (unsigned long)report.private_key_size,
             (unsigned long)report.not_before_epoch_s,
             (unsigned long)report.not_after_epoch_s,
             report.time_check_performed ? "true" : "false",
             validation_result);
}

UA_StatusCode model_get_certificate_state(UA_Server *server,
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
    UA_StatusCode status_code;
    char payload[1536] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    model_write_certificate_state_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_export_certificate_metadata(UA_Server *server,
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
    UA_StatusCode status_code;
    char payload[896] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    model_write_certificate_metadata_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_begin_identity_rotation(UA_Server *server,
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
    UA_Boolean result = false;
    UA_UInt32 payload_size = 0u;
    UA_UInt32 payload_crc32 = 0u;
    UA_UInt32 schema_version = 0u;
    security_identity_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (input_size != 3u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_UINT32]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_UINT32]) || input[2].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    payload_size = *(const UA_UInt32 *)input[0].data;
    payload_crc32 = *(const UA_UInt32 *)input[1].data;
    schema_version = *(const UA_UInt32 *)input[2].data;

    service_result = security_identity_import_service_begin((uint32_t)payload_size,
                                                            (uint32_t)payload_crc32,
                                                            (uint32_t)schema_version);
    result = (service_result == SECURITY_IDENTITY_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginIdentityRotation %s size=%lu crc32=0x%08lx schema=%lu\n",
           result == UA_TRUE ? "accepted" : security_identity_import_service_result_to_string(service_result),
           (unsigned long)payload_size,
           (unsigned long)payload_crc32,
           (unsigned long)schema_version);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_write_identity_rotation_chunk(UA_Server *server,
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
    const UA_ByteString *chunk = NULL;
    UA_Boolean result = false;
    security_identity_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = security_identity_import_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == SECURITY_IDENTITY_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteIdentityRotationChunk failed: %s (size=%lu)\n",
               security_identity_import_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_abort_identity_rotation(UA_Server *server,
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
    UA_Boolean result = false;
    security_identity_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    service_result = security_identity_import_service_abort();
    result = (service_result == SECURITY_IDENTITY_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortIdentityRotation %s\n",
           result == UA_TRUE ? "succeeded" : security_identity_import_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_validate_staged_identity(UA_Server *server,
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
    security_identity_import_service_report_t report = {0};
    char payload[512] = {0};
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    (void)security_identity_import_service_validate_staged(&report);
    model_write_identity_import_report_json(payload, sizeof(payload), &report);

    printf("[OPC UA]\tValidateStagedIdentity %s\n",
           report.valid ? "OK" : security_identity_import_service_result_to_string(report.result));
    fflush(stdout);
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

/* @note Unlike model_enter_commissioning_mode()/model_exit_commissioning_mode()
 * in `model_ota.c`, a successful apply here does not itself call
 * model_schedule_reboot() even though the new identity only takes effect
 * after a reboot ("reboot required" is reported in the audit/event text and
 * left for the operator/client to act on). */
UA_StatusCode model_apply_staged_identity(UA_Server *server,
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
    UA_Boolean result = false;
    security_identity_import_service_report_t report = {0};
    const char *details = "staged identity validation failed";
    (void)server;
    (void)session_id;
    (void)method_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    if (model_require_admin_session(session_context, output_size, output) != UA_STATUSCODE_GOOD)
    {
        return UA_STATUSCODE_GOOD;
    }

    if (security_identity_import_service_apply_staged(&report) == SECURITY_IDENTITY_IMPORT_RESULT_OK)
    {
        result = true;
        details = "staged identity persisted; reboot required";
    }
    else
    {
        details = security_identity_import_service_result_to_string(report.result);
    }

    if (result == UA_TRUE)
    {
        printf("[OPC UA]\tApplyStagedIdentity succeeded; reboot required\n");
    }
    else
    {
        printf("[OPC UA]\tApplyStagedIdentity failed: %s\n", details);
    }
    fflush(stdout);
    model_audit_session_event(session_context,
                              "identity-rotation-apply",
                              "device-identity",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result == UA_TRUE ? "Staged identity applied; reboot required"
                                                       : "Staged identity apply failed",
                                     result == UA_TRUE ? 300u : 700u,
                                     "IdentityRotation",
                                     result == UA_TRUE ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

void model_write_trust_import_report_json(char *buffer,
                                                 size_t buffer_size,
                                                 const security_trust_import_service_report_t *report)
{
    if (buffer == NULL || buffer_size == 0u || report == NULL)
    {
        return;
    }

    snprintf(buffer,
             buffer_size,
             "{\"sessionActive\":%s,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"valid\":%s,\"payloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"certificateCount\":%lu,\"result\":\"%s\"}",
             report->session_active ? "true" : "false",
             report->payload_complete ? "true" : "false",
             report->payload_crc32_matches ? "true" : "false",
             report->valid ? "true" : "false",
             (unsigned long)report->payload_size,
             (unsigned long)report->expected_payload_crc32,
             (unsigned long)report->schema_version,
             (unsigned long)report->certificate_count,
             security_trust_import_service_result_to_string(report->result));
}

/* @pre @p buffer must already hold a complete JSON object (its last
 * non-NUL character must be '}'); otherwise this is a silent no-op.
 * @note Splices the new field in by overwriting the trailing '}' and
 * re-appending it after the new field via `snprintf`; if the composed
 * result would not fit in @p buffer_size, @p buffer is NUL-terminated at
 * its last byte (truncated) rather than overflowed. */
void model_append_json_object_field(char *buffer,
                                           size_t buffer_size,
                                           const char *field_name,
                                           const char *object_json)
{
    size_t length;
    const char quote = '"';
    int written;

    if (buffer == NULL || buffer_size == 0u || field_name == NULL || field_name[0] == '\0' ||
        object_json == NULL || object_json[0] == '\0')
    {
        return;
    }

    length = strlen(buffer);
    if (length == 0u || buffer[length - 1u] != '}')
    {
        return;
    }

    buffer[length - 1u] = '\0';
    written = snprintf(buffer + length - 1u,
                       buffer_size - (length - 1u),
                       ",%c%s%c:%s}",
                       quote,
                       field_name,
                       quote,
                       object_json);
    if (written < 0 || (size_t)written >= (buffer_size - (length - 1u)))
    {
        buffer[buffer_size - 1u] = '\0';
    }
}

void model_write_trust_state_json(char *buffer, size_t buffer_size)
{
    security_service_blob_view_t trust_list = {0};
    security_trust_import_service_status_t import_status = {0};
    security_trust_import_service_report_t report = {0};
    security_state_service_snapshot_t snapshot = {0};
    char expiry_summary[SECURITY_TRUST_EXPIRY_SERVICE_SUMMARY_MAX_LEN] = "{}";
    uint32_t trust_list_version = 0u;
    const bool have_trust_list = security_service_get_trust_list(&trust_list);
    const uint32_t trusted_client_count = security_service_get_trusted_client_count();
    const char *validation_result = "NONE";
    bool validation_valid = false;
    uint32_t validation_cert_count = 0u;

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    security_trust_import_service_get_status(&import_status);
    if (import_status.session_active)
    {
        (void)security_trust_import_service_validate_staged(&report);
        validation_result = security_trust_import_service_result_to_string(report.result);
        validation_valid = report.valid;
        validation_cert_count = report.certificate_count;
    }

    (void)security_service_get_trust_list_version(&trust_list_version);
    (void)security_state_service_get_snapshot(&snapshot);
    (void)security_trust_expiry_service_get_summary_json(expiry_summary, sizeof(expiry_summary));
    snprintf(buffer,
             buffer_size,
             "{\"commissioningMode\":%s,\"commissioningState\":\"%s\",\"haveTrustList\":%s,\"trustListSizeBytes\":%lu,\"trustListVersion\":%lu,\"activeTrustGeneration\":%lu,\"pendingTrustGeneration\":%lu,\"trustedClientCount\":%lu,\"maxChunkBytes\":%u,\"import\":{\"sessionActive\":%s,\"expectedPayloadSizeBytes\":%lu,\"payloadCrc32\":%lu,\"schemaVersion\":%lu,\"bytesWritten\":%lu,\"remainingBytes\":%lu,\"payloadComplete\":%s,\"payloadCrc32Matches\":%s,\"lastResult\":\"%s\"},\"stagedValidation\":{\"available\":%s,\"valid\":%s,\"certificateCount\":%lu,\"result\":\"%s\"}}",
             snapshot.commissioning_mode ? "true" : "false",
             model_security_commissioning_state_to_string(),
             have_trust_list ? "true" : "false",
             (unsigned long)trust_list.length,
             (unsigned long)trust_list_version,
             (unsigned long)snapshot.active_trust_generation,
             (unsigned long)snapshot.pending_trust_generation,
             (unsigned long)trusted_client_count,
             (unsigned)SECURITY_TRUST_IMPORT_SERVICE_MAX_CHUNK_BYTES,
             import_status.session_active ? "true" : "false",
             (unsigned long)import_status.expected_payload_size,
             (unsigned long)import_status.expected_payload_crc32,
             (unsigned long)import_status.schema_version,
             (unsigned long)import_status.bytes_written,
             (unsigned long)import_status.remaining_bytes,
             import_status.payload_complete ? "true" : "false",
             import_status.payload_crc32_matches ? "true" : "false",
             security_trust_import_service_result_to_string(import_status.last_result),
             import_status.session_active ? "true" : "false",
             validation_valid ? "true" : "false",
             (unsigned long)validation_cert_count,
             validation_result);
    model_append_json_object_field(buffer, buffer_size, "expiry", expiry_summary);
}

UA_StatusCode model_get_trust_state(UA_Server *server,
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
    UA_StatusCode status_code;
    char payload[1408] = {0};
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    model_write_trust_state_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

/* @note `payload` aliases `security_service`'s own trust-list buffer
 * (const-cast away) rather than copying it; this is safe only because
 * model_set_bytestring_method_output() deep-copies the bytes
 * (`UA_Variant_setScalarCopy`) synchronously before returning, so the alias
 * never outlives this call. */
UA_StatusCode model_export_trust_list(UA_Server *server,
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
    UA_StatusCode status_code;
    security_service_blob_view_t trust_list = {0};
    UA_ByteString payload = UA_BYTESTRING_NULL;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, 0u, NULL);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (security_service_get_trust_list(&trust_list) && trust_list.length > 0u)
    {
        payload.data = (UA_Byte *)(uintptr_t)trust_list.data;
        payload.length = trust_list.length;
    }

    model_set_bytestring_method_output(output_size, output, &payload);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_begin_trust_list_import(UA_Server *server,
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
    UA_StatusCode status_code;
    UA_Boolean result = false;
    UA_UInt32 payload_size = 0u;
    UA_UInt32 payload_crc32 = 0u;
    UA_UInt32 schema_version = 0u;
    security_trust_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (input_size != 3u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_UINT32]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_UINT32]) || input[2].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    payload_size = *(const UA_UInt32 *)input[0].data;
    payload_crc32 = *(const UA_UInt32 *)input[1].data;
    schema_version = *(const UA_UInt32 *)input[2].data;

    service_result = security_trust_import_service_begin((uint32_t)payload_size,
                                                         (uint32_t)payload_crc32,
                                                         (uint32_t)schema_version);
    result = (service_result == SECURITY_TRUST_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginTrustListImport %s size=%lu crc32=0x%08lx schema=%lu\n",
           result == UA_TRUE ? "accepted" : security_trust_import_service_result_to_string(service_result),
           (unsigned long)payload_size,
           (unsigned long)payload_crc32,
           (unsigned long)schema_version);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

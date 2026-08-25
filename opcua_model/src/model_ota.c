/**
 * @file
 * @brief `Device.Maintenance*` method callbacks for the staged
 *        chunked-upload workflows (trust list, full configuration, firmware/
 *        OTA image, device identity is in `model_security.c`) plus a handful
 *        of unrelated maintenance methods (channel config, self-test,
 *        diagnostics, one-shot trust-list set, commissioning mode, user
 *        management). Despite the file name, firmware-update (OTA) methods
 *        are a minority of this file; see `docs/code/modules/opcua_model.md`
 *        for the naming caveat.
 *
 * Responsibilities:
 * - Bridges `services/firmware_update_service` (backed by `ota/`/
 *   `drivers/flash_operation_coordinator`), `services/config_import_service`,
 *   `services/security_trust_import_service`, and `services/config_service`
 *   into open62541 `UA_MethodCallback`s.
 * - Every chunked-upload family (trust-list import, config import,
 *   firmware update) follows the same Begin/Write*Chunk/Abort/Validate/
 *   Apply-or-Finalize state machine, fully described per-family in
 *   `opcua_model_internal.h`.
 *
 * Ownership:
 * No Flash/session state is owned here — each service owns its own staging
 * buffer and state machine; these callbacks only validate method arguments,
 * translate service result codes to `UA_StatusCode`/boolean method outputs,
 * and emit audit/event records.
 *
 * Concurrency:
 * All callbacks run synchronously on `OpcUaTask` inside
 * `UA_Server_run_iterate`; Flash writes performed by the underlying
 * services happen synchronously within the call, not on a separate task.
 *
 * Error-translation pattern (common to nearly every method below): argument
 * validation failures return `UA_STATUSCODE_BADINVALIDARGUMENT`/
 * `UA_STATUSCODE_BADOUTOFRANGE` directly from the callback, but a failed
 * *service* operation (bad chunk, staging full, validation failure, etc.)
 * is reported by returning `UA_STATUSCODE_GOOD` with a `false` boolean
 * method output — callers must check the output value, not just the
 * `UA_StatusCode`, to detect a rejected operation.
 */
#include "opcua_model_internal.h"

UA_StatusCode model_write_trust_list_chunk(UA_Server *server,
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
    UA_StatusCode status_code;
    UA_Boolean result = false;
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

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = security_trust_import_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == SECURITY_TRUST_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteTrustListChunk failed: %s (size=%lu)\n",
               security_trust_import_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_abort_trust_list_import(UA_Server *server,
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
    security_trust_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = security_trust_import_service_abort();
    result = (service_result == SECURITY_TRUST_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortTrustListImport %s\n",
           result == UA_TRUE ? "succeeded" : security_trust_import_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_validate_imported_trust_list(UA_Server *server,
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
    security_trust_import_service_report_t report = {0};
    char payload[384] = {0};
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

    (void)security_trust_import_service_validate_staged(&report);
    model_write_trust_import_report_json(payload, sizeof(payload), &report);

    printf("[OPC UA]\tValidateImportedTrustList %s\n",
           report.valid ? "OK" : security_trust_import_service_result_to_string(report.result));
    fflush(stdout);
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

/* Applying the staged trust list and refreshing the live UA_ServerConfig PKI
 * store are treated as separate failure domains: a refresh failure after a
 * successful apply still reports `result == UA_TRUE` (the trust list *is*
 * persisted) but with a distinct "runtime PKI refresh failed" detail/event
 * message and a higher event severity (600 vs. 300), since the new trust
 * list will only take full effect after a reboot in that case. */
UA_StatusCode model_apply_imported_trust_list(UA_Server *server,
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
    UA_StatusCode refresh_status = UA_STATUSCODE_GOOD;
    security_trust_import_service_report_t report = {0};
    const char *details = "imported trust list validation failed";
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (security_trust_import_service_apply_staged(&report) == SECURITY_TRUST_IMPORT_RESULT_OK)
    {
        result = true;
        details = "imported trust list applied";
        refresh_status = model_refresh_runtime_trust_backends(server);
        if (refresh_status != UA_STATUSCODE_GOOD)
        {
            details = "trust list applied but runtime PKI refresh failed";
            printf("[OPC UA]\tApplyImportedTrustList runtime PKI refresh failed: %s\n",
                   UA_StatusCode_name(refresh_status));
            fflush(stdout);
        }
    }
    else
    {
        details = security_trust_import_service_result_to_string(report.result);
    }

    if (result == UA_TRUE)
    {
        printf("[OPC UA]\tApplyImportedTrustList succeeded\n");
    }
    else
    {
        printf("[OPC UA]\tApplyImportedTrustList failed: %s\n", details);
    }
    fflush(stdout);
    model_audit_session_event(session_context,
                              "trust-import-apply",
                              "device-security",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result == UA_TRUE
                                         ? (refresh_status == UA_STATUSCODE_GOOD
                                                ? "Imported trust list applied"
                                                : "Imported trust list applied; runtime PKI refresh failed")
                                         : "Imported trust list apply failed",
                                     result == UA_TRUE
                                         ? (refresh_status == UA_STATUSCODE_GOOD ? 300u : 600u)
                                         : 700u,
                                     "TrustListImport",
                                     result == UA_TRUE ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_begin_config_import(UA_Server *server,
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
    config_import_service_result_t service_result;
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

    service_result = config_import_service_begin((uint32_t)payload_size,
                                                 (uint32_t)payload_crc32,
                                                 (uint32_t)schema_version);
    result = (service_result == CONFIG_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginConfigImport %s size=%lu crc32=0x%08lx schema=%lu\n",
           result == UA_TRUE ? "accepted" : config_import_service_result_to_string(service_result),
           (unsigned long)payload_size,
           (unsigned long)payload_crc32,
           (unsigned long)schema_version);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_write_config_chunk(UA_Server *server,
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
    UA_StatusCode status_code;
    UA_Boolean result = false;
    config_import_service_result_t service_result;
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

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = config_import_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == CONFIG_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteConfigChunk failed: %s (size=%lu)\n",
               config_import_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_abort_config_import(UA_Server *server,
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
    config_import_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = config_import_service_abort();
    result = (service_result == CONFIG_IMPORT_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortConfigImport %s\n",
           result == UA_TRUE ? "succeeded" : config_import_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_validate_imported_config(UA_Server *server,
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
    config_validate_service_report_t report = {0};
    char payload[384] = {0};
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

    (void)config_validate_service_validate_imported(&report, NULL);
    if (!config_validate_service_write_report_json(payload, sizeof(payload), &report))
    {
        model_set_string_method_output(output_size, output, "{}");
        return UA_STATUSCODE_GOOD;
    }

    printf("[OPC UA]\tValidateImportedConfig %s\n",
           report.valid ? "OK" : config_validate_service_result_to_string(report.result));
    fflush(stdout);
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

/* Four-stage chain, each stage attributing a distinct `details` message on
 * failure: (1) re-validate the staged import into a fresh candidate
 * `device_profile_config_t` (`config_validate_service_validate_imported`),
 * (2) persist it via `config_service_replace_and_save` (Flash write), (3)
 * refresh the AMB location model to match, (4) discard the now-applied
 * import session. Only success at all four stages clears the import session
 * and reports `result == UA_TRUE`; any earlier failure leaves the staged
 * import session intact so the caller can inspect/retry it. */
UA_StatusCode model_apply_imported_config(UA_Server *server,
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
    config_validate_service_report_t report = {0};
    device_profile_config_t candidate = {0};
    const char *details = "imported configuration validation failed";
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    if (config_validate_service_validate_imported(&report, &candidate) != CONFIG_VALIDATE_RESULT_OK)
    {
        if (report.message[0] != '\0')
        {
            details = report.message;
        }
    }
    else if (!config_service_replace_and_save(&candidate, true))
    {
        details = "failed to persist imported configuration";
    }
    else if ((status_code = model_refresh_amb_locations(server,
                                                        config_service_get())) != UA_STATUSCODE_GOOD)
    {
        details = "configuration persisted but location model refresh failed";
    }
    else
    {
        (void)config_import_service_abort();
        result = true;
        details = "imported configuration applied";
    }

    if (result == UA_TRUE)
    {
        printf("[OPC UA]\tApplyImportedConfig succeeded\n");
    }
    else
    {
        printf("[OPC UA]\tApplyImportedConfig failed: %s\n", details);
    }
    fflush(stdout);
    model_audit_session_event(session_context,
                              "config-import-apply",
                              "device-configuration",
                              result == UA_TRUE,
                              details);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    if (result == UA_TRUE)
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_config_changed_event(server,
                                        "Imported configuration applied",
                                        300u,
                                        "ConfigurationImport",
                                        "*",
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

/* Emits one of two JSON shapes depending on `status.ota_state_valid`: a
 * short form (session/chunk counters only) when the underlying `ota/`
 * state machine has no valid persisted state yet, or the short form plus
 * nested OTA state fields (state name, image size/CRC, staged bank
 * offset/size, boot attempts, last error) once it does. Callers must not
 * assume the `otaState*`/`otaBootAttempts`/`otaLastError` fields are present
 * unless `otaStateValid` is true. */
void model_format_firmware_update_status_json(char *buffer, size_t buffer_size)
{
    firmware_update_service_status_t status = {0};

    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    firmware_update_service_get_status(&status);
    if (!status.ota_state_valid)
    {
        (void)snprintf(buffer,
                       buffer_size,
                       "{\"sessionActive\":%s,\"bytesWritten\":%lu,\"expectedImageSize\":%lu,\"remainingBytes\":%lu,\"expectedImageCrc32\":\"0x%08lx\",\"versionNumber\":%lu,\"lastResult\":\"%s\",\"maxChunkBytes\":%u,\"otaStateValid\":false}",
                       status.session_active ? "true" : "false",
                       (unsigned long)status.bytes_written,
                       (unsigned long)status.expected_image_size,
                       (unsigned long)status.remaining_bytes,
                       (unsigned long)status.expected_image_crc32,
                       (unsigned long)status.version_number,
                       firmware_update_service_result_to_string(status.last_result),
                       (unsigned)FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES);
        return;
    }

    (void)snprintf(buffer,
                   buffer_size,
                   "{\"sessionActive\":%s,\"bytesWritten\":%lu,\"expectedImageSize\":%lu,\"remainingBytes\":%lu,\"expectedImageCrc32\":\"0x%08lx\",\"versionNumber\":%lu,\"lastResult\":\"%s\",\"maxChunkBytes\":%u,\"otaStateValid\":true,\"otaState\":\"%s\",\"otaImageSize\":%lu,\"otaImageCrc32\":\"0x%08lx\",\"otaStagedOffset\":\"0x%08lx\",\"otaStagedSize\":\"0x%08lx\",\"otaBootAttempts\":%lu,\"otaLastError\":\"%s\"}",
                   status.session_active ? "true" : "false",
                   (unsigned long)status.bytes_written,
                   (unsigned long)status.expected_image_size,
                   (unsigned long)status.remaining_bytes,
                   (unsigned long)status.expected_image_crc32,
                   (unsigned long)status.version_number,
                   firmware_update_service_result_to_string(status.last_result),
                   (unsigned)FIRMWARE_UPDATE_SERVICE_MAX_CHUNK_BYTES,
                   ota_state_to_string(status.ota_state.state),
                   (unsigned long)status.ota_state.image_size,
                   (unsigned long)status.ota_state.image_crc32,
                   (unsigned long)status.ota_state.staged_bank_offset,
                   (unsigned long)status.ota_state.staged_bank_size,
                   (unsigned long)status.ota_state.boot_attempts,
                   ota_error_to_string(status.ota_state.last_error));
}

UA_StatusCode model_get_firmware_update_status(UA_Server *server,
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
    char payload[512] = {0};
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

    model_format_firmware_update_status_json(payload, sizeof(payload));
    model_set_string_method_output(output_size, output, payload);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_begin_firmware_update(UA_Server *server,
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
    UA_UInt32 image_size = 0u;
    UA_UInt32 image_crc32 = 0u;
    UA_UInt32 version_number = 0u;
    firmware_update_service_result_t service_result;
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

    image_size = *(const UA_UInt32 *)input[0].data;
    image_crc32 = *(const UA_UInt32 *)input[1].data;
    version_number = *(const UA_UInt32 *)input[2].data;

    service_result = firmware_update_service_begin((uint32_t)image_size,
                                                   (uint32_t)image_crc32,
                                                   (uint32_t)version_number);
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tBeginFirmwareUpdate %s size=%lu crc32=0x%08lx version=%lu\n",
           result == UA_TRUE ? "accepted" : firmware_update_service_result_to_string(service_result),
           (unsigned long)image_size,
           (unsigned long)image_crc32,
           (unsigned long)version_number);
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_write_firmware_chunk(UA_Server *server,
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
    UA_StatusCode status_code;
    UA_Boolean result = false;
    firmware_update_service_result_t service_result;
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

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &chunk))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    service_result = firmware_update_service_write_chunk(chunk->data, chunk->length);
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    if (result != UA_TRUE)
    {
        printf("[OPC UA]\tWriteFirmwareChunk failed: %s (size=%lu)\n",
               firmware_update_service_result_to_string(service_result),
               (unsigned long)chunk->length);
        fflush(stdout);
    }
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_finalize_firmware_update(UA_Server *server,
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
    firmware_update_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = firmware_update_service_finalize();
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tFinalizeFirmwareUpdate %s\n",
           result == UA_TRUE ? "succeeded" : firmware_update_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_abort_firmware_update(UA_Server *server,
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
    firmware_update_service_result_t service_result;
    (void)server;
    (void)session_id;
    (void)method_context;
    (void)object_id;
    (void)object_context;
    (void)input_size;
    (void)input;

    status_code = model_require_method_role(session_context, method_id, output_size, output);
    if (status_code != UA_STATUSCODE_GOOD)
    {
        return status_code;
    }

    service_result = firmware_update_service_abort();
    result = (service_result == FIRMWARE_UPDATE_RESULT_OK) ? UA_TRUE : UA_FALSE;
    printf("[OPC UA]\tAbortFirmwareUpdate %s\n",
           result == UA_TRUE ? "succeeded" : firmware_update_service_result_to_string(service_result));
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_clear_channel_config(UA_Server *server,
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
    UA_UInt32 slot_index = 0u;
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

    if (input_size != 1u || input == NULL ||
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    slot_index = *(const UA_UInt32 *)input[0].data;
    if (slot_index >= PLATFORM_CHANNEL_LIMIT)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    result = config_service_stage_clear_channel((size_t)slot_index);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

/* The method's input list has no `kind`/`value_type` arguments; a
 * newly-`in_use` channel inherits both from whatever
 * config_service_stage_copy_channel() returns for the slot (i.e. its prior
 * staged definition), except that `kind`/`value_type` are force-reset to 0
 * when the copied entry's own `flags` field is unset, so a slot with no
 * prior driver-catalog metadata cannot end up with a stale nonzero
 * kind/value_type. */
UA_StatusCode model_set_channel_config(UA_Server *server,
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
    static const size_t expected_input_size = 13u;
    UA_StatusCode status;
    UA_Boolean result = false;
    UA_UInt32 slot_index = 0u;
    UA_Boolean in_use = false;
    UA_Boolean enabled = false;
    UA_UInt32 driver_id = 0u;
    UA_UInt32 source_index = 0u;
    UA_UInt32 source_flags = 0u;
    config_channel_definition_t channel = {0};
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
        !UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_UINT32]) || input[0].data == NULL ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_BOOLEAN]) || input[1].data == NULL ||
        !UA_Variant_hasScalarType(&input[2], &UA_TYPES[UA_TYPES_BOOLEAN]) || input[2].data == NULL ||
        !UA_Variant_hasScalarType(&input[3], &UA_TYPES[UA_TYPES_UINT32]) || input[3].data == NULL ||
        !UA_Variant_hasScalarType(&input[4], &UA_TYPES[UA_TYPES_UINT32]) || input[4].data == NULL ||
        !UA_Variant_hasScalarType(&input[5], &UA_TYPES[UA_TYPES_UINT32]) || input[5].data == NULL ||
        !UA_Variant_hasScalarType(&input[6], &UA_TYPES[UA_TYPES_UINT32]) || input[6].data == NULL ||
        !UA_Variant_hasScalarType(&input[7], &UA_TYPES[UA_TYPES_UINT32]) || input[7].data == NULL ||
        !UA_Variant_hasScalarType(&input[8], &UA_TYPES[UA_TYPES_UINT32]) || input[8].data == NULL ||
        !UA_Variant_hasScalarType(&input[9], &UA_TYPES[UA_TYPES_DOUBLE]) || input[9].data == NULL ||
        !UA_Variant_hasScalarType(&input[10], &UA_TYPES[UA_TYPES_DOUBLE]) || input[10].data == NULL ||
        !UA_Variant_hasScalarType(&input[11], &UA_TYPES[UA_TYPES_DOUBLE]) || input[11].data == NULL ||
        !UA_Variant_hasScalarType(&input[12], &UA_TYPES[UA_TYPES_DOUBLE]) || input[12].data == NULL)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    slot_index = *(const UA_UInt32 *)input[0].data;
    in_use = *(const UA_Boolean *)input[1].data;
    enabled = *(const UA_Boolean *)input[2].data;
    driver_id = *(const UA_UInt32 *)input[3].data;
    source_index = *(const UA_UInt32 *)input[4].data;
    source_flags = *(const UA_UInt32 *)input[5].data;

    if (slot_index >= PLATFORM_CHANNEL_LIMIT ||
        driver_id > UINT8_MAX ||
        source_index > UINT8_MAX ||
        source_flags > UINT8_MAX)
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    if (in_use != UA_TRUE)
    {
        result = config_service_stage_clear_channel((size_t)slot_index);
    }
    else
    {
        (void)config_service_stage_copy_channel((size_t)slot_index, &channel);
        channel.in_use = 1u;
        channel.driver_id = (uint8_t)driver_id;
        channel.source_index = (uint8_t)source_index;
        channel.source_flags = (uint8_t)source_flags;
        channel.settings.enabled = enabled == UA_TRUE;
        channel.settings.sample_period_ms = *(const UA_UInt32 *)input[6].data;
        channel.settings.publish_period_ms = *(const UA_UInt32 *)input[7].data;
        channel.settings.history_interval_ms = *(const UA_UInt32 *)input[8].data;
        channel.settings.scale = *(const UA_Double *)input[9].data;
        channel.settings.offset = *(const UA_Double *)input[10].data;
        channel.settings.warning_threshold = *(const UA_Double *)input[11].data;
        channel.settings.critical_threshold = *(const UA_Double *)input[12].data;
        if (channel.flags == 0u)
        {
            channel.kind = 0u;
            channel.value_type = 0u;
        }

        result = config_service_stage_set_channel((size_t)slot_index, &channel);
    }

    printf("[OPC UA]\tSetChannelConfig %s for slot %u\n",
           result ? "accepted" : "failed",
           (unsigned)slot_index);
    fflush(stdout);

    model_audit_session_event(session_context,
                              "channel-stage-update",
                              "device-channel-configuration",
                              result,
                              result ? "channel configuration staged" : "channel configuration staging failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        char changed_key[32];

        snprintf(changed_key, sizeof(changed_key), "ChannelSlot%u", (unsigned)slot_index);
        model_emit_config_changed_event(server,
                                        result ? "Channel configuration staged" : "Channel configuration staging failed",
                                        result ? 250u : 700u,
                                        "Channels",
                                        changed_key,
                                        model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_self_test(UA_Server *server,
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
    network_manager_status_t network_status = {0};
    time_service_status_t time_status = {0};
    sensor_manager_snapshot_t sensor_snapshot = {0};
    telemetry_channel_snapshot_t channel = {0};
    const device_profile_config_t *config = NULL;
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

    config = config_service_get();
    if (config == NULL || config->opcua_port == 0u || config->device_name[0] == '\0')
    {
        result = false;
    }

    network_manager_get_status(&network_status);
    if (!network_status.initialized)
    {
        result = false;
    }

    time_service_get_status(&time_status);
    if (time_status.quality == SERVICE_QUALITY_BAD)
    {
        result = false;
    }

    if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &channel))
    {
        sensor_manager_get_snapshot(&sensor_snapshot);
        if (!channel.channel.settings.enabled || !sensor_snapshot.enabled)
        {
            result = false;
        }
    }

    diag_service_set_last_fault_code(result ? DIAG_FAULT_CODE_NONE : DIAG_FAULT_CODE_SELF_TEST_FAILED);
    printf("[OPC UA]\tSelfTest %s\n", result ? "passed" : "failed");
    fflush(stdout);
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result ? "Self-test passed" : "Self-test failed",
                                     result ? 250u : 750u,
                                     "SelfTest",
                                     result ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_clear_diagnostics(UA_Server *server,
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

    diag_service_clear();
    printf("[OPC UA]\tDiagnostics cleared\n");
    fflush(stdout);
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

/* Same degraded-success pattern as model_apply_imported_trust_list(): a
 * runtime PKI refresh failure after a successful `security_service_set_trust_list`
 * write is only logged, not reflected in the boolean method output (`result`
 * stays true), since the trust list itself was persisted successfully. */
UA_StatusCode model_set_trust_list(UA_Server *server,
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
    const UA_ByteString *trust_list = NULL;
    UA_StatusCode status;
    UA_StatusCode refresh_status = UA_STATUSCODE_GOOD;
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

    if (input_size != 1u || input == NULL || !model_get_input_ua_bytestring(&input[0], &trust_list))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_set_trust_list(trust_list->data, trust_list->length);
    if (result && server != NULL)
    {
        refresh_status = model_refresh_runtime_trust_backends(server);
        if (refresh_status != UA_STATUSCODE_GOOD)
        {
            printf("[OPC UA]\tTrust list persisted but runtime PKI refresh failed: %s\n",
                   UA_StatusCode_name(refresh_status));
            fflush(stdout);
        }
    }

    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_enter_commissioning_mode(UA_Server *server,
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

    result = security_service_set_commissioning_mode(true);
    if (result)
    {
        model_schedule_reboot(250u);
    }
    printf("[OPC UA]\tEnterCommissioningMode %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_audit_session_event(session_context,
                              "commissioning-enter",
                              "device-security",
                              result,
                              result ? "commissioning mode enabled" : "commissioning enable failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result ? "Commissioning mode enabled" : "Commissioning mode enable failed",
                                     result ? 400u : 700u,
                                     "EnterCommissioningMode",
                                     result ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

/* Unlike security_service_set_commissioning_mode()'s other callers, this
 * callback itself checks that a non-empty trust list exists before allowing
 * strict mode to be re-armed (a dedicated audit event + rejection, not a
 * generic UA_STATUSCODE_BAD*, is used when that precondition fails), since
 * exiting commissioning with an empty trust list would lock out every
 * client. */
UA_StatusCode model_exit_commissioning_mode(UA_Server *server,
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
    security_service_blob_view_t trust_list = {0};
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

    if (!security_service_get_trust_list(&trust_list) || trust_list.length == 0u)
    {
        printf("[OPC UA]\tExitCommissioningMode failed: trust list is empty\n");
        fflush(stdout);
        model_audit_session_event(session_context,
                                  "commissioning-exit",
                                  "device-security",
                                  false,
                                  "trust list is empty");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
        {
            char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
            model_emit_maintenance_event(server,
                                         "Exit commissioning mode failed: trust list is empty",
                                         700u,
                                         "ExitCommissioningMode",
                                         1u,
                                         model_session_actor_name(session_context, actor, sizeof(actor)));
        }
#endif
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_GOOD;
    }

    result = security_service_set_commissioning_mode(false);
    if (result)
    {
        model_schedule_reboot(250u);
    }
    printf("[OPC UA]\tExitCommissioningMode %s\n", result ? "succeeded" : "failed");
    fflush(stdout);
    model_audit_session_event(session_context,
                              "commissioning-exit",
                              "device-security",
                              result,
                              result ? "strict mode scheduled after reboot" : "commissioning disable failed");
#ifdef UA_ENABLE_SUBSCRIPTIONS_EVENTS
    {
        char actor[SECURITY_SERVICE_USERNAME_MAX_LEN];
        model_emit_maintenance_event(server,
                                     result ? "Commissioning mode disabled" : "Commissioning mode disable failed",
                                     result ? 400u : 700u,
                                     "ExitCommissioningMode",
                                     result ? 0u : 1u,
                                     model_session_actor_name(session_context, actor, sizeof(actor)));
    }
#endif
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_add_user(UA_Server *server,
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
    const UA_String *password = NULL;
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

    if (input_size != 3u ||
        !model_copy_input_string(&input[0], username, sizeof(username)) ||
        !model_get_input_ua_string(&input[1], &password) ||
        !model_copy_input_string(&input[2], role_text, sizeof(role_text)))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (!security_service_role_from_string(role_text, &role))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADOUTOFRANGE;
    }

    result = security_service_add_user(username, password->data, password->length, role);
    printf("[OPC UA]\tAddUser %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "user-add",
                              username,
                              result,
                              security_service_role_to_string(role));
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_change_password(UA_Server *server,
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
    const UA_String *old_password = NULL;
    const UA_String *new_password = NULL;
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

    if (input_size != 3u ||
        !model_copy_input_string(&input[0], username, sizeof(username)) ||
        !model_get_input_ua_string(&input[1], &old_password) ||
        !model_get_input_ua_string(&input[2], &new_password))
    {
        model_set_boolean_method_output(output_size, output, false);
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    result = security_service_change_password(username,
                                              old_password->data,
                                              old_password->length,
                                              new_password->data,
                                              new_password->length);
    printf("[OPC UA]\tChangePassword %s for %s\n",
           result ? "succeeded" : "failed",
           username);
    fflush(stdout);
    model_audit_session_event(session_context,
                              "password-change",
                              username,
                              result,
                              result ? "password updated" : "password update failed");
    model_set_boolean_method_output(output_size, output, result);
    return UA_STATUSCODE_GOOD;
}

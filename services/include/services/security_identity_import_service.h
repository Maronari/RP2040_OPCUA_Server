#ifndef SERVICES_SECURITY_IDENTITY_IMPORT_SERVICE_H
#define SERVICES_SECURITY_IDENTITY_IMPORT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_IDENTITY_IMPORT_SERVICE_MAX_CHUNK_BYTES 4096u
#define SECURITY_IDENTITY_IMPORT_SERVICE_SCHEMA_VERSION 1u

typedef enum
{
    SECURITY_IDENTITY_IMPORT_RESULT_NONE = 0,
    SECURITY_IDENTITY_IMPORT_RESULT_OK = 1,
    SECURITY_IDENTITY_IMPORT_RESULT_BUSY = 2,
    SECURITY_IDENTITY_IMPORT_RESULT_NOT_ACTIVE = 3,
    SECURITY_IDENTITY_IMPORT_RESULT_INVALID_ARGUMENT = 4,
    SECURITY_IDENTITY_IMPORT_RESULT_CHUNK_TOO_LARGE = 5,
    SECURITY_IDENTITY_IMPORT_RESULT_TOO_LARGE = 6,
    SECURITY_IDENTITY_IMPORT_RESULT_OUT_OF_MEMORY = 7,
    SECURITY_IDENTITY_IMPORT_RESULT_NOT_READY = 8,
    SECURITY_IDENTITY_IMPORT_RESULT_CRC_MISMATCH = 9,
    SECURITY_IDENTITY_IMPORT_RESULT_SCHEMA_MISMATCH = 10,
    SECURITY_IDENTITY_IMPORT_RESULT_PACKAGE_INVALID = 11,
    SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_PARSE_FAILED = 12,
    SECURITY_IDENTITY_IMPORT_RESULT_PRIVATE_KEY_PARSE_FAILED = 13,
    SECURITY_IDENTITY_IMPORT_RESULT_KEY_PAIR_MISMATCH = 14,
    SECURITY_IDENTITY_IMPORT_RESULT_APPLICATION_URI_MISMATCH = 15,
    SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_NOT_YET_VALID = 16,
    SECURITY_IDENTITY_IMPORT_RESULT_CERTIFICATE_EXPIRED = 17,
    SECURITY_IDENTITY_IMPORT_RESULT_APPLY_FAILED = 18,
    SECURITY_IDENTITY_IMPORT_RESULT_INTERNAL_ERROR = 19,
} security_identity_import_service_result_t;

typedef struct
{
    bool session_active;
    uint32_t expected_payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    uint32_t bytes_written;
    uint32_t remaining_bytes;
    bool payload_complete;
    bool payload_crc32_matches;
    security_identity_import_service_result_t last_result;
} security_identity_import_service_status_t;

typedef struct
{
    bool session_active;
    bool payload_complete;
    bool payload_crc32_matches;
    bool valid;
    bool time_check_performed;
    bool reboot_required;
    uint32_t payload_size;
    uint32_t expected_payload_crc32;
    uint32_t schema_version;
    uint32_t certificate_size;
    uint32_t private_key_size;
    uint32_t not_before_epoch_s;
    uint32_t not_after_epoch_s;
    security_identity_import_service_result_t result;
} security_identity_import_service_report_t;

void security_identity_import_service_init(void);
security_identity_import_service_result_t security_identity_import_service_begin(uint32_t payload_size,
                                                                                uint32_t payload_crc32,
                                                                                uint32_t schema_version);
security_identity_import_service_result_t security_identity_import_service_write_chunk(const uint8_t *data,
                                                                                      size_t size);
security_identity_import_service_result_t security_identity_import_service_abort(void);
void security_identity_import_service_get_status(security_identity_import_service_status_t *out_status);
security_identity_import_service_result_t security_identity_import_service_validate_staged(
    security_identity_import_service_report_t *out_report);
security_identity_import_service_result_t security_identity_import_service_apply_staged(
    security_identity_import_service_report_t *out_report);
const char *security_identity_import_service_result_to_string(security_identity_import_service_result_t result);

#endif

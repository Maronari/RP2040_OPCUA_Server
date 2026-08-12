#ifndef SERVICES_CONFIG_VALIDATE_SERVICE_H
#define SERVICES_CONFIG_VALIDATE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <services/config_service.h>

#define CONFIG_VALIDATE_SERVICE_MESSAGE_MAX_LEN 128u

typedef enum
{
    CONFIG_VALIDATE_RESULT_NONE = 0,
    CONFIG_VALIDATE_RESULT_OK = 1,
    CONFIG_VALIDATE_RESULT_NO_IMPORT_SESSION = 2,
    CONFIG_VALIDATE_RESULT_INCOMPLETE_PAYLOAD = 3,
    CONFIG_VALIDATE_RESULT_CRC_MISMATCH = 4,
    CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA = 5,
    CONFIG_VALIDATE_RESULT_INVALID_SIZE = 6,
    CONFIG_VALIDATE_RESULT_INVALID_NETWORK = 7,
    CONFIG_VALIDATE_RESULT_INVALID_PORT = 8,
    CONFIG_VALIDATE_RESULT_INVALID_TIMEZONE = 9,
    CONFIG_VALIDATE_RESULT_INVALID_WATCHDOG = 10,
    CONFIG_VALIDATE_RESULT_DUPLICATE_CHANNEL_ID = 11,
    CONFIG_VALIDATE_RESULT_INVALID_CHANNEL = 12,
    CONFIG_VALIDATE_RESULT_INTERNAL_ERROR = 13,
    CONFIG_VALIDATE_RESULT_INVALID_JSON = 14,
    CONFIG_VALIDATE_RESULT_MISSING_FIELD = 15,
    CONFIG_VALIDATE_RESULT_INVALID_LOCATION_PATH = 16,
    CONFIG_VALIDATE_RESULT_RESOURCE_CONFLICT = 17,
    CONFIG_VALIDATE_RESULT_UNSUPPORTED_VALUE_TYPE = 18,
} config_validate_service_result_t;

typedef struct
{
    bool valid;
    config_validate_service_result_t result;
    uint32_t schema_version;
    uint32_t payload_size;
    uint32_t payload_crc32;
    char message[CONFIG_VALIDATE_SERVICE_MESSAGE_MAX_LEN];
} config_validate_service_report_t;

config_validate_service_result_t config_validate_service_validate_imported(config_validate_service_report_t *out_report,
                                                                          device_profile_config_t *out_candidate);
config_validate_service_result_t config_validate_service_validate_candidate(
    const device_profile_config_t *candidate,
    char *message,
    size_t message_size);
bool config_validate_service_write_report_json(char *buffer,
                                               size_t buffer_size,
                                               const config_validate_service_report_t *report);
const char *config_validate_service_result_to_string(config_validate_service_result_t result);

#endif

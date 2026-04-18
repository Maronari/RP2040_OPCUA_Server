#ifndef SERVICES_CONFIG_IMPORT_SERVICE_H
#define SERVICES_CONFIG_IMPORT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_IMPORT_SERVICE_MAX_CHUNK_BYTES 4096u

typedef enum
{
    CONFIG_IMPORT_RESULT_NONE = 0,
    CONFIG_IMPORT_RESULT_OK = 1,
    CONFIG_IMPORT_RESULT_BUSY = 2,
    CONFIG_IMPORT_RESULT_NOT_ACTIVE = 3,
    CONFIG_IMPORT_RESULT_INVALID_ARGUMENT = 4,
    CONFIG_IMPORT_RESULT_CHUNK_TOO_LARGE = 5,
    CONFIG_IMPORT_RESULT_TOO_LARGE = 6,
    CONFIG_IMPORT_RESULT_OUT_OF_MEMORY = 7,
    CONFIG_IMPORT_RESULT_INTERNAL_ERROR = 8,
} config_import_service_result_t;

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
    config_import_service_result_t last_result;
} config_import_service_status_t;

void config_import_service_init(void);
config_import_service_result_t config_import_service_begin(uint32_t payload_size,
                                                           uint32_t payload_crc32,
                                                           uint32_t schema_version);
config_import_service_result_t config_import_service_write_chunk(const uint8_t *data, size_t size);
config_import_service_result_t config_import_service_abort(void);
void config_import_service_get_status(config_import_service_status_t *out_status);
bool config_import_service_get_payload_view(const uint8_t **out_payload,
                                            size_t *out_payload_size,
                                            uint32_t *out_expected_crc32,
                                            uint32_t *out_schema_version);
const char *config_import_service_result_to_string(config_import_service_result_t result);

#endif

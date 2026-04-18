#ifndef OTA_OTA_FLASH_H
#define OTA_OTA_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota/ota_state.h"

#define OTA_FLASH_WRITER_PAGE_BUFFER_SIZE 256u

typedef enum {
    OTA_FLASH_STATUS_OK = 0,
    OTA_FLASH_STATUS_INVALID_ARGUMENT = 1,
    OTA_FLASH_STATUS_OUT_OF_RANGE = 2,
    OTA_FLASH_STATUS_FLASH_OP_FAILED = 3,
    OTA_FLASH_STATUS_VERIFY_FAILED = 4,
    OTA_FLASH_STATUS_WRITER_STATE_ERROR = 5,
} ota_flash_status_t;

typedef struct {
    uint32_t image_size;
    uint32_t bytes_written;
    size_t buffered_bytes;
    bool active;
    uint8_t page_buffer[OTA_FLASH_WRITER_PAGE_BUFFER_SIZE];
} ota_flash_writer_t;

bool ota_flash_read_state(ota_state_record_t *out_record);
ota_flash_status_t ota_flash_write_state(const ota_state_record_t *record);
ota_flash_status_t ota_flash_reset_state(void);

ota_flash_status_t ota_flash_prepare_bank_b(uint32_t image_size);
ota_flash_status_t ota_flash_program_bank_b(uint32_t image_offset,
                                            const void *data,
                                            size_t size);
ota_flash_status_t ota_flash_verify_bank_b(uint32_t image_size, uint32_t expected_crc32);

void ota_flash_writer_init(ota_flash_writer_t *writer);
ota_flash_status_t ota_flash_writer_begin(ota_flash_writer_t *writer, uint32_t image_size);
ota_flash_status_t ota_flash_writer_write(ota_flash_writer_t *writer,
                                          const void *data,
                                          size_t size);
ota_flash_status_t ota_flash_writer_finish(ota_flash_writer_t *writer,
                                           uint32_t expected_crc32,
                                           uint32_t version_number);

const char *ota_flash_status_to_string(ota_flash_status_t status);

#endif

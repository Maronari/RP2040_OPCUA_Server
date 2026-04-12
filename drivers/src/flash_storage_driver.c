#include "drivers/flash_storage_driver.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#define FLASH_STORAGE_MAGIC       0x43464731u /* "CFG1" */
#define FLASH_STORAGE_VERSION     1u
#define FLASH_STORAGE_REGION_SIZE FLASH_SECTOR_SIZE
#define FLASH_STORAGE_OFFSET      (PICO_FLASH_SIZE_BYTES - FLASH_STORAGE_REGION_SIZE)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t payload_size;
    uint32_t crc32;
} flash_storage_header_t;

typedef struct {
    const uint8_t *image;
} flash_storage_program_context_t;

static uint8_t g_flash_storage_sector_buffer[FLASH_STORAGE_REGION_SIZE];

static size_t flash_storage_payload_capacity(void)
{
    return FLASH_STORAGE_REGION_SIZE - sizeof(flash_storage_header_t);
}

static uint32_t flash_storage_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8u; ++bit)
        {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

static bool flash_storage_validate_header(const flash_storage_header_t *header, size_t expected_size)
{
    if (header == NULL)
    {
        return false;
    }

    if (header->magic != FLASH_STORAGE_MAGIC || header->version != FLASH_STORAGE_VERSION)
    {
        return false;
    }

    if (header->payload_size != expected_size || expected_size > flash_storage_payload_capacity())
    {
        return false;
    }

    return true;
}

static void __not_in_flash_func(flash_storage_program_sector)(void *param)
{
    const flash_storage_program_context_t *context = (const flash_storage_program_context_t *)param;

    flash_range_erase(FLASH_STORAGE_OFFSET, FLASH_STORAGE_REGION_SIZE);
    flash_range_program(FLASH_STORAGE_OFFSET, context->image, FLASH_STORAGE_REGION_SIZE);
}

bool flash_storage_driver_load(void *buffer, size_t size)
{
    const uint8_t *flash = (const uint8_t *)(XIP_BASE + FLASH_STORAGE_OFFSET);
    const flash_storage_header_t *header = (const flash_storage_header_t *)flash;
    const uint8_t *payload = flash + sizeof(flash_storage_header_t);

    if (buffer == NULL || size == 0u)
    {
        return false;
    }

    if (!flash_storage_validate_header(header, size))
    {
        return false;
    }

    if (flash_storage_crc32(payload, size) != header->crc32)
    {
        return false;
    }

    memcpy(buffer, payload, size);
    return true;
}

bool flash_storage_driver_load_partial(void *buffer, size_t buffer_size, size_t *out_payload_size)
{
    const uint8_t *flash = (const uint8_t *)(XIP_BASE + FLASH_STORAGE_OFFSET);
    const flash_storage_header_t *header = (const flash_storage_header_t *)flash;
    const uint8_t *payload = flash + sizeof(flash_storage_header_t);

    if (buffer == NULL || buffer_size == 0u || header == NULL)
    {
        return false;
    }

    if (header->magic != FLASH_STORAGE_MAGIC ||
        header->version != FLASH_STORAGE_VERSION ||
        header->payload_size == 0u ||
        header->payload_size > flash_storage_payload_capacity() ||
        header->payload_size > buffer_size)
    {
        return false;
    }

    if (flash_storage_crc32(payload, header->payload_size) != header->crc32)
    {
        return false;
    }

    memset(buffer, 0, buffer_size);
    memcpy(buffer, payload, header->payload_size);

    if (out_payload_size != NULL)
    {
        *out_payload_size = header->payload_size;
    }

    return true;
}

bool flash_storage_driver_save(const void *buffer, size_t size)
{
    flash_storage_header_t *header = (flash_storage_header_t *)g_flash_storage_sector_buffer;
    flash_storage_program_context_t context = {
        .image = g_flash_storage_sector_buffer,
    };

    if (buffer == NULL || size == 0u || size > flash_storage_payload_capacity())
    {
        return false;
    }

    memset(g_flash_storage_sector_buffer, 0xFF, sizeof(g_flash_storage_sector_buffer));

    header->magic = FLASH_STORAGE_MAGIC;
    header->version = FLASH_STORAGE_VERSION;
    header->reserved = 0u;
    header->payload_size = (uint32_t)size;
    header->crc32 = flash_storage_crc32((const uint8_t *)buffer, size);

    memcpy(g_flash_storage_sector_buffer + sizeof(flash_storage_header_t), buffer, size);

    return flash_safe_execute(flash_storage_program_sector, &context, 1000u) == PICO_OK;
}

#include "services/config_store_service.h"

#include <string.h>

#include <config_store_limits.h>
#include "hardware/flash.h"
#include "ota/ota_state.h"
#include "pico/flash.h"
#include "pico/platform.h"

typedef struct
{
    uint32_t flash_offset;
    size_t byte_count;
} config_store_erase_context_t;

typedef struct
{
    uint32_t flash_offset;
    const uint8_t *data;
    size_t byte_count;
} config_store_program_context_t;

static uint8_t g_config_store_page[OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES];

_Static_assert(sizeof(config_store_slot_header_t) < OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES,
               "Config slot header must fit inside one config slot");

static uint32_t config_store_round_up(uint32_t value, uint32_t alignment)
{
    if (alignment == 0u)
    {
        return 0u;
    }

    return (uint32_t)(((value + alignment - 1u) / alignment) * alignment);
}

static bool config_store_is_valid_slot_index(size_t slot_index)
{
    return slot_index < CONFIG_STORE_SLOT_COUNT;
}

static uint32_t config_store_slot_offset_bytes(size_t slot_index)
{
    return slot_index == 0u ? OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES : OTA_LAYOUT_CONFIG_SLOT_1_OFFSET_BYTES;
}

static uintptr_t config_store_slot_origin(size_t slot_index)
{
    return slot_index == 0u ? OTA_LAYOUT_CONFIG_SLOT_0_ORIGIN : OTA_LAYOUT_CONFIG_SLOT_1_ORIGIN;
}

static const config_store_slot_header_t *config_store_slot_header(size_t slot_index)
{
    return (const config_store_slot_header_t *)config_store_slot_origin(slot_index);
}

static const uint8_t *config_store_slot_payload(size_t slot_index)
{
    return (const uint8_t *)(config_store_slot_origin(slot_index) + sizeof(config_store_slot_header_t));
}

static void __not_in_flash_func(config_store_flash_erase_range)(void *param)
{
    const config_store_erase_context_t *context = (const config_store_erase_context_t *)param;

    flash_range_erase(context->flash_offset, context->byte_count);
}

static void __not_in_flash_func(config_store_flash_program_range)(void *param)
{
    const config_store_program_context_t *context = (const config_store_program_context_t *)param;

    flash_range_program(context->flash_offset, context->data, context->byte_count);
}

static bool config_store_execute_erase(uint32_t flash_offset, size_t byte_count)
{
    config_store_erase_context_t context = {
        .flash_offset = flash_offset,
        .byte_count = byte_count,
    };

    if (byte_count == 0u || (byte_count % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES) != 0u)
    {
        return false;
    }

    return flash_safe_execute(config_store_flash_erase_range, &context, 1000u) == PICO_OK;
}

static bool config_store_execute_program(uint32_t flash_offset, const uint8_t *data, size_t byte_count)
{
    config_store_program_context_t context = {
        .flash_offset = flash_offset,
        .data = data,
        .byte_count = byte_count,
    };

    if (data == NULL || byte_count == 0u || (byte_count % OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) != 0u)
    {
        return false;
    }

    return flash_safe_execute(config_store_flash_program_range, &context, 1000u) == PICO_OK;
}

static bool config_store_read_slot_info(size_t slot_index, config_store_record_info_t *out_info)
{
    const config_store_slot_header_t *header;
    const uint8_t *payload;

    if (!config_store_is_valid_slot_index(slot_index))
    {
        return false;
    }

    header = config_store_slot_header(slot_index);
    payload = config_store_slot_payload(slot_index);

    if (header->magic != CONFIG_STORE_SLOT_MAGIC ||
        header->header_version != CONFIG_STORE_SLOT_HEADER_VERSION ||
        (header->flags & CONFIG_STORE_SLOT_FLAG_VALID) == 0u ||
        header->payload_size == 0u ||
        header->payload_size > CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES)
    {
        return false;
    }

    if (ota_crc32_compute(payload, header->payload_size) != header->payload_crc32)
    {
        return false;
    }

    if (out_info != NULL)
    {
        out_info->slot_index = slot_index;
        out_info->payload_format = header->payload_format;
        out_info->schema_version = header->schema_version;
        out_info->generation = header->generation;
        out_info->payload_size = header->payload_size;
        out_info->payload_crc32 = header->payload_crc32;
        out_info->flags = header->flags;
    }

    return true;
}

static bool config_store_select_active_record(config_store_record_info_t *out_info)
{
    config_store_record_info_t slot0 = {0};
    config_store_record_info_t slot1 = {0};
    const bool slot0_valid = config_store_read_slot_info(0u, &slot0);
    const bool slot1_valid = config_store_read_slot_info(1u, &slot1);

    if (!slot0_valid && !slot1_valid)
    {
        return false;
    }

    if (!slot1_valid || (slot0_valid && slot0.generation >= slot1.generation))
    {
        if (out_info != NULL)
        {
            *out_info = slot0;
        }
        return true;
    }

    if (out_info != NULL)
    {
        *out_info = slot1;
    }
    return true;
}

static void config_store_fill_program_page(uint32_t page_offset,
                                           const config_store_slot_header_t *header,
                                           const uint8_t *payload,
                                           size_t payload_size)
{
    const uint8_t *header_bytes = (const uint8_t *)header;

    memset(g_config_store_page, 0xFF, sizeof(g_config_store_page));

    for (size_t index = 0u; index < sizeof(g_config_store_page); ++index)
    {
        const uint32_t absolute_offset = page_offset + (uint32_t)index;

        if (absolute_offset < sizeof(*header))
        {
            g_config_store_page[index] = header_bytes[absolute_offset];
            continue;
        }

        if ((absolute_offset - sizeof(*header)) < payload_size)
        {
            g_config_store_page[index] = payload[absolute_offset - sizeof(*header)];
        }
    }
}

bool config_store_get_active_info(config_store_record_info_t *out_info)
{
    return config_store_select_active_record(out_info);
}

bool config_store_load_active(void *buffer,
                              size_t buffer_size,
                              size_t *out_payload_size,
                              config_store_record_info_t *out_info)
{
    config_store_record_info_t active_info = {0};
    const uint8_t *payload = NULL;

    if (buffer == NULL || buffer_size == 0u || !config_store_select_active_record(&active_info))
    {
        return false;
    }

    if (active_info.payload_size > buffer_size)
    {
        return false;
    }

    payload = config_store_slot_payload(active_info.slot_index);
    memset(buffer, 0, buffer_size);
    memcpy(buffer, payload, active_info.payload_size);

    if (out_payload_size != NULL)
    {
        *out_payload_size = active_info.payload_size;
    }

    if (out_info != NULL)
    {
        *out_info = active_info;
    }

    return true;
}

bool config_store_save_payload(const void *payload,
                               size_t payload_size,
                               uint16_t payload_format,
                               uint32_t schema_version,
                               config_store_record_info_t *out_info)
{
    config_store_record_info_t active_info = {0};
    const bool have_active = config_store_select_active_record(&active_info);
    const size_t target_slot = (!have_active || active_info.slot_index != 0u) ? 0u : 1u;
    const uint32_t generation = have_active ? (active_info.generation + 1u) : 1u;
    const uint32_t image_size = (uint32_t)(sizeof(config_store_slot_header_t) + payload_size);
    const uint32_t program_size = config_store_round_up(image_size, OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES);
    config_store_slot_header_t header = {0};

    if (payload == NULL || payload_size == 0u || payload_size > CONFIG_STORE_MAX_PAYLOAD_SIZE_BYTES)
    {
        return false;
    }

    header.magic = CONFIG_STORE_SLOT_MAGIC;
    header.header_version = CONFIG_STORE_SLOT_HEADER_VERSION;
    header.payload_format = payload_format;
    header.schema_version = schema_version;
    header.generation = generation;
    header.payload_size = (uint32_t)payload_size;
    header.payload_crc32 = ota_crc32_compute(payload, payload_size);
    header.flags = CONFIG_STORE_SLOT_FLAG_VALID;

    if (!config_store_execute_erase(config_store_slot_offset_bytes(target_slot), OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES))
    {
        return false;
    }

    for (uint32_t offset = 0u; offset < program_size; offset += OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES)
    {
        config_store_fill_program_page(offset, &header, (const uint8_t *)payload, payload_size);
        if (!config_store_execute_program(config_store_slot_offset_bytes(target_slot) + offset,
                                          g_config_store_page,
                                          sizeof(g_config_store_page)))
        {
            return false;
        }
    }

    if (out_info != NULL)
    {
        out_info->slot_index = target_slot;
        out_info->payload_format = payload_format;
        out_info->schema_version = schema_version;
        out_info->generation = generation;
        out_info->payload_size = (uint32_t)payload_size;
        out_info->payload_crc32 = header.payload_crc32;
        out_info->flags = header.flags;
    }

    return true;
}

bool config_store_reset(void)
{
    return config_store_execute_erase(OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES, OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES) &&
           config_store_execute_erase(OTA_LAYOUT_CONFIG_SLOT_1_OFFSET_BYTES, OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES);
}

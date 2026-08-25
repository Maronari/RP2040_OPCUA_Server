/**
 * @file
 * @brief Implementation of the two-slot Flash-backed configuration record
 * store declared in `services/config_store_service.h`.
 *
 * Responsibilities:
 * - Read/write `config_store_slot_header_t` + opaque payload pairs at the
 *   two fixed Flash offsets from `ota_layout.h`
 *   (`OTA_LAYOUT_CONFIG_SLOT_0/1_OFFSET_BYTES`/`_ORIGIN`).
 * - Select the active slot by generation counter (higher wins; slot 0 wins
 *   ties), and always write new payloads to the *other* slot so the
 *   previous record stays intact until the new one's header commits.
 *
 * Ownership: `g_config_store_page` is the only module-owned state, a
 * single-page write-staging buffer reused across every program call.
 *
 * Concurrency: no internal synchronization; all actual Flash mutation goes
 * through `flash_operation_erase_sector()`/`flash_operation_program_page()`,
 * which serialize via the single `flash_operation_coordinator` mutex.
 * Reads bypass the coordinator entirely and dereference the XIP-mapped
 * slot header/payload directly (`config_store_slot_header()`/
 * `config_store_slot_payload()`) — see
 * [persistence.md](../../docs/code/persistence.md) for this layering note.
 */
#include "services/config_store_service.h"

#include <string.h>

#include <config_store_limits.h>
#include "drivers/flash_operation_coordinator.h"
#include "ota/ota_state.h"

/** Single-page (one Flash program unit) write-staging buffer, reused by
 * every call to `config_store_save_payload()`. Filled with header+payload
 * bytes (0xFF padded) by `config_store_fill_program_page()` before each
 * `flash_operation_program_page()` call. */
static uint8_t g_config_store_page[OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES];

_Static_assert(sizeof(config_store_slot_header_t) < OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES,
               "Config slot header must fit inside one config slot");

/**
 * @brief Rounds @p value up to the next multiple of @p alignment.
 * @return Rounded value; 0 if @p alignment is 0.
 */
static uint32_t config_store_round_up(uint32_t value, uint32_t alignment)
{
    if (alignment == 0u)
    {
        return 0u;
    }

    return (uint32_t)(((value + alignment - 1u) / alignment) * alignment);
}

/** @brief Returns whether @p slot_index names one of the `CONFIG_STORE_SLOT_COUNT` slots. */
static bool config_store_is_valid_slot_index(size_t slot_index)
{
    return slot_index < CONFIG_STORE_SLOT_COUNT;
}

/** @brief Returns the Flash byte offset (for erase/program calls) of slot @p slot_index. */
static uint32_t config_store_slot_offset_bytes(size_t slot_index)
{
    return slot_index == 0u ? OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES : OTA_LAYOUT_CONFIG_SLOT_1_OFFSET_BYTES;
}

/** @brief Returns the XIP-mapped address of slot @p slot_index for direct read access. */
static uintptr_t config_store_slot_origin(size_t slot_index)
{
    return slot_index == 0u ? OTA_LAYOUT_CONFIG_SLOT_0_ORIGIN : OTA_LAYOUT_CONFIG_SLOT_1_ORIGIN;
}

/**
 * @brief Returns a read-only pointer to slot @p slot_index's header, via a
 * direct XIP-mapped dereference (not a driver read call).
 */
static const config_store_slot_header_t *config_store_slot_header(size_t slot_index)
{
    return (const config_store_slot_header_t *)config_store_slot_origin(slot_index);
}

/**
 * @brief Returns a read-only pointer to slot @p slot_index's payload bytes
 * (immediately following the header), via a direct XIP-mapped dereference.
 */
static const uint8_t *config_store_slot_payload(size_t slot_index)
{
    return (const uint8_t *)(config_store_slot_origin(slot_index) + sizeof(config_store_slot_header_t));
}

/**
 * @brief Erases @p byte_count bytes starting at @p flash_offset, one
 * `OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES` sector at a time.
 * @return true if all sectors erased; false if the range is misaligned or
 * any `flash_operation_erase_sector()` call fails (in which case earlier
 * sectors in the range are already erased — no rollback).
 * @note Performs Flash erase via `flash_operation_coordinator`.
 */
static bool config_store_execute_erase(uint32_t flash_offset, size_t byte_count)
{
    if (byte_count == 0u ||
        (flash_offset % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES) != 0u ||
        (byte_count % OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES) != 0u)
    {
        return false;
    }

    for (size_t offset = 0u; offset < byte_count; offset += OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES)
    {
        if (flash_operation_erase_sector(flash_offset + (uint32_t)offset) != FLASH_OPERATION_OK)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Programs exactly one Flash page (`OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES`)
 * at @p flash_offset from @p data.
 * @param data Page-sized buffer; must be exactly
 * `OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES` long.
 * @return true on success; false if arguments are misaligned/mis-sized or
 * `flash_operation_program_page()` fails.
 * @note Performs a Flash program operation via `flash_operation_coordinator`.
 */
static bool config_store_execute_program(uint32_t flash_offset, const uint8_t *data, size_t byte_count)
{
    if (data == NULL || byte_count != OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES ||
        (flash_offset % OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) != 0u)
    {
        return false;
    }

    return flash_operation_program_page(flash_offset, data) == FLASH_OPERATION_OK;
}

/**
 * @brief Reads and validates slot @p slot_index's header (magic, header
 * version, valid flag, payload size bound, payload CRC32), populating
 * @p out_info only if all checks pass.
 * @return true if the slot holds a structurally valid, CRC-matching record;
 * false otherwise (a "ragged"/erased/corrupt slot is treated identically —
 * this is not a transactional store, so no distinct corruption state is
 * surfaced beyond "invalid").
 * @note Reads the slot header/payload via direct XIP-mapped dereference.
 */
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

/**
 * @brief Selects the active config-store record: the valid slot with the
 * higher generation counter, preferring slot 0 on a tie or when slot 1 is
 * invalid.
 * @param out_info Filled with the selected slot's metadata on success;
 * optional (may be NULL).
 * @return true if at least one slot is valid; false if both are invalid.
 */
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

/**
 * @brief Fills `g_config_store_page` with the header+payload bytes that
 * belong at absolute Flash offset @p page_offset within the target slot,
 * padding unused bytes with `0xFF` (erased-Flash value).
 * @param page_offset Absolute byte offset of this page within the slot
 * being written (0 = header start).
 * @param header Slot header being written; bytes copied in where they fall
 * within this page.
 * @param payload Payload bytes; copied in where they fall within this page
 * (i.e. at offsets `>= sizeof(*header)`).
 * @param payload_size Payload length, bounding how many payload bytes are
 * copied.
 * @note Mutates the shared `g_config_store_page` staging buffer; caller must
 * not call this concurrently for two different pages in flight (it never is
 * — `config_store_save_payload()` calls it once per page, synchronously).
 */
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

/* Erase-then-write: the target slot (the inactive one, or slot 0 if
 * neither is currently active) is fully erased before any page is
 * programmed, so a power loss mid-write leaves that slot invalid/erased
 * while the other slot (if it was active) remains untouched and selectable
 * on the next boot. */
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

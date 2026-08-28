/**
 * @file
 * @brief Implements the OTA state record I/O and Bank B staging writer
 *        declared in `ota/ota_flash.h`.
 *
 * Responsibilities:
 * - Erase-then-program the single OTA state Flash sector on every state
 *   write.
 * - Buffer arbitrary-sized caller writes into full Flash program pages and
 *   flush them into Bank B (`ota_flash_writer_*` family).
 * - Bounds-check every offset/size against Bank B's configured capacity
 *   before touching Flash.
 *
 * Ownership:
 * `g_ota_state_sector_buffer` is a private scratch buffer sized to one
 * Flash erase block, reused across every `ota_flash_write_state()` call; it
 * holds no state between calls (always fully overwritten before use).
 *
 * Concurrency:
 * No internal locking. All Flash access is funneled through
 * `ota_flash_backend_erase()`/`ota_flash_backend_program()`
 * (`ota_flash_backend.h`); on the application side that backend is
 * serialized by `drivers/flash_operation_coordinator`, but callers of this
 * module (e.g. `firmware_update_service.c`) must still ensure only one OTA
 * write session is active at a time, since `ota_flash_writer_t` state and
 * the shared `g_ota_state_sector_buffer` are not protected here.
 */

#include "ota/ota_flash.h"

#include <string.h>

#include "ota_flash_backend.h"
#include "ota_layout.h"
#include "pico/stdlib.h"

/** Scratch buffer for one OTA-state Flash sector, reused by every
 * `ota_flash_write_state()` call. Placed in `.scratch_x` RAM (SRAM striped
 * bank, see `__scratch_x`) rather than the default heap/BSS so it remains
 * valid even during flash-critical sections where normal RAM banking may be
 * restricted. */
static uint8_t __scratch_x("ota_flash") g_ota_state_sector_buffer[OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES];

_Static_assert(OTA_FLASH_WRITER_PAGE_BUFFER_SIZE == OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES,
               "OTA writer page buffer must match flash program size");

/**
 * @brief Computes the Bank B byte offset of the page currently buffered in
 *        @p writer.
 *
 * `bytes_written` includes the bytes still sitting in `page_buffer`
 * (`buffered_bytes`), so subtracting them back out yields the offset where
 * that buffered page actually starts once flushed.
 *
 * @param writer Writer whose current page offset is computed. Must not be
 *        NULL (caller-guaranteed; not checked here).
 * @return Byte offset within Bank B of the start of the buffered page.
 */
static uint32_t ota_flash_bank_b_offset_for_writer(const ota_flash_writer_t *writer)
{
    return (uint32_t)(writer->bytes_written - writer->buffered_bytes);
}

/**
 * @brief Rounds @p value up to the next multiple of @p alignment.
 *
 * Used to compute the number of Flash erase-block-aligned bytes needed to
 * cover an arbitrary image size.
 *
 * @param value Value to round up.
 * @param alignment Alignment to round to; must be non-zero.
 * @return @p value rounded up to a multiple of @p alignment, or 0 if
 *         @p alignment is 0.
 */
static uint32_t ota_flash_round_up(uint32_t value, uint32_t alignment)
{
    if (alignment == 0u)
    {
        return 0u;
    }

    return (uint32_t)(((value + alignment - 1u) / alignment) * alignment);
}

/**
 * @brief Checks whether a `[offset, offset + size)` byte range lies
 *        entirely within Bank B's configured capacity.
 *
 * Written to avoid overflow: computed as
 * `offset <= BANK_B_SIZE - size` rather than `offset + size <= BANK_B_SIZE`,
 * so it cannot wrap around for very large @p offset/@p size.
 *
 * @param offset Candidate start offset within Bank B.
 * @param size Candidate range length in bytes.
 * @return true if the whole range fits within Bank B; false otherwise
 *         (including when @p size alone exceeds Bank B's capacity).
 */
static bool ota_flash_range_is_within_bank_b(uint32_t offset, size_t size)
{
    return offset <= OTA_LAYOUT_BANK_B_SIZE_BYTES &&
           size <= OTA_LAYOUT_BANK_B_SIZE_BYTES &&
           offset <= (OTA_LAYOUT_BANK_B_SIZE_BYTES - (uint32_t)size);
}

/**
 * @brief Returns a read-only XIP-mapped pointer into Bank B at @p offset.
 *
 * @param offset Byte offset within Bank B (not validated here — callers
 *        must bounds-check first, e.g. via `ota_flash_range_is_within_bank_b()`).
 * @return Pointer into memory-mapped Flash (XIP address space); dereferencing
 *         it performs a direct Flash read, not a RAM access.
 */
static const uint8_t *ota_flash_bank_b_pointer(uint32_t offset)
{
    return (const uint8_t *)(OTA_LAYOUT_BANK_B_ORIGIN + offset);
}

/**
 * @brief Flushes the writer's currently buffered page to Bank B.
 *
 * For a non-final page the buffer must be exactly one full Flash program
 * page (`OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES`); for the final page a short
 * buffer is padded with `0xFF` up to a full page before programming, since
 * Flash can only be programmed in whole pages. A zero-length buffer is a
 * no-op success (nothing to flush). On any failure path the caller
 * (`ota_flash_writer_write()`/`ota_flash_writer_finish()`) is responsible
 * for marking the writer inactive; this function only reports the status.
 *
 * @param writer Writer whose buffered page should be flushed. Must be
 *        non-NULL and active.
 * @param final_page true if this is the last page of the image (allows a
 *        short, padded final page); false requires a full page.
 * @return `OTA_FLASH_STATUS_OK` on success (including the no-op empty-buffer
 *         case); `OTA_FLASH_STATUS_WRITER_STATE_ERROR` if @p writer is
 *         NULL/inactive, or if a non-final page is short;
 *         `OTA_FLASH_STATUS_OUT_OF_RANGE` if the resulting page falls
 *         outside Bank B; otherwise propagates `ota_flash_backend_program()`
 *         failures.
 */
static ota_flash_status_t ota_flash_flush_writer_page(ota_flash_writer_t *writer, bool final_page)
{
    uint32_t page_offset;
    size_t page_size;

    if (writer == NULL || !writer->active)
    {
        return OTA_FLASH_STATUS_WRITER_STATE_ERROR;
    }

    page_offset = ota_flash_bank_b_offset_for_writer(writer);
    page_size = writer->buffered_bytes;

    if (page_size == 0u)
    {
        return OTA_FLASH_STATUS_OK;
    }

    if (!final_page && page_size != OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES)
    {
        return OTA_FLASH_STATUS_WRITER_STATE_ERROR;
    }

    if (final_page && page_size < OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES)
    {
        memset(&writer->page_buffer[page_size],
               0xFF,
               OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES - page_size);
        page_size = OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES;
    }

    if (!ota_flash_range_is_within_bank_b(page_offset, page_size))
    {
        return OTA_FLASH_STATUS_OUT_OF_RANGE;
    }

    return ota_flash_backend_program(OTA_LAYOUT_BANK_B_OFFSET_BYTES + page_offset,
                                     writer->page_buffer,
                                     page_size);
}

bool ota_flash_read_state(ota_state_record_t *out_record)
{
    const ota_state_record_t *record = (const ota_state_record_t *)OTA_LAYOUT_STATE_ORIGIN;

    if (out_record == NULL)
    {
        return false;
    }

    memcpy(out_record, record, sizeof(*out_record));
    return ota_state_validate(out_record);
}

/* Erase-then-program: the full sector is erased before the new record is
 * programmed, so a power loss between the two steps leaves the state region
 * erased (invalid) rather than reverted to the previous record. */
ota_flash_status_t ota_flash_write_state(const ota_state_record_t *record)
{
    ota_state_record_t local_record;
    ota_flash_status_t status;

    if (record == NULL)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    local_record = *record;
    ota_state_finalize(&local_record);

    memset(g_ota_state_sector_buffer, 0xFF, sizeof(g_ota_state_sector_buffer));
    memcpy(g_ota_state_sector_buffer, &local_record, sizeof(local_record));

    status = ota_flash_backend_erase(OTA_LAYOUT_STATE_OFFSET_BYTES,
                                     OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES);
    if (status != OTA_FLASH_STATUS_OK)
    {
        return status;
    }

    return ota_flash_backend_program(OTA_LAYOUT_STATE_OFFSET_BYTES,
                                     g_ota_state_sector_buffer,
                                     OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES);
}

ota_flash_status_t ota_flash_reset_state(void)
{
    ota_state_record_t record;

    ota_state_reset_record(&record);
    return ota_flash_write_state(&record);
}

ota_flash_status_t ota_flash_prepare_bank_b(uint32_t image_size)
{
    const uint32_t erase_bytes = ota_flash_round_up(image_size, OTA_LAYOUT_FLASH_ERASE_SIZE_BYTES);

    if (image_size == 0u || image_size > OTA_LAYOUT_BANK_B_SIZE_BYTES)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    return ota_flash_backend_erase(OTA_LAYOUT_BANK_B_OFFSET_BYTES, erase_bytes);
}

ota_flash_status_t ota_flash_program_bank_b(uint32_t image_offset, const void *data, size_t size)
{
    if (data == NULL || size == 0u)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    if ((image_offset % OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) != 0u ||
        (size % OTA_LAYOUT_FLASH_PROGRAM_SIZE_BYTES) != 0u)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    if (!ota_flash_range_is_within_bank_b(image_offset, size))
    {
        return OTA_FLASH_STATUS_OUT_OF_RANGE;
    }

    return ota_flash_backend_program(OTA_LAYOUT_BANK_B_OFFSET_BYTES + image_offset,
                                     (const uint8_t *)data,
                                     size);
}

ota_flash_status_t ota_flash_verify_bank_b(uint32_t image_size, uint32_t expected_crc32)
{
    const uint8_t *image;

    if (image_size == 0u || image_size > OTA_LAYOUT_BANK_B_SIZE_BYTES)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    image = ota_flash_bank_b_pointer(0u);
    if (ota_crc32_compute(image, image_size) != expected_crc32)
    {
        return OTA_FLASH_STATUS_VERIFY_FAILED;
    }

    return OTA_FLASH_STATUS_OK;
}

void ota_flash_writer_init(ota_flash_writer_t *writer)
{
    if (writer == NULL)
    {
        return;
    }

    memset(writer, 0, sizeof(*writer));
}

/* Discards any previous session's OTA state (reset to IDLE) before erasing
 * Bank B, so an interrupted begin() cannot leave a stale STAGED/COPYING
 * record pointing at a partially-erased Bank B. */
ota_flash_status_t ota_flash_writer_begin(ota_flash_writer_t *writer, uint32_t image_size)
{
    ota_flash_status_t status;

    if (writer == NULL || image_size == 0u || image_size > OTA_LAYOUT_BANK_B_SIZE_BYTES)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    ota_flash_writer_init(writer);
    status = ota_flash_reset_state();
    if (status != OTA_FLASH_STATUS_OK)
    {
        return status;
    }

    status = ota_flash_prepare_bank_b(image_size);
    if (status != OTA_FLASH_STATUS_OK)
    {
        return status;
    }

    writer->image_size = image_size;
    writer->active = true;
    memset(writer->page_buffer, 0xFF, sizeof(writer->page_buffer));
    return OTA_FLASH_STATUS_OK;
}

ota_flash_status_t ota_flash_writer_write(ota_flash_writer_t *writer, const void *data, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)data;

    if (writer == NULL || !writer->active || data == NULL || size == 0u)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    if (writer->bytes_written + size > writer->image_size)
    {
        return OTA_FLASH_STATUS_OUT_OF_RANGE;
    }

    while (size > 0u)
    {
        const size_t space = sizeof(writer->page_buffer) - writer->buffered_bytes;
        const size_t chunk = size < space ? size : space;

        memcpy(&writer->page_buffer[writer->buffered_bytes], cursor, chunk);
        writer->buffered_bytes += chunk;
        writer->bytes_written += (uint32_t)chunk;
        cursor += chunk;
        size -= chunk;

        if (writer->buffered_bytes == sizeof(writer->page_buffer))
        {
            ota_flash_status_t flush_status = ota_flash_flush_writer_page(writer, false);
            if (flush_status != OTA_FLASH_STATUS_OK)
            {
                writer->active = false;
                return flush_status;
            }

            writer->buffered_bytes = 0u;
            memset(writer->page_buffer, 0xFF, sizeof(writer->page_buffer));
        }
    }

    return OTA_FLASH_STATUS_OK;
}

ota_flash_status_t ota_flash_writer_finish(ota_flash_writer_t *writer,
                                           uint32_t expected_crc32,
                                           uint32_t version_number)
{
    ota_flash_status_t status;
    ota_state_record_t record;

    if (writer == NULL || !writer->active)
    {
        return OTA_FLASH_STATUS_INVALID_ARGUMENT;
    }

    if (writer->bytes_written != writer->image_size)
    {
        writer->active = false;
        return OTA_FLASH_STATUS_WRITER_STATE_ERROR;
    }

    if (writer->buffered_bytes > 0u)
    {
        status = ota_flash_flush_writer_page(writer, true);
        if (status != OTA_FLASH_STATUS_OK)
        {
            writer->active = false;
            return status;
        }
    }

    status = ota_flash_verify_bank_b(writer->image_size, expected_crc32);
    if (status != OTA_FLASH_STATUS_OK)
    {
        writer->active = false;
        return status;
    }

    ota_state_reset_record(&record);
    record.state = OTA_IMAGE_STATE_STAGED_IN_BANK_B;
    record.image_size = writer->image_size;
    record.image_crc32 = expected_crc32;
    record.staged_bank_offset = OTA_LAYOUT_BANK_B_OFFSET_BYTES;
    record.staged_bank_size = OTA_LAYOUT_BANK_B_SIZE_BYTES;
    record.version_number = version_number;
    record.boot_attempts = 0u;
    record.last_error = OTA_IMAGE_ERROR_NONE;

    status = ota_flash_write_state(&record);
    writer->active = false;
    return status;
}

const char *ota_flash_status_to_string(ota_flash_status_t status)
{
    switch (status)
    {
    case OTA_FLASH_STATUS_OK:
        return "OK";
    case OTA_FLASH_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case OTA_FLASH_STATUS_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case OTA_FLASH_STATUS_FLASH_OP_FAILED:
        return "FLASH_OP_FAILED";
    case OTA_FLASH_STATUS_VERIFY_FAILED:
        return "VERIFY_FAILED";
    case OTA_FLASH_STATUS_WRITER_STATE_ERROR:
        return "WRITER_STATE_ERROR";
    default:
        return "UNKNOWN";
    }
}

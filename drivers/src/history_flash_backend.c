#include "drivers/history_flash_backend.h"

#include <string.h>

#include "services/maintenance_journal_codec.h"

#if defined(PICO_ON_DEVICE)
#include "drivers/flash_operation_coordinator.h"
#include "ota_layout.h"
#endif

/*
 * Record layout (32 bytes total, unchanged since the byte budget is frozen
 * -- see docs/releases/v0.4.3.md T3 and HISTORY_FLASH_BACKEND_RECORD_SIZE's
 * _Static_assert chain in the header). Widening channel_id_hash from 1 byte
 * to 4 bytes (see the header's "Channel identity binding" comment) required
 * finding 3 bytes elsewhere within the fixed 32-byte record: SEQUENCE
 * narrows from 64 to 32 bits.  This backend already compares sequence
 * numbers with plain unsigned '>' (no modular/serial-wraparound handling,
 * unlike drivers/maintenance_journal_backend.c's serial_newer()), so a
 * 32-bit counter changes nothing about that comparison logic -- it only
 * changes the number of appends before wraparound, from practically
 * unbounded to ~4.29 billion per channel (physical-slot lifetime), which is
 * decades of continuous operation even at a sustained 10 Hz record rate and
 * is judged an acceptable trade against the identity-collision risk this
 * change closes.
 *
 * The freed bytes also fund an explicit 1-byte RECORD_FORMAT_VERSION field
 * at the exact offset the pre-widening layout always wrote as its fixed
 * 0xA5 RECORD_MARKER_VALID byte (see below): this makes the format bump
 * self-protecting without needing any additional bytes, and follows the
 * same "explicit schema/version field embedded in the record, checked on
 * decode" precedent as MAINTENANCE_JOURNAL_SCHEMA_VERSION /
 * RECORD_OFFSET_SCHEMA in maintenance_journal_backend.c.
 */
enum
{
    RECORD_OFFSET_SEQUENCE = 0u,     /* 4 bytes, uint32 (was 8 bytes/uint64) */
    RECORD_OFFSET_TIMESTAMP = 4u,    /* 8 bytes */
    RECORD_OFFSET_RAW_VALUE = 12u,   /* 8 bytes */
    RECORD_OFFSET_VALUE_TYPE = 20u,  /* 1 byte */
    RECORD_OFFSET_QUALITY = 21u,     /* 1 byte */
    RECORD_OFFSET_HASH = 22u,        /* 4 bytes, uint32 (was 1 byte) */
    RECORD_OFFSET_MARKER = 26u,      /* 1 byte */
    RECORD_OFFSET_FORMAT = 27u,      /* 1 byte -- the old layout's fixed marker offset */
    RECORD_OFFSET_CRC = 28u,         /* 4 bytes */
    RECORD_CRC_COVERED_SIZE = 28u
};

#define RECORD_MARKER_VALID 0xA5u
#define RECORD_MARKER_ERASED 0xFFu

/* Bumped from the implicit pre-widening layout (8-bit channel_id_hash,
 * 64-bit sequence, no format byte) to this 32-bit-hash/32-bit-sequence
 * layout. Deliberately != RECORD_MARKER_VALID (0xA5) and != 0xFF (erased):
 * every record written by the pre-widening backend always has 0xA5 at this
 * exact byte offset (its old RECORD_OFFSET_MARKER), so decode_record()'s
 * format check below is a 100% reliable, deterministic rejection of any
 * old-format record -- not merely a probabilistic one -- because it does
 * not depend on the CRC (which is computed over the same physical byte
 * range in both layouts and therefore still validates for genuine
 * old-format data) or on the widened hash coincidentally mismatching. */
#define RECORD_FORMAT_VERSION 0x02u

typedef enum
{
    RECORD_STATE_BLANK = 0,
    RECORD_STATE_VALID,
    RECORD_STATE_CORRUPT
} record_state_t;

/* Only the one-page-at-a-time read scratch stays static internal SRAM (256
 * bytes, negligible); the much larger per-channel scan working set
 * (history_flash_backend_scan_entry_t[], ~40 bytes x up to 256 entries) is
 * supplied by the caller -- see history_flash_backend_restore()'s doc
 * comment in the header for why. */
static uint8_t g_restore_page[HISTORY_FLASH_BACKEND_PAGE_SIZE];

static void write_u32(uint8_t *p, uint32_t v)
{
    for (size_t i = 0u; i < 4u; ++i)
    {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t v = 0u;
    for (size_t i = 0u; i < 4u; ++i)
    {
        v |= (uint32_t)p[i] << (8u * i);
    }
    return v;
}

static void write_u64(uint8_t *p, uint64_t v)
{
    for (size_t i = 0u; i < 8u; ++i)
    {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint64_t read_u64(const uint8_t *p)
{
    uint64_t v = 0u;
    for (size_t i = 0u; i < 8u; ++i)
    {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

/* Full 32-bit FNV-1a hash of channel_id, unfolded (previous revisions of
 * this backend XOR-folded this down to 8 bits for channel_id_hash; the
 * fold is gone, not the hash primitive -- see the header's "Channel
 * identity binding" comment for why the false-accept rate this bounds
 * matters). */
static uint32_t hash_channel_id(const char *channel_id)
{
    uint32_t hash = 2166136261u;
    const char *p = channel_id;

    if (p == NULL)
    {
        return 0u;
    }
    while (*p != '\0')
    {
        hash ^= (uint8_t)(*p);
        hash *= 16777619u;
        ++p;
    }
    return hash;
}

static uint64_t pack_raw_value(const history_flash_backend_value_t *value,
                               telemetry_channel_value_type_t type)
{
    switch (type)
    {
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    {
        uint32_t bits = 0u;
        memcpy(&bits, &value->f32, sizeof(bits));
        return (uint64_t)bits;
    }
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
    {
        uint64_t bits = 0u;
        memcpy(&bits, &value->f64, sizeof(bits));
        return bits;
    }
    case TELEMETRY_CHANNEL_VALUE_INT32:
        return (uint64_t)(uint32_t)value->i32;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        return (uint64_t)value->u32;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        return (uint64_t)value->i64;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        return value->u64;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return value->boolean ? 1u : 0u;
    default:
        return 0u;
    }
}

static void unpack_raw_value(uint64_t raw, telemetry_channel_value_type_t type,
                             history_flash_backend_value_t *out_value)
{
    memset(out_value, 0, sizeof(*out_value));
    switch (type)
    {
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    {
        uint32_t bits = (uint32_t)raw;
        memcpy(&out_value->f32, &bits, sizeof(out_value->f32));
        break;
    }
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        memcpy(&out_value->f64, &raw, sizeof(out_value->f64));
        break;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        out_value->i32 = (int32_t)(uint32_t)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        out_value->u32 = (uint32_t)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        out_value->i64 = (int64_t)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        out_value->u64 = raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        out_value->boolean = raw != 0u;
        break;
    default:
        break;
    }
}

static void encode_record(const history_flash_backend_record_t *record, uint32_t sequence,
                          uint32_t channel_id_hash, uint8_t out[HISTORY_FLASH_BACKEND_RECORD_SIZE])
{
    memset(out, 0xFF, HISTORY_FLASH_BACKEND_RECORD_SIZE);
    write_u32(&out[RECORD_OFFSET_SEQUENCE], sequence);
    write_u64(&out[RECORD_OFFSET_TIMESTAMP], record->source_timestamp_unix_us);
    write_u64(&out[RECORD_OFFSET_RAW_VALUE], pack_raw_value(&record->value, record->value_type));
    out[RECORD_OFFSET_VALUE_TYPE] = (uint8_t)record->value_type;
    out[RECORD_OFFSET_QUALITY] = (uint8_t)record->quality;
    write_u32(&out[RECORD_OFFSET_HASH], channel_id_hash);
    out[RECORD_OFFSET_MARKER] = RECORD_MARKER_VALID;
    out[RECORD_OFFSET_FORMAT] = RECORD_FORMAT_VERSION;
    write_u32(&out[RECORD_OFFSET_CRC], maintenance_journal_codec_crc32(out, RECORD_CRC_COVERED_SIZE));
}

static record_state_t decode_record(const uint8_t in[HISTORY_FLASH_BACKEND_RECORD_SIZE],
                                    uint32_t expected_hash, uint32_t *out_sequence,
                                    history_flash_backend_record_t *out_record)
{
    if (in[RECORD_OFFSET_MARKER] == RECORD_MARKER_ERASED)
    {
        for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_RECORD_SIZE; ++i)
        {
            if (in[i] != RECORD_MARKER_ERASED)
            {
                return RECORD_STATE_CORRUPT;
            }
        }
        return RECORD_STATE_BLANK;
    }
    if (in[RECORD_OFFSET_MARKER] != RECORD_MARKER_VALID)
    {
        return RECORD_STATE_CORRUPT;
    }
    if (read_u32(&in[RECORD_OFFSET_CRC]) != maintenance_journal_codec_crc32(in, RECORD_CRC_COVERED_SIZE))
    {
        return RECORD_STATE_CORRUPT;
    }
    if (in[RECORD_OFFSET_FORMAT] != RECORD_FORMAT_VERSION)
    {
        /* Either a genuinely unknown future format, or -- the case this
         * exists to guarantee -- a record written by the pre-widening
         * backend, whose CRC is byte-range-identical and would otherwise
         * still validate under the new field layout. Never decode it under
         * the current schema. */
        return RECORD_STATE_CORRUPT;
    }
    if (read_u32(&in[RECORD_OFFSET_HASH]) != expected_hash)
    {
        /* CRC-consistent and current-format but belongs to a different
         * channel identity than currently bound to this physical slot (e.g.
         * configuration changed across reboot). Never expose it as this
         * channel's history. */
        return RECORD_STATE_CORRUPT;
    }
    if (out_sequence != NULL)
    {
        *out_sequence = read_u32(&in[RECORD_OFFSET_SEQUENCE]);
    }
    if (out_record != NULL)
    {
        out_record->source_timestamp_unix_us = read_u64(&in[RECORD_OFFSET_TIMESTAMP]);
        out_record->value_type = (telemetry_channel_value_type_t)in[RECORD_OFFSET_VALUE_TYPE];
        out_record->quality = (service_quality_t)in[RECORD_OFFSET_QUALITY];
        unpack_raw_value(read_u64(&in[RECORD_OFFSET_RAW_VALUE]), out_record->value_type, &out_record->value);
    }
    return RECORD_STATE_VALID;
}

static bool page_is_blank(const uint8_t page[HISTORY_FLASH_BACKEND_PAGE_SIZE])
{
    for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_PAGE_SIZE; ++i)
    {
        if (page[i] != RECORD_MARKER_ERASED)
        {
            return false;
        }
    }
    return true;
}

void history_flash_backend_init(history_flash_backend_t *backend,
                                const history_flash_backend_storage_t *storage)
{
    if (backend == NULL)
    {
        return;
    }
    memset(backend, 0, sizeof(*backend));
    if (storage != NULL)
    {
        backend->storage = *storage;
    }
    backend->ready = backend->storage.read != NULL && backend->storage.program_page != NULL &&
                     backend->storage.erase_sector != NULL;
}

history_flash_backend_result_t history_flash_backend_bind_channel(history_flash_backend_t *backend,
                                                                   uint8_t slot,
                                                                   const char *channel_id)
{
    history_flash_backend_channel_state_t *state;

    if (backend == NULL || slot >= HISTORY_FLASH_BACKEND_CHANNEL_COUNT)
    {
        return HISTORY_FLASH_BACKEND_INVALID_ARGUMENT;
    }
    state = &backend->channels[slot];
    memset(state, 0, sizeof(*state));
    memset(state->staging_page, 0xFF, sizeof(state->staging_page));

    if (channel_id == NULL || channel_id[0] == '\0')
    {
        state->bound = false;
        return HISTORY_FLASH_BACKEND_OK;
    }

    state->bound = true;
    state->channel_id_hash = hash_channel_id(channel_id);
    state->active_sector = 0u;
    state->next_page_in_sector = 0u;
    state->staging_count = 0u;
    state->next_sequence = 0u;
    return HISTORY_FLASH_BACKEND_OK;
}

static bool read_page(history_flash_backend_t *backend, uint32_t offset,
                      uint8_t out[HISTORY_FLASH_BACKEND_PAGE_SIZE])
{
    return backend->storage.read(backend->storage.context, offset, out, HISTORY_FLASH_BACKEND_PAGE_SIZE);
}

history_flash_backend_result_t history_flash_backend_restore(history_flash_backend_t *backend,
                                                              uint8_t slot,
                                                              history_flash_backend_scan_entry_t *scratch,
                                                              size_t scratch_capacity,
                                                              history_flash_backend_restore_cb callback,
                                                              void *user_context)
{
    history_flash_backend_channel_state_t *state;
    uint32_t channel_base;
    uint8_t pages_used[HISTORY_FLASH_BACKEND_SECTORS_PER_CHANNEL] = {0};
    uint32_t sector_max_sequence[HISTORY_FLASH_BACKEND_SECTORS_PER_CHANNEL] = {0};
    bool sector_has_sequence[HISTORY_FLASH_BACKEND_SECTORS_PER_CHANNEL] = {false, false};
    size_t entry_count = 0u;
    int sector_to_drop = -1;
    uint32_t max_replayed_sequence = 0u;
    bool has_replayed_sequence = false;

    if (backend == NULL || !backend->ready || slot >= HISTORY_FLASH_BACKEND_CHANNEL_COUNT ||
        scratch == NULL || scratch_capacity < HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL)
    {
        return HISTORY_FLASH_BACKEND_INVALID_ARGUMENT;
    }
    state = &backend->channels[slot];
    if (!state->bound)
    {
        return HISTORY_FLASH_BACKEND_NOT_BOUND;
    }
    channel_base = (uint32_t)slot * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES;

    for (uint8_t sector = 0u; sector < HISTORY_FLASH_BACKEND_SECTORS_PER_CHANNEL; ++sector)
    {
        const uint32_t sector_base = channel_base + (uint32_t)sector * HISTORY_FLASH_BACKEND_SECTOR_SIZE;
        bool stop_counting = false;

        for (uint8_t page = 0u; page < HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR; ++page)
        {
            const uint32_t page_offset = sector_base + (uint32_t)page * HISTORY_FLASH_BACKEND_PAGE_SIZE;

            if (!read_page(backend, page_offset, g_restore_page))
            {
                return HISTORY_FLASH_BACKEND_IO;
            }
            if (page_is_blank(g_restore_page))
            {
                stop_counting = true;
                break;
            }
            if (!stop_counting)
            {
                pages_used[sector] = (uint8_t)(page + 1u);
            }
            for (uint8_t record_index = 0u; record_index < HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE; ++record_index)
            {
                const uint8_t *record_bytes = &g_restore_page[(size_t)record_index * HISTORY_FLASH_BACKEND_RECORD_SIZE];
                uint32_t sequence = 0u;
                history_flash_backend_record_t decoded = {0};
                const record_state_t decoded_state =
                    decode_record(record_bytes, state->channel_id_hash, &sequence, &decoded);

                if (decoded_state != RECORD_STATE_VALID)
                {
                    continue;
                }
                if (!sector_has_sequence[sector] || sequence > sector_max_sequence[sector])
                {
                    sector_max_sequence[sector] = sequence;
                    sector_has_sequence[sector] = true;
                }
                if (entry_count < scratch_capacity)
                {
                    scratch[entry_count].sequence = sequence;
                    scratch[entry_count].sector = sector;
                    scratch[entry_count].record = decoded;
                    ++entry_count;
                }
            }
        }
    }

    {
        const bool full0 = pages_used[0] >= HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR;
        const bool full1 = pages_used[1] >= HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR;

        if (full0 && full1)
        {
            /* Interrupted swap: the lower-sequence sector is the stale
             * generation that should already have been erased. */
            const uint8_t stale_sector = (sector_max_sequence[0] <= sector_max_sequence[1]) ? 0u : 1u;

            sector_to_drop = (int)stale_sector;
            state->active_sector = stale_sector;
            state->next_page_in_sector = 0u;
        }
        else if (full0 != full1)
        {
            const uint8_t other = full0 ? 1u : 0u;

            state->active_sector = other;
            state->next_page_in_sector = pages_used[other];
        }
        else if (pages_used[0] == 0u && pages_used[1] == 0u)
        {
            state->active_sector = 0u;
            state->next_page_in_sector = 0u;
        }
        else
        {
            uint8_t active = (pages_used[0] >= pages_used[1]) ? 0u : 1u;

            if (pages_used[0] == pages_used[1])
            {
                active = (sector_max_sequence[0] >= sector_max_sequence[1]) ? 0u : 1u;
            }
            state->active_sector = active;
            state->next_page_in_sector = pages_used[active];
        }
    }

    /* Bounded insertion sort (<=256 entries, run once at startup). */
    for (size_t i = 1u; i < entry_count; ++i)
    {
        history_flash_backend_scan_entry_t item = scratch[i];
        size_t j = i;

        while (j > 0u && scratch[j - 1u].sequence > item.sequence)
        {
            scratch[j] = scratch[j - 1u];
            --j;
        }
        scratch[j] = item;
    }

    for (size_t i = 0u; i < entry_count; ++i)
    {
        if ((int)scratch[i].sector == sector_to_drop)
        {
            continue;
        }
        if (callback != NULL)
        {
            callback(user_context, &scratch[i].record);
        }
        max_replayed_sequence = scratch[i].sequence;
        has_replayed_sequence = true;
    }

    state->next_sequence = has_replayed_sequence ? max_replayed_sequence + 1u : 0u;
    state->staging_count = 0u;
    memset(state->staging_page, 0xFF, sizeof(state->staging_page));

    if (sector_to_drop >= 0)
    {
        const uint32_t drop_sector_base =
            channel_base + (uint32_t)sector_to_drop * HISTORY_FLASH_BACKEND_SECTOR_SIZE;

        if (backend->storage.erase_sector == NULL ||
            !backend->storage.erase_sector(backend->storage.context, drop_sector_base))
        {
            return HISTORY_FLASH_BACKEND_IO;
        }
    }

    return HISTORY_FLASH_BACKEND_OK;
}

static history_flash_backend_result_t flush_staging_if_full(history_flash_backend_t *backend, uint8_t slot)
{
    history_flash_backend_channel_state_t *state = &backend->channels[slot];
    uint32_t channel_base;
    uint32_t sector_base;
    uint32_t page_offset;

    if (state->staging_count < HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE)
    {
        return HISTORY_FLASH_BACKEND_OK;
    }
    if (state->next_page_in_sector >= HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR)
    {
        /* A previous sector swap (erase) failed; refuse further writes for
         * this channel until it is retried successfully. */
        return HISTORY_FLASH_BACKEND_IO;
    }

    channel_base = (uint32_t)slot * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES;
    sector_base = channel_base + (uint32_t)state->active_sector * HISTORY_FLASH_BACKEND_SECTOR_SIZE;
    page_offset = sector_base + (uint32_t)state->next_page_in_sector * HISTORY_FLASH_BACKEND_PAGE_SIZE;

    if (!backend->storage.program_page(backend->storage.context, page_offset, state->staging_page))
    {
        return HISTORY_FLASH_BACKEND_IO;
    }

    memset(state->staging_page, 0xFF, sizeof(state->staging_page));
    state->staging_count = 0u;
    state->next_page_in_sector++;

    if (state->next_page_in_sector >= HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR)
    {
        const uint8_t next_active = (uint8_t)(1u - state->active_sector);
        const uint32_t next_sector_base = channel_base + (uint32_t)next_active * HISTORY_FLASH_BACKEND_SECTOR_SIZE;

        if (!backend->storage.erase_sector(backend->storage.context, next_sector_base))
        {
            /* Leave next_page_in_sector at the out-of-range sentinel so a
             * later append retries the erase before accepting more writes;
             * the just-filled sector remains valid and readable meanwhile. */
            return HISTORY_FLASH_BACKEND_IO;
        }
        state->active_sector = next_active;
        state->next_page_in_sector = 0u;
    }

    return HISTORY_FLASH_BACKEND_OK;
}

history_flash_backend_result_t history_flash_backend_append(history_flash_backend_t *backend,
                                                             uint8_t slot,
                                                             const history_flash_backend_record_t *record)
{
    history_flash_backend_channel_state_t *state;
    size_t offset_in_page;
    uint8_t encoded[HISTORY_FLASH_BACKEND_RECORD_SIZE];

    if (backend == NULL || !backend->ready || slot >= HISTORY_FLASH_BACKEND_CHANNEL_COUNT || record == NULL)
    {
        return HISTORY_FLASH_BACKEND_INVALID_ARGUMENT;
    }
    state = &backend->channels[slot];
    if (!state->bound)
    {
        return HISTORY_FLASH_BACKEND_NOT_BOUND;
    }

    if (flush_staging_if_full(backend, slot) != HISTORY_FLASH_BACKEND_OK)
    {
        /* Accepted data loss: this newest sample is not persisted to Flash
         * because a previous page is still stuck; the PSRAM ring (owned by
         * services/history_service.c) already retains it in RAM. */
        return HISTORY_FLASH_BACKEND_IO;
    }

    offset_in_page = (size_t)state->staging_count * HISTORY_FLASH_BACKEND_RECORD_SIZE;
    encode_record(record, state->next_sequence, state->channel_id_hash, encoded);
    memcpy(&state->staging_page[offset_in_page], encoded, HISTORY_FLASH_BACKEND_RECORD_SIZE);
    state->next_sequence++;
    state->staging_count++;

    (void)flush_staging_if_full(backend, slot);
    return HISTORY_FLASH_BACKEND_OK;
}

#if defined(PICO_ON_DEVICE)
static bool history_flash_read(void *unused, uint32_t offset, uint8_t *dst, size_t n)
{
    (void)unused;
    if (offset > OTA_LAYOUT_HISTORY_SIZE_BYTES || n > OTA_LAYOUT_HISTORY_SIZE_BYTES - offset)
    {
        return false;
    }
    memcpy(dst, (const void *)(uintptr_t)(OTA_LAYOUT_HISTORY_ORIGIN + offset), n);
    return true;
}

static bool history_flash_program(void *unused, uint32_t offset, const uint8_t page[HISTORY_FLASH_BACKEND_PAGE_SIZE])
{
    (void)unused;
    if (offset > OTA_LAYOUT_HISTORY_SIZE_BYTES - HISTORY_FLASH_BACKEND_PAGE_SIZE)
    {
        return false;
    }
    return flash_operation_program_page(OTA_LAYOUT_HISTORY_OFFSET_BYTES + offset, page) == FLASH_OPERATION_OK;
}

static bool history_flash_erase(void *unused, uint32_t offset)
{
    (void)unused;
    if (offset > OTA_LAYOUT_HISTORY_SIZE_BYTES - HISTORY_FLASH_BACKEND_SECTOR_SIZE)
    {
        return false;
    }
    return flash_operation_erase_sector(OTA_LAYOUT_HISTORY_OFFSET_BYTES + offset) == FLASH_OPERATION_OK;
}

void history_flash_backend_init_flash(history_flash_backend_t *backend)
{
    const history_flash_backend_storage_t storage = {
        .context = NULL,
        .read = history_flash_read,
        .program_page = history_flash_program,
        .erase_sector = history_flash_erase,
    };
    history_flash_backend_init(backend, &storage);
}
#else
void history_flash_backend_init_flash(history_flash_backend_t *backend)
{
    history_flash_backend_init(backend, NULL);
}
#endif

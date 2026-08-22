#include "drivers/history_flash_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "services/maintenance_journal_codec.h"

/* NOR-flash-realistic mock: program can only clear bits (AND semantics);
 * erase sets a whole 4 KiB sector back to 0xFF. This is large enough to
 * cover every one of the 16 physical channel slots so offset math across
 * slots is exercised, not just slot 0. */
static uint8_t g_flash[HISTORY_FLASH_BACKEND_TOTAL_SIZE_BYTES];

static void reset_flash(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
}

static bool mock_read(void *context, uint32_t offset, uint8_t *destination, size_t size)
{
    (void)context;
    if (offset > sizeof(g_flash) || size > sizeof(g_flash) - offset)
    {
        return false;
    }
    memcpy(destination, &g_flash[offset], size);
    return true;
}

static bool mock_program(void *context, uint32_t offset, const uint8_t page[HISTORY_FLASH_BACKEND_PAGE_SIZE])
{
    (void)context;
    if (offset > sizeof(g_flash) - HISTORY_FLASH_BACKEND_PAGE_SIZE || (offset % HISTORY_FLASH_BACKEND_PAGE_SIZE) != 0u)
    {
        return false;
    }
    for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_PAGE_SIZE; ++i)
    {
        g_flash[offset + i] &= page[i]; /* NOR: only 1 -> 0 transitions */
    }
    return true;
}

static bool mock_erase(void *context, uint32_t offset)
{
    (void)context;
    if (offset > sizeof(g_flash) - HISTORY_FLASH_BACKEND_SECTOR_SIZE ||
        (offset % HISTORY_FLASH_BACKEND_SECTOR_SIZE) != 0u)
    {
        return false;
    }
    memset(&g_flash[offset], 0xFF, HISTORY_FLASH_BACKEND_SECTOR_SIZE);
    return true;
}

static const history_flash_backend_storage_t g_mock_storage = {
    .context = NULL,
    .read = mock_read,
    .program_page = mock_program,
    .erase_sector = mock_erase,
};

/* Host test stand-in for the caller-owned restore scratch buffer that a
 * real caller (services/history_service.c) allocates from PSRAM. */
static history_flash_backend_scan_entry_t g_restore_scratch[HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL];

typedef struct
{
    history_flash_backend_record_t records[HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL];
    size_t count;
} replay_capture_t;

static void capture_replay(void *user_context, const history_flash_backend_record_t *record)
{
    replay_capture_t *capture = (replay_capture_t *)user_context;

    assert(capture->count < HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL);
    capture->records[capture->count++] = *record;
}

static history_flash_backend_record_t make_record(telemetry_channel_value_type_t type, uint64_t timestamp,
                                                   uint64_t raw)
{
    history_flash_backend_record_t record;

    memset(&record, 0, sizeof(record));
    record.value_type = type;
    record.quality = SERVICE_QUALITY_GOOD;
    record.source_timestamp_unix_us = timestamp;
    switch (type)
    {
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
        record.value.f32 = (float)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        record.value.f64 = (double)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        record.value.i32 = (int32_t)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        record.value.u32 = (uint32_t)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        record.value.i64 = (int64_t)raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        record.value.u64 = raw;
        break;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        record.value.boolean = raw != 0u;
        break;
    default:
        break;
    }
    return record;
}

static void test_empty_restore(void)
{
    history_flash_backend_t backend;
    replay_capture_t capture = {0};

    reset_flash();
    history_flash_backend_init(&backend, &g_mock_storage);
    assert(backend.ready);
    assert(history_flash_backend_bind_channel(&backend, 0u, "channel.a") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 0u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture) == HISTORY_FLASH_BACKEND_OK);
    assert(capture.count == 0u);
    assert(backend.channels[0].active_sector == 0u);
    assert(backend.channels[0].next_page_in_sector == 0u);
    assert(backend.channels[0].next_sequence == 0u);
}

static void test_mixed_value_types_round_trip(void)
{
    static const telemetry_channel_value_type_t types[HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE] = {
        TELEMETRY_CHANNEL_VALUE_FLOAT,  TELEMETRY_CHANNEL_VALUE_DOUBLE, TELEMETRY_CHANNEL_VALUE_INT32,
        TELEMETRY_CHANNEL_VALUE_UINT32, TELEMETRY_CHANNEL_VALUE_INT64,  TELEMETRY_CHANNEL_VALUE_UINT64,
        TELEMETRY_CHANNEL_VALUE_BOOLEAN, TELEMETRY_CHANNEL_VALUE_FLOAT,
    };
    history_flash_backend_t backend;
    replay_capture_t capture = {0};

    reset_flash();
    history_flash_backend_init(&backend, &g_mock_storage);
    assert(history_flash_backend_bind_channel(&backend, 3u, "channel.mixed") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 3u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture) == HISTORY_FLASH_BACKEND_OK);
    assert(capture.count == 0u);

    /* Exactly one page's worth: nothing flushes to Flash until the 8th
     * append (this is the "batched, not synchronous" write policy). */
    for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE - 1u; ++i)
    {
        const history_flash_backend_record_t record = make_record(types[i], 1000u + i, i + 1u);

        assert(history_flash_backend_append(&backend, 3u, &record) == HISTORY_FLASH_BACKEND_OK);
    }
    assert(g_flash[3u * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES] == 0xFFu); /* still unwritten */

    {
        const history_flash_backend_record_t last =
            make_record(types[HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE - 1u], 1007u, 8u);
        assert(history_flash_backend_append(&backend, 3u, &last) == HISTORY_FLASH_BACKEND_OK);
    }
    assert(g_flash[3u * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES] != 0xFFu); /* page flushed */

    /* Restart: rebind and restore, verify every type/value/timestamp
     * survives in the original order. */
    {
        history_flash_backend_t restarted;
        replay_capture_t restore_capture = {0};

        history_flash_backend_init(&restarted, &g_mock_storage);
        assert(history_flash_backend_bind_channel(&restarted, 3u, "channel.mixed") == HISTORY_FLASH_BACKEND_OK);
        assert(history_flash_backend_restore(&restarted, 3u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &restore_capture) ==
               HISTORY_FLASH_BACKEND_OK);
        assert(restore_capture.count == HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE);
        for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE; ++i)
        {
            assert(restore_capture.records[i].value_type == types[i]);
            assert(restore_capture.records[i].source_timestamp_unix_us == 1000u + i);
        }
        assert(restore_capture.records[0].value.f32 == 1.0f);
        assert(restore_capture.records[1].value.f64 == 2.0);
        assert(restore_capture.records[2].value.i32 == 3);
        assert(restore_capture.records[3].value.u32 == 4u);
        assert(restore_capture.records[4].value.i64 == 5);
        assert(restore_capture.records[5].value.u64 == 6u);
        assert(restore_capture.records[6].value.boolean == true);
        assert(restore_capture.records[7].value.f32 == 8.0f);
        assert(restarted.channels[3].next_page_in_sector == 1u);
        assert(restarted.channels[3].next_sequence == HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE);
    }
}

static void test_sector_wraparound(void)
{
    history_flash_backend_t backend;
    replay_capture_t capture = {0};
    const size_t total_records = HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL + 40u; /* force >1 full swap */

    reset_flash();
    history_flash_backend_init(&backend, &g_mock_storage);
    assert(history_flash_backend_bind_channel(&backend, 7u, "channel.ring") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 7u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture) == HISTORY_FLASH_BACKEND_OK);

    for (size_t i = 0u; i < total_records; ++i)
    {
        const history_flash_backend_record_t record =
            make_record(TELEMETRY_CHANNEL_VALUE_UINT32, 2000u + i, i);
        assert(history_flash_backend_append(&backend, 7u, &record) == HISTORY_FLASH_BACKEND_OK);
    }

    {
        history_flash_backend_t restarted;
        replay_capture_t restore_capture = {0};

        history_flash_backend_init(&restarted, &g_mock_storage);
        assert(history_flash_backend_bind_channel(&restarted, 7u, "channel.ring") == HISTORY_FLASH_BACKEND_OK);
        assert(history_flash_backend_restore(&restarted, 7u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &restore_capture) ==
               HISTORY_FLASH_BACKEND_OK);

        /* Retention degrades to whole-sector granularity: at least one full
         * sector (128 records) and never more than the frozen 256-record
         * capacity are retrievable after repeated swaps. */
        assert(restore_capture.count >= HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR * HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE);
        assert(restore_capture.count <= HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL);
        for (size_t i = 1u; i < restore_capture.count; ++i)
        {
            assert(restore_capture.records[i].source_timestamp_unix_us >
                   restore_capture.records[i - 1u].source_timestamp_unix_us);
        }
        /* The newest record must always survive a swap. */
        assert(restore_capture.records[restore_capture.count - 1u].source_timestamp_unix_us ==
               2000u + total_records - 1u);
    }
}

static void test_torn_record_is_skipped(void)
{
    history_flash_backend_t backend;
    replay_capture_t capture = {0};

    reset_flash();
    history_flash_backend_init(&backend, &g_mock_storage);
    assert(history_flash_backend_bind_channel(&backend, 1u, "channel.torn") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 1u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture) == HISTORY_FLASH_BACKEND_OK);

    for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE; ++i)
    {
        const history_flash_backend_record_t record = make_record(TELEMETRY_CHANNEL_VALUE_INT32, 3000u + i, i);
        assert(history_flash_backend_append(&backend, 1u, &record) == HISTORY_FLASH_BACKEND_OK);
    }

    /* Simulate power loss mid-write corrupting one record's payload bytes
     * (NOR flash can only clear further bits, so flipping a payload byte
     * toward 0x00 is a physically realizable torn-write outcome). Target the
     * first byte of the raw-value field (record-relative offset 12, see
     * RECORD_OFFSET_RAW_VALUE in drivers/src/history_flash_backend.c): for
     * record index 3 the raw value is 3, so that byte is non-zero and
     * clearing it is guaranteed to change the CRC-covered bytes (unlike a
     * fixed offset that could land on an already-zero byte of a
     * variable-width field and silently no-op). */
    {
        const uint32_t channel_base = 1u * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES;
        g_flash[channel_base + 3u * HISTORY_FLASH_BACKEND_RECORD_SIZE + 12u] = 0x00u;
    }

    {
        history_flash_backend_t restarted;
        replay_capture_t restore_capture = {0};

        history_flash_backend_init(&restarted, &g_mock_storage);
        assert(history_flash_backend_bind_channel(&restarted, 1u, "channel.torn") == HISTORY_FLASH_BACKEND_OK);
        assert(history_flash_backend_restore(&restarted, 1u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &restore_capture) ==
               HISTORY_FLASH_BACKEND_OK);

        /* 7 of 8 records survive; the torn one is skipped, never exposed. */
        assert(restore_capture.count == HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE - 1u);
        for (size_t i = 0u; i < restore_capture.count; ++i)
        {
            assert(restore_capture.records[i].source_timestamp_unix_us != 3003u);
        }
    }
}

static void test_foreign_channel_hash_mismatch_is_skipped(void)
{
    history_flash_backend_t backend;
    replay_capture_t capture = {0};

    reset_flash();
    history_flash_backend_init(&backend, &g_mock_storage);
    assert(history_flash_backend_bind_channel(&backend, 2u, "channel.original") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 2u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture) == HISTORY_FLASH_BACKEND_OK);
    for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE; ++i)
    {
        const history_flash_backend_record_t record = make_record(TELEMETRY_CHANNEL_VALUE_UINT32, 4000u + i, i);
        assert(history_flash_backend_append(&backend, 2u, &record) == HISTORY_FLASH_BACKEND_OK);
    }

    /* Configuration changed across reboot: slot 2 is now a different
     * channel. Its old physical data must never be exposed as this
     * channel's history. */
    {
        history_flash_backend_t restarted;
        replay_capture_t restore_capture = {0};

        history_flash_backend_init(&restarted, &g_mock_storage);
        assert(history_flash_backend_bind_channel(&restarted, 2u, "channel.replaced") == HISTORY_FLASH_BACKEND_OK);
        assert(history_flash_backend_restore(&restarted, 2u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &restore_capture) ==
               HISTORY_FLASH_BACKEND_OK);
        assert(restore_capture.count == 0u);
        assert(restarted.channels[2].next_sequence == 0u);
    }
}

static void test_interrupted_swap_recovers_newer_generation(void)
{
    history_flash_backend_t backend;
    replay_capture_t capture = {0};
    const uint32_t channel_base = 5u * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES;

    reset_flash();
    history_flash_backend_init(&backend, &g_mock_storage);
    assert(history_flash_backend_bind_channel(&backend, 5u, "channel.swap") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 5u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture) == HISTORY_FLASH_BACKEND_OK);

    /* Fill sector A completely (128 records), then fill sector B completely
     * too (128 more) without letting the driver's own erase-on-swap run
     * ahead of us: append exactly enough records to complete sector B's
     * last page and stop there, leaving sector A un-erased even though the
     * implementation would normally have erased it already. To simulate a
     * genuinely *interrupted* swap (crash between "B just completed" and
     * "A erased"), manually restore A's bytes after the fact. */
    {
        uint8_t sector_a_snapshot[HISTORY_FLASH_BACKEND_SECTOR_SIZE];
        const size_t total_to_fill_both_sectors =
            HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR * HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE * 2u;

        for (size_t i = 0u; i < total_to_fill_both_sectors; ++i)
        {
            const history_flash_backend_record_t record = make_record(TELEMETRY_CHANNEL_VALUE_UINT32, 5000u + i, i);

            assert(history_flash_backend_append(&backend, 5u, &record) == HISTORY_FLASH_BACKEND_OK);
            if (i == HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR * HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE - 1u)
            {
                /* Sector A just became full and the driver already erased
                 * sector B ahead of use (that erase is idempotent/harmless
                 * to redo). Snapshot A here, before it eventually gets
                 * reclaimed by a later natural swap. */
                memcpy(sector_a_snapshot, &g_flash[channel_base], sizeof(sector_a_snapshot));
            }
        }

        /* At this point the driver has already advanced past both sectors
         * naturally (steady state). Force the specific "interrupted swap"
         * byte pattern: sector A restored to its old, still-fully-valid
         * generation (as if the erase that should have reclaimed it never
         * completed), while sector B holds the newer generation. */
        memcpy(&g_flash[channel_base], sector_a_snapshot, sizeof(sector_a_snapshot));
    }

    {
        history_flash_backend_t restarted;
        replay_capture_t restore_capture = {0};

        history_flash_backend_init(&restarted, &g_mock_storage);
        assert(history_flash_backend_bind_channel(&restarted, 5u, "channel.swap") == HISTORY_FLASH_BACKEND_OK);
        assert(history_flash_backend_restore(&restarted, 5u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &restore_capture) ==
               HISTORY_FLASH_BACKEND_OK);

        /* Only the newer generation (higher sequence numbers) is replayed;
         * the stale, should-have-been-erased sector is dropped and actually
         * erased as part of completing the interrupted swap. */
        assert(restore_capture.count > 0u);
        for (size_t i = 1u; i < restore_capture.count; ++i)
        {
            assert(restore_capture.records[i].source_timestamp_unix_us >
                   restore_capture.records[i - 1u].source_timestamp_unix_us);
        }
        /* The recovered active sector must be freshly erased and ready. */
        assert(restarted.channels[5].next_page_in_sector == 0u);
    }
}

/* Regression coverage for widening channel_id_hash from 8 to 32 bits: a
 * record hand-crafted exactly as the pre-widening backend would have
 * written it must never be decoded as valid under the current layout, even
 * though its CRC32 (computed over the same [0,28) byte range in both
 * layouts) still validates. Only the new RECORD_FORMAT_VERSION byte -- at
 * the exact offset the old layout always wrote as its fixed 0xA5
 * valid-marker byte -- can and does catch this. */
static void test_old_format_record_is_rejected(void)
{
    history_flash_backend_t backend;
    replay_capture_t capture = {0};
    uint8_t old_record[HISTORY_FLASH_BACKEND_RECORD_SIZE];
    uint32_t channel_base;

    reset_flash();
    memset(old_record, 0xFF, sizeof(old_record));
    /* Old layout: RECORD_OFFSET_SEQUENCE = 0, 8 bytes (uint64). */
    for (size_t i = 0u; i < 8u; ++i)
    {
        old_record[0u + i] = (uint8_t)(0x1122334455667788ULL >> (8u * i));
    }
    /* Old layout: RECORD_OFFSET_TIMESTAMP = 8, 8 bytes. */
    for (size_t i = 0u; i < 8u; ++i)
    {
        old_record[8u + i] = (uint8_t)(9000ULL >> (8u * i));
    }
    /* Old layout: RECORD_OFFSET_RAW_VALUE = 16, 8 bytes. */
    for (size_t i = 0u; i < 8u; ++i)
    {
        old_record[16u + i] = (uint8_t)(42ULL >> (8u * i));
    }
    old_record[24] = (uint8_t)TELEMETRY_CHANNEL_VALUE_UINT32; /* old RECORD_OFFSET_VALUE_TYPE */
    old_record[25] = (uint8_t)SERVICE_QUALITY_GOOD;           /* old RECORD_OFFSET_QUALITY */
    /* Old RECORD_OFFSET_HASH (1 byte) deliberately chosen to equal 0xA5:
     * this byte lands on the *new* layout's RECORD_OFFSET_MARKER, so the
     * new marker check spuriously passes too -- proving the format check,
     * not the marker check, is what rejects this record. */
    old_record[26] = 0xA5u;
    old_record[27] = 0xA5u; /* old RECORD_OFFSET_MARKER (RECORD_MARKER_VALID) */
    {
        const uint32_t crc = maintenance_journal_codec_crc32(old_record, 28u);

        for (size_t i = 0u; i < 4u; ++i)
        {
            old_record[28u + i] = (uint8_t)(crc >> (8u * i));
        }
    }

    channel_base = 9u * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES;
    memcpy(&g_flash[channel_base], old_record, sizeof(old_record));

    history_flash_backend_init(&backend, &g_mock_storage);
    assert(history_flash_backend_bind_channel(&backend, 9u, "channel.upgraded") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 9u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture) == HISTORY_FLASH_BACKEND_OK);

    assert(capture.count == 0u);
    assert(backend.channels[9].next_sequence == 0u);
}

/* Regression coverage that channel_id_hash is genuinely a full 32-bit value
 * now, not an 8-bit value zero-extended into a wider field: two distinct
 * channel ids must produce distinct stored hashes, and the upper three
 * bytes of the hash (which an unwidened 8-bit scheme could never populate)
 * must not all be zero. */
static void test_channel_id_hash_uses_full_32_bits(void)
{
    history_flash_backend_t backend;
    replay_capture_t capture_a = {0};
    replay_capture_t capture_b = {0};
    /* RECORD_OFFSET_HASH in drivers/src/history_flash_backend.c. */
    const uint32_t hash_offset = 22u;

    reset_flash();
    history_flash_backend_init(&backend, &g_mock_storage);
    assert(history_flash_backend_bind_channel(&backend, 4u, "channel.hash.a") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_bind_channel(&backend, 6u, "channel.hash.b") == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 4u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture_a) == HISTORY_FLASH_BACKEND_OK);
    assert(history_flash_backend_restore(&backend, 6u, g_restore_scratch, HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL, capture_replay, &capture_b) == HISTORY_FLASH_BACKEND_OK);

    for (size_t i = 0u; i < HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE; ++i)
    {
        const history_flash_backend_record_t record_a = make_record(TELEMETRY_CHANNEL_VALUE_UINT32, 6000u + i, i);
        const history_flash_backend_record_t record_b = make_record(TELEMETRY_CHANNEL_VALUE_UINT32, 7000u + i, i);

        assert(history_flash_backend_append(&backend, 4u, &record_a) == HISTORY_FLASH_BACKEND_OK);
        assert(history_flash_backend_append(&backend, 6u, &record_b) == HISTORY_FLASH_BACKEND_OK);
    }

    {
        const uint32_t base_a = 4u * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES;
        const uint32_t base_b = 6u * HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES;
        const uint8_t *hash_a = &g_flash[base_a + hash_offset];
        const uint8_t *hash_b = &g_flash[base_b + hash_offset];

        assert(memcmp(hash_a, hash_b, 4u) != 0);
        assert(hash_a[1] != 0u || hash_a[2] != 0u || hash_a[3] != 0u);
        assert(hash_b[1] != 0u || hash_b[2] != 0u || hash_b[3] != 0u);
    }
}

int main(void)
{
    test_empty_restore();
    test_mixed_value_types_round_trip();
    test_sector_wraparound();
    test_torn_record_is_skipped();
    test_foreign_channel_hash_mismatch_is_skipped();
    test_interrupted_swap_recovers_newer_generation();
    test_old_format_record_is_rejected();
    test_channel_id_hash_uses_full_32_bits();
    printf("history flash backend host suite: PASS\n");
    return 0;
}

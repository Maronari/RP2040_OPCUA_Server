#include "services/maintenance_journal_codec.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum
{
    TEST_CRC_OFFSET = 730u,
    TEST_COMMIT_OFFSET = 768u,
    TEST_OPERATION_ID_OFFSET = 74u,
    TEST_SUPPLIER_OFFSET = 122u,
    TEST_PERSONNEL_OFFSET = 170u,
    TEST_ACTOR_OFFSET = 218u,
    TEST_RESULT_OFFSET = 250u,
    TEST_SERVICED_OFFSET = 346u,
    TEST_REPLACED_OFFSET = 538u
};

_Static_assert(MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE == 734u,
               "release byte budget changed");
_Static_assert(MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE +
                   MAINTENANCE_JOURNAL_EXTENSION_MARGIN <=
               MAINTENANCE_JOURNAL_RECORD_BODY_SIZE,
               "schema 1 must leave extension bytes erased");
_Static_assert(MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE ==
                   MAINTENANCE_JOURNAL_RECORD_BODY_SIZE +
                       MAINTENANCE_JOURNAL_FLASH_PROGRAM_SIZE,
               "record layout changed");
_Static_assert(MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY +
                   MAINTENANCE_SERVICE_TEXT_CAPACITY * 2u +
                   MAINTENANCE_SERVICE_ACTOR_CAPACITY +
                   MAINTENANCE_SERVICE_RESULT_CAPACITY +
                   MAINTENANCE_SERVICE_MAX_PARTS *
                       MAINTENANCE_SERVICE_PART_TEXT_CAPACITY * 2u ==
               656u,
               "public string capacities no longer produce 734 bytes");

static void put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
    data[2] = (uint8_t)(value >> 16u);
    data[3] = (uint8_t)(value >> 24u);
}

static void recompute_crc(uint8_t *slot)
{
    put_u32(&slot[TEST_CRC_OFFSET],
            maintenance_journal_codec_crc32(slot, TEST_CRC_OFFSET));
}

static void expect_no_output_mutation(const uint8_t *slot, size_t size)
{
    maintenance_journal_record_t output;
    maintenance_journal_record_t before;

    memset(&output, 0xa5, sizeof(output));
    before = output;
    assert(maintenance_journal_codec_decode(slot, size, &output) !=
           MAINTENANCE_JOURNAL_CODEC_OK);
    assert(memcmp(&output, &before, sizeof(output)) == 0);
}

static maintenance_journal_record_t make_record(maintenance_journal_record_type_t type)
{
    maintenance_journal_record_t record;

    memset(&record, 0, sizeof(record));
    record.epoch = type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT
                       ? UINT64_C(0x0102030405060708)
                       : UINT64_C(0x2122232425262728);
    record.sequence = type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT
                          ? UINT64_C(0x1112131415161718)
                          : UINT64_C(0x3132333435363738);
    record.command_time_utc_100ns = type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT
                                        ? INT64_C(0x0123456789abcdef)
                                        : INT64_C(0x012345678);
    record.type = type;
    record.operation = type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT
                           ? MAINTENANCE_JOURNAL_OPERATION_PLAN
                           : MAINTENANCE_JOURNAL_OPERATION_RESET;
    record.transition = type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT
                            ? MAINTENANCE_SERVICE_TRANSITION_FINISHED_TO_PLANNED
                            : MAINTENANCE_SERVICE_TRANSITION_NONE;
    record.snapshot.state = type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT
                                ? MAINTENANCE_SERVICE_STATE_PLANNED
                                : MAINTENANCE_SERVICE_STATE_FINISHED;
    record.snapshot.revision = type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT
                                   ? UINT32_C(0x10203040)
                                   : UINT32_C(0x50607080);
    record.snapshot.committed_at_utc_100ns = INT64_C(0x0102030405060708);
    record.snapshot.has_activity = true;
    record.snapshot.activity.planned_date_utc_100ns = INT64_C(0x0102030405060708);
    record.snapshot.activity.estimated_downtime_ms = 12.5;
    record.snapshot.activity.method = MAINTENANCE_SERVICE_METHOD_REMOTE;
    record.snapshot.activity.configuration_changed = type == MAINTENANCE_JOURNAL_RECORD_RESET;
    record.snapshot.activity.parts_serviced_count = 2u;
    record.snapshot.activity.parts_replaced_count = 1u;
    strcpy(record.snapshot.activity.operation_id, "op-id");
    strcpy(record.snapshot.activity.personnel, "crew");
    strcpy(record.snapshot.activity.actor, "alice");
    strcpy(record.snapshot.activity.result,
           type == MAINTENANCE_JOURNAL_RECORD_RESET ? "done" : "");
    strcpy(record.snapshot.activity.parts_serviced[0], "bolt");
    strcpy(record.snapshot.activity.parts_serviced[1], "nut");
    strcpy(record.snapshot.activity.parts_replaced[0], "seal");
    return record;
}

/*
 * This is an independent, complete schema-1 golden-slot construction.  It
 * deliberately has no shared helper with the codec: every byte (including
 * all erased bytes and the commit page) is compared below.
 */
static void make_golden_slot(const maintenance_journal_record_t *record,
                             uint8_t golden[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE])
{
    uint64_t downtime_bits;
    const char *strings[] = {record->snapshot.activity.operation_id,
                             record->snapshot.activity.supplier,
                             record->snapshot.activity.personnel,
                             record->snapshot.activity.actor,
                             record->snapshot.activity.result,
                             record->snapshot.activity.parts_serviced[0],
                             record->snapshot.activity.parts_serviced[1], "", "",
                             record->snapshot.activity.parts_replaced[0], "", "", ""};
    const size_t capacities[] = {48u, 48u, 48u, 32u, 96u, 48u, 48u,
                                 48u, 48u, 48u, 48u, 48u, 48u};
    size_t offsets[] = {TEST_OPERATION_ID_OFFSET, TEST_SUPPLIER_OFFSET,
                        TEST_PERSONNEL_OFFSET, TEST_ACTOR_OFFSET, TEST_RESULT_OFFSET,
                        TEST_SERVICED_OFFSET, TEST_SERVICED_OFFSET + 48u,
                        TEST_SERVICED_OFFSET + 96u, TEST_SERVICED_OFFSET + 144u,
                        TEST_REPLACED_OFFSET, TEST_REPLACED_OFFSET + 48u,
                        TEST_REPLACED_OFFSET + 96u, TEST_REPLACED_OFFSET + 144u};
    size_t index;

    memset(golden, 0xff, MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE);
    put_u32(&golden[0], MAINTENANCE_JOURNAL_RECORD_MAGIC);
    golden[4] = 1u;
    golden[5] = 0u;
    golden[6] = (uint8_t)record->type;
    golden[7] = 0u;
    golden[8] = 0xdeu;
    golden[9] = 0x02u;
    golden[10] = 40u;
    golden[11] = 0u;
    golden[12] = (uint8_t)record->epoch;
    golden[13] = (uint8_t)(record->epoch >> 8u);
    golden[14] = (uint8_t)(record->epoch >> 16u);
    golden[15] = (uint8_t)(record->epoch >> 24u);
    golden[16] = (uint8_t)(record->epoch >> 32u);
    golden[17] = (uint8_t)(record->epoch >> 40u);
    golden[18] = (uint8_t)(record->epoch >> 48u);
    golden[19] = (uint8_t)(record->epoch >> 56u);
    for (index = 0u; index < 8u; ++index)
    {
        golden[20u + index] = (uint8_t)(record->sequence >> (index * 8u));
        golden[32u + index] = (uint8_t)((uint64_t)record->command_time_utc_100ns >>
                                        (index * 8u));
        golden[45u + index] = (uint8_t)((uint64_t)record->snapshot.committed_at_utc_100ns >>
                                        (index * 8u));
        golden[54u + index] = (uint8_t)((uint64_t)record->snapshot.activity.planned_date_utc_100ns >>
                                        (index * 8u));
    }
    golden[28] = (uint8_t)record->operation;
    golden[29] = (uint8_t)record->transition;
    golden[40] = (uint8_t)record->snapshot.state;
    put_u32(&golden[41], record->snapshot.revision);
    golden[53] = 1u;
    memcpy(&downtime_bits, &record->snapshot.activity.estimated_downtime_ms,
           sizeof(downtime_bits));
    for (index = 0u; index < 8u; ++index)
    {
        golden[62u + index] = (uint8_t)(downtime_bits >> (index * 8u));
    }
    golden[70] = (uint8_t)record->snapshot.activity.method;
    golden[71] = record->snapshot.activity.configuration_changed ? 1u : 0u;
    golden[72] = 2u;
    golden[73] = 1u;
    for (index = 0u; index < sizeof(strings) / sizeof(strings[0]); ++index)
    {
        size_t length = strlen(strings[index]);

        assert(length < capacities[index]);
        golden[offsets[index]] = (uint8_t)length;
        memcpy(&golden[offsets[index] + 1u], strings[index], length);
    }
    recompute_crc(golden);
    put_u32(&golden[TEST_COMMIT_OFFSET], MAINTENANCE_JOURNAL_COMMIT_WORD);
}

static void test_golden_vectors(void)
{
    maintenance_journal_record_t records[2];
    uint8_t encoded[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    uint8_t golden[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    maintenance_journal_record_t decoded;
    size_t index;

    assert(maintenance_journal_codec_crc32((const uint8_t *)"123456789", 9u) ==
           UINT32_C(0xcbf43926));
    records[0] = make_record(MAINTENANCE_JOURNAL_RECORD_SNAPSHOT);
    records[1] = make_record(MAINTENANCE_JOURNAL_RECORD_RESET);
    for (index = 0u; index < 2u; ++index)
    {
        make_golden_slot(&records[index], golden);
        assert(maintenance_journal_codec_encode(&records[index], encoded,
                                                sizeof(encoded)) ==
               MAINTENANCE_JOURNAL_CODEC_OK);
        for (size_t byte_index = 0u; byte_index < sizeof(golden); ++byte_index)
        {
            if (encoded[byte_index] != golden[byte_index])
            {
                fprintf(stderr, "golden mismatch record %zu byte %zu: %02x != %02x\\n",
                        index, byte_index, encoded[byte_index], golden[byte_index]);
                assert(false);
            }
        }
        assert(memcmp(encoded, golden, sizeof(golden)) == 0);
        assert(maintenance_journal_codec_decode(golden, sizeof(golden), &decoded) ==
               MAINTENANCE_JOURNAL_CODEC_OK);
        assert(decoded.epoch == records[index].epoch);
        assert(decoded.sequence == records[index].sequence);
        assert(decoded.type == records[index].type);
        assert(memcmp(&decoded.snapshot, &records[index].snapshot,
                      sizeof(decoded.snapshot)) == 0);
    }
}

static void test_decode_negatives(void)
{
    maintenance_journal_record_t record = make_record(MAINTENANCE_JOURNAL_RECORD_SNAPSHOT);
    uint8_t slot[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    uint8_t modified[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    size_t index;

    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    for (index = 0u; index < TEST_CRC_OFFSET; ++index)
    {
        memcpy(modified, slot, sizeof(modified));
        modified[index] ^= 0x01u;
        expect_no_output_mutation(modified, sizeof(modified));
    }
    for (index = TEST_CRC_OFFSET; index < sizeof(slot); ++index)
    {
        memcpy(modified, slot, sizeof(modified));
        modified[index] ^= 0x01u;
        expect_no_output_mutation(modified, sizeof(modified));
    }
    for (index = 0u; index < sizeof(slot); ++index)
    {
        expect_no_output_mutation(slot, index);
    }
    memcpy(modified, slot, sizeof(modified));
    modified[734] = 0u;
    expect_no_output_mutation(modified, sizeof(modified));
    memcpy(modified, slot, sizeof(modified));
    modified[767] = 0u;
    expect_no_output_mutation(modified, sizeof(modified));

    /* Recomputed CRC makes structural, enum, boolean, UTF-8 and padding checks explicit. */
    memcpy(modified, slot, sizeof(modified));
    modified[6] = 3u;
    recompute_crc(modified);
    expect_no_output_mutation(modified, sizeof(modified));
    memcpy(modified, slot, sizeof(modified));
    modified[40] = 3u;
    recompute_crc(modified);
    expect_no_output_mutation(modified, sizeof(modified));
    memcpy(modified, slot, sizeof(modified));
    modified[53] = 2u;
    recompute_crc(modified);
    expect_no_output_mutation(modified, sizeof(modified));
    memcpy(modified, slot, sizeof(modified));
    modified[75] = 0xc0u; /* overlong UTF-8 lead byte */
    recompute_crc(modified);
    expect_no_output_mutation(modified, sizeof(modified));
    memcpy(modified, slot, sizeof(modified));
    modified[80] = 0u; /* non-erased string padding */
    recompute_crc(modified);
    expect_no_output_mutation(modified, sizeof(modified));
}

static void fill_text(char *text, size_t capacity, size_t length)
{
    size_t index;

    assert(length < capacity);
    for (index = 0u; index < length; ++index)
    {
        text[index] = 'x';
    }
    text[length] = '\0';
}

static void test_encode_boundaries(void)
{
    maintenance_journal_record_t record = make_record(MAINTENANCE_JOURNAL_RECORD_SNAPSHOT);
    uint8_t slot[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    char *texts[] = {record.snapshot.activity.operation_id, record.snapshot.activity.supplier,
                     record.snapshot.activity.personnel, record.snapshot.activity.actor,
                     record.snapshot.activity.result, record.snapshot.activity.parts_serviced[0],
                     record.snapshot.activity.parts_replaced[0]};
    const size_t capacities[] = {48u, 48u, 48u, 32u, 96u, 48u, 48u};
    size_t index;

    for (index = 0u; index < sizeof(texts) / sizeof(texts[0]); ++index)
    {
        fill_text(texts[index], capacities[index], capacities[index] - 1u);
    }
    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memset(record.snapshot.activity.operation_id, 'x',
           sizeof(record.snapshot.activity.operation_id));
    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD);

    record = make_record(MAINTENANCE_JOURNAL_RECORD_SNAPSHOT);
    record.snapshot.activity.parts_serviced_count = MAINTENANCE_SERVICE_MAX_PARTS;
    record.snapshot.activity.parts_replaced_count = MAINTENANCE_SERVICE_MAX_PARTS;
    for (index = 0u; index < MAINTENANCE_SERVICE_MAX_PARTS; ++index)
    {
        fill_text(record.snapshot.activity.parts_serviced[index],
                  MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                  MAINTENANCE_SERVICE_PART_TEXT_CAPACITY - 1u);
        fill_text(record.snapshot.activity.parts_replaced[index],
                  MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                  MAINTENANCE_SERVICE_PART_TEXT_CAPACITY - 1u);
    }
    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);

    record = make_record(MAINTENANCE_JOURNAL_RECORD_SNAPSHOT);
    record.snapshot.activity.estimated_downtime_ms = -0.0;
    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    assert(slot[62] == 0u && slot[63] == 0u && slot[64] == 0u && slot[65] == 0u &&
           slot[66] == 0u && slot[67] == 0u && slot[68] == 0u && slot[69] == 0u);
    assert(maintenance_journal_codec_decode(slot, sizeof(slot), &record) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    assert(record.snapshot.activity.estimated_downtime_ms == 0.0 &&
           !signbit(record.snapshot.activity.estimated_downtime_ms));
    record.snapshot.activity.estimated_downtime_ms = NAN;
    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD);
    record.snapshot.activity.estimated_downtime_ms = INFINITY;
    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD);
    record.snapshot.activity.estimated_downtime_ms = MAINTENANCE_SERVICE_MAX_DOWNTIME_MS + 1.0;
    assert(maintenance_journal_codec_encode(&record, slot, sizeof(slot)) ==
           MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD);
}

int main(void)
{
    test_golden_vectors();
    test_decode_negatives();
    test_encode_boundaries();
    puts("maintenance journal codec: PASS");
    return 0;
}

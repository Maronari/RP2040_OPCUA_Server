#include "ota_state_v2_design.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "check failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return false; \
        } \
    } while (0)

static const uint8_t p256_order[32] = {
    0xffu, 0xffu, 0xffu, 0xffu, 0x00u, 0x00u, 0x00u, 0x00u,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0xbcu, 0xe6u, 0xfau, 0xadu, 0xa7u, 0x17u, 0x9eu, 0x84u,
    0xf3u, 0xb9u, 0xcau, 0xc2u, 0xfcu, 0x63u, 0x25u, 0x51u,
};
static const uint8_t p256_half_order[32] = {
    0x7fu, 0xffu, 0xffu, 0xffu, 0x80u, 0x00u, 0x00u, 0x00u,
    0x7fu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0xdeu, 0x73u, 0x7du, 0x56u, 0xd3u, 0x8bu, 0xcfu, 0x42u,
    0x79u, 0xcdu, 0xe5u, 0x61u, 0x7eu, 0x31u, 0x92u, 0xa8u,
};

static void put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned int index = 0u; index < 4u; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8u));
    }
}

static void put_id(uint8_t destination[32], const char *text)
{
    const size_t length = strlen(text);

    memset(destination, 0, 32u);
    memcpy(destination, text, length);
}

static void make_manifest(uint8_t manifest[OTA_V2_MANIFEST_BYTES], uint32_t version)
{
    memset(manifest, 0, OTA_V2_MANIFEST_BYTES);
    memcpy(manifest, "RP2OTA06", 8u);
    put16(manifest + 8u, 1u);
    put16(manifest + 10u, 256u);
    put32(manifest + 12u, OTA_V2_MANIFEST_BYTES);
    put32(manifest + 16u, 1u);
    put32(manifest + 20u, 7u);
    put32(manifest + 24u, 4096u);
    put32(manifest + 28u, version);
    put32(manifest + 32u, UINT32_C(0x10040000));
    put32(manifest + 36u, 1u);
    put32(manifest + 40u, 10u);
    put32(manifest + 44u, 1u);
    put_id(manifest + 48u, "waveshare_core2350b_16m8m");
    put_id(manifest + 80u, "firmware.1");
    put_id(manifest + 112u, "base");
    manifest[144u] = 0x5au;
    manifest[256u + 31u] = 1u;
    manifest[288u + 31u] = 1u;
}

static ota_v2_record_t make_record(ota_v2_phase_t phase, uint64_t generation)
{
    ota_v2_record_t record;

    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.phase = (uint32_t)phase;
    record.confirmed_version_floor = 5u;
    record.failure_reason = 0u;
    record.policy_id = OTA_V2_POLICY_ID;
    record.presence_bits = 1u;
    make_manifest(record.confirmed, 5u);
    if (phase != OTA_V2_PHASE_CONFIRMED) {
        record.presence_bits |= 2u;
        make_manifest(record.candidate, 6u);
    }
    if (phase >= OTA_V2_PHASE_INSTALL) {
        record.presence_bits |= 4u;
        memcpy(record.backup, record.confirmed, OTA_V2_MANIFEST_BYTES);
        record.backup[256u + 31u] = 2u; /* Same signed identity, different valid signature scalar. */
    }
    return record;
}

static void fix_crc_and_marker(uint8_t sector[OTA_V2_SECTOR_BYTES])
{
    uint8_t record[OTA_V2_RECORD_BYTES];
    uint32_t crc;

    memcpy(record, sector, sizeof(record));
    memset(record + 60u, 0, 4u);
    crc = ota_v2_crc32(record, sizeof(record));
    put32(sector + 60u, crc);
    put32(sector + 3840u + 16u, crc);
}

static bool expect_first(const uint8_t old_sector[OTA_V2_SECTOR_BYTES],
                         const uint8_t other_sector[OTA_V2_SECTOR_BYTES])
{
    ota_v2_record_t selected;

    return ota_v2_select_sectors(old_sector, other_sector, &selected) == OTA_V2_SELECT_FIRST &&
           selected.generation == 1u;
}

static bool expect_second_generation(const uint8_t first_sector[OTA_V2_SECTOR_BYTES],
                                     const uint8_t second_sector[OTA_V2_SECTOR_BYTES],
                                     uint64_t generation)
{
    ota_v2_record_t selected;

    return ota_v2_select_sectors(first_sector, second_sector, &selected) == OTA_V2_SELECT_SECOND &&
           selected.generation == generation;
}

static bool test_crc_known_vector(void)
{
    static const uint8_t known_vector[] = "123456789";

    CHECK(ota_v2_crc32(known_vector, sizeof(known_vector) - 1u) == UINT32_C(0xcbf43926));
    CHECK(ota_v2_crc32(NULL, 1u) == 0u);
    return true;
}

static bool test_wire_golden(void)
{
    static const uint8_t expected_header[60] = {
        0x52u, 0x50u, 0x32u, 0x4fu, 0x53u, 0x54u, 0x30u, 0x36u,
        0x02u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u,
    };
    uint8_t sector[OTA_V2_SECTOR_BYTES];
    const ota_v2_record_t record = make_record(OTA_V2_PHASE_CONFIRMED, 1u);

    CHECK(ota_v2_encode_sector(&record, sector));
    CHECK(memcmp(sector, expected_header, sizeof(expected_header)) == 0);
    CHECK(sector[60u] == 0x7bu && sector[61u] == 0xa7u &&
          sector[62u] == 0x87u && sector[63u] == 0xacu);
    CHECK(memcmp(sector + OTA_V2_COMMIT_OFFSET, "RP2COM06", 8u) == 0);
    CHECK(sector[OTA_V2_COMMIT_OFFSET + 8u] == 1u);
    CHECK(memcmp(sector + OTA_V2_COMMIT_OFFSET + 9u,
                 "\0\0\0\0\0\0\0", 7u) == 0);
    CHECK(sector[OTA_V2_COMMIT_OFFSET + 16u] == 0x7bu &&
          sector[OTA_V2_COMMIT_OFFSET + 17u] == 0xa7u &&
          sector[OTA_V2_COMMIT_OFFSET + 18u] == 0x87u &&
          sector[OTA_V2_COMMIT_OFFSET + 19u] == 0xacu);
    return true;
}

static bool test_positive_phases(void)
{
    uint8_t sector[OTA_V2_SECTOR_BYTES];
    ota_v2_record_t decoded;

    for (unsigned int phase = OTA_V2_PHASE_CONFIRMED;
         phase <= OTA_V2_PHASE_RESTORE; ++phase) {
        const ota_v2_record_t input = make_record((ota_v2_phase_t)phase, (uint64_t)phase + 1u);

        CHECK(ota_v2_encode_sector(&input, sector));
        CHECK(ota_v2_decode_sector(sector, sizeof(sector), &decoded));
        CHECK(decoded.phase == phase);
        CHECK(decoded.generation == input.generation);
        CHECK(decoded.presence_bits == input.presence_bits);
        CHECK(memcmp(decoded.confirmed, input.confirmed, OTA_V2_MANIFEST_BYTES) == 0);
    }
    return true;
}

static bool test_rejects_corruption_unknowns_and_length(void)
{
    uint8_t valid[OTA_V2_SECTOR_BYTES];
    uint8_t changed[OTA_V2_SECTOR_BYTES];
    uint8_t extended[OTA_V2_SECTOR_BYTES + 1u];
    ota_v2_record_t record = make_record(OTA_V2_PHASE_INSTALL, 1u);
    ota_v2_record_t decoded;

    CHECK(ota_v2_encode_sector(&record, valid));
    CHECK(!ota_v2_decode_sector(valid, sizeof(valid) - 1u, &decoded));
    memcpy(extended, valid, sizeof(valid));
    extended[sizeof(valid)] = 0xffu;
    CHECK(!ota_v2_decode_sector(extended, sizeof(extended), &decoded));

    memcpy(changed, valid, sizeof(changed));
    changed[0] ^= 1u;
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    changed[1024u] = 0u;
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    changed[3840u + 20u] = 1u;
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));

    memcpy(changed, valid, sizeof(changed));
    put16(changed + 8u, 3u);
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    put32(changed + 12u, 6u);
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    put32(changed + 28u, 16u);
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    put32(changed + 32u, 2u);
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    put32(changed + 36u, 8u);
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    changed[40u] = 1u;
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    changed[64u + 176u] = 1u;
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    memcpy(changed, valid, sizeof(changed));
    changed[64u + 48u] = (uint8_t)' ';
    fix_crc_and_marker(changed);
    CHECK(!ota_v2_decode_sector(changed, sizeof(changed), &decoded));
    return true;
}

static bool test_signature_scalar_boundaries(void)
{
    uint8_t sector[OTA_V2_SECTOR_BYTES];
    ota_v2_record_t record = make_record(OTA_V2_PHASE_CONFIRMED, 1u);

    memcpy(record.confirmed + 256u, p256_order, sizeof(p256_order));
    CHECK(!ota_v2_encode_sector(&record, sector));
    make_manifest(record.confirmed, 5u);
    memset(record.confirmed + 256u, 0, 32u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    make_manifest(record.confirmed, 5u);
    memset(record.confirmed + 288u, 0, 32u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    make_manifest(record.confirmed, 5u);
    memcpy(record.confirmed + 288u, p256_half_order, sizeof(p256_half_order));
    CHECK(ota_v2_encode_sector(&record, sector));
    record.confirmed[288u] = 0x80u;
    CHECK(!ota_v2_encode_sector(&record, sector));
    return true;
}

static bool test_generation_successor_and_conflict(void)
{
    uint8_t first[OTA_V2_SECTOR_BYTES];
    uint8_t second[OTA_V2_SECTOR_BYTES];
    ota_v2_record_t selected;
    ota_v2_record_t current = make_record(OTA_V2_PHASE_CONFIRMED, 1u);
    ota_v2_record_t next = make_record(OTA_V2_PHASE_VERIFIED, 2u);
    ota_v2_record_t conflict = make_record(OTA_V2_PHASE_CONFIRMED, 1u);
    uint8_t unchanged[OTA_V2_SECTOR_BYTES];

    CHECK(ota_v2_encode_successor(NULL, &current, first));
    CHECK(ota_v2_encode_successor(&current, &next, second));
    CHECK(ota_v2_select_sectors(first, second, &selected) == OTA_V2_SELECT_SECOND);
    CHECK(selected.generation == 2u);
    CHECK(!ota_v2_encode_successor(&current, &current, second));
    current = make_record(OTA_V2_PHASE_CONFIRMED, UINT64_MAX - 1u);
    next = make_record(OTA_V2_PHASE_VERIFIED, UINT64_MAX);
    CHECK(ota_v2_encode_successor(&current, &next, second));
    CHECK(ota_v2_select_sectors(first, second, &selected) == OTA_V2_SELECT_SECOND);
    CHECK(selected.generation == UINT64_MAX);
    current = next;
    next = make_record(OTA_V2_PHASE_VERIFIED, 1u);
    memset(second, 0xa5, sizeof(second));
    memcpy(unchanged, second, sizeof(unchanged));
    CHECK(!ota_v2_encode_successor(&current, &next, second));
    CHECK(memcmp(second, unchanged, sizeof(second)) == 0);

    conflict.failure_reason = 1u;
    current = make_record(OTA_V2_PHASE_CONFIRMED, 1u);
    CHECK(ota_v2_encode_sector(&current, first));
    CHECK(ota_v2_encode_sector(&conflict, second));
    CHECK(ota_v2_select_sectors(first, second, &selected) == OTA_V2_SELECT_CONFLICT);
    memcpy(second, first, sizeof(second));
    CHECK(ota_v2_select_sectors(first, second, &selected) == OTA_V2_SELECT_FIRST);
    return true;
}

static bool test_phase_presence_and_identity_rules(void)
{
    uint8_t sector[OTA_V2_SECTOR_BYTES];
    ota_v2_record_t record = make_record(OTA_V2_PHASE_CONFIRMED, 1u);

    record.presence_bits = 5u;
    memcpy(record.backup, record.confirmed, OTA_V2_MANIFEST_BYTES);
    CHECK(!ota_v2_encode_sector(&record, sector));
    record = make_record(OTA_V2_PHASE_VERIFIED, 1u);
    record.presence_bits = 7u;
    memcpy(record.backup, record.confirmed, OTA_V2_MANIFEST_BYTES);
    CHECK(!ota_v2_encode_sector(&record, sector));
    record = make_record(OTA_V2_PHASE_INSTALL, 1u);
    make_manifest(record.backup, 5u);
    record.backup[144u] ^= 1u;
    CHECK(!ota_v2_encode_sector(&record, sector));
    record = make_record(OTA_V2_PHASE_VERIFIED, 1u);
    make_manifest(record.candidate, 5u);
    record.candidate[144u] ^= 1u;
    CHECK(!ota_v2_encode_sector(&record, sector));
    record = make_record(OTA_V2_PHASE_VERIFIED, 1u);
    make_manifest(record.candidate, 4u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    return true;
}

static bool test_manifest_id_boundaries(void)
{
    uint8_t sector[OTA_V2_SECTOR_BYTES];
    ota_v2_record_t record = make_record(OTA_V2_PHASE_CONFIRMED, 1u);

    memset(record.confirmed + 48u, (uint8_t)'A', 31u);
    record.confirmed[48u + 31u] = 0u;
    CHECK(ota_v2_encode_sector(&record, sector));
    memset(record.confirmed + 48u, (uint8_t)'A', 32u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    make_manifest(record.confirmed, 5u);
    put_id(record.confirmed + 48u, "hw");
    record.confirmed[48u + 3u] = (uint8_t)'X';
    CHECK(!ota_v2_encode_sector(&record, sector));
    return true;
}

static bool test_manifest_capacity_and_ranges(void)
{
    uint8_t sector[OTA_V2_SECTOR_BYTES];
    ota_v2_record_t record = make_record(OTA_V2_PHASE_CONFIRMED, 1u);

    put32(record.confirmed + 24u, 1u);
    CHECK(ota_v2_encode_sector(&record, sector));
    put32(record.confirmed + 24u, OTA_V2_IMAGE_CAPACITY);
    CHECK(ota_v2_encode_sector(&record, sector));
    put32(record.confirmed + 24u, 0u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    put32(record.confirmed + 24u, OTA_V2_IMAGE_CAPACITY + 1u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    make_manifest(record.confirmed, 5u);
    put32(record.confirmed + 36u, 11u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    make_manifest(record.confirmed, 5u);
    put32(record.confirmed + 20u, 0u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    make_manifest(record.confirmed, 5u);
    put32(record.confirmed + 44u, 0u);
    CHECK(!ota_v2_encode_sector(&record, sector));
    return true;
}

static void nor_program_prefix(uint8_t destination[OTA_V2_SECTOR_BYTES],
                               const uint8_t source[OTA_V2_SECTOR_BYTES], size_t byte_count)
{
    for (size_t index = 0u; index < byte_count; ++index) {
        destination[index] &= source[index];
    }
}

static bool test_every_torn_write_keeps_old_sector_selectable(void)
{
    uint8_t old_sector[OTA_V2_SECTOR_BYTES];
    uint8_t old_snapshot[OTA_V2_SECTOR_BYTES];
    uint8_t intended[OTA_V2_SECTOR_BYTES];
    uint8_t intended_snapshot[OTA_V2_SECTOR_BYTES];
    uint8_t destination[OTA_V2_SECTOR_BYTES];
    const ota_v2_record_t old_record = make_record(OTA_V2_PHASE_CONFIRMED, 1u);
    const ota_v2_record_t new_record = make_record(OTA_V2_PHASE_INSTALL, 2u);

    CHECK(ota_v2_encode_sector(&old_record, old_sector));
    CHECK(ota_v2_encode_successor(&old_record, &new_record, intended));
    memcpy(old_snapshot, old_sector, sizeof(old_snapshot));
    memcpy(intended_snapshot, intended, sizeof(intended_snapshot));

    for (size_t prefix = 0u; prefix <= OTA_V2_SECTOR_BYTES; ++prefix) {
        memcpy(destination, old_sector, sizeof(destination));
        memset(destination, 0xff, prefix); /* Interrupted erase of a valid stale sector. */
        CHECK(expect_second_generation(destination, intended, 2u));
        CHECK(memcmp(intended, intended_snapshot, sizeof(intended)) == 0);
    }
    for (size_t prefix = 0u; prefix <= OTA_V2_SECTOR_BYTES; ++prefix) {
        memset(destination, 0, sizeof(destination));
        memset(destination, 0xff, prefix); /* Interrupted erase. */
        CHECK(expect_first(old_sector, destination));
        CHECK(memcmp(old_sector, old_snapshot, sizeof(old_sector)) == 0);
    }
    for (size_t prefix = 0u; prefix <= OTA_V2_RECORD_BYTES; ++prefix) {
        memset(destination, 0xff, sizeof(destination));
        memcpy(destination, intended, prefix); /* Interrupted record program. */
        CHECK(expect_first(old_sector, destination));
        CHECK(memcmp(old_sector, old_snapshot, sizeof(old_sector)) == 0);
    }
    for (size_t prefix = 0u; prefix <= OTA_V2_COMMIT_BYTES; ++prefix) {
        memset(destination, 0xff, sizeof(destination));
        memcpy(destination, intended, OTA_V2_RECORD_BYTES);
        memcpy(destination + OTA_V2_COMMIT_OFFSET,
               intended + OTA_V2_COMMIT_OFFSET, prefix); /* Interrupted marker page. */
        if (prefix == OTA_V2_COMMIT_BYTES) {
            CHECK(ota_v2_select_sectors(old_sector, destination, NULL) == OTA_V2_SELECT_SECOND);
        } else {
            CHECK(expect_first(old_sector, destination));
        }
        CHECK(memcmp(old_sector, old_snapshot, sizeof(old_sector)) == 0);
    }
    for (size_t prefix = 0u; prefix <= OTA_V2_SECTOR_BYTES; ++prefix) {
        memcpy(destination, old_sector, sizeof(destination));
        nor_program_prefix(destination, intended, prefix); /* Program without a complete erase. */
        CHECK(expect_first(old_sector, destination));
        CHECK(memcmp(old_sector, old_snapshot, sizeof(old_sector)) == 0);
    }
    return true;
}

int main(void)
{
    if (!test_crc_known_vector() || !test_wire_golden() || !test_positive_phases() ||
        !test_rejects_corruption_unknowns_and_length() ||
        !test_signature_scalar_boundaries() || !test_generation_successor_and_conflict() ||
        !test_phase_presence_and_identity_rules() || !test_manifest_id_boundaries() ||
        !test_manifest_capacity_and_ranges() ||
        !test_every_torn_write_keeps_old_sector_selectable()) {
        return 1;
    }
    puts("ota_state_v2_design: PASS");
    return 0;
}

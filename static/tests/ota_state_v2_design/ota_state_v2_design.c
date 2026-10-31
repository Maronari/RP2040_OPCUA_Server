#include "ota_state_v2_design.h"

#include <limits.h>
#include <string.h>

enum {
    RECORD_FORMAT = 2u,
    RECORD_HEADER_BYTES = 64u,
    RECORD_CRC_OFFSET = 60u,
    MANIFEST_FORMAT = 1u,
    MANIFEST_SIGNED_BYTES = 256u,
    MANIFEST_LAYOUT_ID = 1u,
    MANIFEST_EXECUTION_ORIGIN = 0x10040000u,
    PRESENCE_CONFIRMED = 1u << 0,
    PRESENCE_CANDIDATE = 1u << 1,
    PRESENCE_BACKUP = 1u << 2,
    PRESENCE_ALLOWED = PRESENCE_CONFIRMED | PRESENCE_CANDIDATE | PRESENCE_BACKUP,
};

static const uint8_t record_magic[8] = {'R', 'P', '2', 'O', 'S', 'T', '0', '6'};
static const uint8_t commit_magic[8] = {'R', 'P', '2', 'C', 'O', 'M', '0', '6'};
static const uint8_t manifest_magic[8] = {'R', 'P', '2', 'O', 'T', 'A', '0', '6'};
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

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value = 0u;

    for (unsigned int index = 0u; index < 8u; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8u);
    }
    return value;
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned int index = 0u; index < 4u; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8u));
    }
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
    for (unsigned int index = 0u; index < 8u; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8u));
    }
}

uint32_t ota_v2_crc32(const uint8_t *bytes, size_t byte_count)
{
    uint32_t crc = UINT32_C(0xffffffff);

    if (bytes == NULL) {
        return 0u;
    }
    for (size_t index = 0u; index < byte_count; ++index) {
        crc ^= bytes[index];
        for (unsigned int bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static bool all_value(const uint8_t *bytes, size_t byte_count, uint8_t value)
{
    if (bytes == NULL) {
        return false;
    }
    for (size_t index = 0u; index < byte_count; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static bool canonical_id(const uint8_t id[32])
{
    size_t length = 0u;

    while (length < 32u && id[length] != 0u) {
        const uint8_t c = id[length];
        const bool allowed = (c >= (uint8_t)'A' && c <= (uint8_t)'Z') ||
                             (c >= (uint8_t)'a' && c <= (uint8_t)'z') ||
                             (c >= (uint8_t)'0' && c <= (uint8_t)'9') ||
                             c == (uint8_t)'_' || c == (uint8_t)'.' || c == (uint8_t)'-';
        if (!allowed) {
            return false;
        }
        ++length;
    }
    return length != 0u && length < 32u && all_value(id + length, 32u - length, 0u);
}

static bool nonzero(const uint8_t *bytes, size_t byte_count)
{
    if (bytes == NULL) {
        return false;
    }
    for (size_t index = 0u; index < byte_count; ++index) {
        if (bytes[index] != 0u) {
            return true;
        }
    }
    return false;
}

static bool manifest_structural_valid(const uint8_t manifest[OTA_V2_MANIFEST_BYTES])
{
    const uint32_t image_size = read_le32(manifest + 24u);
    const uint32_t minimum_board = read_le32(manifest + 36u);
    const uint32_t maximum_board = read_le32(manifest + 40u);

    return memcmp(manifest, manifest_magic, sizeof(manifest_magic)) == 0 &&
           read_le16(manifest + 8u) == MANIFEST_FORMAT &&
           read_le16(manifest + 10u) == MANIFEST_SIGNED_BYTES &&
           read_le32(manifest + 12u) == OTA_V2_MANIFEST_BYTES &&
           read_le32(manifest + 16u) == MANIFEST_LAYOUT_ID &&
           read_le32(manifest + 20u) != 0u &&
           image_size != 0u && image_size <= OTA_V2_IMAGE_CAPACITY &&
           read_le32(manifest + 32u) == MANIFEST_EXECUTION_ORIGIN &&
           minimum_board <= maximum_board && read_le32(manifest + 44u) != 0u &&
           canonical_id(manifest + 48u) && canonical_id(manifest + 80u) &&
           canonical_id(manifest + 112u) && all_value(manifest + 176u, 80u, 0u) &&
           nonzero(manifest + 256u, 32u) && memcmp(manifest + 256u, p256_order, 32u) < 0 &&
           nonzero(manifest + 288u, 32u) && memcmp(manifest + 288u, p256_half_order, 32u) <= 0;
}

static bool descriptors_valid(const ota_v2_record_t *record)
{
    const uint32_t phase = record->phase;
    const uint32_t presence = record->presence_bits;
    const bool candidate_present = (presence & PRESENCE_CANDIDATE) != 0u;
    const bool backup_present = (presence & PRESENCE_BACKUP) != 0u;

    if ((presence & ~PRESENCE_ALLOWED) != 0u ||
        (presence & PRESENCE_CONFIRMED) == 0u ||
        !manifest_structural_valid(record->confirmed) ||
        record->confirmed_version_floor != read_le32(record->confirmed + 28u)) {
        return false;
    }
    if (candidate_present) {
        const uint32_t candidate_version = read_le32(record->candidate + 28u);
        if (!manifest_structural_valid(record->candidate) ||
            candidate_version < record->confirmed_version_floor ||
            (candidate_version == record->confirmed_version_floor &&
             memcmp(record->candidate, record->confirmed, OTA_V2_MANIFEST_PREFIX_BYTES) != 0)) {
            return false;
        }
    } else if (!all_value(record->candidate, sizeof(record->candidate), 0u)) {
        return false;
    }
    if (backup_present) {
        if (!manifest_structural_valid(record->backup) ||
            memcmp(record->backup, record->confirmed, OTA_V2_MANIFEST_PREFIX_BYTES) != 0) {
            return false;
        }
    } else if (!all_value(record->backup, sizeof(record->backup), 0u)) {
        return false;
    }

    switch (phase) {
    case OTA_V2_PHASE_CONFIRMED:
        return presence == PRESENCE_CONFIRMED;
    case OTA_V2_PHASE_VERIFIED:
    case OTA_V2_PHASE_BACKUP:
        return presence == (PRESENCE_CONFIRMED | PRESENCE_CANDIDATE);
    case OTA_V2_PHASE_INSTALL:
    case OTA_V2_PHASE_LAUNCH_CONSUMED:
    case OTA_V2_PHASE_RESTORE:
        return presence == PRESENCE_ALLOWED;
    default:
        return false;
    }
}

static bool record_semantic_valid(const ota_v2_record_t *record)
{
    return record != NULL && record->generation != 0u &&
           record->phase <= OTA_V2_PHASE_RESTORE &&
           record->failure_reason <= OTA_V2_REASON_RECOVERY_REQUIRED &&
           record->policy_id == OTA_V2_POLICY_ID && descriptors_valid(record);
}

static void record_to_bytes(const ota_v2_record_t *record, uint8_t bytes[OTA_V2_RECORD_BYTES])
{
    memset(bytes, 0, OTA_V2_RECORD_BYTES);
    memcpy(bytes, record_magic, sizeof(record_magic));
    write_le16(bytes + 8u, RECORD_FORMAT);
    write_le16(bytes + 10u, OTA_V2_RECORD_BYTES);
    write_le32(bytes + 12u, record->phase);
    write_le64(bytes + 16u, record->generation);
    write_le32(bytes + 24u, record->confirmed_version_floor);
    write_le32(bytes + 28u, record->failure_reason);
    write_le32(bytes + 32u, record->policy_id);
    write_le32(bytes + 36u, record->presence_bits);
    memcpy(bytes + 64u, record->confirmed, OTA_V2_MANIFEST_BYTES);
    memcpy(bytes + 384u, record->candidate, OTA_V2_MANIFEST_BYTES);
    memcpy(bytes + 704u, record->backup, OTA_V2_MANIFEST_BYTES);
    write_le32(bytes + RECORD_CRC_OFFSET, ota_v2_crc32(bytes, OTA_V2_RECORD_BYTES));
}

static bool bytes_to_record(const uint8_t bytes[OTA_V2_RECORD_BYTES], ota_v2_record_t *record)
{
    uint8_t crc_bytes[OTA_V2_RECORD_BYTES];
    const uint32_t saved_crc = read_le32(bytes + RECORD_CRC_OFFSET);

    if (memcmp(bytes, record_magic, sizeof(record_magic)) != 0 ||
        read_le16(bytes + 8u) != RECORD_FORMAT ||
        read_le16(bytes + 10u) != OTA_V2_RECORD_BYTES ||
        !all_value(bytes + 40u, 20u, 0u)) {
        return false;
    }
    memcpy(crc_bytes, bytes, sizeof(crc_bytes));
    memset(crc_bytes + RECORD_CRC_OFFSET, 0, sizeof(uint32_t));
    if (saved_crc != ota_v2_crc32(crc_bytes, sizeof(crc_bytes))) {
        return false;
    }
    memset(record, 0, sizeof(*record));
    record->phase = read_le32(bytes + 12u);
    record->generation = read_le64(bytes + 16u);
    record->confirmed_version_floor = read_le32(bytes + 24u);
    record->failure_reason = read_le32(bytes + 28u);
    record->policy_id = read_le32(bytes + 32u);
    record->presence_bits = read_le32(bytes + 36u);
    memcpy(record->confirmed, bytes + 64u, OTA_V2_MANIFEST_BYTES);
    memcpy(record->candidate, bytes + 384u, OTA_V2_MANIFEST_BYTES);
    memcpy(record->backup, bytes + 704u, OTA_V2_MANIFEST_BYTES);
    return record_semantic_valid(record);
}

bool ota_v2_encode_sector(const ota_v2_record_t *record, uint8_t sector[OTA_V2_SECTOR_BYTES])
{
    uint8_t *commit;
    uint32_t crc;

    if (sector == NULL || !record_semantic_valid(record)) {
        return false;
    }
    memset(sector, 0xff, OTA_V2_SECTOR_BYTES);
    record_to_bytes(record, sector);
    crc = read_le32(sector + RECORD_CRC_OFFSET);
    commit = sector + OTA_V2_COMMIT_OFFSET;
    memcpy(commit, commit_magic, sizeof(commit_magic));
    write_le64(commit + 8u, record->generation);
    write_le32(commit + 16u, crc);
    memset(commit + 20u, 0, OTA_V2_COMMIT_BYTES - 20u);
    return true;
}

bool ota_v2_decode_sector(const uint8_t *sector, size_t sector_bytes, ota_v2_record_t *out_record)
{
    ota_v2_record_t decoded;
    const uint8_t *commit;

    if (sector == NULL || sector_bytes != OTA_V2_SECTOR_BYTES || out_record == NULL ||
        !all_value(sector + OTA_V2_RECORD_BYTES,
                   OTA_V2_COMMIT_OFFSET - OTA_V2_RECORD_BYTES, 0xffu) ||
        !bytes_to_record(sector, &decoded)) {
        return false;
    }
    commit = sector + OTA_V2_COMMIT_OFFSET;
    if (memcmp(commit, commit_magic, sizeof(commit_magic)) != 0 ||
        read_le64(commit + 8u) != decoded.generation ||
        read_le32(commit + 16u) != read_le32(sector + RECORD_CRC_OFFSET) ||
        !all_value(commit + 20u, OTA_V2_COMMIT_BYTES - 20u, 0u)) {
        return false;
    }
    *out_record = decoded;
    return true;
}

ota_v2_select_t ota_v2_select_sectors(const uint8_t first[OTA_V2_SECTOR_BYTES],
                                       const uint8_t second[OTA_V2_SECTOR_BYTES],
                                       ota_v2_record_t *out_record)
{
    ota_v2_record_t first_record;
    ota_v2_record_t second_record;
    const bool first_valid = ota_v2_decode_sector(first, OTA_V2_SECTOR_BYTES, &first_record);
    const bool second_valid = ota_v2_decode_sector(second, OTA_V2_SECTOR_BYTES, &second_record);

    if (!first_valid && !second_valid) {
        return OTA_V2_SELECT_NONE;
    }
    if (first_valid && !second_valid) {
        if (out_record != NULL) {
            *out_record = first_record;
        }
        return OTA_V2_SELECT_FIRST;
    }
    if (!first_valid) {
        if (out_record != NULL) {
            *out_record = second_record;
        }
        return OTA_V2_SELECT_SECOND;
    }
    if (first_record.generation == second_record.generation) {
        if (memcmp(first, second, OTA_V2_SECTOR_BYTES) != 0) {
            return OTA_V2_SELECT_CONFLICT;
        }
        if (out_record != NULL) {
            *out_record = first_record;
        }
        return OTA_V2_SELECT_FIRST;
    }
    if (first_record.generation > second_record.generation) {
        if (out_record != NULL) {
            *out_record = first_record;
        }
        return OTA_V2_SELECT_FIRST;
    }
    if (out_record != NULL) {
        *out_record = second_record;
    }
    return OTA_V2_SELECT_SECOND;
}

bool ota_v2_encode_successor(const ota_v2_record_t *current_or_null,
                             const ota_v2_record_t *next,
                             uint8_t sector[OTA_V2_SECTOR_BYTES])
{
    if (next == NULL || (current_or_null == NULL && next->generation != 1u) ||
        (current_or_null != NULL &&
         (!record_semantic_valid(current_or_null) || current_or_null->generation == UINT64_MAX ||
          next->generation != current_or_null->generation + 1u))) {
        return false;
    }
    return ota_v2_encode_sector(next, sector);
}

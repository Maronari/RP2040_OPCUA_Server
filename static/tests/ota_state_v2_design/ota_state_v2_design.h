#ifndef OTA_STATE_V2_DESIGN_H
#define OTA_STATE_V2_DESIGN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* T0 wire-contract prototype only. It is not linked by firmware targets. */
enum {
    OTA_V2_SECTOR_BYTES = 4096u,
    OTA_V2_RECORD_BYTES = 1024u,
    OTA_V2_COMMIT_OFFSET = 3840u,
    OTA_V2_COMMIT_BYTES = 256u,
    OTA_V2_MANIFEST_BYTES = 320u,
    OTA_V2_MANIFEST_PREFIX_BYTES = 256u,
    OTA_V2_IMAGE_CAPACITY = 5271552u,
    OTA_V2_POLICY_ID = 1u,
};

typedef enum {
    OTA_V2_PHASE_CONFIRMED = 0u,
    OTA_V2_PHASE_VERIFIED = 1u,
    OTA_V2_PHASE_BACKUP = 2u,
    OTA_V2_PHASE_INSTALL = 3u,
    OTA_V2_PHASE_LAUNCH_CONSUMED = 4u,
    OTA_V2_PHASE_RESTORE = 5u,
} ota_v2_phase_t;

typedef enum {
    OTA_V2_REASON_NONE = 0u,
    OTA_V2_REASON_MANIFEST_REQUIRED = 1u,
    OTA_V2_REASON_MANIFEST_INVALID = 2u,
    OTA_V2_REASON_UNKNOWN_KEY = 3u,
    OTA_V2_REASON_SIGNATURE_INVALID = 4u,
    OTA_V2_REASON_IMAGE_HASH_MISMATCH = 5u,
    OTA_V2_REASON_INCOMPATIBLE = 6u,
    OTA_V2_REASON_VERSION_REJECTED = 7u,
    OTA_V2_REASON_STORAGE_ERROR = 8u,
    OTA_V2_REASON_STATE_INVALID = 9u,
    OTA_V2_REASON_BACKUP_INVALID = 10u,
    OTA_V2_REASON_CANDIDATE_INVALID = 11u,
    OTA_V2_REASON_HEALTH_TIMEOUT = 12u,
    OTA_V2_REASON_HEALTH_FAILED = 13u,
    OTA_V2_REASON_ALLOCATION_FAILED = 14u,
    OTA_V2_REASON_RECOVERY_REQUIRED = 15u,
} ota_v2_reason_t;

typedef enum {
    OTA_V2_SELECT_NONE = 0,
    OTA_V2_SELECT_FIRST = 1,
    OTA_V2_SELECT_SECOND = 2,
    OTA_V2_SELECT_CONFLICT = 3,
} ota_v2_select_t;

typedef struct {
    uint64_t generation;
    uint32_t phase;
    uint32_t confirmed_version_floor;
    uint32_t failure_reason;
    uint32_t policy_id;
    uint32_t presence_bits;
    uint8_t confirmed[OTA_V2_MANIFEST_BYTES];
    uint8_t candidate[OTA_V2_MANIFEST_BYTES];
    uint8_t backup[OTA_V2_MANIFEST_BYTES];
} ota_v2_record_t;

/* Creates a fully erased sector except for the record and final commit page. */
bool ota_v2_encode_sector(const ota_v2_record_t *record,
                          uint8_t sector[OTA_V2_SECTOR_BYTES]);

/* Requires exactly one complete 4 KiB sector and returns only committed records. */
bool ota_v2_decode_sector(const uint8_t *sector, size_t sector_bytes,
                          ota_v2_record_t *out_record);

/* Selects a newest committed record; equal-generation nonidentical sectors conflict. */
ota_v2_select_t ota_v2_select_sectors(const uint8_t first[OTA_V2_SECTOR_BYTES],
                                       const uint8_t second[OTA_V2_SECTOR_BYTES],
                                       ota_v2_record_t *out_record);

/* Encodes generation one or the successor of current; rollover is refused. */
bool ota_v2_encode_successor(const ota_v2_record_t *current_or_null,
                             const ota_v2_record_t *next,
                             uint8_t sector[OTA_V2_SECTOR_BYTES]);

uint32_t ota_v2_crc32(const uint8_t *bytes, size_t byte_count);

#endif /* OTA_STATE_V2_DESIGN_H */

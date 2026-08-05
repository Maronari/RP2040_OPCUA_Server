#ifndef SERVICES_MAINTENANCE_JOURNAL_CODEC_H
#define SERVICES_MAINTENANCE_JOURNAL_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "services/maintenance_service.h"

/*
 * Schema-1 maintenance records are canonical byte strings, not serialized C
 * structs.  A backend writes bytes [0, 767] first and the final commit page
 * only after read-verifying those bytes.
 */
#define MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE 1024u
#define MAINTENANCE_JOURNAL_RECORD_BODY_SIZE 768u
#define MAINTENANCE_JOURNAL_FLASH_PROGRAM_SIZE 256u
#define MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE 734u
#define MAINTENANCE_JOURNAL_EXTENSION_MARGIN 32u
#define MAINTENANCE_JOURNAL_ENCODED_HEADER_SIZE 40u
#define MAINTENANCE_JOURNAL_SCHEMA_VERSION 1u

/* "MRJ1" and "JMC1" expressed as little-endian uint32_t values. */
#define MAINTENANCE_JOURNAL_RECORD_MAGIC 0x314a524du
#define MAINTENANCE_JOURNAL_COMMIT_WORD 0x31434d4au

typedef enum
{
    MAINTENANCE_JOURNAL_RECORD_SNAPSHOT = 1,
    MAINTENANCE_JOURNAL_RECORD_RESET = 2
} maintenance_journal_record_type_t;

/* Values 0..3 intentionally match maintenance_service_operation_t. */
typedef enum
{
    MAINTENANCE_JOURNAL_OPERATION_PLAN = 0,
    MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE = 1,
    MAINTENANCE_JOURNAL_OPERATION_START = 2,
    MAINTENANCE_JOURNAL_OPERATION_FINISH = 3,
    MAINTENANCE_JOURNAL_OPERATION_RESET = 4
} maintenance_journal_operation_t;

typedef enum
{
    MAINTENANCE_JOURNAL_CODEC_OK = 0,
    MAINTENANCE_JOURNAL_CODEC_INVALID_ARGUMENT = 1,
    MAINTENANCE_JOURNAL_CODEC_BUFFER_TOO_SMALL = 2,
    MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD = 3,
    MAINTENANCE_JOURNAL_CODEC_MALFORMED = 4,
    MAINTENANCE_JOURNAL_CODEC_UNSUPPORTED = 5,
    MAINTENANCE_JOURNAL_CODEC_INCOMPLETE = 6
} maintenance_journal_codec_result_t;

typedef struct
{
    uint64_t epoch;
    uint64_t sequence;
    int64_t command_time_utc_100ns;
    maintenance_journal_record_type_t type;
    maintenance_journal_operation_t operation;
    maintenance_service_transition_t transition;
    maintenance_service_snapshot_t snapshot;
} maintenance_journal_record_t;

/* CRC-32/ISO-HDLC, provided so host tests and a storage backend share it. */
uint32_t maintenance_journal_codec_crc32(const uint8_t *data, size_t size);

/* Produces all 1024 canonical bytes, including the exact commit page. */
maintenance_journal_codec_result_t maintenance_journal_codec_encode(
    const maintenance_journal_record_t *record,
    uint8_t *out_slot,
    size_t out_slot_size);

/*
 * Accepts only an exact schema-1 committed slot.  On every non-OK result
 * out_record is untouched, allowing recovery to retain its known-good state.
 */
maintenance_journal_codec_result_t maintenance_journal_codec_decode(
    const uint8_t *slot,
    size_t slot_size,
    maintenance_journal_record_t *out_record);

bool maintenance_journal_codec_has_exact_commit_page(const uint8_t *slot,
                                                      size_t slot_size);

#endif

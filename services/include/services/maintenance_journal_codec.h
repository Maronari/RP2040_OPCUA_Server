/**
 * @file
 * @brief Schema-1 maintenance journal record codec: encodes/decodes the
 *        canonical on-Flash byte layout used by
 *        `drivers/include/drivers/maintenance_journal_backend.h`.
 *
 * Pure encode/decode/CRC logic with no Flash or FreeRTOS dependency, so it
 * is directly host-testable. Records are canonical byte strings, not
 * serialized C structs cast onto a buffer: every field has a fixed byte
 * offset (see `OFFSET_*` in the `.c` file) and every unused byte must equal
 * a fixed fill value (0xFF) for a slot to decode/validate successfully -
 * see maintenance_journal_codec_decode() and the internal
 * `*_is_canonical()` helpers. This canonical-byte-string discipline is what
 * lets the backend CRC and diff raw Flash pages directly.
 *
 * A backend (`maintenance_journal_backend.c`) writes bytes [0, 767] (the
 * body) first and the final 256-byte commit page only after read-verifying
 * those bytes; see docs/maintenance_journal.md for the full commit-state
 * classification this two-phase write enables.
 */
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

/** @brief Discriminates a snapshot record (post-transition state) from a reset record. */
typedef enum
{
    MAINTENANCE_JOURNAL_RECORD_SNAPSHOT = 1,
    MAINTENANCE_JOURNAL_RECORD_RESET = 2
} maintenance_journal_record_type_t;

/**
 * @brief Which maintenance-service operation produced this journal record.
 *
 * Values 0..3 intentionally match `maintenance_service_operation_t` so the
 * two enums can be compared/converted directly; `RESET` (4) has no
 * `maintenance_service_operation_t` counterpart.
 */
typedef enum
{
    MAINTENANCE_JOURNAL_OPERATION_PLAN = 0,
    MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE = 1,
    MAINTENANCE_JOURNAL_OPERATION_START = 2,
    MAINTENANCE_JOURNAL_OPERATION_FINISH = 3,
    MAINTENANCE_JOURNAL_OPERATION_RESET = 4
} maintenance_journal_operation_t;

/** @brief Result/error code for every `maintenance_journal_codec_*` function. */
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

/**
 * @brief Decoded/pre-encode form of one journal record: lineage (epoch/
 *        sequence), command metadata, and the maintenance snapshot it records.
 *
 * `epoch`/`sequence` together give each record a total order across the
 * journal's lifetime (reset events advance the epoch); see
 * `maintenance_journal_backend_t` for how a backend tracks the allocated
 * epoch/sequence.
 */
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

/**
 * @brief CRC-32/ISO-HDLC (same polynomial/reflection as zlib `crc32`) over
 *        @p size bytes of @p data.
 *
 * Provided so host tests and a storage backend share exactly one
 * implementation; also used internally by maintenance_journal_codec_encode()/
 * maintenance_journal_codec_decode() to compute/verify the record CRC field.
 *
 * @param data Bytes to checksum; may be NULL only if @p size is 0.
 * @param size Number of bytes to checksum.
 * @return The CRC-32 value; 0 if @p data is NULL and @p size is nonzero.
 */
uint32_t maintenance_journal_codec_crc32(const uint8_t *data, size_t size);

/**
 * @brief Encodes @p record into the canonical 1024-byte schema-1 slot layout,
 *        including the exact commit page.
 *
 * Validates every field of @p record (lineage bounds, enum ranges, the
 * `type`/`operation`/`transition` combination, activity presence/absence
 * consistency with `state`/`committed_at_utc_100ns`) before writing
 * anything; returns MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD without
 * partial output on any validation failure. On success, fills all 1024
 * bytes of @p out_slot: unused/reserved bytes are 0xFF, the record CRC-32
 * covers bytes [0, `OFFSET_CRC`), and the final 4 bytes of the body carry
 * the CRC. The commit word is written last, mirroring (in one call) the
 * two-phase body-then-commit discipline a real Flash backend performs
 * across two separate program operations.
 *
 * @param record Record to encode; validated internally.
 * @param out_slot Destination buffer; must be at least
 *        MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE bytes.
 * @param out_slot_size Capacity of @p out_slot.
 * @return MAINTENANCE_JOURNAL_CODEC_OK on success;
 *         MAINTENANCE_JOURNAL_CODEC_INVALID_ARGUMENT if @p record or
 *         @p out_slot is NULL; MAINTENANCE_JOURNAL_CODEC_BUFFER_TOO_SMALL
 *         if @p out_slot_size is too small; MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD
 *         if @p record fails validation.
 */
maintenance_journal_codec_result_t maintenance_journal_codec_encode(
    const maintenance_journal_record_t *record,
    uint8_t *out_slot,
    size_t out_slot_size);

/**
 * @brief Decodes and fully validates an exact schema-1 committed slot into @p out_record.
 *
 * Accepts only an exact schema-1 committed slot: requires an exact commit
 * page (maintenance_journal_codec_has_exact_commit_page()), correct magic/
 * schema/length header fields, all reserved/padding bytes exactly 0xFF, a
 * matching CRC-32, and every decoded field within its valid range/
 * combination (mirroring the checks performed by
 * maintenance_journal_codec_encode()). On every non-OK result @p out_record
 * is left untouched, allowing recovery logic to retain its known-good state
 * rather than being clobbered by a partially-decoded corrupt record.
 *
 * @param slot Candidate 1024-byte slot buffer.
 * @param slot_size Size of @p slot; must equal exactly
 *        MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE (any other size, larger or
 *        smaller, is rejected).
 * @param out_record Destination for the decoded record; untouched unless
 *        the return is MAINTENANCE_JOURNAL_CODEC_OK.
 * @return MAINTENANCE_JOURNAL_CODEC_OK on success;
 *         MAINTENANCE_JOURNAL_CODEC_INVALID_ARGUMENT if @p slot or
 *         @p out_record is NULL; MAINTENANCE_JOURNAL_CODEC_BUFFER_TOO_SMALL
 *         if @p slot_size is smaller than the slot size;
 *         MAINTENANCE_JOURNAL_CODEC_MALFORMED for any other size mismatch
 *         or a structurally invalid/non-canonical slot;
 *         MAINTENANCE_JOURNAL_CODEC_INCOMPLETE if the commit page is not exact.
 */
maintenance_journal_codec_result_t maintenance_journal_codec_decode(
    const uint8_t *slot,
    size_t slot_size,
    maintenance_journal_record_t *out_record);

/**
 * @brief Checks whether @p slot's trailing 256-byte commit page is exactly
 *        the commit word followed by all-0xFF filler.
 *
 * This is the sole "was this record durably committed" test used by both
 * maintenance_journal_codec_decode() and backend commit-state
 * classification (`maintenance_journal_backend_classify_commit()`): a
 * readable but non-exact commit page (e.g. torn write) must be treated as
 * not committed, never repaired or reinterpreted.
 *
 * @param slot Candidate slot buffer.
 * @param slot_size Size of @p slot; must equal exactly
 *        MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE for this to return true.
 * @return true only if @p slot is non-NULL, exactly slot-sized, and its
 *         commit page is the exact commit-word-plus-0xFF pattern.
 */
bool maintenance_journal_codec_has_exact_commit_page(const uint8_t *slot,
                                                      size_t slot_size);

#endif

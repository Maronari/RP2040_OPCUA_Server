/**
 * @file
 * @brief Transactional maintenance-journal Flash backend: commit-page/CRC
 *        durability, multi-generation chain-of-custody recovery.
 *
 * This is a physical Flash driver contract, not a service state machine --
 * see docs/maintenance_journal.md for the full schema, recovery
 * classification, and power-loss guarantees, and
 * services/src/maintenance_service.c for the caller that turns this into
 * the OPC UA maintenance workflow. Unlike drivers/history_flash_backend.c,
 * every mutation here goes through an explicit commit-page classification
 * (maintenance_journal_backend_classify_commit()) so a torn write is never
 * mistaken for a committed record.
 */
#ifndef DRIVERS_MAINTENANCE_JOURNAL_BACKEND_H
#define DRIVERS_MAINTENANCE_JOURNAL_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/maintenance_journal_codec.h"

/* This is a physical Flash driver contract, not a service state machine. */
#define MAINTENANCE_JOURNAL_SECTOR_COUNT 8u
#define MAINTENANCE_JOURNAL_SECTOR_SIZE 4096u
#define MAINTENANCE_JOURNAL_HEADER_SIZE 256u
#define MAINTENANCE_JOURNAL_SLOTS_PER_SECTOR 3u
#define MAINTENANCE_JOURNAL_RESERVED_TAIL_SIZE 768u
#define MAINTENANCE_JOURNAL_TOTAL_SLOTS 24u
#define MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS 15u
#define MAINTENANCE_JOURNAL_MAX_TRANSIENT_RECORDS 16u
#define MAINTENANCE_JOURNAL_MIN_ERASED_SECTORS 2u
#define MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX 4u

#define MAINTENANCE_JOURNAL_HEADER_MAGIC 0x31484a4du /* "MJH1", little endian */
#define MAINTENANCE_JOURNAL_HEADER_FORMAT_VERSION 1u
#define MAINTENANCE_JOURNAL_HEADER_VERSION 1u

/**
 * @brief Physical post-state classification of one 1024-byte record slot,
 *        derived solely by reading it back after a commit-program attempt
 *        (the program API's own success/failure result never participates).
 */
typedef enum
{
    MAINTENANCE_JOURNAL_COMMIT_COMMITTED = 0,   /**< Commit page exact + record body/CRC/lineage decode correctly. */
    MAINTENANCE_JOURNAL_COMMIT_NOT_COMMITTED,   /**< Commit page erased, or readable but not exact. */
    MAINTENANCE_JOURNAL_COMMIT_INDETERMINATE,   /**< Commit page could not be read/classified at all. */
    MAINTENANCE_JOURNAL_COMMIT_CORRUPT,         /**< Exact commit marker but the record body/CRC fails to decode under the current schema. */
    /* Exact marker plus a recognized record envelope with an unknown schema.
     * It is preserved, never decoded as a snapshot, and blocks mutation. */
    MAINTENANCE_JOURNAL_COMMIT_UNKNOWN          /**< Exact marker, recognized envelope, unknown schema version -- preserved, never decoded. */
} maintenance_journal_commit_state_t;

/** @brief Result codes for every maintenance_journal_backend_* mutating/query call. */
typedef enum
{
    MAINTENANCE_JOURNAL_BACKEND_OK = 0,
    MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT, /**< NULL/malformed argument, or a candidate record that fails codec encoding. */
    MAINTENANCE_JOURNAL_BACKEND_IO,               /**< A storage read/program/erase callback returned false. */
    MAINTENANCE_JOURNAL_BACKEND_CAPACITY,         /**< No erased sector/slot available and reclamation could not free one. */
    MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE,    /**< A commit page could not be classified; mutations are blocked until reconciled. */
    MAINTENANCE_JOURNAL_BACKEND_DEGRADED,         /**< Recovery found older corruption but the newest snapshot is still unambiguous. */
    MAINTENANCE_JOURNAL_BACKEND_FATAL,            /**< Non-erased media with no unambiguous supported snapshot. */
    MAINTENANCE_JOURNAL_BACKEND_UNSUPPORTED,      /**< A newer exact-committed record uses an unrecognized schema version. */
    MAINTENANCE_JOURNAL_BACKEND_ORDER,            /**< Sequence/epoch counter is at its reserved maximum and cannot advance further. */
    MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND         /**< Export cursor references a generation/epoch/sequence no longer present. */
} maintenance_journal_backend_result_t;

/** @brief Overall recovery outcome after maintenance_journal_backend_recover(); mirrors docs/maintenance_journal.md's classification. */
typedef enum
{
    MAINTENANCE_JOURNAL_RECOVERY_EMPTY = 0,     /**< Region is fully erased; caller restores the default Finished/revision-1 snapshot. */
    MAINTENANCE_JOURNAL_RECOVERY_OK,            /**< One newest supported snapshot selected unambiguously. */
    MAINTENANCE_JOURNAL_RECOVERY_DEGRADED,      /**< Older corruption exists but the newest snapshot is still unambiguous. */
    MAINTENANCE_JOURNAL_RECOVERY_INDETERMINATE, /**< A commit page could not be read/classified; mutation and reset are blocked. */
    MAINTENANCE_JOURNAL_RECOVERY_FATAL,         /**< Non-erased media contains no unambiguous supported snapshot; mutation and reset are blocked. */
    MAINTENANCE_JOURNAL_RECOVERY_UNSUPPORTED    /**< A newer exact-committed record uses an unknown schema; mutation and reset are blocked. */
} maintenance_journal_recovery_state_t;

/** @brief Snapshot of how much write headroom (spare erased sectors) the journal currently has. */
typedef enum
{
    MAINTENANCE_JOURNAL_CAPACITY_READY = 0,   /**< At least MAINTENANCE_JOURNAL_MIN_ERASED_SECTORS erased sectors available. */
    MAINTENANCE_JOURNAL_CAPACITY_CONSTRAINED, /**< Below the minimum spare-sector count but mutations are not blocked. */
    MAINTENANCE_JOURNAL_CAPACITY_BLOCKED      /**< mutations_blocked is set (indeterminate/fatal/unsupported recovery). */
} maintenance_journal_capacity_state_t;

/** @brief Storage-callback signature: read @p size bytes at @p offset into @p destination. */
typedef bool (*maintenance_journal_read_fn)(void *context, uint32_t offset,
                                            uint8_t *destination, size_t size);
/** @brief Storage-callback signature: program one 256-byte page at @p offset. */
typedef bool (*maintenance_journal_program_fn)(void *context, uint32_t offset,
                                               const uint8_t page[256]);
/** @brief Storage-callback signature: erase one MAINTENANCE_JOURNAL_SECTOR_SIZE sector at @p offset. */
typedef bool (*maintenance_journal_erase_fn)(void *context, uint32_t offset);

/**
 * @brief Injected storage backend: RP2350 XIP+flash_operation_coordinator in
 *        production (maintenance_journal_backend_init_flash()), or
 *        host-test fakes via maintenance_journal_backend_init().
 */
typedef struct
{
    void *context;
    maintenance_journal_read_fn read;
    maintenance_journal_program_fn program_page;
    maintenance_journal_erase_fn erase_sector;
} maintenance_journal_storage_t;

/** @brief Physical coordinates plus decoded content of one committed record; identifies the current "active" (newest) record. */
typedef struct
{
    uint8_t sector;
    uint8_t slot;
    uint64_t epoch;
    uint64_t generation;
    maintenance_journal_record_t record;
} maintenance_journal_record_location_t;

/**
 * @brief Opaque paging cursor for maintenance_journal_backend_export_page().
 *
 * Owned by the caller (services/maintenance_service.c's export continuation
 * token state); `generation` ties the cursor to a specific logical
 * generation of the journal so a reset/reclamation between pages is
 * detected (MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND) rather than silently
 * returning inconsistent data.
 */
typedef struct
{
    uint64_t epoch;
    uint64_t upper_sequence;
    uint64_t next_sequence;
    uint32_t generation;
} maintenance_journal_export_cursor_t;

/**
 * @brief Top-level backend instance: one per running firmware image.
 *
 * Ownership: allocated and owned by services/maintenance_service.c
 * (typically a single static/global instance initialized once via
 * maintenance_journal_backend_init_flash()).
 *
 * Synchronization: no internal lock. Per docs/code/persistence.md, appends
 * happen without the maintenance service holding its own critical section;
 * callers must not call the mutating entry points
 * (recover/append/reset) concurrently from more than one task.
 */
typedef struct
{
    maintenance_journal_storage_t storage;
    maintenance_journal_recovery_state_t recovery;
    maintenance_journal_record_location_t active;
    uint32_t corrupt_record_count;
    uint32_t non_exact_commit_count;
    uint32_t unknown_record_count;
    uint32_t erase_count; /* Runtime only; not persisted in an immutable header. */
    uint32_t sector_erase_count[MAINTENANCE_JOURNAL_SECTOR_COUNT];
    uint8_t valid_record_count;
    uint8_t retained_record_count;
    uint8_t transient_record_peak;
    uint8_t erased_sector_count;
    uint8_t unknown_sector_mask;
    uint64_t allocated_epoch;
    uint64_t allocated_sequence;
    uint32_t logical_generation;
    maintenance_journal_capacity_state_t capacity_state;
    maintenance_journal_backend_result_t last_error;
    bool has_active;
    bool mutations_blocked;
} maintenance_journal_backend_t;

_Static_assert(MAINTENANCE_JOURNAL_SECTOR_COUNT * MAINTENANCE_JOURNAL_SECTOR_SIZE == 32768u,
               "journal physical capacity changed");
_Static_assert(MAINTENANCE_JOURNAL_HEADER_SIZE +
                   MAINTENANCE_JOURNAL_SLOTS_PER_SECTOR * MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE +
                   MAINTENANCE_JOURNAL_RESERVED_TAIL_SIZE == MAINTENANCE_JOURNAL_SECTOR_SIZE,
               "sector page layout changed");

/**
 * @brief Classify one 1024-byte record slot's physical post-state.
 *
 * Calls exactly one complete 1024-byte read after a commit-program attempt.
 * The program API's own success/failure result deliberately does not
 * participate: physical post-state (what can actually be read back) is the
 * sole authority. Exact pages with an unknown record schema are classified
 * UNKNOWN, not CORRUPT, so recovery can preserve their sector rather than
 * treating them as damage.
 *
 * @param slot_offset Byte offset of the slot's first page from the start of
 *        the journal region.
 * @param slot_scratch Caller-owned MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE
 *        (1024-byte) scratch buffer used for the read.
 * @return See maintenance_journal_commit_state_t.
 */
maintenance_journal_commit_state_t maintenance_journal_backend_classify_commit(
    const maintenance_journal_storage_t *storage, uint32_t slot_offset,
    uint8_t slot_scratch[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE]);

/**
 * @brief Zero-initialize @p backend and install @p storage.
 *
 * Leaves the backend with recovery == MAINTENANCE_JOURNAL_RECOVERY_INDETERMINATE
 * and mutations_blocked == true until maintenance_journal_backend_recover()
 * runs -- the backend is never usable for reads/writes purely from init().
 *
 * @param backend Instance to initialize; must not be NULL (no-op if it is).
 * @param storage Callback set to copy in; may be NULL for a not-ready backend.
 */
void maintenance_journal_backend_init(maintenance_journal_backend_t *backend,
                                      const maintenance_journal_storage_t *storage);

/**
 * @brief Scan all 8 sectors/24 slots, classify every commit, and select the
 *        single authoritative "active" (newest) record.
 *
 * Must be called once before any append()/reset()/export_*() call. Performs
 * no writes except through classify_commit()'s read-only inspection. Sets
 * `backend->recovery` to the outcome (see maintenance_journal_recovery_state_t)
 * and `backend->mutations_blocked` accordingly; INDETERMINATE/FATAL/
 * UNSUPPORTED preserve any prior unambiguous `active` record read but block
 * every subsequent mutation until re-recovered successfully.
 *
 * @return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT if @p backend is
 *         NULL or has no read callback; otherwise a result mirroring
 *         `backend->recovery` (OK for EMPTY/OK, DEGRADED, INDETERMINATE,
 *         FATAL, or UNSUPPORTED).
 * @note Hardware side effect: read-only; issues many Flash reads but no
 *       erase/program.
 */
maintenance_journal_backend_result_t maintenance_journal_backend_recover(
    maintenance_journal_backend_t *backend);

/**
 * @brief Append one new logical record after the current active record.
 *
 * Allocates the next free slot in the active sector (or opens a new sector,
 * erasing a spare, if the active sector is full), assigns the next
 * sequence number, programs the three body pages then the commit page, and
 * classifies the result via maintenance_journal_backend_classify_commit().
 * On success, opportunistically reclaims (erases) whole sectors that hold
 * only reclaimable/non-retained records once the retained-record count
 * exceeds MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS (best-effort; failure to
 * reclaim does not fail the append that already succeeded).
 *
 * @param candidate Record to append; its epoch/sequence fields are
 *        overwritten by the backend (caller-supplied values are ignored).
 * @param out_location Optional; receives the physical location and final
 *        decoded content of the newly committed record.
 * @return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT for a NULL backend/
 *         candidate or a missing program/erase callback;
 *         MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE if mutations are
 *         currently blocked (including a newly-indeterminate commit
 *         classification from this very call); MAINTENANCE_JOURNAL_BACKEND_ORDER
 *         if the sequence counter has reached its reserved maximum;
 *         MAINTENANCE_JOURNAL_BACKEND_CAPACITY if no spare sector/slot could
 *         be freed; MAINTENANCE_JOURNAL_BACKEND_IO on a storage callback
 *         failure or a non-exact commit-page classification;
 *         MAINTENANCE_JOURNAL_BACKEND_OK on success.
 * @pre Requires a prior successful (non-blocked) maintenance_journal_backend_recover().
 * @note Hardware side effect: up to 4 Flash program calls (3 body pages +
 *       1 commit page) plus, occasionally, one sector erase and one header
 *       program for a new sector.
 */
maintenance_journal_backend_result_t maintenance_journal_backend_append(
    maintenance_journal_backend_t *backend, const maintenance_journal_record_t *candidate,
    maintenance_journal_record_location_t *out_location);

/**
 * @brief Start a new logical epoch, recording @p snapshot as a RESET record.
 *
 * Idempotent: if the backend is logically empty (no active record), returns
 * OK without writing anything; if the active record is already itself a
 * RESET record, returns OK with @p out_location set to it without writing
 * again. Otherwise opens a new sector (erasing a spare) and programs one
 * RESET record with epoch = active.epoch + 1, sequence = 1, preserving the
 * workflow state/revision carried in @p snapshot.
 *
 * @param snapshot Base record whose type/operation/transition/epoch/sequence
 *        fields are overwritten by the backend; its `snapshot` payload
 *        (workflow state, revision, etc.) is preserved as given.
 * @param out_location Optional; receives the physical location of the
 *        (possibly pre-existing) RESET record.
 * @return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT for NULL arguments;
 *         MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE if mutations are
 *         blocked; MAINTENANCE_JOURNAL_BACKEND_CAPACITY if the epoch counter
 *         is exhausted or no spare sector is available; MAINTENANCE_JOURNAL_BACKEND_IO
 *         on a storage/commit failure; MAINTENANCE_JOURNAL_BACKEND_OK
 *         (idempotent no-op or newly-written) otherwise.
 * @note Hardware side effect: one sector erase, one header program, and one
 *       record program (body + commit pages) on the non-idempotent path.
 */
maintenance_journal_backend_result_t maintenance_journal_backend_reset(
    maintenance_journal_backend_t *backend, const maintenance_journal_record_t *snapshot,
    maintenance_journal_record_location_t *out_location);

/**
 * @brief Initialize an export cursor spanning the active record's full epoch.
 *
 * @param out_cursor Set to {epoch=0, upper_sequence=0, next_sequence=0} if
 *        the backend has no active record (empty journal), or to the active
 *        record's epoch/sequence as the upper bound otherwise. `generation`
 *        is always set to `backend->logical_generation`.
 * @return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT for NULL arguments;
 *         MAINTENANCE_JOURNAL_BACKEND_OK otherwise.
 */
maintenance_journal_backend_result_t maintenance_journal_backend_export_begin(
    const maintenance_journal_backend_t *backend,
    maintenance_journal_export_cursor_t *out_cursor);

/**
 * @brief Read the next page (up to MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX
 *        records) of a paginated export walk, oldest-to-newest, advancing
 *        @p cursor in place.
 *
 * RESET-type records are skipped from @p out_records (internal bookkeeping,
 * not client-visible history) but still advance the cursor.
 *
 * @param cursor In/out; `cursor->generation` must match
 *        `backend->logical_generation` or MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND
 *        is returned (the journal was reset/reclaimed since this cursor was
 *        issued).
 * @param out_records Destination array, at least MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX entries.
 * @param out_count Set to the number of records actually written to @p out_records.
 * @param out_more Set to true if further records remain beyond this page.
 * @return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT for NULL arguments;
 *         MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND for a stale/invalid cursor;
 *         MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE if a record slot fails
 *         to read/decode mid-walk; MAINTENANCE_JOURNAL_BACKEND_OK otherwise.
 */
maintenance_journal_backend_result_t maintenance_journal_backend_export_page(
    maintenance_journal_backend_t *backend, maintenance_journal_export_cursor_t *cursor,
    maintenance_journal_record_t out_records[MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX],
    uint8_t *out_count, bool *out_more);

/**
 * @brief RP2350 application binding of maintenance_journal_backend_init().
 *
 * Reads are direct XIP reads, program/erase are routed through the shared
 * flash_operation_coordinator at OTA_LAYOUT_MAINTENANCE_JOURNAL_OFFSET_BYTES
 * + offset. The scratch buffers used internally are static internal SRAM
 * and never live on a FreeRTOS task stack. Host tests should instead call
 * maintenance_journal_backend_init() directly with fake storage callbacks.
 *
 * @param backend Instance to initialize; must not be NULL.
 */
void maintenance_journal_backend_init_flash(maintenance_journal_backend_t *backend);

#endif

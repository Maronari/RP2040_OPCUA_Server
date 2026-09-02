/**
 * @file
 * @brief Staged/running/last-known-good Flash transaction backend for
 *        device configuration records.
 *
 * This is a physical Flash driver contract, following the same
 * physical-read-back commit-page classification technique as
 * drivers/maintenance_journal_backend (study that file first -- it is this
 * module's direct template) instead of the non-transactional
 * drivers/history_flash_backend pattern. Unlike the journal, this backend
 * does not keep a multi-generation chain-of-custody: each of the three
 * logical roles below owns exactly one physical Flash region and holds at
 * most one record at a time (v0.5.0 plan T3 scope decision 2 -- one
 * physical copy per role, not a second A/B copy for "atomic-swap safety",
 * since commit-page/CRC physical read-back classification already proves
 * atomicity without one).
 *
 * Roles:
 *   - CONFIG_TXN_ROLE_STAGED: a candidate configuration written by
 *     config_transaction_backend_stage(), not yet promoted.
 *   - CONFIG_TXN_ROLE_RUNNING: the configuration the firmware is currently
 *     (or, immediately after reboot, was last) running.
 *   - CONFIG_TXN_ROLE_LAST_KNOWN_GOOD: the configuration RUNNING held
 *     immediately before the most recent successful commit -- the
 *     power-loss-safe fallback target.
 *
 * Physical layout (target binding,
 * config_transaction_backend_init_flash()): the three roles are carved out
 * of the existing 128 KiB config-slot pair (OTA_LAYOUT_CONFIG_SLOT_0/1,
 * unchanged by this module -- see ota/cmake/ota_layout.cmake) as three
 * equal CONFIG_TXN_ROLE_SIZE_BYTES regions, leaving a small reserved tail
 * unused (see the .c file's target-only offset constants). No new Flash
 * region and no OTA-bank shrink; this repartitions the same budget the
 * pre-v0.5.0 two-slot config_store_service already owned.
 *
 * Per-role physical write protocol (see config_transaction_backend_stage()
 * for the reference implementation, reused for every role write): erase the
 * whole role region, program a header page (magic, per-record CRC32, a
 * RECORD_FORMAT_VERSION-style marker, caller schema_version, payload
 * size/CRC), program the payload, read back and byte-compare the just
 * written body against what was intended (defense against a torn body
 * write), then program a single dedicated commit page as the LAST write,
 * at a fixed offset from the end of the role region, exactly once. The
 * commit page's own physical exactness (never its own program call's
 * return value) is the sole atomicity signal -- see
 * config_transaction_backend_classify_commit().
 *
 * Ownership: caller (services/config_store_service.c) owns one static
 * config_transaction_backend_t instance and calls
 * config_transaction_backend_recover() once before any other mutating call,
 * mirroring maintenance_journal_backend_t's contract.
 *
 * Concurrency: no internal lock, same single-task-caller assumption as
 * maintenance_journal_backend_t -- see that header's doc comment. All
 * mutating Flash operations route through the shared
 * drivers/flash_operation_coordinator, never a second direct-write path.
 */
#ifndef DRIVERS_CONFIG_TRANSACTION_BACKEND_H
#define DRIVERS_CONFIG_TRANSACTION_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Flash erase-sector size this backend programs/erases in. */
#define CONFIG_TXN_SECTOR_SIZE_BYTES 4096u
/** @brief Flash program-page size; also the fixed size of the header page and the commit page. */
#define CONFIG_TXN_PAGE_SIZE_BYTES 256u
/** @brief Sectors reserved per role (staged/running/last-known-good each get this many, equally). */
#define CONFIG_TXN_ROLE_SECTOR_COUNT 10u
/** @brief Bytes reserved per role: CONFIG_TXN_ROLE_SECTOR_COUNT * CONFIG_TXN_SECTOR_SIZE_BYTES (40960). */
#define CONFIG_TXN_ROLE_SIZE_BYTES (CONFIG_TXN_ROLE_SECTOR_COUNT * CONFIG_TXN_SECTOR_SIZE_BYTES)
/** @brief Largest payload one role can hold: role size minus the header page and the commit page (40448 bytes -- device_profile_config_t is 5,624 bytes as of v0.5.0, a >7x margin). */
#define CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES (CONFIG_TXN_ROLE_SIZE_BYTES - CONFIG_TXN_PAGE_SIZE_BYTES - CONFIG_TXN_PAGE_SIZE_BYTES)

/** Magic identifying a valid config_txn_role_header_t ("CTX1"). */
#define CONFIG_TXN_RECORD_MAGIC 0x31585443u
/** This backend's own on-Flash record layout version (the RECORD_FORMAT_VERSION-style marker); bump only when config_txn_role_header_t's shape changes. */
#define CONFIG_TXN_RECORD_FORMAT_VERSION 1u
/** Magic identifying a valid, physically-landed commit page ("CTXC"). */
#define CONFIG_TXN_COMMIT_MAGIC 0x43585443u

/** @brief The three logical roles this backend manages, one physical region each. */
typedef enum
{
    CONFIG_TXN_ROLE_STAGED = 0,
    CONFIG_TXN_ROLE_RUNNING,
    CONFIG_TXN_ROLE_LAST_KNOWN_GOOD,
    CONFIG_TXN_ROLE_COUNT
} config_txn_role_t;

/**
 * @brief Physical post-state classification of one role's record, derived
 *        solely by reading Flash back after a write attempt (a program/erase
 *        callback's own success/failure result never participates in this
 *        decision -- see config_transaction_backend_classify_commit()).
 */
typedef enum
{
    CONFIG_TXN_COMMIT_COMMITTED = 0,  /**< Commit page exact + header/payload CRC and structure decode correctly. */
    CONFIG_TXN_COMMIT_NOT_COMMITTED,  /**< Commit page erased, or readable but not an exact commit page -- a torn/incomplete write, safely discardable. */
    CONFIG_TXN_COMMIT_INDETERMINATE, /**< A physical read failed outright; state cannot be classified at all. */
    CONFIG_TXN_COMMIT_CORRUPT,       /**< Exact commit marker present but the header/payload fails CRC or structural validation. */
    CONFIG_TXN_COMMIT_UNKNOWN        /**< Exact commit marker, recognized header envelope, unrecognized record_format_version -- preserved, never decoded. */
} config_txn_commit_state_t;

/** @brief Result codes for every config_transaction_backend_* mutating/query call. */
typedef enum
{
    CONFIG_TXN_BACKEND_OK = 0,
    CONFIG_TXN_BACKEND_INVALID_ARGUMENT, /**< NULL/malformed argument, or a payload too large for one role. */
    CONFIG_TXN_BACKEND_IO,               /**< A storage read/program/erase callback failed, or a just-written body/commit page failed its own read-back verification. */
    CONFIG_TXN_BACKEND_INDETERMINATE,    /**< Mutations are blocked (backend->mutations_blocked), including a newly-indeterminate classification from this very call. */
    CONFIG_TXN_BACKEND_NOT_FOUND         /**< The requested role currently has no committed record. */
} config_transaction_backend_result_t;

/**
 * @brief Overall boot-time recovery outcome after
 *        config_transaction_backend_recover(), summarizing all three roles'
 *        physical states for the caller (config_store_service.c today;
 *        v0.5.0 T4's transaction pipeline eventually) to act on. This
 *        backend only classifies and reports -- it never itself decides to
 *        fall back or discard; see config_transaction_backend_recover_fallback_to_last_known_good().
 */
typedef enum
{
    CONFIG_TXN_RECOVERY_EMPTY = 0,        /**< No role holds a committed record (fresh/erased Flash) -- caller should seed compiled defaults. */
    CONFIG_TXN_RECOVERY_OK,               /**< RUNNING is committed; normal boot. */
    CONFIG_TXN_RECOVERY_DEGRADED,         /**< RUNNING is committed, but STAGED and/or LAST_KNOWN_GOOD shows CORRUPT/UNKNOWN evidence worth surfacing -- RUNNING is still safe to use as-is. */
    CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED,/**< RUNNING is not usable (NOT_COMMITTED/CORRUPT/UNKNOWN) but LAST_KNOWN_GOOD is committed -- caller should fall back (see config_transaction_backend_recover_fallback_to_last_known_good()). */
    CONFIG_TXN_RECOVERY_INDETERMINATE,    /**< A role's physical state could not be read/classified at all; mutations are blocked until re-recovered successfully. */
    CONFIG_TXN_RECOVERY_FATAL             /**< Neither RUNNING nor LAST_KNOWN_GOOD is usable, and at least one role shows corruption evidence (not simply erased); mutations are blocked. */
} config_transaction_recovery_state_t;

/** @brief Storage-callback signature: read @p size bytes at @p offset (relative to the start of this backend's whole 3-role region) into @p destination. */
typedef bool (*config_txn_read_fn)(void *context, uint32_t offset, uint8_t *destination, size_t size);
/** @brief Storage-callback signature: program one CONFIG_TXN_PAGE_SIZE_BYTES page at @p offset. */
typedef bool (*config_txn_program_fn)(void *context, uint32_t offset, const uint8_t page[CONFIG_TXN_PAGE_SIZE_BYTES]);
/** @brief Storage-callback signature: erase one CONFIG_TXN_SECTOR_SIZE_BYTES sector at @p offset. */
typedef bool (*config_txn_erase_fn)(void *context, uint32_t offset);

/**
 * @brief Injected storage backend: RP2350 XIP+flash_operation_coordinator in
 *        production (config_transaction_backend_init_flash()), or host-test
 *        fakes via config_transaction_backend_init().
 */
typedef struct
{
    void *context;
    config_txn_read_fn read;
    config_txn_program_fn program_page;
    config_txn_erase_fn erase_sector;
} config_transaction_storage_t;

/**
 * @brief Cached metadata for one role's record, refreshed by
 *        config_transaction_backend_recover() and by every mutating call
 *        that touches that role.
 */
typedef struct
{
    config_txn_commit_state_t commit_state;
    uint16_t record_format_version;
    uint16_t payload_format;
    uint32_t schema_version;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t generation; /**< Monotonic counter, meaningful for RUNNING; callers already depend on this field. */
} config_transaction_record_info_t;

/**
 * @brief Top-level backend instance: one per running firmware image.
 *
 * Ownership: allocated and owned by services/config_store_service.c
 * (a single static instance, lazily initialized via
 * config_transaction_backend_init_flash() + config_transaction_backend_recover()
 * on first use).
 *
 * Synchronization: no internal lock -- see the file-level doc comment.
 */
typedef struct
{
    config_transaction_storage_t storage;
    config_transaction_record_info_t roles[CONFIG_TXN_ROLE_COUNT];
    config_transaction_recovery_state_t recovery;
    config_transaction_backend_result_t last_error;
    bool recovered;
    bool mutations_blocked;
} config_transaction_backend_t;

_Static_assert(CONFIG_TXN_ROLE_SIZE_BYTES % CONFIG_TXN_SECTOR_SIZE_BYTES == 0u,
               "role size must be sector aligned");
_Static_assert(CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES > 0u && CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES < CONFIG_TXN_ROLE_SIZE_BYTES,
               "role payload capacity must leave room for the header and commit pages");

/**
 * @brief Classify one role's physical post-state from Flash, independent of
 *        any program/erase call's own return value.
 *
 * Reads the role's fixed-offset commit page first (the sole atomicity
 * signal: exact match required, never trusted from a write call's result),
 * then -- only if the commit page is exact -- reads and validates the
 * header page (magic, record_format_version, per-field CRC) and finally the
 * payload (length-bounded CRC32), reading the payload in
 * CONFIG_TXN_PAGE_SIZE_BYTES chunks so no full-payload-sized scratch buffer
 * is required.
 *
 * @param storage Storage callbacks; must have a non-NULL `read`.
 * @param role_offset Byte offset of this role's region from the start of
 *        the whole 3-role Flash region.
 * @param out_info Optional; populated with the decoded metadata when the
 *        result is CONFIG_TXN_COMMIT_COMMITTED (left untouched otherwise).
 * @return See config_txn_commit_state_t.
 * @note Hardware side effect: read-only.
 */
config_txn_commit_state_t config_transaction_backend_classify_commit(
    const config_transaction_storage_t *storage, uint32_t role_offset,
    config_transaction_record_info_t *out_info);

/**
 * @brief Zero-initialize @p backend and install @p storage.
 *
 * Leaves the backend with recovery == CONFIG_TXN_RECOVERY_INDETERMINATE and
 * mutations_blocked == true until config_transaction_backend_recover() runs
 * -- the backend is never usable for reads/writes purely from init().
 *
 * @param backend Instance to initialize; must not be NULL (no-op if it is).
 * @param storage Callback set to copy in; may be NULL for a not-ready backend.
 */
void config_transaction_backend_init(config_transaction_backend_t *backend,
                                     const config_transaction_storage_t *storage);

/**
 * @brief Classify all three roles and compute the overall recovery outcome.
 *
 * Must be called once before any stage()/commit()/discard_staged()/
 * recover_fallback_to_last_known_good()/read() call. Performs no writes.
 * Sets `backend->recovery` (see config_transaction_recovery_state_t) and
 * `backend->mutations_blocked` accordingly.
 *
 * @return CONFIG_TXN_BACKEND_INVALID_ARGUMENT if @p backend is NULL or has
 *         no read callback; CONFIG_TXN_BACKEND_INDETERMINATE if any role
 *         could not be classified; CONFIG_TXN_BACKEND_OK otherwise
 *         (including the FATAL/DEGRADED/FALLBACK_REQUIRED/EMPTY outcomes --
 *         check `backend->recovery` for the actual classification).
 * @note Hardware side effect: read-only.
 */
config_transaction_backend_result_t config_transaction_backend_recover(
    config_transaction_backend_t *backend);

/**
 * @brief Write @p payload to the STAGED role as a new candidate.
 *
 * Erases the whole STAGED region, programs the header and payload, reads
 * both back to verify, then programs the commit page exactly once and
 * classifies the physical result. A torn/incomplete write (power loss
 * during this call) classifies as CONFIG_TXN_COMMIT_NOT_COMMITTED/CORRUPT
 * on the next config_transaction_backend_recover() and is safely
 * discardable -- it is never mistaken for a valid staged candidate.
 *
 * @param payload Payload bytes to stage; must not be NULL.
 * @param payload_size Must be non-zero and
 *        <= CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES.
 * @param payload_format Caller-defined tag (e.g. config_store_payload_format_t);
 *        stored verbatim, not interpreted here.
 * @param schema_version Caller-defined tag (e.g. the config schema version);
 *        stored verbatim, not interpreted here.
 * @param out_info Optional; populated with the newly staged record's
 *        metadata on success.
 * @return CONFIG_TXN_BACKEND_INVALID_ARGUMENT for NULL/oversized arguments;
 *         CONFIG_TXN_BACKEND_INDETERMINATE if mutations are blocked;
 *         CONFIG_TXN_BACKEND_IO on a storage failure or a failed
 *         post-write classification; CONFIG_TXN_BACKEND_OK once COMMITTED
 *         is confirmed for the STAGED role.
 * @note Hardware side effect: one sector-count erase plus header/payload/
 *       commit page programs.
 */
config_transaction_backend_result_t config_transaction_backend_stage(
    config_transaction_backend_t *backend, const void *payload, size_t payload_size,
    uint16_t payload_format, uint32_t schema_version,
    config_transaction_record_info_t *out_info);

/**
 * @brief Discard the STAGED role's content by erasing its region.
 * @return CONFIG_TXN_BACKEND_INDETERMINATE if mutations are blocked;
 *         CONFIG_TXN_BACKEND_IO on an erase/read-verify failure;
 *         CONFIG_TXN_BACKEND_OK otherwise (including when STAGED already
 *         held nothing).
 * @note Hardware side effect: one sector-count erase.
 */
config_transaction_backend_result_t config_transaction_backend_discard_staged(
    config_transaction_backend_t *backend);

/**
 * @brief Promote STAGED to RUNNING, preserving crash safety throughout.
 *
 * Ordering (frozen by v0.5.0 T3): if RUNNING currently holds a committed
 * record, it is first copied into LAST_KNOWN_GOOD (erase+program+commit+
 * classify that role) and confirmed committed -- only *then* is RUNNING
 * itself overwritten with STAGED's content. A power loss during the first
 * step leaves RUNNING untouched (still the valid old value) and at worst a
 * torn LAST_KNOWN_GOOD write, safely retried later; a power loss during the
 * second step leaves LAST_KNOWN_GOOD holding the just-confirmed old RUNNING
 * value while RUNNING itself is torn (config_transaction_backend_recover()
 * on the next boot correctly reports CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED).
 * Neither ordering can ever leave both RUNNING and LAST_KNOWN_GOOD invalid
 * at once.
 *
 * On success, STAGED is best-effort erased (a failure to erase it does not
 * fail the commit that already landed -- RUNNING/LAST_KNOWN_GOOD are what
 * matters for correctness; a still-present STAGED record after a successful
 * commit is harmless leftover, not a hazard).
 *
 * @param out_running_info Optional; populated with the new RUNNING record's
 *        metadata on success.
 * @return CONFIG_TXN_BACKEND_NOT_FOUND if STAGED has no committed record to
 *         promote; CONFIG_TXN_BACKEND_INDETERMINATE if mutations are
 *         blocked; CONFIG_TXN_BACKEND_IO if the LAST_KNOWN_GOOD promotion
 *         or the RUNNING write fails its own classification (in which case
 *         RUNNING is left as whatever it was before this call: untouched if
 *         the LAST_KNOWN_GOOD step failed, or torn if the RUNNING write
 *         itself failed -- in the latter case, recover Flash via
 *         config_transaction_backend_recover_fallback_to_last_known_good());
 *         CONFIG_TXN_BACKEND_OK once RUNNING is confirmed to hold STAGED's
 *         former content.
 * @note Hardware side effect: up to two role rewrites (LAST_KNOWN_GOOD then
 *       RUNNING) plus a best-effort STAGED erase.
 */
config_transaction_backend_result_t config_transaction_backend_commit(
    config_transaction_backend_t *backend, config_transaction_record_info_t *out_running_info);

/**
 * @brief Recover from a RUNNING that is not usable by copying
 *        LAST_KNOWN_GOOD's content into RUNNING.
 *
 * Intended for the CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED outcome (see
 * config_transaction_backend_recover()) -- self-heals RUNNING so a
 * subsequent boot no longer needs to fall back.
 *
 * @param out_running_info Optional; populated with the restored RUNNING
 *        record's metadata on success.
 * @return CONFIG_TXN_BACKEND_NOT_FOUND if LAST_KNOWN_GOOD has no committed
 *         record; CONFIG_TXN_BACKEND_INDETERMINATE if mutations are
 *         blocked; CONFIG_TXN_BACKEND_IO on a write/classification failure;
 *         CONFIG_TXN_BACKEND_OK once RUNNING is confirmed to hold
 *         LAST_KNOWN_GOOD's content.
 * @note Hardware side effect: one role rewrite (RUNNING).
 */
config_transaction_backend_result_t config_transaction_backend_recover_fallback_to_last_known_good(
    config_transaction_backend_t *backend, config_transaction_record_info_t *out_running_info);

/**
 * @brief Erase all three roles unconditionally (factory reset).
 * @return CONFIG_TXN_BACKEND_IO if any role's erase/read-verify fails (a
 *         failure on a later role leaves earlier roles already erased --
 *         no rollback); CONFIG_TXN_BACKEND_OK otherwise. Re-runs
 *         config_transaction_backend_recover() internally before returning
 *         so `backend`'s cached state reflects the now-empty roles.
 * @note Hardware side effect: three sector-count erases.
 */
config_transaction_backend_result_t config_transaction_backend_reset_all(
    config_transaction_backend_t *backend);

/**
 * @brief Read @p role's committed payload into @p buffer.
 * @param buffer Destination; must be non-NULL with buffer_size >= the
 *        role's payload_size.
 * @param out_payload_size Optional; set to the role's payload_size on success.
 * @param out_info Optional; set to the role's metadata on success.
 * @return CONFIG_TXN_BACKEND_NOT_FOUND if @p role has no committed record;
 *         CONFIG_TXN_BACKEND_INVALID_ARGUMENT for a NULL/undersized buffer;
 *         CONFIG_TXN_BACKEND_IO on a storage read failure or a CRC mismatch
 *         against the cached metadata (defensive re-check: Flash content
 *         may have changed since the last recover()/write of this role);
 *         CONFIG_TXN_BACKEND_OK otherwise.
 * @note Hardware side effect: read-only.
 */
config_transaction_backend_result_t config_transaction_backend_read(
    const config_transaction_backend_t *backend, config_txn_role_t role,
    void *buffer, size_t buffer_size, size_t *out_payload_size,
    config_transaction_record_info_t *out_info);

/**
 * @brief Returns a read-only pointer to @p role's cached metadata (as of
 *        the last recover() or mutating call that touched it).
 * @return NULL if @p backend is NULL or @p role is out of range.
 */
const config_transaction_record_info_t *config_transaction_backend_role_info(
    const config_transaction_backend_t *backend, config_txn_role_t role);

/**
 * @brief RP2350 application binding of config_transaction_backend_init().
 *
 * Reads are direct XIP reads; program/erase route through the shared
 * flash_operation_coordinator at the three role offsets carved out of
 * OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES (see the .c file). Host tests
 * should instead call config_transaction_backend_init() directly with fake
 * storage callbacks.
 *
 * @param backend Instance to initialize; must not be NULL.
 */
void config_transaction_backend_init_flash(config_transaction_backend_t *backend);

#endif

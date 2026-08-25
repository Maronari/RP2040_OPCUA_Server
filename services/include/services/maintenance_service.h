/**
 * @file
 * @brief Maintenance-event/counter workflow service: PLAN -> START -> FINISH
 *        (with optional RESCHEDULE) state machine over a durable Flash
 *        journal.
 *
 * Responsibilities:
 * - Owns the single current maintenance activity/state
 *   (`maintenance_service_snapshot_t`) and its revision counter.
 * - Exposes a two-phase prepare/apply/commit (or abort) transaction API so
 *   callers - normally the OPC UA method-call layer - can validate and
 *   stage a state change, persist it to the maintenance journal, and only
 *   then publish it atomically.
 * - Delegates all Flash I/O to `drivers/maintenance_journal_backend.h`
 *   (`maintenance_journal_backend_init/_recover/_append/_reset/_export_*`);
 *   never touches Flash registers directly.
 *
 * See docs/code/persistence.md ("Персистентность журнала обслуживания")
 * and docs/maintenance_journal.md for the journal's on-Flash format and
 * transactional (commit-page classified) recovery model - unlike
 * `history_service`'s Flash ring, the maintenance journal *is*
 * transactional.
 */
#ifndef SERVICES_MAINTENANCE_SERVICE_H
#define SERVICES_MAINTENANCE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/* Every character capacity includes the terminating NUL byte. */
#define MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY 48u
#define MAINTENANCE_SERVICE_TEXT_CAPACITY 48u
#define MAINTENANCE_SERVICE_ACTOR_CAPACITY 32u
#define MAINTENANCE_SERVICE_RESULT_CAPACITY 96u
#define MAINTENANCE_SERVICE_MAX_PARTS 4u
#define MAINTENANCE_SERVICE_EXPORT_PAGE_MAX 4u
#define MAINTENANCE_SERVICE_PART_TEXT_CAPACITY 48u
#define MAINTENANCE_SERVICE_MAX_DOWNTIME_MS 2592000000.0 /* 30 days */

/** @brief Maintenance workflow state; the current state gates which `prepare_*` calls are legal (see each function's `@pre`). */
typedef enum
{
    MAINTENANCE_SERVICE_STATE_FINISHED = 0,
    MAINTENANCE_SERVICE_STATE_PLANNED = 1,
    MAINTENANCE_SERVICE_STATE_EXECUTING = 2
} maintenance_service_state_t;

/** @brief How a maintenance activity was/will be carried out. */
typedef enum
{
    MAINTENANCE_SERVICE_METHOD_LOCAL = 0,
    MAINTENANCE_SERVICE_METHOD_REMOTE = 1
} maintenance_service_method_t;

/** @brief Which workflow operation produced a transaction/journal record; values 0-3 map 1:1 onto `maintenance_journal_operation_t`. */
typedef enum
{
    MAINTENANCE_SERVICE_OPERATION_PLAN = 0,
    MAINTENANCE_SERVICE_OPERATION_RESCHEDULE = 1,
    MAINTENANCE_SERVICE_OPERATION_START = 2,
    MAINTENANCE_SERVICE_OPERATION_FINISH = 3
} maintenance_service_operation_t;

/** @brief State transition associated with a workflow operation (RESCHEDULE has no state change, hence NONE). */
typedef enum
{
    MAINTENANCE_SERVICE_TRANSITION_NONE = 0,
    MAINTENANCE_SERVICE_TRANSITION_FINISHED_TO_PLANNED = 1,
    MAINTENANCE_SERVICE_TRANSITION_PLANNED_TO_EXECUTING = 2,
    MAINTENANCE_SERVICE_TRANSITION_EXECUTING_TO_FINISHED = 3
} maintenance_service_transition_t;

/**
 * @brief Result/error code returned by every `maintenance_service_*` API.
 *
 * BUSY specifically means "another transaction is already pending, or this
 * transaction token is stale/no longer matches the pending one" - it is the
 * expected outcome of a concurrent or out-of-order prepare/apply/commit
 * call, not an internal fault.
 */
typedef enum
{
    MAINTENANCE_SERVICE_RESULT_OK = 0,
    MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED = 1,
    MAINTENANCE_SERVICE_RESULT_BUSY = 2,
    MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT = 3,
    MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE = 4,
    MAINTENANCE_SERVICE_RESULT_INVALID_STATE = 5,
    MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR = 6,
    MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE = 7
} maintenance_service_result_t;

/** @brief Outcome of maintenance_service_reset_journal_ex(): whether the reset actually changed anything. */
typedef enum
{
    MAINTENANCE_SERVICE_RESET_NO_CHANGE = 0,
    MAINTENANCE_SERVICE_RESET_APPLIED = 1
} maintenance_service_reset_outcome_t;

/** @brief Details of one planned/executed/finished maintenance activity (the workflow's payload). */
typedef struct
{
    int64_t planned_date_utc_100ns;
    double estimated_downtime_ms;
    maintenance_service_method_t method;
    bool configuration_changed;
    uint8_t parts_serviced_count;
    uint8_t parts_replaced_count;
    char operation_id[MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY];
    char supplier[MAINTENANCE_SERVICE_TEXT_CAPACITY];
    char personnel[MAINTENANCE_SERVICE_TEXT_CAPACITY];
    char parts_serviced[MAINTENANCE_SERVICE_MAX_PARTS]
                       [MAINTENANCE_SERVICE_PART_TEXT_CAPACITY];
    char parts_replaced[MAINTENANCE_SERVICE_MAX_PARTS]
                       [MAINTENANCE_SERVICE_PART_TEXT_CAPACITY];
    char actor[MAINTENANCE_SERVICE_ACTOR_CAPACITY];
    char result[MAINTENANCE_SERVICE_RESULT_CAPACITY];
} maintenance_activity_t;

/**
 * @brief Point-in-time, by-value copy of the maintenance workflow state.
 *
 * Callers never receive a pointer into service storage - every accessor
 * (`maintenance_service_get_snapshot()`, `_commit()`, etc.) copies out a
 * full struct. `has_activity` is false only for the freshly-initialized
 * (never-planned) FINISHED state; every PLANNED/EXECUTING/FINISHED-after-a-
 * cycle snapshot has an activity.
 */
typedef struct
{
    maintenance_activity_t activity;
    maintenance_service_state_t state;
    uint32_t revision;
    int64_t committed_at_utc_100ns;
    bool has_activity;
} maintenance_service_snapshot_t;

/**
 * @brief Cursor for paged export of committed journal records
 *        (maintenance_service_export_begin()/_export_page()).
 *
 * A logical, bounded journal view. It contains decoded service state only;
 * callers never observe Flash addresses, slot bytes, or driver-owned
 * buffers.
 */
typedef struct
{
    uint64_t epoch;
    uint64_t upper_sequence;
    uint64_t next_sequence;
    uint32_t generation;
} maintenance_service_export_cursor_t;

/** @brief One decoded, exported journal record: lineage plus the workflow operation/transition/snapshot it represents. */
typedef struct
{
    uint64_t epoch;
    uint64_t sequence;
    maintenance_service_operation_t operation;
    maintenance_service_transition_t transition;
    maintenance_service_snapshot_t snapshot;
} maintenance_service_export_record_t;

/**
 * @brief Opaque capability for the one bounded in-flight transaction.
 *
 * A token is valid only until abort or commit. It contains no pointers and
 * no OPC UA types, so it is safe to keep on a method callback stack. Every
 * `prepare_*`/`_apply`/`_commit`/`_abort`/`_get_prepared_snapshot` call
 * compares the passed transaction byte-for-byte against the single pending
 * transaction (maintenance_service_transaction_matches() in the `.c` file);
 * only one transaction may be in flight at a time.
 */
typedef struct
{
    uint32_t token;
    uint32_t base_revision;
    uint32_t candidate_revision;
    maintenance_service_operation_t operation;
    maintenance_service_transition_t transition;
} maintenance_service_transaction_t;

/**
 * @brief Latched record of a commit whose event publication (to the OPC UA
 *        model layer) is known/suspected to have failed.
 *
 * Set via maintenance_service_mark_event_publication_fault() after a commit
 * succeeds but its downstream event notification could not be confirmed;
 * cleared via maintenance_service_clear_event_publication_fault(). While
 * `active`, maintenance_service_store_prepared() (the internal helper
 * behind every `prepare_*` call) refuses to start a new transaction, so a
 * caller must resolve/acknowledge the fault before further workflow
 * progress.
 */
typedef struct
{
    bool active;
    uint32_t revision;
    maintenance_service_operation_t operation;
} maintenance_service_event_publication_fault_t;

/** @brief Test/provisioning journal storage callback: reads @p size bytes at @p offset into @p destination. Return false on I/O failure. */
typedef bool (*maintenance_service_journal_read_fn)(void *context, uint32_t offset,
                                                    uint8_t *destination, size_t size);
/** @brief Test/provisioning journal storage callback: programs a 256-byte Flash page at @p offset. Return false on I/O failure. */
typedef bool (*maintenance_service_journal_program_fn)(void *context, uint32_t offset,
                                                       const uint8_t page[256]);
/** @brief Test/provisioning journal storage callback: erases the sector containing @p offset. Return false on I/O failure. */
typedef bool (*maintenance_service_journal_erase_fn)(void *context, uint32_t offset);
/** @brief Injectable Flash storage callback table for maintenance_service_init_with_journal(), mirroring `maintenance_journal_storage_t`. */
typedef struct
{
    void *context;
    maintenance_service_journal_read_fn read;
    maintenance_service_journal_program_fn program_page;
    maintenance_service_journal_erase_fn erase_sector;
} maintenance_service_journal_storage_t;

/** @brief Service-facing mirror of `maintenance_journal_recovery_state_t`, reported via maintenance_service_get_journal_diagnostics(). */
typedef enum
{
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_EMPTY = 0,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_OK,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_DEGRADED,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_INDETERMINATE,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_FATAL,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_UNSUPPORTED
} maintenance_service_journal_recovery_t;

/** @brief Read-only snapshot of the underlying journal backend's health, for diagnostics/OPC UA exposure. */
typedef struct
{
    bool enabled;
    bool mutation_blocked;
    maintenance_service_journal_recovery_t recovery;
    uint8_t last_backend_result;
    uint32_t corrupt_record_count;
    uint32_t unknown_record_count;
    uint8_t retained_record_count;
    uint8_t erased_sector_count;
} maintenance_service_journal_diagnostics_t;

/** @brief Request payload for maintenance_service_prepare_plan(); legal only when the workflow is FINISHED. */
typedef struct
{
    int64_t command_time_utc_100ns;
    int64_t planned_date_utc_100ns;
    double estimated_downtime_ms;
    maintenance_service_method_t method;
    char operation_id[MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY];
    char supplier[MAINTENANCE_SERVICE_TEXT_CAPACITY];
    char personnel[MAINTENANCE_SERVICE_TEXT_CAPACITY];
    char actor[MAINTENANCE_SERVICE_ACTOR_CAPACITY];
} maintenance_service_plan_request_t;

/** @brief Request payload for maintenance_service_prepare_reschedule(); legal only when the workflow is PLANNED. */
typedef struct
{
    int64_t command_time_utc_100ns;
    int64_t planned_date_utc_100ns;
    double estimated_downtime_ms;
    maintenance_service_method_t method;
    char supplier[MAINTENANCE_SERVICE_TEXT_CAPACITY];
    char personnel[MAINTENANCE_SERVICE_TEXT_CAPACITY];
} maintenance_service_reschedule_request_t;

/** @brief Request payload for maintenance_service_prepare_start(); legal only when the workflow is PLANNED. */
typedef struct
{
    int64_t command_time_utc_100ns;
    char actor[MAINTENANCE_SERVICE_ACTOR_CAPACITY];
} maintenance_service_start_request_t;

/** @brief Request payload for maintenance_service_prepare_finish(); legal only when the workflow is EXECUTING. */
typedef struct
{
    int64_t command_time_utc_100ns;
    bool configuration_changed;
    uint8_t parts_serviced_count;
    uint8_t parts_replaced_count;
    char parts_serviced[MAINTENANCE_SERVICE_MAX_PARTS]
                       [MAINTENANCE_SERVICE_PART_TEXT_CAPACITY];
    char parts_replaced[MAINTENANCE_SERVICE_MAX_PARTS]
                       [MAINTENANCE_SERVICE_PART_TEXT_CAPACITY];
    char actor[MAINTENANCE_SERVICE_ACTOR_CAPACITY];
    char result[MAINTENANCE_SERVICE_RESULT_CAPACITY];
} maintenance_service_finish_request_t;

/**
 * @brief Initializes the service with the RP2350 on-device Flash journal
 *        binding (or no journal on host builds without `PICO_ON_DEVICE`).
 *
 * Equivalent to `maintenance_service_init_with_journal(NULL)`. Idempotent:
 * returns MAINTENANCE_SERVICE_RESULT_OK immediately if already initialized.
 *
 * @return MAINTENANCE_SERVICE_RESULT_OK on success (including "already
 *         initialized"); a journal-recovery-derived error if Flash
 *         recovery reports FATAL and is not otherwise treated as
 *         recoverable (see maintenance_service_init_with_journal()).
 */
maintenance_service_result_t maintenance_service_init(void);

/**
 * @brief Initializes the service, optionally binding a caller-supplied
 *        journal storage callback table instead of the on-device Flash journal.
 *
 * Test/provisioning binding only; callers must provide @p storage before
 * relying on non-default storage semantics - the service owns workflow
 * state, while this driver callback table remains hardware/host-owned. On
 * device builds (`PICO_ON_DEVICE`), passing NULL binds the real Flash
 * journal (`maintenance_journal_backend_init_flash()`); on host builds,
 * NULL disables the journal entirely (no persistence). Recovers journal
 * state synchronously before returning, seeding the initial snapshot from
 * the journal's active record if one exists.
 *
 * @param storage Caller-owned callback table, or NULL for the default
 *        on-device (or disabled, off-device) binding. Not copied beyond
 *        function pointers/context; must outlive the service.
 * @return MAINTENANCE_SERVICE_RESULT_OK if already initialized or if
 *         initialization (including journal recovery) succeeds or
 *         degrades gracefully; an error derived from the journal recovery
 *         result otherwise (see docs/maintenance_journal.md for the
 *         recovery classification).
 * @pre Must be called before any other `maintenance_service_*` function.
 */
maintenance_service_result_t maintenance_service_init_with_journal(
    const maintenance_service_journal_storage_t *storage);

/**
 * @brief Copies the currently-active (committed) maintenance snapshot to @p out_snapshot.
 *
 * @param out_snapshot Destination; must be non-NULL.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT if @p out_snapshot
 *         is NULL; MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED if the
 *         service has not been initialized.
 * @note Safe to call from any task; internally uses a cross-core critical
 *       section (see docs/code/state-ownership.md).
 */
maintenance_service_result_t maintenance_service_get_snapshot(
    maintenance_service_snapshot_t *out_snapshot);

/**
 * @brief Validates @p request and stages a PLAN transaction (FINISHED -> PLANNED).
 *
 * Prepare does not alter the active snapshot or its revision - it only
 * stages a candidate and issues a transaction token; the change becomes
 * visible only after maintenance_service_apply() + maintenance_service_commit().
 *
 * @param request Plan details (operation id, schedule, method, personnel).
 * @param out_transaction Receives the new transaction token on success;
 *        must be non-NULL.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT for malformed string
 *         fields or a NULL @p out_transaction;
 *         MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE for an invalid schedule/
 *         method/command time; MAINTENANCE_SERVICE_RESULT_INVALID_STATE if
 *         the workflow is not currently FINISHED;
 *         MAINTENANCE_SERVICE_RESULT_BUSY if another transaction is
 *         already pending; MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE
 *         if an event-publication fault is latched or journal mutations
 *         are blocked.
 * @pre Workflow state must be MAINTENANCE_SERVICE_STATE_FINISHED.
 */
maintenance_service_result_t maintenance_service_prepare_plan(
    const maintenance_service_plan_request_t *request,
    maintenance_service_transaction_t *out_transaction);

/**
 * @brief Validates @p request and stages a RESCHEDULE transaction (no state change).
 *
 * See maintenance_service_prepare_plan() for the general prepare/apply/
 * commit contract.
 *
 * @param request New schedule/method/supplier/personnel.
 * @param out_transaction Receives the new transaction token on success;
 *        must be non-NULL.
 * @return Same result vocabulary as maintenance_service_prepare_plan().
 * @pre Workflow state must be MAINTENANCE_SERVICE_STATE_PLANNED with an
 *      existing activity.
 */
maintenance_service_result_t maintenance_service_prepare_reschedule(
    const maintenance_service_reschedule_request_t *request,
    maintenance_service_transaction_t *out_transaction);

/**
 * @brief Validates @p request and stages a START transaction (PLANNED -> EXECUTING).
 *
 * See maintenance_service_prepare_plan() for the general prepare/apply/
 * commit contract.
 *
 * @param request Command time and acting actor.
 * @param out_transaction Receives the new transaction token on success;
 *        must be non-NULL.
 * @return Same result vocabulary as maintenance_service_prepare_plan().
 * @pre Workflow state must be MAINTENANCE_SERVICE_STATE_PLANNED with an
 *      existing activity.
 */
maintenance_service_result_t maintenance_service_prepare_start(
    const maintenance_service_start_request_t *request,
    maintenance_service_transaction_t *out_transaction);

/**
 * @brief Validates @p request and stages a FINISH transaction (EXECUTING -> FINISHED).
 *
 * See maintenance_service_prepare_plan() for the general prepare/apply/
 * commit contract.
 *
 * @param request Result text, actor, and serviced/replaced parts lists.
 * @param out_transaction Receives the new transaction token on success;
 *        must be non-NULL.
 * @return Same result vocabulary as maintenance_service_prepare_plan().
 * @pre Workflow state must be MAINTENANCE_SERVICE_STATE_EXECUTING with an
 *      existing activity.
 */
maintenance_service_result_t maintenance_service_prepare_finish(
    const maintenance_service_finish_request_t *request,
    maintenance_service_transaction_t *out_transaction);

/**
 * @brief Copies the staged (not-yet-committed) candidate snapshot for @p transaction.
 *
 * The prepared snapshot is a copy and is never visible through the OPC UA
 * model until committed; this lets a caller (e.g. the method callback)
 * inspect what it is about to commit.
 *
 * @param transaction Must match the single pending transaction exactly
 *        (token, revisions, operation, transition).
 * @param out_snapshot Destination; must be non-NULL.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT if @p out_snapshot
 *         is NULL; MAINTENANCE_SERVICE_RESULT_BUSY if @p transaction does
 *         not match the pending transaction or persistence is currently in
 *         flight for it.
 */
maintenance_service_result_t maintenance_service_get_prepared_snapshot(
    const maintenance_service_transaction_t *transaction,
    maintenance_service_snapshot_t *out_snapshot);

/**
 * @brief Persists the staged transaction's candidate to the maintenance
 *        journal and copies it into the inactive snapshot slot.
 *
 * Apply copies the candidate to the inactive slot; it remains invisible to
 * maintenance_service_get_snapshot() until maintenance_service_commit()
 * flips the active index. Releases the internal lock before performing
 * Flash I/O (`maintenance_journal_backend_append()`), so this call may
 * block on Flash but never runs Flash I/O with the service lock held.
 * Idempotent for an already-applied matching transaction (returns OK
 * without repeating the journal write). On a journal write failure, the
 * transaction reverts to the PREPARED phase (so a caller may retry apply)
 * and, if the backend reports mutations blocked, the whole journal is
 * latched as mutation-blocked for all future transactions.
 *
 * @param transaction Must match the single pending transaction exactly.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success (or if already
 *         applied); MAINTENANCE_SERVICE_RESULT_BUSY if @p transaction does
 *         not match or the phase has advanced unexpectedly;
 *         MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE if not in the
 *         PREPARED phase, journal mutations are blocked, or journal I/O is
 *         already busy; a journal-backend-derived error on write failure.
 * @note May block on Flash program/erase operations via the maintenance
 *       journal backend.
 */
maintenance_service_result_t maintenance_service_apply(
    const maintenance_service_transaction_t *transaction);

/**
 * @brief Publishes the applied candidate as the new active snapshot,
 *        advancing the workflow state and revision.
 *
 * Commit is the only operation that changes the active snapshot/revision -
 * it flips `active_index` to the already-populated inactive slot. Clears
 * the pending transaction/snapshot on success.
 *
 * @param transaction Must match the single pending transaction exactly,
 *        and the transaction must already be in the APPLIED phase (i.e.
 *        maintenance_service_apply() must have succeeded first).
 * @param out_snapshot Receives the newly-active snapshot on success; must
 *        be non-NULL.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT if @p out_snapshot
 *         is NULL; MAINTENANCE_SERVICE_RESULT_BUSY if @p transaction does
 *         not match or is not yet applied;
 *         MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR if the inactive slot's
 *         revision unexpectedly does not match the transaction's candidate
 *         revision (defensive consistency check).
 */
maintenance_service_result_t maintenance_service_commit(
    const maintenance_service_transaction_t *transaction,
    maintenance_service_snapshot_t *out_snapshot);

/**
 * @brief Discards the pending transaction without publishing its candidate.
 *
 * @param transaction Must match the single pending transaction exactly.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_BUSY if @p transaction does not match
 *         the pending transaction, or if persistence (apply) is currently
 *         in flight for it (abort is refused mid-Flash-write; retry once
 *         apply resolves).
 */
maintenance_service_result_t maintenance_service_abort(
    const maintenance_service_transaction_t *transaction);

/**
 * @brief Latches an event-publication fault for the given committed
 *        @p revision/@p operation, blocking new transactions until cleared.
 *
 * Post-commit event-publication recovery state: called by the caller that
 * committed a transaction but could not confirm the corresponding OPC UA
 * event was published. Idempotent for the same revision/operation; rejects
 * a mismatched second fault while one is already active.
 *
 * @param revision Must match the currently-active snapshot's revision.
 * @param operation Must be a valid `maintenance_service_operation_t`.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT for an invalid
 *         @p operation or zero @p revision;
 *         MAINTENANCE_SERVICE_RESULT_BUSY if @p revision no longer matches
 *         the active snapshot; MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE
 *         if a different fault is already latched.
 */
maintenance_service_result_t maintenance_service_mark_event_publication_fault(
    uint32_t revision, maintenance_service_operation_t operation);

/**
 * @brief Copies the current event-publication fault state to @p out_fault.
 * @param out_fault Destination; must be non-NULL.
 */
maintenance_service_result_t maintenance_service_get_event_publication_fault(
    maintenance_service_event_publication_fault_t *out_fault);

/** @brief Clears any latched event-publication fault, re-enabling new transactions. */
maintenance_service_result_t maintenance_service_clear_event_publication_fault(void);

/**
 * @brief Resets the maintenance journal's storage (starts a new epoch)
 *        while preserving the current workflow state/revision.
 *
 * Equivalent to `maintenance_service_reset_journal_ex(NULL)`. Logical
 * reset is storage-only: it preserves workflow state and revision - the
 * in-memory active snapshot is unchanged; only the durable journal's
 * lineage is reset (recorded as a RESET-type journal record).
 *
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE if the journal
 *         is disabled, mutations are blocked, a transaction is pending, or
 *         journal I/O is already busy; a journal-backend-derived error on
 *         write failure.
 * @note May block on Flash program/erase operations.
 */
maintenance_service_result_t maintenance_service_reset_journal(void);

/**
 * @brief Resets the maintenance journal's storage, reporting whether the
 *        reset actually changed anything.
 *
 * `out_outcome` lets the OPC UA adapter audit only a newly committed
 * reset (MAINTENANCE_SERVICE_RESET_APPLIED) versus a reset that was a
 * no-op because the journal was already empty/already reset
 * (MAINTENANCE_SERVICE_RESET_NO_CHANGE). It may be NULL when a caller only
 * needs the legacy boolean-style result.
 *
 * @param out_outcome Receives the outcome on a successful call; may be NULL.
 * @return Same result vocabulary as maintenance_service_reset_journal().
 * @note May block on Flash program/erase operations.
 */
maintenance_service_result_t maintenance_service_reset_journal_ex(
    maintenance_service_reset_outcome_t *out_outcome);

/**
 * @brief Copies current journal backend health/diagnostics to @p out_diagnostics.
 * @param out_diagnostics Destination; must be non-NULL.
 */
maintenance_service_result_t maintenance_service_get_journal_diagnostics(
    maintenance_service_journal_diagnostics_t *out_diagnostics);

/**
 * @brief Checks whether @p operation_id matches the currently-active
 *        snapshot's activity, for idempotent retry of a Plan call.
 *
 * Plan is the only v0.3.2 operation with a caller supplied durable identity
 * (`operation_id`). A retry uses this before prepare to distinguish its
 * committed outcome from a conflicting Plan without consuming a new
 * journal slot: if a prior Plan call's response was lost but it actually
 * committed, this lets the caller discover that without re-attempting
 * `prepare_plan` (which would fail with INVALID_STATE since the workflow
 * is no longer FINISHED).
 *
 * @param operation_id Caller-supplied durable identity to check.
 * @param out_snapshot Receives the active snapshot if @p out_committed is
 *        true; must be non-NULL.
 * @param out_committed Receives true if the active snapshot's activity has
 *        this operation id; must be non-NULL.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success (regardless of whether
 *         it matched); MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT for a
 *         malformed @p operation_id or NULL output pointers.
 */
maintenance_service_result_t maintenance_service_reconcile_operation_id(
    const char *operation_id, maintenance_service_snapshot_t *out_snapshot,
    bool *out_committed);

/**
 * @brief Starts a paged export of all committed journal records, initializing @p out_cursor.
 *
 * @param out_cursor Destination cursor; must be non-NULL.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_STATE if the backend reports
 *         nothing to export; MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE
 *         if the journal is disabled or I/O is already busy; a
 *         journal-backend-derived error otherwise.
 */
maintenance_service_result_t maintenance_service_export_begin(maintenance_service_export_cursor_t *out_cursor);

/**
 * @brief Reads the next page (up to MAINTENANCE_SERVICE_EXPORT_PAGE_MAX
 *        records) of a journal export started by maintenance_service_export_begin().
 *
 * @param cursor Cursor from export_begin() or a prior export_page() call;
 *        advanced in place on success.
 * @param out_records Destination array of at least
 *        MAINTENANCE_SERVICE_EXPORT_PAGE_MAX records; must be non-NULL.
 * @param out_count Receives the number of records written to @p out_records.
 * @param out_more Receives true if further pages remain.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT for any NULL
 *         pointer argument; MAINTENANCE_SERVICE_RESULT_INVALID_STATE if
 *         the backend reports the cursor as not found (e.g. journal reset
 *         mid-export); MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE if
 *         the journal is disabled or I/O is already busy.
 * @note May block on Flash read operations.
 */
maintenance_service_result_t maintenance_service_export_page(maintenance_service_export_cursor_t *cursor,
    maintenance_service_export_record_t out_records[MAINTENANCE_SERVICE_EXPORT_PAGE_MAX], uint8_t *out_count, bool *out_more);

/** @brief Returns a short, stable, human-readable name for @p result (e.g. "busy"), for logging/diagnostics. Never returns NULL. */
const char *maintenance_service_result_to_string(
    maintenance_service_result_t result);

#endif

/**
 * @file
 * @brief Maintenance workflow state machine implementation; see
 *        services/include/services/maintenance_service.h for the public
 *        API contract.
 *
 * Ownership:
 * `g_maintenance_service_runtime` (double-buffered active/inactive
 * snapshots, pending transaction, event-publication fault, and the
 * embedded `maintenance_journal_backend_t`) and
 * `g_maintenance_service_export_records` are both owned exclusively by
 * this module.
 *
 * Concurrency:
 * Uses `portENTER_CRITICAL`/`portEXIT_CRITICAL` (explicitly documented as
 * working across both RP2350 cores without a FreeRTOS mutex - see
 * maintenance_service_lock()), not `taskENTER_CRITICAL`. Two-phase apply/
 * commit: `prepare_*` stages a candidate under the lock; `apply` copies it,
 * releases the lock, appends to the Flash journal **without** holding the
 * lock, then re-locks only to record the result in the *inactive* snapshot
 * slot; `commit` is the only function that flips `active_index`, publishing
 * the result atomically. The `pending_transaction`'s token plus base/
 * candidate revision numbers guard every step against a stale caller (see
 * maintenance_service_transaction_matches()). See
 * docs/code/state-ownership.md ("Обслуживание") for the authoritative
 * description of this pattern.
 */
#include "services/maintenance_service.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <FreeRTOS.h>

#include "drivers/maintenance_journal_backend.h"

/** @brief Lifecycle phase of the single pending transaction, gating which operation may run next. */
typedef enum
{
    MAINTENANCE_PENDING_NONE = 0,
    MAINTENANCE_PENDING_PREPARED = 1,
    MAINTENANCE_PENDING_PERSISTING = 2,
    MAINTENANCE_PENDING_APPLIED = 3
} maintenance_service_pending_phase_t;

/**
 * @brief All service-owned runtime state: double-buffered snapshots, the
 *        one pending transaction, event-publication fault latch, and the
 *        embedded journal backend.
 *
 * `snapshots[active_index]` is the published/committed state;
 * `snapshots[active_index ^ 1]` is the inactive slot used to stage an
 * applied-but-not-yet-committed candidate.
 */
typedef struct
{
    maintenance_service_snapshot_t snapshots[2];
    maintenance_service_snapshot_t pending_snapshot;
    maintenance_service_transaction_t pending_transaction;
    maintenance_service_event_publication_fault_t event_fault;
    maintenance_journal_backend_t journal;
    bool journal_enabled;
    bool journal_mutations_blocked;
    /* Serializes the driver's static RAM scratch without holding a critical
     * section across Flash I/O. */
    bool journal_io_busy;
    uint32_t next_token;
    uint8_t active_index;
    maintenance_service_pending_phase_t pending_phase;
    bool initialized;
} maintenance_service_runtime_t;

static maintenance_service_runtime_t g_maintenance_service_runtime;
/* Driver records are decoded into this bounded service-owned staging array
 * before copies cross the service boundary. Never allocate it on a task stack. */
static maintenance_journal_record_t
    g_maintenance_service_export_records[MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX];

/** @brief Maps a `maintenance_journal_backend_result_t` to the coarser `maintenance_service_result_t` vocabulary (DEGRADED still reports OK to callers). */
static maintenance_service_result_t maintenance_service_backend_result(
    maintenance_journal_backend_result_t result)
{
    return result == MAINTENANCE_JOURNAL_BACKEND_OK ||
                   result == MAINTENANCE_JOURNAL_BACKEND_DEGRADED
               ? MAINTENANCE_SERVICE_RESULT_OK
               : (result == MAINTENANCE_JOURNAL_BACKEND_CAPACITY ||
                  result == MAINTENANCE_JOURNAL_BACKEND_IO ||
                  result == MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE ||
                  result == MAINTENANCE_JOURNAL_BACKEND_UNSUPPORTED ||
                  result == MAINTENANCE_JOURNAL_BACKEND_FATAL
                      ? MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE
                      : MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR);
}

/** @brief Converts a `maintenance_service_operation_t` to its `maintenance_journal_operation_t` counterpart (values are defined to match 1:1). */
static maintenance_journal_operation_t maintenance_service_journal_operation(
    maintenance_service_operation_t operation)
{
    return (maintenance_journal_operation_t)operation;
}

/** @brief Builds a `maintenance_journal_record_t` (SNAPSHOT type) from a candidate snapshot and its transaction, for journal append. */
static maintenance_journal_record_t maintenance_service_make_record(
    const maintenance_service_snapshot_t *snapshot,
    const maintenance_service_transaction_t *transaction)
{
    maintenance_journal_record_t record = {0};
    record.command_time_utc_100ns = snapshot->committed_at_utc_100ns;
    record.type = MAINTENANCE_JOURNAL_RECORD_SNAPSHOT;
    record.operation = maintenance_service_journal_operation(transaction->operation);
    record.transition = transaction->transition;
    record.snapshot = *snapshot;
    return record;
}

/** @brief Checks that @p text is NUL-terminated within @p capacity bytes (may be empty). */
static bool maintenance_service_has_terminated_string(const char *text,
                                                       size_t capacity)
{
    return text != NULL && capacity > 0u && memchr(text, '\0', capacity) != NULL;
}

/** @brief Checks that @p text is NUL-terminated within @p capacity bytes and non-empty. */
static bool maintenance_service_has_nonempty_string(const char *text,
                                                     size_t capacity)
{
    return maintenance_service_has_terminated_string(text, capacity) && text[0] != '\0';
}

/** @brief Copies @p source into @p destination, zero-padding the remaining @p capacity bytes (bounded, always NUL-terminated within capacity). */
static void maintenance_service_copy_string(char *destination,
                                            const char *source,
                                            size_t capacity)
{
    size_t length = 0u;

    while (length < capacity && source[length] != '\0')
    {
        ++length;
    }

    memset(destination, 0, capacity);
    memcpy(destination, source, length);
}

/** @brief Copies @p count part-name strings from @p source into @p destination, zero-padding both the unused strings and any beyond @p count. */
static void maintenance_service_copy_parts(
    char destination[MAINTENANCE_SERVICE_MAX_PARTS]
                    [MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
    const char source[MAINTENANCE_SERVICE_MAX_PARTS]
                     [MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
    uint8_t count)
{
    uint8_t index;

    memset(destination, 0, MAINTENANCE_SERVICE_MAX_PARTS *
                           MAINTENANCE_SERVICE_PART_TEXT_CAPACITY);
    for (index = 0u; index < count; ++index)
    {
        maintenance_service_copy_string(destination[index], source[index],
                                        MAINTENANCE_SERVICE_PART_TEXT_CAPACITY);
    }
}

/** @brief Range-checks a `maintenance_service_method_t` value. */
static bool maintenance_service_method_is_valid(maintenance_service_method_t method)
{
    return method == MAINTENANCE_SERVICE_METHOD_LOCAL ||
           method == MAINTENANCE_SERVICE_METHOD_REMOTE;
}

/** @brief Range-checks a `maintenance_service_operation_t` value (excludes the journal-only RESET operation). */
static bool maintenance_service_operation_is_valid(maintenance_service_operation_t operation)
{
    return operation == MAINTENANCE_SERVICE_OPERATION_PLAN ||
           operation == MAINTENANCE_SERVICE_OPERATION_RESCHEDULE ||
           operation == MAINTENANCE_SERVICE_OPERATION_START ||
           operation == MAINTENANCE_SERVICE_OPERATION_FINISH;
}

/** @brief Validates a planned-date/estimated-downtime pair: positive future date, finite downtime strictly within `(0, MAINTENANCE_SERVICE_MAX_DOWNTIME_MS]`. */
static bool maintenance_service_schedule_is_valid(int64_t planned_date_utc_100ns,
                                                  double estimated_downtime_ms)
{
    return planned_date_utc_100ns > 0 && isfinite(estimated_downtime_ms) &&
           estimated_downtime_ms > 0.0 &&
           estimated_downtime_ms <= MAINTENANCE_SERVICE_MAX_DOWNTIME_MS;
}

/** @brief Validates a parts list: @p count within bounds and every used entry a non-empty terminated string. */
static bool maintenance_service_parts_are_valid(
    uint8_t count,
    const char parts[MAINTENANCE_SERVICE_MAX_PARTS]
                    [MAINTENANCE_SERVICE_PART_TEXT_CAPACITY])
{
    uint8_t index;

    if (count > MAINTENANCE_SERVICE_MAX_PARTS || parts == NULL)
    {
        return false;
    }

    for (index = 0u; index < count; ++index)
    {
        if (!maintenance_service_has_nonempty_string(
                parts[index], MAINTENANCE_SERVICE_PART_TEXT_CAPACITY))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Checks initialization and enters the cross-core critical section
 *        guarding all `g_maintenance_service_runtime` fields.
 *
 * The public API is task-only. The bounded snapshot copies are protected
 * on both RP2350 cores without allocating a FreeRTOS mutex
 * (`portENTER_CRITICAL`/`portEXIT_CRITICAL`, not `taskENTER_CRITICAL`).
 * Callers must pair a successful return with maintenance_service_unlock();
 * on MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED, no lock is held.
 *
 * @return MAINTENANCE_SERVICE_RESULT_OK if the lock was entered;
 *         MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED if the service has
 *         not been initialized (no lock taken).
 */
static maintenance_service_result_t maintenance_service_lock(void)
{
    if (!g_maintenance_service_runtime.initialized)
    {
        return MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED;
    }

    /* The public API is task-only. The bounded snapshot copies are protected
     * on both RP2350 cores without allocating a FreeRTOS mutex. */
    portENTER_CRITICAL();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

/** @brief Exits the critical section entered by a successful maintenance_service_lock(). */
static void maintenance_service_unlock(void)
{
    portEXIT_CRITICAL();
}

/**
 * @brief Checks whether @p transaction is byte-for-byte the single pending
 *        transaction (token, revisions, operation, transition all match).
 *
 * The core anti-staleness guard for every transaction-scoped API
 * (`_get_prepared_snapshot`/`_apply`/`_commit`/`_abort`): a caller with an
 * outdated or foreign token cannot advance or observe another caller's
 * in-flight transaction.
 *
 * @note Must be called with the runtime lock held (reads
 *       `g_maintenance_service_runtime` without its own synchronization).
 */
static bool maintenance_service_transaction_matches(
    const maintenance_service_transaction_t *transaction)
{
    const maintenance_service_transaction_t *pending =
        &g_maintenance_service_runtime.pending_transaction;

    return transaction != NULL &&
           g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_NONE &&
           transaction->token != 0u &&
           transaction->token == pending->token &&
           transaction->base_revision == pending->base_revision &&
           transaction->candidate_revision == pending->candidate_revision &&
           transaction->operation == pending->operation &&
           transaction->transition == pending->transition;
}

/**
 * @brief Allocates the next transaction token, skipping the reserved value 0.
 * @note Must be called with the runtime lock held.
 */
static uint32_t maintenance_service_next_token(void)
{
    ++g_maintenance_service_runtime.next_token;
    if (g_maintenance_service_runtime.next_token == 0u)
    {
        ++g_maintenance_service_runtime.next_token;
    }
    return g_maintenance_service_runtime.next_token;
}

/** @brief Validates a plan request's string fields (INVALID_ARGUMENT) then its schedule/method/command time (OUT_OF_RANGE). */
static maintenance_service_result_t maintenance_service_validate_plan(
    const maintenance_service_plan_request_t *request)
{
    if (request == NULL ||
        !maintenance_service_has_nonempty_string(
            request->operation_id, MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY) ||
        !maintenance_service_has_terminated_string(
            request->supplier, MAINTENANCE_SERVICE_TEXT_CAPACITY) ||
        !maintenance_service_has_terminated_string(
            request->personnel, MAINTENANCE_SERVICE_TEXT_CAPACITY) ||
        !maintenance_service_has_nonempty_string(
            request->actor, MAINTENANCE_SERVICE_ACTOR_CAPACITY))
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }

    if (request->command_time_utc_100ns <= 0 ||
        !maintenance_service_schedule_is_valid(request->planned_date_utc_100ns,
                                                request->estimated_downtime_ms) ||
        !maintenance_service_method_is_valid(request->method))
    {
        return MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE;
    }

    return MAINTENANCE_SERVICE_RESULT_OK;
}

/** @brief Validates a reschedule request's string fields (INVALID_ARGUMENT) then its schedule/method/command time (OUT_OF_RANGE). */
static maintenance_service_result_t maintenance_service_validate_reschedule(
    const maintenance_service_reschedule_request_t *request)
{
    if (request == NULL ||
        !maintenance_service_has_terminated_string(
            request->supplier, MAINTENANCE_SERVICE_TEXT_CAPACITY) ||
        !maintenance_service_has_terminated_string(
            request->personnel, MAINTENANCE_SERVICE_TEXT_CAPACITY))
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }

    if (request->command_time_utc_100ns <= 0 ||
        !maintenance_service_schedule_is_valid(request->planned_date_utc_100ns,
                                                request->estimated_downtime_ms) ||
        !maintenance_service_method_is_valid(request->method))
    {
        return MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE;
    }

    return MAINTENANCE_SERVICE_RESULT_OK;
}

/** @brief Validates a start request: non-NULL, positive command time (OUT_OF_RANGE), non-empty actor (INVALID_ARGUMENT). */
static maintenance_service_result_t maintenance_service_validate_start(
    const maintenance_service_start_request_t *request)
{
    if (request == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }

    if (request->command_time_utc_100ns <= 0)
    {
        return MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE;
    }

    return maintenance_service_has_nonempty_string(
               request->actor, MAINTENANCE_SERVICE_ACTOR_CAPACITY)
               ? MAINTENANCE_SERVICE_RESULT_OK
               : MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
}

/** @brief Validates a finish request's actor/result/parts fields (INVALID_ARGUMENT) then its command time (OUT_OF_RANGE). */
static maintenance_service_result_t maintenance_service_validate_finish(
    const maintenance_service_finish_request_t *request)
{
    if (request == NULL ||
        !maintenance_service_has_nonempty_string(
            request->actor, MAINTENANCE_SERVICE_ACTOR_CAPACITY) ||
        !maintenance_service_has_nonempty_string(
            request->result, MAINTENANCE_SERVICE_RESULT_CAPACITY) ||
        !maintenance_service_parts_are_valid(request->parts_serviced_count,
                                             request->parts_serviced) ||
        !maintenance_service_parts_are_valid(request->parts_replaced_count,
                                             request->parts_replaced))
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }

    if (request->command_time_utc_100ns <= 0)
    {
        return MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE;
    }

    return MAINTENANCE_SERVICE_RESULT_OK;
}

/**
 * @brief Shared "stage a new transaction" tail for every `prepare_*` call:
 *        allocates a token, sets the candidate revision, and publishes it
 *        as the single pending transaction/snapshot.
 *
 * Refuses to start a new transaction if an event-publication fault is
 * latched, journal mutations are blocked, another transaction is already
 * pending, or @p before's revision does not match the currently-active
 * snapshot (stale caller). @p candidate's revision field is overwritten
 * with `before->revision + 1`, regardless of what the caller passed in.
 *
 * @param before Snapshot the candidate was derived from (must match the
 *        current active snapshot's revision).
 * @param candidate Candidate snapshot to stage; its `revision` field is
 *        overwritten by this call.
 * @param operation Workflow operation this transaction represents.
 * @param transition State transition this transaction represents.
 * @param out_transaction Receives the new transaction token; must be
 *        non-NULL.
 * @return MAINTENANCE_SERVICE_RESULT_OK on success;
 *         MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT for a NULL
 *         parameter; MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE if an
 *         event fault is latched or mutations are blocked;
 *         MAINTENANCE_SERVICE_RESULT_BUSY if another transaction is
 *         pending; MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR if @p before
 *         is stale relative to the active snapshot.
 */
static maintenance_service_result_t maintenance_service_store_prepared(
    const maintenance_service_snapshot_t *before,
    maintenance_service_snapshot_t *candidate,
    maintenance_service_operation_t operation,
    maintenance_service_transition_t transition,
    maintenance_service_transaction_t *out_transaction)
{
    maintenance_service_result_t result;
    maintenance_service_transaction_t transaction;

    if (before == NULL || candidate == NULL || out_transaction == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }

    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }

    if (g_maintenance_service_runtime.event_fault.active)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE;
    }
    if (g_maintenance_service_runtime.journal_mutations_blocked)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE;
    }
    if (g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_NONE)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }

    if (g_maintenance_service_runtime.snapshots[
            g_maintenance_service_runtime.active_index].revision != before->revision ||
        before->revision == UINT32_MAX)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR;
    }

    memset(&transaction, 0, sizeof(transaction));
    transaction.token = maintenance_service_next_token();
    transaction.base_revision = before->revision;
    transaction.candidate_revision = before->revision + 1u;
    transaction.operation = operation;
    transaction.transition = transition;
    candidate->revision = transaction.candidate_revision;
    g_maintenance_service_runtime.pending_transaction = transaction;
    g_maintenance_service_runtime.pending_snapshot = *candidate;
    g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_PREPARED;
    *out_transaction = transaction;
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_init_with_journal(
    const maintenance_service_journal_storage_t *storage)
{
    maintenance_service_snapshot_t initial_snapshot;
    maintenance_journal_backend_result_t journal_result = MAINTENANCE_JOURNAL_BACKEND_OK;

    if (g_maintenance_service_runtime.initialized)
    {
        return MAINTENANCE_SERVICE_RESULT_OK;
    }

    memset(&g_maintenance_service_runtime, 0, sizeof(g_maintenance_service_runtime));
    memset(&initial_snapshot, 0, sizeof(initial_snapshot));
    initial_snapshot.state = MAINTENANCE_SERVICE_STATE_FINISHED;
    initial_snapshot.revision = 1u;
    initial_snapshot.has_activity = false;
    g_maintenance_service_runtime.snapshots[0] = initial_snapshot;
    g_maintenance_service_runtime.snapshots[1] = initial_snapshot;
    g_maintenance_service_runtime.active_index = 0u;

    if (storage != NULL)
    {
        const maintenance_journal_storage_t backend_storage = {
            .context = storage->context,
            .read = storage->read,
            .program_page = storage->program_page,
            .erase_sector = storage->erase_sector};
        maintenance_journal_backend_init(&g_maintenance_service_runtime.journal,
                                         &backend_storage);
        g_maintenance_service_runtime.journal_enabled = true;
    }
#if defined(PICO_ON_DEVICE)
    else
    {
        maintenance_journal_backend_init_flash(&g_maintenance_service_runtime.journal);
        g_maintenance_service_runtime.journal_enabled = true;
    }
#endif
    if (g_maintenance_service_runtime.journal_enabled)
    {
        journal_result = maintenance_journal_backend_recover(
            &g_maintenance_service_runtime.journal);
        if (g_maintenance_service_runtime.journal.has_active)
        {
            initial_snapshot = g_maintenance_service_runtime.journal.active.record.snapshot;
            g_maintenance_service_runtime.snapshots[0] = initial_snapshot;
            g_maintenance_service_runtime.snapshots[1] = initial_snapshot;
        }
        g_maintenance_service_runtime.journal_mutations_blocked =
            g_maintenance_service_runtime.journal.mutations_blocked;
        /* Recovery has a coherent read snapshot even when mutation is blocked. */
        if (journal_result != MAINTENANCE_JOURNAL_BACKEND_OK &&
            journal_result != MAINTENANCE_JOURNAL_BACKEND_DEGRADED &&
            journal_result != MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE &&
            journal_result != MAINTENANCE_JOURNAL_BACKEND_FATAL &&
            journal_result != MAINTENANCE_JOURNAL_BACKEND_UNSUPPORTED)
        {
            return maintenance_service_backend_result(journal_result);
        }
    }
    g_maintenance_service_runtime.initialized = true;
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_init(void)
{
    return maintenance_service_init_with_journal(NULL);
}

maintenance_service_result_t maintenance_service_get_snapshot(
    maintenance_service_snapshot_t *out_snapshot)
{
    maintenance_service_result_t result;

    if (out_snapshot == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }

    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    *out_snapshot = g_maintenance_service_runtime.snapshots[
        g_maintenance_service_runtime.active_index];
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_prepare_plan(
    const maintenance_service_plan_request_t *request,
    maintenance_service_transaction_t *out_transaction)
{
    maintenance_service_snapshot_t before;
    maintenance_service_snapshot_t candidate;
    maintenance_service_result_t result;

    if (out_transaction == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_validate_plan(request);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    result = maintenance_service_get_snapshot(&before);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (before.state != MAINTENANCE_SERVICE_STATE_FINISHED)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_STATE;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.state = MAINTENANCE_SERVICE_STATE_PLANNED;
    candidate.has_activity = true;
    candidate.committed_at_utc_100ns = request->command_time_utc_100ns;
    candidate.activity.planned_date_utc_100ns = request->planned_date_utc_100ns;
    candidate.activity.estimated_downtime_ms = request->estimated_downtime_ms;
    candidate.activity.method = request->method;
    maintenance_service_copy_string(candidate.activity.operation_id, request->operation_id,
                                    sizeof(candidate.activity.operation_id));
    maintenance_service_copy_string(candidate.activity.supplier, request->supplier,
                                    sizeof(candidate.activity.supplier));
    maintenance_service_copy_string(candidate.activity.personnel, request->personnel,
                                    sizeof(candidate.activity.personnel));
    maintenance_service_copy_string(candidate.activity.actor, request->actor,
                                    sizeof(candidate.activity.actor));
    return maintenance_service_store_prepared(
        &before, &candidate, MAINTENANCE_SERVICE_OPERATION_PLAN,
        MAINTENANCE_SERVICE_TRANSITION_FINISHED_TO_PLANNED, out_transaction);
}

maintenance_service_result_t maintenance_service_prepare_reschedule(
    const maintenance_service_reschedule_request_t *request,
    maintenance_service_transaction_t *out_transaction)
{
    maintenance_service_snapshot_t before;
    maintenance_service_snapshot_t candidate;
    maintenance_service_result_t result;

    if (out_transaction == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_validate_reschedule(request);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    result = maintenance_service_get_snapshot(&before);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (before.state != MAINTENANCE_SERVICE_STATE_PLANNED || !before.has_activity)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_STATE;
    }

    candidate = before;
    candidate.committed_at_utc_100ns = request->command_time_utc_100ns;
    candidate.activity.planned_date_utc_100ns = request->planned_date_utc_100ns;
    candidate.activity.estimated_downtime_ms = request->estimated_downtime_ms;
    candidate.activity.method = request->method;
    maintenance_service_copy_string(candidate.activity.supplier, request->supplier,
                                    sizeof(candidate.activity.supplier));
    maintenance_service_copy_string(candidate.activity.personnel, request->personnel,
                                    sizeof(candidate.activity.personnel));
    return maintenance_service_store_prepared(
        &before, &candidate, MAINTENANCE_SERVICE_OPERATION_RESCHEDULE,
        MAINTENANCE_SERVICE_TRANSITION_NONE, out_transaction);
}

maintenance_service_result_t maintenance_service_prepare_start(
    const maintenance_service_start_request_t *request,
    maintenance_service_transaction_t *out_transaction)
{
    maintenance_service_snapshot_t before;
    maintenance_service_snapshot_t candidate;
    maintenance_service_result_t result;

    if (out_transaction == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_validate_start(request);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    result = maintenance_service_get_snapshot(&before);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (before.state != MAINTENANCE_SERVICE_STATE_PLANNED || !before.has_activity)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_STATE;
    }

    candidate = before;
    candidate.state = MAINTENANCE_SERVICE_STATE_EXECUTING;
    candidate.committed_at_utc_100ns = request->command_time_utc_100ns;
    maintenance_service_copy_string(candidate.activity.actor, request->actor,
                                    sizeof(candidate.activity.actor));
    return maintenance_service_store_prepared(
        &before, &candidate, MAINTENANCE_SERVICE_OPERATION_START,
        MAINTENANCE_SERVICE_TRANSITION_PLANNED_TO_EXECUTING, out_transaction);
}

maintenance_service_result_t maintenance_service_prepare_finish(
    const maintenance_service_finish_request_t *request,
    maintenance_service_transaction_t *out_transaction)
{
    maintenance_service_snapshot_t before;
    maintenance_service_snapshot_t candidate;
    maintenance_service_result_t result;

    if (out_transaction == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_validate_finish(request);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    result = maintenance_service_get_snapshot(&before);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (before.state != MAINTENANCE_SERVICE_STATE_EXECUTING || !before.has_activity)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_STATE;
    }

    candidate = before;
    candidate.state = MAINTENANCE_SERVICE_STATE_FINISHED;
    candidate.committed_at_utc_100ns = request->command_time_utc_100ns;
    candidate.activity.configuration_changed = request->configuration_changed;
    candidate.activity.parts_serviced_count = request->parts_serviced_count;
    candidate.activity.parts_replaced_count = request->parts_replaced_count;
    maintenance_service_copy_parts(candidate.activity.parts_serviced,
                                   request->parts_serviced,
                                   request->parts_serviced_count);
    maintenance_service_copy_parts(candidate.activity.parts_replaced,
                                   request->parts_replaced,
                                   request->parts_replaced_count);
    maintenance_service_copy_string(candidate.activity.actor, request->actor,
                                    sizeof(candidate.activity.actor));
    maintenance_service_copy_string(candidate.activity.result, request->result,
                                    sizeof(candidate.activity.result));
    return maintenance_service_store_prepared(
        &before, &candidate, MAINTENANCE_SERVICE_OPERATION_FINISH,
        MAINTENANCE_SERVICE_TRANSITION_EXECUTING_TO_FINISHED, out_transaction);
}

maintenance_service_result_t maintenance_service_get_prepared_snapshot(
    const maintenance_service_transaction_t *transaction,
    maintenance_service_snapshot_t *out_snapshot)
{
    maintenance_service_result_t result;

    if (out_snapshot == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!maintenance_service_transaction_matches(transaction))
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }
    if (g_maintenance_service_runtime.pending_phase == MAINTENANCE_PENDING_PERSISTING)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }
    *out_snapshot = g_maintenance_service_runtime.pending_snapshot;
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_apply(
    const maintenance_service_transaction_t *transaction)
{
    maintenance_service_result_t result;
    maintenance_service_snapshot_t candidate;
    maintenance_service_transaction_t persisted_transaction;
    maintenance_journal_record_t record;
    maintenance_journal_backend_result_t journal_result;
    uint8_t inactive_index;

    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!maintenance_service_transaction_matches(transaction))
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }
    if (g_maintenance_service_runtime.pending_phase == MAINTENANCE_PENDING_APPLIED)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_OK;
    }
    if (g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_PREPARED ||
        g_maintenance_service_runtime.journal_mutations_blocked ||
        g_maintenance_service_runtime.journal_io_busy)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE;
    }

    /* The pending transaction serializes persistence but the critical section
     * ends before any driver/Flash operation. */
    candidate = g_maintenance_service_runtime.pending_snapshot;
    persisted_transaction = g_maintenance_service_runtime.pending_transaction;
    g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_PERSISTING;
    if (g_maintenance_service_runtime.journal_enabled)
    {
        g_maintenance_service_runtime.journal_io_busy = true;
    }
    maintenance_service_unlock();

    if (g_maintenance_service_runtime.journal_enabled)
    {
        record = maintenance_service_make_record(&candidate, &persisted_transaction);
        journal_result = maintenance_journal_backend_append(
            &g_maintenance_service_runtime.journal, &record, NULL);
        result = maintenance_service_backend_result(journal_result);
    }
    else
    {
        result = MAINTENANCE_SERVICE_RESULT_OK;
    }

    if (maintenance_service_lock() != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR;
    }
    g_maintenance_service_runtime.journal_io_busy = false;
    if (!maintenance_service_transaction_matches(&persisted_transaction) ||
        g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_PERSISTING)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        if (g_maintenance_service_runtime.journal_enabled &&
            g_maintenance_service_runtime.journal.mutations_blocked)
        {
            g_maintenance_service_runtime.journal_mutations_blocked = true;
        }
        g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_PREPARED;
        maintenance_service_unlock();
        return result;
    }

    inactive_index = (uint8_t)(g_maintenance_service_runtime.active_index ^ 1u);
    g_maintenance_service_runtime.snapshots[inactive_index] =
        g_maintenance_service_runtime.pending_snapshot;
    g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_APPLIED;
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_commit(
    const maintenance_service_transaction_t *transaction,
    maintenance_service_snapshot_t *out_snapshot)
{
    maintenance_service_result_t result;
    uint8_t inactive_index;

    if (out_snapshot == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!maintenance_service_transaction_matches(transaction) ||
        g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_APPLIED)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }

    inactive_index = (uint8_t)(g_maintenance_service_runtime.active_index ^ 1u);
    if (g_maintenance_service_runtime.snapshots[inactive_index].revision !=
        transaction->candidate_revision)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR;
    }
    g_maintenance_service_runtime.active_index = inactive_index;
    *out_snapshot = g_maintenance_service_runtime.snapshots[inactive_index];
    g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_NONE;
    memset(&g_maintenance_service_runtime.pending_transaction, 0,
           sizeof(g_maintenance_service_runtime.pending_transaction));
    memset(&g_maintenance_service_runtime.pending_snapshot, 0,
           sizeof(g_maintenance_service_runtime.pending_snapshot));
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_abort(
    const maintenance_service_transaction_t *transaction)
{
    maintenance_service_result_t result;

    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!maintenance_service_transaction_matches(transaction))
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }
    if (g_maintenance_service_runtime.pending_phase == MAINTENANCE_PENDING_PERSISTING)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }

    g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_NONE;
    memset(&g_maintenance_service_runtime.pending_transaction, 0,
           sizeof(g_maintenance_service_runtime.pending_transaction));
    memset(&g_maintenance_service_runtime.pending_snapshot, 0,
           sizeof(g_maintenance_service_runtime.pending_snapshot));
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_mark_event_publication_fault(
    uint32_t revision, maintenance_service_operation_t operation)
{
    maintenance_service_result_t result;
    const maintenance_service_snapshot_t *active;

    if (!maintenance_service_operation_is_valid(operation) || revision == 0u)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    active = &g_maintenance_service_runtime.snapshots[
        g_maintenance_service_runtime.active_index];
    if (active->revision != revision)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }
    if (g_maintenance_service_runtime.event_fault.active &&
        (g_maintenance_service_runtime.event_fault.revision != revision ||
         g_maintenance_service_runtime.event_fault.operation != operation))
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE;
    }

    g_maintenance_service_runtime.event_fault.active = true;
    g_maintenance_service_runtime.event_fault.revision = revision;
    g_maintenance_service_runtime.event_fault.operation = operation;
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_get_event_publication_fault(
    maintenance_service_event_publication_fault_t *out_fault)
{
    maintenance_service_result_t result;

    if (out_fault == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    *out_fault = g_maintenance_service_runtime.event_fault;
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_clear_event_publication_fault(void)
{
    maintenance_service_result_t result = maintenance_service_lock();

    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    memset(&g_maintenance_service_runtime.event_fault, 0,
           sizeof(g_maintenance_service_runtime.event_fault));
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_reset_journal_ex(
    maintenance_service_reset_outcome_t *out_outcome)
{
    maintenance_service_result_t result;
    maintenance_service_snapshot_t snapshot;
    maintenance_journal_record_t reset_record = {0};
    maintenance_journal_backend_result_t backend_result;
    bool already_empty;

    if (out_outcome != NULL)
    {
        *out_outcome = MAINTENANCE_SERVICE_RESET_NO_CHANGE;
    }

    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!g_maintenance_service_runtime.journal_enabled ||
        g_maintenance_service_runtime.journal_mutations_blocked ||
        g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_NONE ||
        g_maintenance_service_runtime.journal_io_busy)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE;
    }
    snapshot = g_maintenance_service_runtime.snapshots[
        g_maintenance_service_runtime.active_index];
    already_empty = !g_maintenance_service_runtime.journal.has_active ||
                    g_maintenance_service_runtime.journal.active.record.type ==
                        MAINTENANCE_JOURNAL_RECORD_RESET;
    g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_PERSISTING;
    g_maintenance_service_runtime.journal_io_busy = true;
    maintenance_service_unlock();

    reset_record.command_time_utc_100ns = snapshot.committed_at_utc_100ns;
    reset_record.type = MAINTENANCE_JOURNAL_RECORD_RESET;
    reset_record.operation = MAINTENANCE_JOURNAL_OPERATION_RESET;
    reset_record.transition = MAINTENANCE_SERVICE_TRANSITION_NONE;
    reset_record.snapshot = snapshot;
    backend_result = maintenance_journal_backend_reset(
        &g_maintenance_service_runtime.journal, &reset_record, NULL);
    result = maintenance_service_backend_result(backend_result);

    if (maintenance_service_lock() != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR;
    }
    if (g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_PERSISTING)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
    }
    g_maintenance_service_runtime.journal_io_busy = false;
    if (result != MAINTENANCE_SERVICE_RESULT_OK &&
        g_maintenance_service_runtime.journal.mutations_blocked)
    {
        g_maintenance_service_runtime.journal_mutations_blocked = true;
    }
    g_maintenance_service_runtime.pending_phase = MAINTENANCE_PENDING_NONE;
    if (result == MAINTENANCE_SERVICE_RESULT_OK && !already_empty &&
        out_outcome != NULL)
    {
        *out_outcome = MAINTENANCE_SERVICE_RESET_APPLIED;
    }
    maintenance_service_unlock();
    return result;
}

maintenance_service_result_t maintenance_service_reset_journal(void)
{
    return maintenance_service_reset_journal_ex(NULL);
}

maintenance_service_result_t maintenance_service_get_journal_diagnostics(
    maintenance_service_journal_diagnostics_t *out_diagnostics)
{
    maintenance_service_result_t result;
    const maintenance_journal_backend_t *journal;

    if (out_diagnostics == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    journal = &g_maintenance_service_runtime.journal;
    memset(out_diagnostics, 0, sizeof(*out_diagnostics));
    out_diagnostics->enabled = g_maintenance_service_runtime.journal_enabled;
    out_diagnostics->mutation_blocked = g_maintenance_service_runtime.journal_mutations_blocked;
    out_diagnostics->recovery = (maintenance_service_journal_recovery_t)journal->recovery;
    out_diagnostics->last_backend_result = (uint8_t)journal->last_error;
    out_diagnostics->corrupt_record_count = journal->corrupt_record_count;
    out_diagnostics->unknown_record_count = journal->unknown_record_count;
    out_diagnostics->retained_record_count = journal->retained_record_count;
    out_diagnostics->erased_sector_count = journal->erased_sector_count;
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_reconcile_operation_id(
    const char *operation_id, maintenance_service_snapshot_t *out_snapshot,
    bool *out_committed)
{
    maintenance_service_result_t result;
    const maintenance_service_snapshot_t *active;

    if (!maintenance_service_has_nonempty_string(operation_id,
                                                  MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY) ||
        out_snapshot == NULL || out_committed == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    active = &g_maintenance_service_runtime.snapshots[
        g_maintenance_service_runtime.active_index];
    *out_committed = active->has_activity &&
                     strcmp(active->activity.operation_id, operation_id) == 0;
    if (*out_committed)
    {
        *out_snapshot = *active;
    }
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_export_begin(
    maintenance_service_export_cursor_t *out_cursor)
{
    maintenance_service_result_t result;
    maintenance_journal_export_cursor_t cursor;
    maintenance_journal_backend_result_t backend_result;

    if (out_cursor == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!g_maintenance_service_runtime.journal_enabled ||
        g_maintenance_service_runtime.journal_io_busy)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE;
    }
    backend_result = maintenance_journal_backend_export_begin(
        &g_maintenance_service_runtime.journal, &cursor);
    maintenance_service_unlock();
    if (backend_result == MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_STATE;
    }
    result = maintenance_service_backend_result(backend_result);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    out_cursor->epoch = cursor.epoch;
    out_cursor->upper_sequence = cursor.upper_sequence;
    out_cursor->next_sequence = cursor.next_sequence;
    out_cursor->generation = cursor.generation;
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_export_page(
    maintenance_service_export_cursor_t *cursor,
    maintenance_service_export_record_t
        out_records[MAINTENANCE_SERVICE_EXPORT_PAGE_MAX],
    uint8_t *out_count, bool *out_more)
{
    maintenance_service_result_t result;
    maintenance_journal_export_cursor_t backend_cursor;
    maintenance_journal_backend_result_t backend_result;
    uint8_t count;

    if (cursor == NULL || out_records == NULL || out_count == NULL || out_more == NULL)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT;
    }
    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!g_maintenance_service_runtime.journal_enabled ||
        g_maintenance_service_runtime.journal_io_busy)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE;
    }
    backend_cursor.epoch = cursor->epoch;
    backend_cursor.upper_sequence = cursor->upper_sequence;
    backend_cursor.next_sequence = cursor->next_sequence;
    backend_cursor.generation = cursor->generation;
    g_maintenance_service_runtime.journal_io_busy = true;
    maintenance_service_unlock();

    backend_result = maintenance_journal_backend_export_page(
        &g_maintenance_service_runtime.journal, &backend_cursor,
        g_maintenance_service_export_records, &count, out_more);

    if (maintenance_service_lock() != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR;
    }
    g_maintenance_service_runtime.journal_io_busy = false;
    maintenance_service_unlock();
    if (backend_result == MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND)
    {
        return MAINTENANCE_SERVICE_RESULT_INVALID_STATE;
    }
    result = maintenance_service_backend_result(backend_result);
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    for (uint8_t index = 0u; index < count; ++index)
    {
        out_records[index].epoch = g_maintenance_service_export_records[index].epoch;
        out_records[index].sequence = g_maintenance_service_export_records[index].sequence;
        out_records[index].operation =
            (maintenance_service_operation_t)g_maintenance_service_export_records[index].operation;
        out_records[index].transition = g_maintenance_service_export_records[index].transition;
        out_records[index].snapshot = g_maintenance_service_export_records[index].snapshot;
    }
    cursor->next_sequence = backend_cursor.next_sequence;
    *out_count = count;
    return MAINTENANCE_SERVICE_RESULT_OK;
}

const char *maintenance_service_result_to_string(
    maintenance_service_result_t result)
{
    switch (result)
    {
    case MAINTENANCE_SERVICE_RESULT_OK:
        return "ok";
    case MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED:
        return "not_initialized";
    case MAINTENANCE_SERVICE_RESULT_BUSY:
        return "busy";
    case MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT:
        return "invalid_argument";
    case MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE:
        return "out_of_range";
    case MAINTENANCE_SERVICE_RESULT_INVALID_STATE:
        return "invalid_state";
    case MAINTENANCE_SERVICE_RESULT_INTERNAL_ERROR:
        return "internal_error";
    case MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE:
        return "resource_unavailable";
    default:
        return "unknown";
    }
}

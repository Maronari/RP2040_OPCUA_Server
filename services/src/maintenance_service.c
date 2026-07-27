#include "services/maintenance_service.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <FreeRTOS.h>

typedef enum
{
    MAINTENANCE_PENDING_NONE = 0,
    MAINTENANCE_PENDING_PREPARED = 1,
    MAINTENANCE_PENDING_APPLIED = 2
} maintenance_service_pending_phase_t;

typedef struct
{
    maintenance_service_snapshot_t snapshots[2];
    maintenance_service_snapshot_t pending_snapshot;
    maintenance_service_transaction_t pending_transaction;
    maintenance_service_event_publication_fault_t event_fault;
    uint32_t next_token;
    uint8_t active_index;
    maintenance_service_pending_phase_t pending_phase;
    bool initialized;
} maintenance_service_runtime_t;

static maintenance_service_runtime_t g_maintenance_service_runtime;

static bool maintenance_service_has_terminated_string(const char *text,
                                                       size_t capacity)
{
    return text != NULL && capacity > 0u && memchr(text, '\0', capacity) != NULL;
}

static bool maintenance_service_has_nonempty_string(const char *text,
                                                     size_t capacity)
{
    return maintenance_service_has_terminated_string(text, capacity) && text[0] != '\0';
}

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

static bool maintenance_service_method_is_valid(maintenance_service_method_t method)
{
    return method == MAINTENANCE_SERVICE_METHOD_LOCAL ||
           method == MAINTENANCE_SERVICE_METHOD_REMOTE;
}

static bool maintenance_service_operation_is_valid(maintenance_service_operation_t operation)
{
    return operation == MAINTENANCE_SERVICE_OPERATION_PLAN ||
           operation == MAINTENANCE_SERVICE_OPERATION_RESCHEDULE ||
           operation == MAINTENANCE_SERVICE_OPERATION_START ||
           operation == MAINTENANCE_SERVICE_OPERATION_FINISH;
}

static bool maintenance_service_schedule_is_valid(int64_t planned_date_utc_100ns,
                                                  double estimated_downtime_ms)
{
    return planned_date_utc_100ns > 0 && isfinite(estimated_downtime_ms) &&
           estimated_downtime_ms > 0.0 &&
           estimated_downtime_ms <= MAINTENANCE_SERVICE_MAX_DOWNTIME_MS;
}

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

static void maintenance_service_unlock(void)
{
    portEXIT_CRITICAL();
}

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

static uint32_t maintenance_service_next_token(void)
{
    ++g_maintenance_service_runtime.next_token;
    if (g_maintenance_service_runtime.next_token == 0u)
    {
        ++g_maintenance_service_runtime.next_token;
    }
    return g_maintenance_service_runtime.next_token;
}

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

maintenance_service_result_t maintenance_service_init(void)
{
    maintenance_service_snapshot_t initial_snapshot;

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
    g_maintenance_service_runtime.initialized = true;
    return MAINTENANCE_SERVICE_RESULT_OK;
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
    *out_snapshot = g_maintenance_service_runtime.pending_snapshot;
    maintenance_service_unlock();
    return MAINTENANCE_SERVICE_RESULT_OK;
}

maintenance_service_result_t maintenance_service_apply(
    const maintenance_service_transaction_t *transaction)
{
    maintenance_service_result_t result;
    uint8_t inactive_index;

    result = maintenance_service_lock();
    if (result != MAINTENANCE_SERVICE_RESULT_OK)
    {
        return result;
    }
    if (!maintenance_service_transaction_matches(transaction) ||
        g_maintenance_service_runtime.pending_phase != MAINTENANCE_PENDING_PREPARED)
    {
        maintenance_service_unlock();
        return MAINTENANCE_SERVICE_RESULT_BUSY;
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

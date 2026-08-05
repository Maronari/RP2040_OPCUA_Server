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

typedef enum
{
    MAINTENANCE_SERVICE_STATE_FINISHED = 0,
    MAINTENANCE_SERVICE_STATE_PLANNED = 1,
    MAINTENANCE_SERVICE_STATE_EXECUTING = 2
} maintenance_service_state_t;

typedef enum
{
    MAINTENANCE_SERVICE_METHOD_LOCAL = 0,
    MAINTENANCE_SERVICE_METHOD_REMOTE = 1
} maintenance_service_method_t;

typedef enum
{
    MAINTENANCE_SERVICE_OPERATION_PLAN = 0,
    MAINTENANCE_SERVICE_OPERATION_RESCHEDULE = 1,
    MAINTENANCE_SERVICE_OPERATION_START = 2,
    MAINTENANCE_SERVICE_OPERATION_FINISH = 3
} maintenance_service_operation_t;

typedef enum
{
    MAINTENANCE_SERVICE_TRANSITION_NONE = 0,
    MAINTENANCE_SERVICE_TRANSITION_FINISHED_TO_PLANNED = 1,
    MAINTENANCE_SERVICE_TRANSITION_PLANNED_TO_EXECUTING = 2,
    MAINTENANCE_SERVICE_TRANSITION_EXECUTING_TO_FINISHED = 3
} maintenance_service_transition_t;

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

typedef enum
{
    MAINTENANCE_SERVICE_RESET_NO_CHANGE = 0,
    MAINTENANCE_SERVICE_RESET_APPLIED = 1
} maintenance_service_reset_outcome_t;

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

/* A by-value copy. Callers never receive a pointer into service storage. */
typedef struct
{
    maintenance_activity_t activity;
    maintenance_service_state_t state;
    uint32_t revision;
    int64_t committed_at_utc_100ns;
    bool has_activity;
} maintenance_service_snapshot_t;

/* A logical, bounded journal view.  It contains decoded service state only;
 * callers never observe Flash addresses, slot bytes, or driver-owned buffers. */
typedef struct
{
    uint64_t epoch;
    uint64_t upper_sequence;
    uint64_t next_sequence;
    uint32_t generation;
} maintenance_service_export_cursor_t;

typedef struct
{
    uint64_t epoch;
    uint64_t sequence;
    maintenance_service_operation_t operation;
    maintenance_service_transition_t transition;
    maintenance_service_snapshot_t snapshot;
} maintenance_service_export_record_t;

/*
 * Opaque capability for the one bounded in-flight transaction. A token is
 * valid only until abort or commit. It contains no pointers and no OPC UA
 * types, so it is safe to keep on a method callback stack.
 */
typedef struct
{
    uint32_t token;
    uint32_t base_revision;
    uint32_t candidate_revision;
    maintenance_service_operation_t operation;
    maintenance_service_transition_t transition;
} maintenance_service_transaction_t;

typedef struct
{
    bool active;
    uint32_t revision;
    maintenance_service_operation_t operation;
} maintenance_service_event_publication_fault_t;

typedef bool (*maintenance_service_journal_read_fn)(void *context, uint32_t offset,
                                                    uint8_t *destination, size_t size);
typedef bool (*maintenance_service_journal_program_fn)(void *context, uint32_t offset,
                                                       const uint8_t page[256]);
typedef bool (*maintenance_service_journal_erase_fn)(void *context, uint32_t offset);
typedef struct
{
    void *context;
    maintenance_service_journal_read_fn read;
    maintenance_service_journal_program_fn program_page;
    maintenance_service_journal_erase_fn erase_sector;
} maintenance_service_journal_storage_t;

typedef enum
{
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_EMPTY = 0,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_OK,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_DEGRADED,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_INDETERMINATE,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_FATAL,
    MAINTENANCE_SERVICE_JOURNAL_RECOVERY_UNSUPPORTED
} maintenance_service_journal_recovery_t;

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

typedef struct
{
    int64_t command_time_utc_100ns;
    int64_t planned_date_utc_100ns;
    double estimated_downtime_ms;
    maintenance_service_method_t method;
    char supplier[MAINTENANCE_SERVICE_TEXT_CAPACITY];
    char personnel[MAINTENANCE_SERVICE_TEXT_CAPACITY];
} maintenance_service_reschedule_request_t;

typedef struct
{
    int64_t command_time_utc_100ns;
    char actor[MAINTENANCE_SERVICE_ACTOR_CAPACITY];
} maintenance_service_start_request_t;

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

maintenance_service_result_t maintenance_service_init(void);
/* Test/provisioning binding only; callers must provide it before init. The
 * service owns state, while this driver callback table remains hardware-owned. */
maintenance_service_result_t maintenance_service_init_with_journal(
    const maintenance_service_journal_storage_t *storage);
maintenance_service_result_t maintenance_service_get_snapshot(
    maintenance_service_snapshot_t *out_snapshot);

/* Prepare does not alter the active snapshot or its revision. */
maintenance_service_result_t maintenance_service_prepare_plan(
    const maintenance_service_plan_request_t *request,
    maintenance_service_transaction_t *out_transaction);
maintenance_service_result_t maintenance_service_prepare_reschedule(
    const maintenance_service_reschedule_request_t *request,
    maintenance_service_transaction_t *out_transaction);
maintenance_service_result_t maintenance_service_prepare_start(
    const maintenance_service_start_request_t *request,
    maintenance_service_transaction_t *out_transaction);
maintenance_service_result_t maintenance_service_prepare_finish(
    const maintenance_service_finish_request_t *request,
    maintenance_service_transaction_t *out_transaction);

/* The prepared snapshot is a copy and is never visible through the model. */
maintenance_service_result_t maintenance_service_get_prepared_snapshot(
    const maintenance_service_transaction_t *transaction,
    maintenance_service_snapshot_t *out_snapshot);

/* Apply copies the candidate to the inactive slot; it remains invisible. */
maintenance_service_result_t maintenance_service_apply(
    const maintenance_service_transaction_t *transaction);

/* Commit is the only operation that changes the active snapshot/revision. */
maintenance_service_result_t maintenance_service_commit(
    const maintenance_service_transaction_t *transaction,
    maintenance_service_snapshot_t *out_snapshot);
maintenance_service_result_t maintenance_service_abort(
    const maintenance_service_transaction_t *transaction);

/* Post-commit event-publication recovery state. */
maintenance_service_result_t maintenance_service_mark_event_publication_fault(
    uint32_t revision, maintenance_service_operation_t operation);
maintenance_service_result_t maintenance_service_get_event_publication_fault(
    maintenance_service_event_publication_fault_t *out_fault);
maintenance_service_result_t maintenance_service_clear_event_publication_fault(void);

/* Logical reset is storage-only: it preserves workflow state and revision. */
maintenance_service_result_t maintenance_service_reset_journal(void);
/* `out_outcome` lets the OPC UA adapter audit only a newly committed reset.
 * It may be NULL when a caller only needs the legacy result. */
maintenance_service_result_t maintenance_service_reset_journal_ex(
    maintenance_service_reset_outcome_t *out_outcome);
maintenance_service_result_t maintenance_service_get_journal_diagnostics(
    maintenance_service_journal_diagnostics_t *out_diagnostics);
/* Plan is the only v0.3.2 operation with a caller supplied durable identity.
 * A retry uses this before prepare to distinguish its committed outcome from a
 * conflicting Plan without consuming a new journal slot. */
maintenance_service_result_t maintenance_service_reconcile_operation_id(
    const char *operation_id, maintenance_service_snapshot_t *out_snapshot,
    bool *out_committed);
maintenance_service_result_t maintenance_service_export_begin(maintenance_service_export_cursor_t *out_cursor);
maintenance_service_result_t maintenance_service_export_page(maintenance_service_export_cursor_t *cursor,
    maintenance_service_export_record_t out_records[MAINTENANCE_SERVICE_EXPORT_PAGE_MAX], uint8_t *out_count, bool *out_more);

const char *maintenance_service_result_to_string(
    maintenance_service_result_t result);

#endif

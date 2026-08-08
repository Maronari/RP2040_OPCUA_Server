#include "services/maintenance_service.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static maintenance_service_plan_request_t plan_request(void)
{
    maintenance_service_plan_request_t request = {0};
    request.command_time_utc_100ns = 10;
    request.planned_date_utc_100ns = 20;
    request.estimated_downtime_ms = MAINTENANCE_SERVICE_MAX_DOWNTIME_MS;
    request.method = MAINTENANCE_SERVICE_METHOD_REMOTE;
    strcpy(request.operation_id, "legal-plan");
    strcpy(request.supplier, "supplier");
    strcpy(request.personnel, "personnel");
    strcpy(request.actor, "planner");
    return request;
}

static maintenance_service_reschedule_request_t reschedule_request(void)
{
    maintenance_service_reschedule_request_t request = {0};
    request.command_time_utc_100ns = 30;
    request.planned_date_utc_100ns = 40;
    request.estimated_downtime_ms = 1.0;
    request.method = MAINTENANCE_SERVICE_METHOD_LOCAL;
    strcpy(request.supplier, "rescheduled");
    strcpy(request.personnel, "crew");
    return request;
}

static maintenance_service_start_request_t start_request(void)
{
    maintenance_service_start_request_t request = {.command_time_utc_100ns = 50};
    strcpy(request.actor, "operator");
    return request;
}

static maintenance_service_finish_request_t finish_request(void)
{
    maintenance_service_finish_request_t request = {.command_time_utc_100ns = 60};
    strcpy(request.actor, "operator");
    strcpy(request.result, "completed");
    request.parts_serviced_count = MAINTENANCE_SERVICE_MAX_PARTS;
    request.parts_replaced_count = MAINTENANCE_SERVICE_MAX_PARTS;
    for (uint8_t index = 0u; index < MAINTENANCE_SERVICE_MAX_PARTS; ++index)
    {
        strcpy(request.parts_serviced[index], "service");
        strcpy(request.parts_replaced[index], "replace");
    }
    return request;
}

static void commit(maintenance_service_transaction_t *transaction,
                   maintenance_service_snapshot_t *snapshot)
{
    assert(maintenance_service_apply(transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_commit(transaction, snapshot) == MAINTENANCE_SERVICE_RESULT_OK);
}

static void assert_snapshot_unchanged(const maintenance_service_snapshot_t *before)
{
    maintenance_service_snapshot_t after = {0};
    assert(maintenance_service_get_snapshot(&after) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(memcmp(before, &after, sizeof(after)) == 0);
}

int main(void)
{
    maintenance_service_plan_request_t plan = plan_request();
    maintenance_service_reschedule_request_t reschedule = reschedule_request();
    maintenance_service_start_request_t start = start_request();
    maintenance_service_finish_request_t finish = finish_request();
    maintenance_service_transaction_t transaction = {0};
    maintenance_service_snapshot_t snapshot = {0};
    maintenance_service_snapshot_t before = {0};

    assert(maintenance_service_get_snapshot(&snapshot) == MAINTENANCE_SERVICE_RESULT_NOT_INITIALIZED);
    assert(maintenance_service_prepare_plan(NULL, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT);
    assert(maintenance_service_init() == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_get_snapshot(&snapshot) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_FINISHED && !snapshot.has_activity);

    before = snapshot;
    plan.estimated_downtime_ms = NAN;
    assert(maintenance_service_prepare_plan(&plan, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE);
    assert_snapshot_unchanged(&before);
    plan = plan_request();
    plan.operation_id[0] = '\0';
    assert(maintenance_service_prepare_plan(&plan, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT);
    assert_snapshot_unchanged(&before);
    plan = plan_request();
    assert(maintenance_service_prepare_start(&start, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_STATE);
    assert(maintenance_service_prepare_finish(&finish, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_STATE);

    assert(maintenance_service_prepare_plan(&plan, &transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_prepare_plan(&plan, &transaction) == MAINTENANCE_SERVICE_RESULT_BUSY);
    commit(&transaction, &snapshot);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED);

    before = snapshot;
    reschedule.estimated_downtime_ms = 0.0;
    assert(maintenance_service_prepare_reschedule(&reschedule, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_OUT_OF_RANGE);
    assert_snapshot_unchanged(&before);
    reschedule = reschedule_request();
    assert(maintenance_service_prepare_reschedule(&reschedule, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    commit(&transaction, &snapshot);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED &&
           snapshot.activity.planned_date_utc_100ns == reschedule.planned_date_utc_100ns);

    assert(maintenance_service_prepare_plan(&plan, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_STATE);
    assert(maintenance_service_prepare_finish(&finish, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_STATE);
    assert(maintenance_service_prepare_start(&start, &transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    commit(&transaction, &snapshot);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_EXECUTING);

    before = snapshot;
    finish.parts_serviced_count = MAINTENANCE_SERVICE_MAX_PARTS + 1u;
    assert(maintenance_service_prepare_finish(&finish, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT);
    assert_snapshot_unchanged(&before);
    finish = finish_request();
    finish.result[0] = '\0';
    assert(maintenance_service_prepare_finish(&finish, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_ARGUMENT);
    assert_snapshot_unchanged(&before);
    finish = finish_request();
    assert(maintenance_service_prepare_reschedule(&reschedule, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_STATE);
    assert(maintenance_service_prepare_start(&start, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_STATE);
    assert(maintenance_service_prepare_finish(&finish, &transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    commit(&transaction, &snapshot);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_FINISHED &&
           snapshot.activity.parts_serviced_count == MAINTENANCE_SERVICE_MAX_PARTS);

    assert(maintenance_service_prepare_finish(&finish, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_INVALID_STATE);
    return 0;
}

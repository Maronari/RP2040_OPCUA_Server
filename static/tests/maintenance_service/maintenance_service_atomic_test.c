#include "services/maintenance_service.h"

#include <assert.h>
#include <string.h>

static maintenance_service_plan_request_t make_plan_request(void)
{
    maintenance_service_plan_request_t request = {0};

    request.command_time_utc_100ns = 133000000000000000LL;
    request.planned_date_utc_100ns = 133000001000000000LL;
    request.estimated_downtime_ms = 60000.0;
    request.method = MAINTENANCE_SERVICE_METHOD_LOCAL;
    strcpy(request.operation_id, "atomic-test");
    strcpy(request.supplier, "test-supplier");
    strcpy(request.personnel, "test-personnel");
    strcpy(request.actor, "test-service");
    return request;
}

int main(void)
{
    maintenance_service_plan_request_t request = make_plan_request();
    maintenance_service_start_request_t start_request = {0};
    maintenance_service_transaction_t transaction = {0};
    maintenance_service_snapshot_t initial = {0};
    maintenance_service_snapshot_t prepared = {0};
    maintenance_service_snapshot_t visible = {0};
    maintenance_service_snapshot_t committed = {0};
    maintenance_service_event_publication_fault_t fault = {0};

    assert(maintenance_service_init() == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_get_snapshot(&initial) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(initial.state == MAINTENANCE_SERVICE_STATE_FINISHED);

    /* A prepared candidate is not visible and abort leaves the revision intact. */
    assert(maintenance_service_prepare_plan(&request, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_get_prepared_snapshot(&transaction, &prepared) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(prepared.state == MAINTENANCE_SERVICE_STATE_PLANNED);
    assert(maintenance_service_get_snapshot(&visible) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(visible.revision == initial.revision);
    assert(visible.state == initial.state);
    assert(maintenance_service_abort(&transaction) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_get_snapshot(&visible) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(memcmp(&visible, &initial, sizeof(initial)) == 0);

    /* Apply writes only the inactive slot. Commit is the sole visibility point. */
    memset(&transaction, 0, sizeof(transaction));
    assert(maintenance_service_prepare_plan(&request, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_apply(&transaction) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_get_snapshot(&visible) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(memcmp(&visible, &initial, sizeof(initial)) == 0);
    assert(maintenance_service_commit(&transaction, &committed) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(committed.state == MAINTENANCE_SERVICE_STATE_PLANNED);
    assert(committed.revision == initial.revision + 1u);
    assert(maintenance_service_get_snapshot(&visible) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(memcmp(&visible, &committed, sizeof(committed)) == 0);

    /*
     * Exercise the service-level recovery policy used after the adapter
     * observes a public event API failure. This does not replace an
     * adapter-level UA_Server_createEvent injection test.
     */
    assert(maintenance_service_mark_event_publication_fault(
               committed.revision, MAINTENANCE_SERVICE_OPERATION_PLAN) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_get_event_publication_fault(&fault) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(fault.active);
    assert(fault.revision == committed.revision);
    assert(maintenance_service_get_snapshot(&visible) ==
           MAINTENANCE_SERVICE_RESULT_OK);
    assert(memcmp(&visible, &committed, sizeof(committed)) == 0);

    start_request.command_time_utc_100ns = 133000002000000000LL;
    strcpy(start_request.actor, "test-service");
    memset(&transaction, 0, sizeof(transaction));
    assert(maintenance_service_prepare_start(&start_request, &transaction) ==
           MAINTENANCE_SERVICE_RESULT_RESOURCE_UNAVAILABLE);
    return 0;
}

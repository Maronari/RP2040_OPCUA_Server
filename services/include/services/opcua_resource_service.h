#ifndef SERVICES_OPCUA_RESOURCE_SERVICE_H
#define SERVICES_OPCUA_RESOURCE_SERVICE_H

/*
 * Bounded ownership of the resource counters exposed by the OPC UA adapter.
 *
 * This service deliberately has no open62541 dependency.  The adapter calls
 * the mutation functions at the definitive lifecycle/rejection sites, all of
 * which execute on OpcUaTask.  Snapshot readers may execute in another task;
 * the service therefore uses a short FreeRTOS critical section for a coherent
 * copy.  No operation allocates memory or retains caller-owned pointers.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    OPCUA_RESOURCE_SERVICE_QUALITY_GOOD = 0,
    OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA = 1,
    OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NOT_SUPPORTED = 2
} opcua_resource_service_quality_t;

typedef enum
{
    OPCUA_RESOURCE_SERVICE_STATUS_OK = 0,
    OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT = 1,
    OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED = 2,
    OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE = 3,
    OPCUA_RESOURCE_SERVICE_STATUS_ALREADY_ACTIVE = 4,
    OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED = 5,
    OPCUA_RESOURCE_SERVICE_STATUS_CAPACITY_EXCEEDED = 6,
    OPCUA_RESOURCE_SERVICE_STATUS_UNDERFLOW = 7
} opcua_resource_service_status_t;

typedef enum
{
    OPCUA_RESOURCE_SERVICE_SESSIONS = 0,
    OPCUA_RESOURCE_SERVICE_SECURE_CHANNELS = 1,
    OPCUA_RESOURCE_SERVICE_SUBSCRIPTIONS = 2,
    OPCUA_RESOURCE_SERVICE_MONITORED_ITEMS = 3,
    OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS = 4,
    OPCUA_RESOURCE_SERVICE_EVENT_QUEUE = 5,
    OPCUA_RESOURCE_SERVICE_GAUGE_COUNT = 6
} opcua_resource_service_gauge_t;

#define OPCUA_RESOURCE_SERVICE_GAUGE_BIT(gauge) (1u << (uint32_t)(gauge))
#define OPCUA_RESOURCE_SERVICE_ALL_GAUGES_MASK \
    ((1u << (uint32_t)OPCUA_RESOURCE_SERVICE_GAUGE_COUNT) - 1u)

typedef struct
{
    /* A zero capacity is a valid disabled server limit.  An unsupported
     * gauge must have capacity zero so unavailable data cannot masquerade as
     * a configured zero-capacity feature. */
    uint32_t capacities[OPCUA_RESOURCE_SERVICE_GAUGE_COUNT];
    uint32_t supported_gauges_mask;
    bool rejected_requests_supported;
    bool event_queue_drops_supported;
} opcua_resource_service_server_config_t;

typedef struct
{
    uint32_t current;
    uint32_t peak;
    uint32_t capacity;
    bool supported;
    opcua_resource_service_quality_t quality;
} opcua_resource_service_gauge_snapshot_t;

typedef struct
{
    uint64_t value;
    uint64_t wrap_count;
    bool supported;
    opcua_resource_service_quality_t quality;
} opcua_resource_service_counter_snapshot_t;

typedef struct
{
    bool initialized;
    bool server_active;
    opcua_resource_service_gauge_snapshot_t gauges[
        OPCUA_RESOURCE_SERVICE_GAUGE_COUNT];
    opcua_resource_service_counter_snapshot_t rejected_requests;
    opcua_resource_service_counter_snapshot_t event_queue_drops;
} opcua_resource_service_snapshot_t;

/* Reset all service state.  This is boot initialization: both cumulative
 * counters reset here and nowhere else. */
void opcua_resource_service_init(void);

/* Start one server lifetime with immutable configured capacities.  The call
 * fails without changing the existing state for invalid configuration or an
 * already-active server. */
opcua_resource_service_status_t opcua_resource_service_begin_server(
    const opcua_resource_service_server_config_t *config);

/* End the active server lifetime.  All occupancy values and capacities become
 * unavailable; boot-lifetime rejection/drop counters remain intact. */
opcua_resource_service_status_t opcua_resource_service_end_server(void);

/* Acquire/release one bounded server object.  A failed operation never
 * changes current or peak.  Capacity failure is deliberately not counted as
 * a rejected request automatically: the open62541 adapter records only a
 * definitive protocol rejection through opcua_resource_service_record_rejected_request(). */
opcua_resource_service_status_t opcua_resource_service_acquire(
    opcua_resource_service_gauge_t gauge);
opcua_resource_service_status_t opcua_resource_service_release(
    opcua_resource_service_gauge_t gauge);

/* Publish an exact owner-sampled occupancy.  This is used only for the
 * open62541-owned PublishRequests and EventQueue collections, whose members
 * cannot be reconstructed safely from lifecycle callbacks.  The configured
 * capacity remains immutable for every gauge except EventQueue, where the
 * effective capacity changes with the negotiated subscription queues.
 *
 * current must not exceed capacity.  A rejected sample leaves both the
 * current value and the recorded server-lifetime peak unchanged. */
opcua_resource_service_status_t opcua_resource_service_set_current(
    opcua_resource_service_gauge_t gauge,
    uint32_t current,
    uint32_t capacity);

/* Publish one exact owner snapshot including a peak retained at the mutation
 * site.  This preserves short-lived queue peaks that would otherwise be lost
 * between one-second diagnostic samples. peak must be >= current; for
 * fixed-capacity gauges it must also be <= capacity. A valid observation
 * can only increase the service's
 * server-lifetime peak; it never resets a prior peak. EventQueue has a
 * dynamic current capacity: its historical peak may exceed capacity after
 * event MonitoredItems are removed. */
opcua_resource_service_status_t opcua_resource_service_observe(
    opcua_resource_service_gauge_t gauge,
    uint32_t current,
    uint32_t peak,
    uint32_t capacity);

/* Fail closed when the adapter detects that a lifecycle callback cannot be
 * reconciled with the bounded provider state (for example, an unexpected
 * release underflow).  This changes only the quality of an active, supported
 * gauge to BadNoData.  Its last counts are retained for diagnosis, and no
 * later acquire/release/observation can restore Good; only server rebuild
 * (begin_server) establishes a new coherent lifetime. */
opcua_resource_service_status_t opcua_resource_service_invalidate(
    opcua_resource_service_gauge_t gauge);

/* Record boot-lifetime protocol outcomes.  They are valid only when the
 * corresponding callback/site is observable and declared supported in the
 * current server configuration. */
opcua_resource_service_status_t
opcua_resource_service_record_rejected_request(void);
opcua_resource_service_status_t
opcua_resource_service_record_event_queue_drop(void);

/* Add a non-negative owner-sampled delta.  The counters have boot lifetime;
 * their UInt64 value wraps to zero and their wrap count saturates.  The
 * operation is valid only during an active server lifetime so an obsolete
 * provider cannot alter a retained boot metric after server teardown. */
opcua_resource_service_status_t
opcua_resource_service_add_rejected_requests(uint64_t delta);
opcua_resource_service_status_t
opcua_resource_service_add_event_queue_drops(uint64_t delta);
/* An owner snapshot gap makes a boot-lifetime total unknowable until reboot.
 * Invalidation preserves the stored count but publishes BadNoData. */
opcua_resource_service_status_t
opcua_resource_service_invalidate_rejected_requests(void);
opcua_resource_service_status_t
opcua_resource_service_invalidate_event_queue_drops(void);

/* Coherently copy the complete fixed-size state.  Returns false before boot
 * initialization and never partially writes out_snapshot. */
bool opcua_resource_service_get_snapshot(
    opcua_resource_service_snapshot_t *out_snapshot);

#ifdef OPCUA_RESOURCE_SERVICE_TEST_HOOKS
/* Host-only overflow seam.  It is excluded from firmware builds and exists so
 * the specified UInt64 wrapping/saturating-wrap policy is testable without
 * performing 2^64 lifecycle operations. */
bool opcua_resource_service_test_set_counters(uint64_t rejected_value,
                                              uint64_t rejected_wrap_count,
                                              uint64_t drop_value,
                                              uint64_t drop_wrap_count);
#endif

#endif

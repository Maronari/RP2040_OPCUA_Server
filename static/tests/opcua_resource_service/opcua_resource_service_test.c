#include "services/opcua_resource_service.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static unsigned g_critical_depth;

void opcua_resource_service_test_enter_critical(void)
{
    ++g_critical_depth;
}

void opcua_resource_service_test_exit_critical(void)
{
    assert(g_critical_depth > 0u);
    --g_critical_depth;
}

static opcua_resource_service_server_config_t complete_config(void)
{
    opcua_resource_service_server_config_t config;
    memset(&config, 0, sizeof(config));
    config.capacities[OPCUA_RESOURCE_SERVICE_SESSIONS] = 2u;
    config.capacities[OPCUA_RESOURCE_SERVICE_SECURE_CHANNELS] = 2u;
    config.capacities[OPCUA_RESOURCE_SERVICE_SUBSCRIPTIONS] = 3u;
    config.capacities[OPCUA_RESOURCE_SERVICE_MONITORED_ITEMS] = 4u;
    config.capacities[OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS] = 5u;
    config.capacities[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE] = 6u;
    config.supported_gauges_mask = OPCUA_RESOURCE_SERVICE_ALL_GAUGES_MASK;
    config.rejected_requests_supported = true;
    config.event_queue_drops_supported = true;
    return config;
}

int main(void)
{
    opcua_resource_service_snapshot_t snapshot;
    opcua_resource_service_server_config_t config = complete_config();

    /* Initial/illegal-state paths leave output/state untouched. */
    assert(!opcua_resource_service_get_snapshot(&snapshot));
    assert(opcua_resource_service_begin_server(NULL) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);
    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED);
    assert(opcua_resource_service_set_current(OPCUA_RESOURCE_SERVICE_EVENT_QUEUE,
                                              0u, 0u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED);
    assert(opcua_resource_service_end_server() ==
           OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED);

    opcua_resource_service_init();
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(!snapshot.server_active);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].quality ==
           OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA);
    assert(opcua_resource_service_begin_server(&config) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_begin_server(&config) ==
           OPCUA_RESOURCE_SERVICE_STATUS_ALREADY_ACTIVE);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.initialized && snapshot.server_active);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].current == 0u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].peak == 0u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].capacity == 2u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].quality ==
           OPCUA_RESOURCE_SERVICE_QUALITY_GOOD);

    /* The two owner-sampled queues accept an exact current value.  Only
     * EventQueue is negotiated dynamically; the production fixed limits for
     * all other gauges cannot be changed by a sample. */
    assert(opcua_resource_service_set_current(
               OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS, 4u, 5u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_set_current(
               OPCUA_RESOURCE_SERVICE_EVENT_QUEUE, 4u, 6u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_set_current(
               OPCUA_RESOURCE_SERVICE_EVENT_QUEUE, 2u, 3u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS].current == 4u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS].peak == 4u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].current == 2u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].peak == 4u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].capacity == 3u);
    assert(opcua_resource_service_observe(
               OPCUA_RESOURCE_SERVICE_EVENT_QUEUE, 1u, 5u, 2u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].peak == 5u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].capacity == 2u);

    /* The port keeps PublishRequests' high-water mark at each mutation.  An
     * owner observation must carry that exact transient peak forward, even
     * when its current occupancy is already lower at sample time. */
    assert(opcua_resource_service_observe(
               OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS, 2u, 5u, 5u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS].current == 2u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS].peak == 5u);
    assert(opcua_resource_service_observe(
               OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS, 3u, 2u, 5u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);
    assert(opcua_resource_service_observe(
               OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS, 2u, 6u, 5u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_CAPACITY_EXCEEDED);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS].current == 2u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS].peak == 5u);
    assert(opcua_resource_service_set_current(
               OPCUA_RESOURCE_SERVICE_PUBLISH_REQUESTS, 1u, 4u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);
    assert(opcua_resource_service_set_current(
               OPCUA_RESOURCE_SERVICE_EVENT_QUEUE, 4u, 3u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_CAPACITY_EXCEEDED);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].current == 1u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].capacity == 2u);

    /* Normal occupancy uses a bounded O(1) increment/decrement, preserving
     * the server-lifetime peak across a removal. */
    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_CAPACITY_EXCEEDED);
    assert(opcua_resource_service_release(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].current == 1u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].peak == 2u);
    assert(opcua_resource_service_release(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_release(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_UNDERFLOW);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].current == 0u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].peak == 2u);

    /* A callback/state mismatch makes the affected result unavailable rather
     * than publishing a misleading Good zero.  Mutations retain their
     * bounded accounting but cannot repair the quality mid-lifetime. */
    assert(opcua_resource_service_invalidate(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_release(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_observe(OPCUA_RESOURCE_SERVICE_SESSIONS,
                                          1u, 1u, 2u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].current == 1u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].peak == 2u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].capacity == 2u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].quality ==
           OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA);

    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_GAUGE_COUNT) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);
    assert(opcua_resource_service_release(OPCUA_RESOURCE_SERVICE_GAUGE_COUNT) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);
    assert(opcua_resource_service_invalidate(OPCUA_RESOURCE_SERVICE_GAUGE_COUNT) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);

    /* The caller alone classifies definitive protocol outcomes; counters are
     * boot-lifetime and therefore survive ending/restarting the server. */
    assert(opcua_resource_service_record_rejected_request() ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_record_event_queue_drop() ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_add_rejected_requests(2u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_add_event_queue_drops(3u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_end_server() == OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(!snapshot.server_active);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].quality ==
           OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA);
    assert(snapshot.rejected_requests.value == 3u);
    assert(snapshot.event_queue_drops.value == 4u);
    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE);
    assert(opcua_resource_service_set_current(OPCUA_RESOURCE_SERVICE_EVENT_QUEUE,
                                              0u, 0u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE);
    assert(opcua_resource_service_invalidate(OPCUA_RESOURCE_SERVICE_SESSIONS) ==
           OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE);
    assert(opcua_resource_service_add_rejected_requests(1u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE);
    assert(opcua_resource_service_end_server() ==
           OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE);
    assert(opcua_resource_service_begin_server(&config) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].current == 0u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].peak == 0u);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_SESSIONS].quality ==
           OPCUA_RESOURCE_SERVICE_QUALITY_GOOD);
    assert(snapshot.rejected_requests.value == 3u);

    /* A declared unsupported measurement is explicit BadNotSupported, never
     * indistinguishable from a supported zero. */
    assert(opcua_resource_service_end_server() == OPCUA_RESOURCE_SERVICE_STATUS_OK);
    config = complete_config();
    config.supported_gauges_mask &=
        ~OPCUA_RESOURCE_SERVICE_GAUGE_BIT(OPCUA_RESOURCE_SERVICE_EVENT_QUEUE);
    config.capacities[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE] = 0u;
    config.event_queue_drops_supported = false;
    assert(opcua_resource_service_begin_server(&config) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_EVENT_QUEUE) ==
           OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED);
    assert(opcua_resource_service_record_event_queue_drop() ==
           OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED);
    assert(opcua_resource_service_invalidate(OPCUA_RESOURCE_SERVICE_EVENT_QUEUE) ==
           OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED);
    assert(opcua_resource_service_set_current(OPCUA_RESOURCE_SERVICE_EVENT_QUEUE,
                                              0u, 0u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(!snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].supported);
    assert(snapshot.gauges[OPCUA_RESOURCE_SERVICE_EVENT_QUEUE].quality ==
           OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NOT_SUPPORTED);
    assert(snapshot.event_queue_drops.quality ==
           OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NOT_SUPPORTED);

    /* Invalid configuration is rejected before its active state can change. */
    assert(opcua_resource_service_end_server() == OPCUA_RESOURCE_SERVICE_STATUS_OK);
    config = complete_config();
    config.supported_gauges_mask = OPCUA_RESOURCE_SERVICE_ALL_GAUGES_MASK | (1u << 31u);
    assert(opcua_resource_service_begin_server(&config) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(!snapshot.server_active);
    config = complete_config();
    config.supported_gauges_mask &=
        ~OPCUA_RESOURCE_SERVICE_GAUGE_BIT(OPCUA_RESOURCE_SERVICE_SESSIONS);
    assert(opcua_resource_service_begin_server(&config) ==
           OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT);

    /* Counter value wraps; wrap count then saturates instead of wrapping. */
    config = complete_config();
    assert(opcua_resource_service_begin_server(&config) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_test_set_counters(UINT64_MAX, 7u,
                                                    UINT64_MAX, UINT64_MAX));
    assert(opcua_resource_service_record_rejected_request() ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_record_event_queue_drop() ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.rejected_requests.value == 0u);
    assert(snapshot.rejected_requests.wrap_count == 8u);
    assert(snapshot.event_queue_drops.value == 0u);
    assert(snapshot.event_queue_drops.wrap_count == UINT64_MAX);

    /* A multi-count sample has the same modular counter semantics as an
     * individual definitive rejection/drop. */
    assert(opcua_resource_service_test_set_counters(UINT64_MAX - 1u,
                                                    UINT64_MAX - 1u,
                                                    UINT64_MAX - 2u,
                                                    5u));
    assert(opcua_resource_service_add_rejected_requests(3u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_add_event_queue_drops(5u) ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.rejected_requests.value == 1u);
    assert(snapshot.rejected_requests.wrap_count == UINT64_MAX);
    assert(snapshot.event_queue_drops.value == 2u);
    assert(snapshot.event_queue_drops.wrap_count == 6u);

    /* A missed owner sample poisons a boot-lifetime total even across a
     * controlled server rebuild. Only boot init can restore Good quality. */
    assert(opcua_resource_service_invalidate_event_queue_drops() ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_invalidate_rejected_requests() ==
           OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.event_queue_drops.quality == OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA);
    assert(snapshot.rejected_requests.quality == OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA);
    assert(opcua_resource_service_end_server() == OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_begin_server(&config) == OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.event_queue_drops.quality == OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA);
    opcua_resource_service_init();
    assert(opcua_resource_service_begin_server(&config) == OPCUA_RESOURCE_SERVICE_STATUS_OK);
    assert(opcua_resource_service_get_snapshot(&snapshot));
    assert(snapshot.event_queue_drops.quality == OPCUA_RESOURCE_SERVICE_QUALITY_GOOD);

    assert(g_critical_depth == 0u);
    return 0;
}

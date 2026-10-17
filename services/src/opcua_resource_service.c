/**
 * @file
 * @brief Fixed-storage provider for v0.5.2 OPC UA resource diagnostics.
 */

#include "services/opcua_resource_service.h"

#include <stddef.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

typedef struct
{
    uint64_t value;
    uint64_t wrap_count;
    bool supported;
    bool valid;
} resource_counter_t;

typedef struct
{
    bool initialized;
    bool server_active;
    opcua_resource_service_gauge_snapshot_t gauges[
        OPCUA_RESOURCE_SERVICE_GAUGE_COUNT];
    resource_counter_t rejected_requests;
    resource_counter_t event_queue_drops;
} resource_service_state_t;

static resource_service_state_t g_resource_service_state;

static bool resource_service_gauge_is_valid(opcua_resource_service_gauge_t gauge)
{
    return gauge >= OPCUA_RESOURCE_SERVICE_SESSIONS &&
           gauge < OPCUA_RESOURCE_SERVICE_GAUGE_COUNT;
}

static void resource_service_add(resource_counter_t *counter, uint64_t delta)
{
    if (delta > UINT64_MAX - counter->value)
    {
        counter->value += delta;
        if (counter->wrap_count < UINT64_MAX)
        {
            ++counter->wrap_count;
        }
        return;
    }

    counter->value += delta;
}

static opcua_resource_service_quality_t resource_service_gauge_quality(
    const opcua_resource_service_gauge_snapshot_t *gauge,
    bool server_active)
{
    if (!gauge->supported)
    {
        return OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NOT_SUPPORTED;
    }
    return server_active ? OPCUA_RESOURCE_SERVICE_QUALITY_GOOD :
                           OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA;
}

static opcua_resource_service_counter_snapshot_t resource_service_counter_snapshot(
    const resource_counter_t *counter)
{
    opcua_resource_service_counter_snapshot_t snapshot = {
        .value = counter->value,
        .wrap_count = counter->wrap_count,
        .supported = counter->supported,
        .quality = !counter->supported ? OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NOT_SUPPORTED :
                   counter->valid ? OPCUA_RESOURCE_SERVICE_QUALITY_GOOD :
                                    OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA,
    };
    return snapshot;
}

void opcua_resource_service_init(void)
{
    uint32_t index;
    taskENTER_CRITICAL();
    memset(&g_resource_service_state, 0, sizeof(g_resource_service_state));
    g_resource_service_state.initialized = true;
    g_resource_service_state.rejected_requests.valid = true;
    g_resource_service_state.event_queue_drops.valid = true;
    for (index = 0u; index < OPCUA_RESOURCE_SERVICE_GAUGE_COUNT; ++index)
    {
        g_resource_service_state.gauges[index].quality =
            OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA;
    }
    taskEXIT_CRITICAL();
}

opcua_resource_service_status_t opcua_resource_service_begin_server(
    const opcua_resource_service_server_config_t *config)
{
    uint32_t index;

    if (config == NULL ||
        (config->supported_gauges_mask & ~OPCUA_RESOURCE_SERVICE_ALL_GAUGES_MASK) != 0u)
    {
        return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0u; index < OPCUA_RESOURCE_SERVICE_GAUGE_COUNT; ++index)
    {
        const bool supported =
            (config->supported_gauges_mask & OPCUA_RESOURCE_SERVICE_GAUGE_BIT(index)) != 0u;
        if (!supported && config->capacities[index] != 0u)
        {
            return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
        }
    }

    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_ALREADY_ACTIVE;
    }

    for (index = 0u; index < OPCUA_RESOURCE_SERVICE_GAUGE_COUNT; ++index)
    {
        const bool supported =
            (config->supported_gauges_mask & OPCUA_RESOURCE_SERVICE_GAUGE_BIT(index)) != 0u;
        opcua_resource_service_gauge_snapshot_t *gauge =
            &g_resource_service_state.gauges[index];
        gauge->current = 0u;
        gauge->peak = 0u;
        gauge->capacity = config->capacities[index];
        gauge->supported = supported;
        gauge->quality = resource_service_gauge_quality(gauge, true);
    }
    g_resource_service_state.rejected_requests.supported =
        config->rejected_requests_supported;
    g_resource_service_state.event_queue_drops.supported =
        config->event_queue_drops_supported;
    g_resource_service_state.server_active = true;
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

opcua_resource_service_status_t opcua_resource_service_end_server(void)
{
    uint32_t index;

    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (!g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE;
    }

    g_resource_service_state.server_active = false;
    for (index = 0u; index < OPCUA_RESOURCE_SERVICE_GAUGE_COUNT; ++index)
    {
        opcua_resource_service_gauge_snapshot_t *gauge =
            &g_resource_service_state.gauges[index];
        gauge->current = 0u;
        gauge->peak = 0u;
        gauge->capacity = 0u;
        gauge->quality = resource_service_gauge_quality(gauge, false);
    }
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

opcua_resource_service_status_t opcua_resource_service_acquire(
    opcua_resource_service_gauge_t gauge)
{
    opcua_resource_service_gauge_snapshot_t *state_gauge;

    if (!resource_service_gauge_is_valid(gauge))
    {
        return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (!g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE;
    }

    state_gauge = &g_resource_service_state.gauges[(uint32_t)gauge];
    if (!state_gauge->supported)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED;
    }
    if (state_gauge->current >= state_gauge->capacity)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_CAPACITY_EXCEEDED;
    }

    ++state_gauge->current;
    if (state_gauge->current > state_gauge->peak)
    {
        state_gauge->peak = state_gauge->current;
    }
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

opcua_resource_service_status_t opcua_resource_service_release(
    opcua_resource_service_gauge_t gauge)
{
    opcua_resource_service_gauge_snapshot_t *state_gauge;

    if (!resource_service_gauge_is_valid(gauge))
    {
        return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (!g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE;
    }

    state_gauge = &g_resource_service_state.gauges[(uint32_t)gauge];
    if (!state_gauge->supported)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED;
    }
    if (state_gauge->current == 0u)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_UNDERFLOW;
    }

    --state_gauge->current;
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

opcua_resource_service_status_t opcua_resource_service_observe(
    opcua_resource_service_gauge_t gauge,
    uint32_t current,
    uint32_t peak,
    uint32_t capacity)
{
    opcua_resource_service_gauge_snapshot_t *state_gauge;

    if (!resource_service_gauge_is_valid(gauge))
    {
        return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (!g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE;
    }

    state_gauge = &g_resource_service_state.gauges[(uint32_t)gauge];
    if (!state_gauge->supported)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED;
    }
    if (current > capacity ||
        (gauge != OPCUA_RESOURCE_SERVICE_EVENT_QUEUE && peak > capacity))
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_CAPACITY_EXCEEDED;
    }
    if (peak < current)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
    }
    if (gauge != OPCUA_RESOURCE_SERVICE_EVENT_QUEUE &&
        capacity != state_gauge->capacity)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    state_gauge->current = current;
    if (gauge == OPCUA_RESOURCE_SERVICE_EVENT_QUEUE)
    {
        state_gauge->capacity = capacity;
    }
    if (peak > state_gauge->peak)
    {
        state_gauge->peak = peak;
    }
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

opcua_resource_service_status_t opcua_resource_service_set_current(
    opcua_resource_service_gauge_t gauge,
    uint32_t current,
    uint32_t capacity)
{
    return opcua_resource_service_observe(gauge, current, current, capacity);
}

opcua_resource_service_status_t opcua_resource_service_invalidate(
    opcua_resource_service_gauge_t gauge)
{
    opcua_resource_service_gauge_snapshot_t *state_gauge;

    if (!resource_service_gauge_is_valid(gauge))
    {
        return OPCUA_RESOURCE_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (!g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE;
    }

    state_gauge = &g_resource_service_state.gauges[(uint32_t)gauge];
    if (!state_gauge->supported)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED;
    }

    state_gauge->quality = OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NO_DATA;
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

static opcua_resource_service_status_t resource_service_add_counter(
    resource_counter_t *counter,
    uint64_t delta)
{
    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (!g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE;
    }
    if (!counter->supported)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED;
    }
    resource_service_add(counter, delta);
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

opcua_resource_service_status_t
opcua_resource_service_record_rejected_request(void)
{
    return opcua_resource_service_add_rejected_requests(1u);
}

opcua_resource_service_status_t
opcua_resource_service_record_event_queue_drop(void)
{
    return opcua_resource_service_add_event_queue_drops(1u);
}

opcua_resource_service_status_t
opcua_resource_service_add_rejected_requests(uint64_t delta)
{
    return resource_service_add_counter(&g_resource_service_state.rejected_requests,
                                        delta);
}

opcua_resource_service_status_t
opcua_resource_service_add_event_queue_drops(uint64_t delta)
{
    return resource_service_add_counter(&g_resource_service_state.event_queue_drops,
                                        delta);
}

static opcua_resource_service_status_t resource_service_invalidate_counter(
    resource_counter_t *counter)
{
    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if (!g_resource_service_state.server_active)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_SERVER_NOT_ACTIVE;
    }
    if (!counter->supported)
    {
        taskEXIT_CRITICAL();
        return OPCUA_RESOURCE_SERVICE_STATUS_NOT_SUPPORTED;
    }
    counter->valid = false;
    taskEXIT_CRITICAL();
    return OPCUA_RESOURCE_SERVICE_STATUS_OK;
}

opcua_resource_service_status_t
opcua_resource_service_invalidate_rejected_requests(void)
{
    return resource_service_invalidate_counter(&g_resource_service_state.rejected_requests);
}

opcua_resource_service_status_t
opcua_resource_service_invalidate_event_queue_drops(void)
{
    return resource_service_invalidate_counter(&g_resource_service_state.event_queue_drops);
}

bool opcua_resource_service_get_snapshot(
    opcua_resource_service_snapshot_t *out_snapshot)
{
    uint32_t index;

    if (out_snapshot == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    out_snapshot->initialized = true;
    out_snapshot->server_active = g_resource_service_state.server_active;
    for (index = 0u; index < OPCUA_RESOURCE_SERVICE_GAUGE_COUNT; ++index)
    {
        out_snapshot->gauges[index] = g_resource_service_state.gauges[index];
    }
    out_snapshot->rejected_requests = resource_service_counter_snapshot(
        &g_resource_service_state.rejected_requests);
    out_snapshot->event_queue_drops = resource_service_counter_snapshot(
        &g_resource_service_state.event_queue_drops);
    taskEXIT_CRITICAL();
    return true;
}

#ifdef OPCUA_RESOURCE_SERVICE_TEST_HOOKS
bool opcua_resource_service_test_set_counters(uint64_t rejected_value,
                                              uint64_t rejected_wrap_count,
                                              uint64_t drop_value,
                                              uint64_t drop_wrap_count)
{
    taskENTER_CRITICAL();
    if (!g_resource_service_state.initialized)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    g_resource_service_state.rejected_requests.value = rejected_value;
    g_resource_service_state.rejected_requests.wrap_count = rejected_wrap_count;
    g_resource_service_state.event_queue_drops.value = drop_value;
    g_resource_service_state.event_queue_drops.wrap_count = drop_wrap_count;
    taskEXIT_CRITICAL();
    return true;
}
#endif

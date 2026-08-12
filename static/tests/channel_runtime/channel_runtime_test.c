#include "services/channel_manager.h"
#include "services/telemetry_driver.h"
#include "services/time_service.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

#define TEST_MAX_CHANNELS PLATFORM_CHANNEL_LIMIT
#define EVENT_START_BASE 100
#define EVENT_STOP_BASE 200
#define EVENT_CONFIGURE_BASE 300
#define EVENT_SELF_TEST_BASE 400

typedef struct
{
    unsigned ordinal;
    bool started;
    bool fail_start;
    bool fail_configure;
    bool fail_self_test;
    bool fail_read;
    bool fail_write;
} loopback_context_t;

static unsigned g_events[TEST_MAX_CHANNELS * 4u];
static size_t g_event_count;

bool time_service_get_utc_time_us(uint64_t *out_unix_time_us)
{
    if (out_unix_time_us != NULL)
    {
        *out_unix_time_us = 1700000000000000ULL;
    }
    return true;
}

static void record_event(unsigned event)
{
    assert(g_event_count < sizeof(g_events) / sizeof(g_events[0]));
    g_events[g_event_count++] = event;
}

static loopback_context_t *context_from(telemetry_channel_t *channel)
{
    return channel != NULL ? (loopback_context_t *)channel->driver_context : NULL;
}

static telemetry_driver_result_t loopback_start(telemetry_channel_t *channel)
{
    loopback_context_t *context = context_from(channel);

    if (context == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (context->started)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    if (context->fail_start)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    context->started = true;
    record_event(EVENT_START_BASE + context->ordinal);
    return TELEMETRY_DRIVER_RESULT_OK;
}

static telemetry_driver_result_t loopback_stop(telemetry_channel_t *channel)
{
    loopback_context_t *context = context_from(channel);

    if (context == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!context->started)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    context->started = false;
    record_event(EVENT_STOP_BASE + context->ordinal);
    return TELEMETRY_DRIVER_RESULT_OK;
}

static telemetry_driver_result_t loopback_read(telemetry_channel_t *channel, uint32_t now_ms)
{
    loopback_context_t *context = context_from(channel);

    (void)now_ms;
    if (context == NULL || !context->started)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    return context->fail_read ? TELEMETRY_DRIVER_RESULT_TIMEOUT : TELEMETRY_DRIVER_RESULT_OK;
}

static telemetry_driver_result_t loopback_write(telemetry_channel_t *channel,
                                                const telemetry_channel_value_t *value)
{
    loopback_context_t *context = context_from(channel);

    if (context == NULL || value == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    if (!context->started)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    if (context->fail_write)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    if (channel->metadata.value_type == TELEMETRY_CHANNEL_VALUE_STRING &&
        memchr(value->string, '\0', TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN) == NULL)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT;
    }
    channel->runtime.value = *value;
    return TELEMETRY_DRIVER_RESULT_OK;
}

static telemetry_driver_result_t loopback_configure(telemetry_channel_t *channel, const void *config)
{
    loopback_context_t *context = context_from(channel);

    (void)config;
    if (context == NULL || !context->started)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    record_event(EVENT_CONFIGURE_BASE + context->ordinal);
    return context->fail_configure ? TELEMETRY_DRIVER_RESULT_UNSUPPORTED : TELEMETRY_DRIVER_RESULT_OK;
}

static telemetry_driver_result_t loopback_self_test(telemetry_channel_t *channel)
{
    loopback_context_t *context = context_from(channel);

    if (context == NULL || !context->started)
    {
        return TELEMETRY_DRIVER_RESULT_INVALID_STATE;
    }
    record_event(EVENT_SELF_TEST_BASE + context->ordinal);
    return context->fail_self_test ? TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE : TELEMETRY_DRIVER_RESULT_OK;
}

static const telemetry_driver_vtbl_t g_loopback_driver = {
    .start = loopback_start,
    .stop = loopback_stop,
    .read = loopback_read,
    .write = loopback_write,
    .configure = loopback_configure,
    .self_test = loopback_self_test,
};

static void reset_events(void)
{
    memset(g_events, 0, sizeof(g_events));
    g_event_count = 0u;
}

static void build_configs(channel_config_t *configs, loopback_context_t *contexts,
                          char ids[][TELEMETRY_CHANNEL_ID_MAX_LEN],
                          char names[][TELEMETRY_CHANNEL_NAME_MAX_LEN], size_t count)
{
    for (size_t index = 0u; index < count; ++index)
    {
        static const telemetry_channel_value_type_t types[] = {
            TELEMETRY_CHANNEL_VALUE_FLOAT, TELEMETRY_CHANNEL_VALUE_DOUBLE,
            TELEMETRY_CHANNEL_VALUE_INT32, TELEMETRY_CHANNEL_VALUE_UINT32,
            TELEMETRY_CHANNEL_VALUE_INT64, TELEMETRY_CHANNEL_VALUE_UINT64,
            TELEMETRY_CHANNEL_VALUE_BOOLEAN, TELEMETRY_CHANNEL_VALUE_STRING,
        };

        memset(&configs[index], 0, sizeof(configs[index]));
        contexts[index] = (loopback_context_t){.ordinal = (unsigned)index};
        ids[index][0] = 'c'; ids[index][1] = (char)('0' + (index / 10u));
        ids[index][2] = (char)('0' + (index % 10u)); ids[index][3] = '\0';
        names[index][0] = 'C'; names[index][1] = (char)('0' + (index / 10u));
        names[index][2] = (char)('0' + (index % 10u)); names[index][3] = '\0';
        configs[index].metadata.id = ids[index];
        configs[index].metadata.name = names[index];
        configs[index].metadata.description = "host-only loopback";
        configs[index].metadata.value_type = types[index % (sizeof(types) / sizeof(types[0]))];
        configs[index].metadata.flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE;
        configs[index].settings.enabled = true;
        configs[index].driver = &g_loopback_driver;
        configs[index].driver_context = &contexts[index];
    }
}

/* The production sensor manager performs the same bounded lifecycle order;
 * this host-only harness leaves the current registry untouched until all
 * candidate starts/configuration/self-tests succeed. */
static bool activate(channel_config_t *configs, size_t count)
{
    size_t started = 0u;

    if (configs == NULL || count == 0u || count > TEST_MAX_CHANNELS)
    {
        return false;
    }
    for (size_t index = 0u; index < count; ++index)
    {
        telemetry_channel_t candidate = {
            .metadata = configs[index].metadata,
            .settings = configs[index].settings,
            .driver = configs[index].driver,
            .driver_context = configs[index].driver_context,
        };
        if (candidate.driver->start(&candidate) != TELEMETRY_DRIVER_RESULT_OK)
        {
            break;
        }
        ++started;
        if (candidate.driver->configure(&candidate, NULL) != TELEMETRY_DRIVER_RESULT_OK ||
            candidate.driver->self_test(&candidate) != TELEMETRY_DRIVER_RESULT_OK)
        {
            break;
        }
    }
    if (started != count)
    {
        while (started > 0u)
        {
            telemetry_channel_t candidate = {
                .metadata = configs[started - 1u].metadata,
                .settings = configs[started - 1u].settings,
                .driver = configs[started - 1u].driver,
                .driver_context = configs[started - 1u].driver_context,
            };
            assert(candidate.driver->stop(&candidate) == TELEMETRY_DRIVER_RESULT_OK);
            --started;
        }
        return false;
    }
    assert(channel_manager_reset());
    return channel_manager_register_from_config(configs, count) == count;
}

static void write_and_verify(size_t index, telemetry_channel_value_t value)
{
    channel_manager_driver_operation_t operation = {0};
    telemetry_channel_snapshot_t snapshot = {0};
    telemetry_driver_result_t result;

    assert(channel_manager_acquire_driver_operation(index, &operation));
    result = operation.snapshot.channel.driver->write(&operation.snapshot.channel, &value);
    assert(result == TELEMETRY_DRIVER_RESULT_OK);
    assert(channel_manager_publish_driver_operation(&operation));
    channel_manager_release_driver_operation(&operation);
    assert(channel_manager_copy_snapshot_by_index(index, &snapshot));
    switch (snapshot.channel.metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_FLOAT: assert(snapshot.channel.runtime.value.f32 == value.f32); break;
    case TELEMETRY_CHANNEL_VALUE_DOUBLE: assert(snapshot.channel.runtime.value.f64 == value.f64); break;
    case TELEMETRY_CHANNEL_VALUE_INT32: assert(snapshot.channel.runtime.value.i32 == value.i32); break;
    case TELEMETRY_CHANNEL_VALUE_UINT32: assert(snapshot.channel.runtime.value.u32 == value.u32); break;
    case TELEMETRY_CHANNEL_VALUE_INT64: assert(snapshot.channel.runtime.value.i64 == value.i64); break;
    case TELEMETRY_CHANNEL_VALUE_UINT64: assert(snapshot.channel.runtime.value.u64 == value.u64); break;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN: assert(snapshot.channel.runtime.value.boolean == value.boolean); break;
    case TELEMETRY_CHANNEL_VALUE_STRING: assert(strcmp(snapshot.channel.runtime.value.string, value.string) == 0); break;
    }
}

static void test_lifecycle_rollback_and_known_good(void)
{
    channel_config_t old_config[1], candidate[4];
    loopback_context_t old_context[1], candidate_context[4];
    char old_ids[1][TELEMETRY_CHANNEL_ID_MAX_LEN], old_names[1][TELEMETRY_CHANNEL_NAME_MAX_LEN];
    char ids[4][TELEMETRY_CHANNEL_ID_MAX_LEN], names[4][TELEMETRY_CHANNEL_NAME_MAX_LEN];
    telemetry_channel_snapshot_t snapshot = {0};

    build_configs(old_config, old_context, old_ids, old_names, 1u);
    reset_events();
    assert(activate(old_config, 1u));
    build_configs(candidate, candidate_context, ids, names, 4u);
    candidate_context[2].fail_start = true;
    reset_events();
    assert(!activate(candidate, 4u));
    assert(g_event_count == 8u);
    assert(g_events[0] == EVENT_START_BASE && g_events[3] == EVENT_START_BASE + 1u);
    assert(g_events[6] == EVENT_STOP_BASE + 1u && g_events[7] == EVENT_STOP_BASE);
    assert(channel_manager_count() == 1u);
    assert(channel_manager_copy_snapshot_by_id("c00", &snapshot));
    assert(snapshot.channel.driver_context == &old_context[0]);

    candidate_context[2].fail_start = false;
    reset_events();
    assert(activate(candidate, 4u));
    assert(channel_manager_count() == 4u);
    assert(g_event_count == 12u);
}

static void test_all_types_boundaries_and_read_failure(void)
{
    channel_config_t configs[TEST_MAX_CHANNELS];
    loopback_context_t contexts[TEST_MAX_CHANNELS];
    char ids[TEST_MAX_CHANNELS][TELEMETRY_CHANNEL_ID_MAX_LEN];
    char names[TEST_MAX_CHANNELS][TELEMETRY_CHANNEL_NAME_MAX_LEN];
    telemetry_channel_value_t value = {0};
    channel_manager_driver_operation_t operation = {0};
    telemetry_channel_snapshot_t snapshot = {0};

    build_configs(configs, contexts, ids, names, TEST_MAX_CHANNELS);
    reset_events();
    assert(activate(configs, TEST_MAX_CHANNELS));
    value.f32 = -3.4028234e38f; write_and_verify(0u, value);
    value.f64 = 1.7976931348623157e308; write_and_verify(1u, value);
    value.i32 = INT32_MIN; write_and_verify(2u, value);
    value.u32 = UINT32_MAX; write_and_verify(3u, value);
    value.i64 = INT64_MIN; write_and_verify(4u, value);
    value.u64 = UINT64_MAX; write_and_verify(5u, value);
    value.boolean = true; write_and_verify(6u, value);
    memset(value.string, 's', TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN - 1u);
    value.string[TELEMETRY_CHANNEL_STRING_VALUE_MAX_LEN - 1u] = '\0';
    write_and_verify(7u, value);

    assert(channel_manager_acquire_driver_operation(7u, &operation));
    memset(value.string, 'x', sizeof(value.string));
    assert(operation.snapshot.channel.driver->write(&operation.snapshot.channel, &value) ==
           TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT);
    channel_manager_release_driver_operation(&operation);
    contexts[3].fail_read = true;
    assert(!channel_manager_poll_all(99u));
    assert(channel_manager_copy_snapshot_by_index(3u, &snapshot));
    assert(snapshot.channel.runtime.quality == SERVICE_QUALITY_BAD);
    assert(snapshot.channel.runtime.error_count == 1u);
    assert(channel_manager_copy_snapshot_by_index(0u, &snapshot));
    assert(snapshot.channel.runtime.source_timestamp_unix_us == 1700000000000000ULL);
}

static void test_invalid_lifecycle_states(void)
{
    loopback_context_t context = {.ordinal = 15u};
    telemetry_channel_t channel = {.driver_context = &context};
    telemetry_channel_value_t value = {0};

    assert(loopback_read(&channel, 0u) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(loopback_write(&channel, &value) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(loopback_configure(&channel, NULL) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(loopback_self_test(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
    assert(loopback_stop(&channel) == TELEMETRY_DRIVER_RESULT_INVALID_STATE);
}

int main(void)
{
    assert(channel_manager_init());
    test_invalid_lifecycle_states();
    test_lifecycle_rollback_and_known_good();
    test_all_types_boundaries_and_read_failure();
    return 0;
}

#include "services/channel_manager.h"
#include "services/telemetry_driver.h"
#include "services/time_service.h"

#include <assert.h>
#include <string.h>

static unsigned g_read_calls;

bool time_service_get_utc_time_us(uint64_t *out_unix_time_us)
{
    if (out_unix_time_us != NULL)
    {
        *out_unix_time_us = 1700000000123456ULL;
    }
    return true;
}

static telemetry_driver_result_t read_ok(telemetry_channel_t *channel, uint32_t now_ms)
{
    ++g_read_calls;
    channel->runtime.value.f32 = (float)now_ms;
    return TELEMETRY_DRIVER_RESULT_OK;
}

static telemetry_driver_result_t read_fail(telemetry_channel_t *channel, uint32_t now_ms)
{
    (void)channel;
    (void)now_ms;
    ++g_read_calls;
    return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
}

static unsigned g_write_calls;
static bool g_last_written_value;

/* v0.5.1 T1: proves the driver-level write() slot dispatches through the
 * real channel_manager_write_value() -> driver->write() chain, exercising a
 * path that had no writable driver to test against before this release. */
static telemetry_driver_result_t write_ok(telemetry_channel_t *channel, const telemetry_channel_value_t *value)
{
    ++g_write_calls;
    g_last_written_value = value->boolean;
    channel->runtime.value.boolean = value->boolean;
    return TELEMETRY_DRIVER_RESULT_OK;
}

static telemetry_driver_result_t write_fail(telemetry_channel_t *channel, const telemetry_channel_value_t *value)
{
    (void)channel;
    (void)value;
    ++g_write_calls;
    return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
}

static const telemetry_driver_vtbl_t g_ok_driver = {.read = read_ok};
static const telemetry_driver_vtbl_t g_fail_driver = {.read = read_fail};
static const telemetry_driver_vtbl_t g_writable_driver = {.read = read_ok, .write = write_ok};
static const telemetry_driver_vtbl_t g_write_fail_driver = {.read = read_ok, .write = write_fail};
static const telemetry_driver_vtbl_t g_no_write_driver = {.read = read_ok};

static telemetry_channel_t make_channel(const char *id, const char *name,
                                        const telemetry_driver_vtbl_t *driver)
{
    telemetry_channel_t channel = {0};
    channel.metadata.id = id;
    channel.metadata.name = name;
    channel.metadata.description = "description";
    channel.metadata.unit = "V";
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    channel.driver = driver;
    channel.settings.enabled = true;
    return channel;
}

static void test_owned_metadata_and_snapshot_copy(void)
{
    char id[] = "one";
    char name[] = "One";
    telemetry_channel_t channel = make_channel(id, name, &g_ok_driver);
    telemetry_channel_snapshot_t snapshot = {0};
    telemetry_channel_snapshot_t clone = {0};

    channel.metadata.source = (telemetry_channel_source_descriptor_t){
        .driver_id = 7u, .source_index = 8u, .source_flags = 9u, .reserved = 0u};

    channel_manager_init();
    assert(channel_manager_register(&channel, NULL));
    strcpy(id, "bad");
    strcpy(name, "Bad");
    memset(&channel, 0, sizeof(channel));
    assert(channel_manager_copy_snapshot_by_id("one", &snapshot));
    assert(strcmp(snapshot.channel.metadata.id, "one") == 0);
    assert(strcmp(snapshot.channel.metadata.name, "One") == 0);
    assert(snapshot.channel.metadata.source.driver_id == 7u);
    assert(snapshot.channel.metadata.source.source_index == 8u);
    assert(snapshot.channel.metadata.source.source_flags == 9u);
    telemetry_channel_snapshot_clone(&clone, &snapshot);
    assert(clone.channel.metadata.id == clone.id);
    assert(strcmp(clone.channel.metadata.id, "one") == 0);
    assert(clone.channel.metadata.source.driver_id == 7u);
}

static void test_capacity_and_stale_operation(void)
{
    telemetry_channel_t channels[PLATFORM_CHANNEL_LIMIT + 1u];
    char ids[PLATFORM_CHANNEL_LIMIT + 1u][8];
    char names[PLATFORM_CHANNEL_LIMIT + 1u][8];
    channel_manager_driver_operation_t operation = {0};
    telemetry_channel_snapshot_t snapshot = {0};

    assert(channel_manager_reset());
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT + 1u; ++index)
    {
        ids[index][0] = 'i'; ids[index][1] = (char)('0' + (index / 10u));
        ids[index][2] = (char)('0' + (index % 10u)); ids[index][3] = '\0';
        names[index][0] = 'n'; names[index][1] = (char)('0' + (index / 10u));
        names[index][2] = (char)('0' + (index % 10u)); names[index][3] = '\0';
        channels[index] = make_channel(ids[index], names[index], &g_ok_driver);
    }
    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        assert(channel_manager_register(&channels[index], NULL));
    }
    assert(!channel_manager_register(&channels[PLATFORM_CHANNEL_LIMIT], NULL));
    assert(channel_manager_count() == PLATFORM_CHANNEL_LIMIT);
    assert(channel_manager_acquire_driver_operation(0u, &operation));
    assert(!channel_manager_reset());
    assert(channel_manager_update_settings("i00", &(telemetry_channel_settings_t){.enabled = false}));
    assert(!channel_manager_publish_driver_operation(&operation));
    channel_manager_release_driver_operation(&operation);
    assert(channel_manager_reset());
    assert(!channel_manager_copy_snapshot_by_index(0u, &snapshot));
}

static void test_config_registration_and_poll_publication(void)
{
    channel_config_t configs[3];
    telemetry_channel_snapshot_t snapshot = {0};

    assert(channel_manager_reset());
    configs[0] = (channel_config_t){.metadata = make_channel("first", "First", &g_ok_driver).metadata,
                                    .settings = {.enabled = true}, .driver = &g_ok_driver};
    configs[1] = (channel_config_t){.metadata = make_channel("first", "Duplicate", &g_ok_driver).metadata,
                                    .settings = {.enabled = true}, .driver = &g_ok_driver};
    configs[2] = (channel_config_t){.metadata = make_channel("second", "Second", &g_fail_driver).metadata,
                                    .settings = {.enabled = true}, .driver = &g_fail_driver};
    assert(channel_manager_register_from_config(NULL, 1u) == 0u);
    assert(channel_manager_register_from_config(configs, 3u) == 2u);
    g_read_calls = 0u;
    assert(!channel_manager_poll_all(1234u));
    assert(g_read_calls == 2u);
    assert(channel_manager_copy_snapshot_by_id("first", &snapshot));
    assert(snapshot.channel.runtime.value.f32 == 1234.0f);
    assert(snapshot.channel.runtime.sample_count == 1u);
    assert(snapshot.channel.runtime.source_timestamp_unix_us == 1700000000123456ULL);
    assert(snapshot.channel.runtime.quality == SERVICE_QUALITY_GOOD);
    assert(channel_manager_copy_snapshot_by_id("second", &snapshot));
    assert(snapshot.channel.runtime.error_count == 1u);
    assert(snapshot.channel.runtime.quality == SERVICE_QUALITY_BAD);
}

static void test_write_dispatch(void)
{
    telemetry_channel_t channel = {0};
    telemetry_channel_snapshot_t snapshot = {0};

    assert(channel_manager_reset());
    channel.metadata.id = "gpio_out";
    channel.metadata.name = "GpioOut";
    channel.metadata.description = "description";
    channel.metadata.unit = "";
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    channel.metadata.flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE;
    channel.driver = &g_writable_driver;
    channel.settings.enabled = true;
    assert(channel_manager_register(&channel, NULL));

    g_write_calls = 0u;
    assert(channel_manager_write_value("gpio_out", TELEMETRY_CHANNEL_VALUE_BOOLEAN,
                                        &(telemetry_channel_value_t){.boolean = true}, &snapshot) ==
           CHANNEL_MANAGER_WRITE_RESULT_OK);
    assert(g_write_calls == 1u);
    assert(g_last_written_value == true);
    assert(snapshot.channel.runtime.value.boolean == true);

    /* Type mismatch is rejected before the driver is ever called. */
    assert(channel_manager_write_value("gpio_out", TELEMETRY_CHANNEL_VALUE_FLOAT,
                                        &(telemetry_channel_value_t){.f32 = 1.0f}, NULL) ==
           CHANNEL_MANAGER_WRITE_RESULT_TYPE_MISMATCH);
    assert(g_write_calls == 1u);

    /* A driver failure propagates as a typed result without crashing. */
    assert(channel_manager_reset());
    channel = (telemetry_channel_t){0};
    channel.metadata.id = "gpio_out_fail";
    channel.metadata.name = "GpioOutFail";
    channel.metadata.description = "description";
    channel.metadata.unit = "";
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    channel.metadata.flags = TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE;
    channel.driver = &g_write_fail_driver;
    channel.settings.enabled = true;
    assert(channel_manager_register(&channel, NULL));
    assert(channel_manager_write_value("gpio_out_fail", TELEMETRY_CHANNEL_VALUE_BOOLEAN,
                                        &(telemetry_channel_value_t){.boolean = true}, NULL) ==
           CHANNEL_MANAGER_WRITE_RESULT_HARDWARE_FAILURE);

    /* A read-only channel (WRITABLE not set) rejects the write before dispatch. */
    assert(channel_manager_reset());
    channel = (telemetry_channel_t){0};
    channel.metadata.id = "ro";
    channel.metadata.name = "ReadOnly";
    channel.metadata.description = "description";
    channel.metadata.unit = "";
    channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    channel.metadata.flags = TELEMETRY_CHANNEL_FLAG_ENABLED;
    channel.driver = &g_no_write_driver;
    channel.settings.enabled = true;
    assert(channel_manager_register(&channel, NULL));
    assert(channel_manager_write_value("ro", TELEMETRY_CHANNEL_VALUE_BOOLEAN,
                                        &(telemetry_channel_value_t){.boolean = true}, NULL) ==
           CHANNEL_MANAGER_WRITE_RESULT_READ_ONLY);
}

int main(void)
{
    test_owned_metadata_and_snapshot_copy();
    test_capacity_and_stale_operation();
    test_config_registration_and_poll_publication();
    test_write_dispatch();
    return 0;
}

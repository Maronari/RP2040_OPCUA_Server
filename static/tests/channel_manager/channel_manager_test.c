#include "services/channel_manager.h"
#include "services/telemetry_driver.h"

#include <assert.h>
#include <string.h>

static unsigned g_poll_calls;

static bool poll_ok(telemetry_channel_t *channel, uint32_t now_ms)
{
    ++g_poll_calls;
    channel->runtime.last_update_ms = now_ms;
    return true;
}

static bool poll_fail(telemetry_channel_t *channel, uint32_t now_ms)
{
    ++g_poll_calls;
    channel->runtime.last_update_ms = now_ms;
    channel->runtime.error_count++;
    return false;
}

static const telemetry_driver_vtbl_t g_ok_driver = {.poll = poll_ok};
static const telemetry_driver_vtbl_t g_fail_driver = {.poll = poll_fail};

static telemetry_channel_t make_channel(const char *id, const char *name,
                                        const telemetry_driver_vtbl_t *driver)
{
    telemetry_channel_t channel = {0};
    channel.metadata.id = id;
    channel.metadata.name = name;
    channel.driver = driver;
    channel.settings.enabled = true;
    return channel;
}

static void test_register_lookup_copy_and_update(void)
{
    telemetry_channel_t channel = make_channel("one", "One", &g_ok_driver);
    telemetry_channel_t duplicate = make_channel("one", "Other", &g_ok_driver);
    telemetry_channel_t empty_id = make_channel("", "Empty id", &g_ok_driver);
    telemetry_channel_t empty_name = make_channel("empty-name", "", &g_ok_driver);
    telemetry_channel_t copy = {0};
    telemetry_channel_settings_t updated = {.enabled = false, .sample_period_ms = 100u};

    channel_manager_init();
    assert(channel_manager_count() == 0u);
    assert(!channel_manager_register(NULL));
    assert(!channel_manager_register(&(telemetry_channel_t){0}));
    assert(!channel_manager_register(&empty_id));
    assert(!channel_manager_register(&empty_name));
    assert(channel_manager_register(&channel));
    assert(!channel_manager_register(&duplicate));
    assert(channel_manager_count() == 1u);
    assert(channel_manager_find_by_id(NULL) == NULL);
    assert(channel_manager_find_by_id("missing") == NULL);
    assert(channel_manager_find_by_name("One") == &channel);
    assert(channel_manager_get_by_index(1u) == NULL);
    assert(!channel_manager_copy_channel(NULL, &copy));
    assert(!channel_manager_copy_channel("one", NULL));
    assert(channel_manager_copy_channel("one", &copy));
    assert(copy.metadata.id == channel.metadata.id && copy.settings.enabled);
    assert(!channel_manager_update_settings(NULL, &updated));
    assert(!channel_manager_update_settings("one", NULL));
    assert(channel_manager_update_settings("one", &updated));
    assert(channel.settings.sample_period_ms == 100u && !channel.settings.enabled);
}

static void test_capacity_and_config_registration(void)
{
    telemetry_channel_t channels[CHANNEL_MANAGER_MAX_CHANNELS + 1u];
    char ids[CHANNEL_MANAGER_MAX_CHANNELS + 1u][8];
    char names[CHANNEL_MANAGER_MAX_CHANNELS + 1u][8];
    channel_config_t configs[3];
    telemetry_channel_t storage[2];

    channel_manager_reset();
    for (size_t index = 0u; index < CHANNEL_MANAGER_MAX_CHANNELS + 1u; ++index)
    {
        ids[index][0] = 'i'; ids[index][1] = (char)('0' + (index / 10u));
        ids[index][2] = (char)('0' + (index % 10u)); ids[index][3] = '\0';
        names[index][0] = 'n'; names[index][1] = (char)('0' + (index / 10u));
        names[index][2] = (char)('0' + (index % 10u)); names[index][3] = '\0';
        channels[index] = make_channel(ids[index], names[index], NULL);
    }
    for (size_t index = 0u; index < CHANNEL_MANAGER_MAX_CHANNELS; ++index)
    {
        assert(channel_manager_register(&channels[index]));
    }
    assert(!channel_manager_register(&channels[CHANNEL_MANAGER_MAX_CHANNELS]));
    assert(channel_manager_count() == CHANNEL_MANAGER_MAX_CHANNELS);

    channel_manager_reset();
    configs[0] = (channel_config_t){.metadata = make_channel("first", "First", &g_ok_driver).metadata,
                                    .settings = {.enabled = true}, .driver = &g_ok_driver};
    configs[1] = (channel_config_t){.metadata = make_channel("first", "Duplicate", &g_ok_driver).metadata,
                                    .settings = {.enabled = true}, .driver = &g_ok_driver};
    configs[2] = (channel_config_t){.metadata = make_channel("second", "Second", &g_fail_driver).metadata,
                                    .settings = {.enabled = true}, .driver = &g_fail_driver};
    assert(channel_manager_register_from_config(NULL, 1u, storage, 2u) == 0u);
    assert(channel_manager_register_from_config(configs, 3u, NULL, 2u) == 0u);
    assert(channel_manager_register_from_config(configs, 3u, storage, 0u) == 0u);
    assert(channel_manager_register_from_config(configs, 3u, storage, 2u) == 2u);
    assert(channel_manager_count() == 2u);
    assert(channel_manager_find_by_id("first") == &storage[0]);
    assert(channel_manager_find_by_id("second") == &storage[1]);
    assert(storage[0].runtime.quality == SERVICE_QUALITY_UNCERTAIN);
}

static void test_poll_failure_does_not_skip_later_channels(void)
{
    telemetry_channel_t first = make_channel("first", "First", &g_fail_driver);
    telemetry_channel_t second = make_channel("second", "Second", &g_ok_driver);
    telemetry_channel_t no_poll = make_channel("third", "Third", NULL);

    channel_manager_reset();
    assert(channel_manager_register(&first));
    assert(channel_manager_register(&second));
    assert(channel_manager_register(&no_poll));
    g_poll_calls = 0u;
    assert(!channel_manager_poll_all(1234u));
    assert(g_poll_calls == 2u);
    assert(first.runtime.last_update_ms == 1234u && first.runtime.error_count == 1u);
    assert(second.runtime.last_update_ms == 1234u);
}

int main(void)
{
    test_register_lookup_copy_and_update();
    test_capacity_and_config_registration();
    test_poll_failure_does_not_skip_later_channels();
    return 0;
}

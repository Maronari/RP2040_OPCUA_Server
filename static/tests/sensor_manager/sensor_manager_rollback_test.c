#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/driver_catalog.h"
#include "services/history_service.h"
#include "services/sensor_manager.h"
#include "psram_allocator.h"
#include "FreeRTOS.h"

#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

static device_profile_config_t g_profile;
static uint32_t g_revision = 1u;
static uint8_t g_hardware_source;
static uint32_t g_fail_start_mask;
static jmp_buf g_delay_exit;

bool psram_allocator_is_ready(void) { return true; }
void *psram_calloc(size_t count, size_t size) { return calloc(count, size); }

TickType_t xTaskGetTickCount(void) { return 100u; }
void vTaskDelayUntil(TickType_t *last_wake_time, TickType_t period)
{
    (void)last_wake_time;
    (void)period;
    longjmp(g_delay_exit, 1);
}

const device_profile_config_t *config_service_get(void) { return &g_profile; }
uint32_t config_service_get_running_revision(void) { return g_revision; }
void config_service_set_runtime_apply_callback(config_service_runtime_apply_fn callback)
{
    (void)callback;
}
bool history_service_record_channel_sample(const char *id, telemetry_channel_value_type_t value_type,
                                           const history_service_value_t *value, service_quality_t quality,
                                           uint64_t timestamp, uint32_t interval)
{
    (void)id; (void)value_type; (void)value; (void)quality; (void)timestamp; (void)interval;
    return true;
}
bool time_service_get_utc_time_us(uint64_t *out_timestamp)
{
    *out_timestamp = 1700000000000000ULL;
    return true;
}

static uint8_t source_of(const telemetry_channel_t *channel)
{
    return ((const uint8_t *)channel->driver_context)[0];
}

static telemetry_driver_result_t fake_start(telemetry_channel_t *channel)
{
    const uint8_t source = source_of(channel);
    if ((g_fail_start_mask & (1u << source)) != 0u)
    {
        return TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE;
    }
    g_hardware_source = source;
    return TELEMETRY_DRIVER_RESULT_OK;
}
static telemetry_driver_result_t fake_stop(telemetry_channel_t *channel)
{
    (void)channel;
    g_hardware_source = 0u;
    return TELEMETRY_DRIVER_RESULT_OK;
}
static telemetry_driver_result_t fake_read(telemetry_channel_t *channel, uint32_t now)
{
    (void)now;
    channel->runtime.value.f32 = (float)g_hardware_source;
    return TELEMETRY_DRIVER_RESULT_OK;
}
static telemetry_driver_result_t fake_configure(telemetry_channel_t *channel, const void *config)
{
    (void)channel; (void)config;
    return TELEMETRY_DRIVER_RESULT_OK;
}
static telemetry_driver_result_t fake_self_test(telemetry_channel_t *channel)
{
    (void)channel;
    return TELEMETRY_DRIVER_RESULT_OK;
}
static const telemetry_driver_vtbl_t g_driver = {
    .start = fake_start, .stop = fake_stop, .read = fake_read,
    .configure = fake_configure, .self_test = fake_self_test,
};
static bool fake_validate_source(uint8_t source, uint8_t flags)
{
    return flags == 0u && source > 0u && source <= 3u;
}
static const driver_catalog_entry_t g_entry = {
    .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
    .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT),
    .validate_source = fake_validate_source,
    .driver = &g_driver,
};
const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t id)
{
    return id == g_entry.driver_id ? &g_entry : NULL;
}
bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry,
                                        telemetry_channel_value_type_t type)
{
    return entry == &g_entry && type == TELEMETRY_CHANNEL_VALUE_FLOAT;
}
bool driver_catalog_get_resource_claims(uint8_t id, uint8_t source, uint8_t flags,
                                        driver_catalog_resource_claim_t *claims, size_t capacity,
                                        size_t *out_count)
{
    if (id != g_entry.driver_id || flags != 0u || source == 0u || source > 3u ||
        claims == NULL || capacity == 0u || out_count == NULL)
    {
        return false;
    }
    claims[0] = (driver_catalog_resource_claim_t){
        .type = DRIVER_CATALOG_RESOURCE_GPIO, .index = source, .shared = false};
    *out_count = 1u;
    return true;
}
bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims, size_t count)
{
    (void)claims;
    return count <= 2u;
}

static void set_channel(size_t index, const char *id, uint8_t source)
{
    config_channel_definition_t *channel = &g_profile.channels[index];
    *channel = (config_channel_definition_t){0};
    channel->in_use = 1u;
    channel->driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP;
    channel->source_index = source;
    channel->value_type = TELEMETRY_CHANNEL_VALUE_FLOAT;
    channel->flags = TELEMETRY_CHANNEL_FLAG_ENABLED;
    channel->settings.enabled = true;
    strcpy(channel->id, id);
    strcpy(channel->name, id);
}

int main(void)
{
    telemetry_channel_snapshot_t snapshot = {0};

    assert(channel_manager_init());
    set_channel(0u, "channel.temperature.1", 1u);
    assert(sensor_manager_init());
    assert(g_hardware_source == 1u);
    assert(channel_manager_copy_snapshot_by_id("channel.temperature.1", &snapshot));

    memset(&g_profile, 0, sizeof(g_profile));
    set_channel(0u, "candidate-a", 2u); /* mutates the same fake peripheral */
    set_channel(1u, "candidate-b", 3u); /* later lifecycle failure */
    g_fail_start_mask = 1u << 3u;
    ++g_revision;
    if (setjmp(g_delay_exit) == 0)
    {
        sensor_manager_task(NULL);
    }

    assert(g_hardware_source == 1u);
    assert(channel_manager_count() == 1u);
    assert(channel_manager_copy_snapshot_by_id("channel.temperature.1", &snapshot));
    assert(source_of(&snapshot.channel) == 1u);

    /* A double fault cannot be reported as an active known-good runtime: the
     * candidate fails and restoration of the old driver fails too. */
    g_fail_start_mask = (1u << 3u) | (1u << 1u);
    if (setjmp(g_delay_exit) == 0)
    {
        sensor_manager_task(NULL);
    }
    sensor_manager_snapshot_t compatibility = {0};
    sensor_manager_get_snapshot(&compatibility);
    assert(!compatibility.enabled);
    assert(compatibility.quality == SERVICE_QUALITY_BAD);
    return 0;
}

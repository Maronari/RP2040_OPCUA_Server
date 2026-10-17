#include "services/benchmark_profile.h"
#include "services/config_compat.h"
#include "services/config_store_service.h"
#include "services/config_validate_service.h"
#include "services/driver_catalog.h"
#include "pico/unique_id.h"

#include <assert.h>
#include <string.h>

static unsigned g_flash_calls;

static bool synthetic_source_is_valid(uint8_t source_index, uint8_t source_flags)
{
    return source_index < BENCHMARK_PROFILE_SYNTHETIC_SOURCE_COUNT && source_flags == 0u;
}

static const driver_catalog_entry_t g_synthetic_entry = {
    .driver_id = CONFIG_CHANNEL_DRIVER_SYNTHETIC,
    .default_kind = TELEMETRY_CHANNEL_KIND_ANALOG,
    .default_value_type = TELEMETRY_CHANNEL_VALUE_DOUBLE,
    .default_flags = TELEMETRY_CHANNEL_FLAG_ENABLED,
    .analog = true,
    .validate_source = synthetic_source_is_valid,
};

const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id)
{
    return driver_id == CONFIG_CHANNEL_DRIVER_SYNTHETIC ? &g_synthetic_entry : NULL;
}

const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name)
{
    (void)name;
    return NULL;
}

size_t driver_catalog_count(void) { return 1u; }
const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index)
{
    return index == 0u ? &g_synthetic_entry : NULL;
}

config_validate_service_result_t config_validate_service_validate_candidate(
    const device_profile_config_t *candidate, char *message, size_t message_size)
{
    (void)message;
    (void)message_size;
    return candidate == NULL ? CONFIG_VALIDATE_RESULT_INTERNAL_ERROR : CONFIG_VALIDATE_RESULT_OK;
}

bool config_compat_decode_versioned(uint32_t schema_version, const uint8_t *payload,
                                    size_t payload_size, device_profile_config_t *out_config,
                                    bool *out_migrated)
{
    (void)schema_version;
    (void)payload;
    (void)payload_size;
    (void)out_config;
    (void)out_migrated;
    ++g_flash_calls;
    return false;
}

bool config_store_load_active(void *buffer, size_t buffer_size, size_t *out_payload_size,
                              config_store_record_info_t *out_info)
{
    (void)buffer;
    (void)buffer_size;
    (void)out_payload_size;
    (void)out_info;
    ++g_flash_calls;
    return false;
}

bool config_store_get_active_info(config_store_record_info_t *out_info)
{
    (void)out_info;
    ++g_flash_calls;
    return false;
}

bool config_store_save_payload(const void *payload, size_t payload_size,
                               uint16_t payload_format, uint32_t schema_version,
                               config_store_record_info_t *out_info)
{
    (void)payload;
    (void)payload_size;
    (void)payload_format;
    (void)schema_version;
    (void)out_info;
    ++g_flash_calls;
    return true;
}

bool config_store_reset(void) { ++g_flash_calls; return true; }
bool config_store_stage_payload(const void *payload, size_t payload_size,
                                uint16_t payload_format, uint32_t schema_version,
                                config_store_record_info_t *out_info)
{
    (void)payload;
    (void)payload_size;
    (void)payload_format;
    (void)schema_version;
    (void)out_info;
    ++g_flash_calls;
    return true;
}
bool config_store_commit_staged(config_store_record_info_t *out_info)
{ (void)out_info; ++g_flash_calls; return true; }
config_transaction_recovery_state_t config_store_get_recovery_state(void)
{ ++g_flash_calls; return CONFIG_TXN_RECOVERY_OK; }
bool config_store_discard_staged(void) { ++g_flash_calls; return true; }
bool config_store_recover_fallback_to_last_known_good(config_store_record_info_t *out_info)
{ (void)out_info; ++g_flash_calls; return true; }

bool alarm_service_clear_channel(const char *channel_id)
{ (void)channel_id; return true; }
bool history_service_clear_channel(const char *channel_id)
{ (void)channel_id; return true; }
void pico_get_unique_board_id(pico_unique_board_id_t *id_out)
{ memset(id_out, 0, sizeof(*id_out)); }

int main(void)
{
    const device_profile_config_t defaults = {
        .fw_revision = "0.5.2-test",
        .build_time = "2026-09-26T00:00:00Z",
        .git_hash = "benchmark-test",
        .opcua_port = 4840u,
    };
    const device_profile_config_t *running;
    uint32_t running_revision_before;
    config_channel_definition_t channel = {0};

    assert(config_service_init(&defaults));
    assert(g_flash_calls == 0u);
    running = config_service_get();
    assert(benchmark_profile_matches_config(running));
    assert(strcmp(running->fw_revision, defaults.fw_revision) == 0);
    assert(config_service_get_running_revision() == 1u);
    assert(config_service_get_staged_revision() == 1u);

    running_revision_before = config_service_get_running_revision();
    assert(!config_service_reset_to_defaults());
    assert(!config_service_apply());
    assert(config_service_rollback_running_to_last_known_good() ==
           CONFIG_SERVICE_ROLLBACK_UNAVAILABLE);
    assert(!config_service_set_timezone_offset_minutes(60));
    assert(!config_service_set_asset_id("changed"));
    assert(!config_service_stage_set_mqtt_pubsub_enabled(true));
    assert(!config_service_stage_set_location_paths("site", "site/line"));
    assert(!config_service_stage_set_identity(&(device_identity_config_t){0}));
    assert(!config_service_stage_set_network(&(device_network_config_t){.opcua_port = 4840u}));
    assert(!config_service_stage_set_channel(0u, &channel));
    assert(!config_service_stage_clear_channel(0u));
    assert(!config_service_replace_and_save(running, true));
    assert(!config_service_save());
    assert(g_flash_calls == 0u);
    assert(config_service_get_running_revision() == running_revision_before);
    assert(benchmark_profile_matches_config(config_service_get()));
    assert(benchmark_profile_matches_config(config_service_get_staged()));
    return 0;
}

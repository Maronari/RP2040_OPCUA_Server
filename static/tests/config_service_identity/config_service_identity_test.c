#include "services/config_service.h"
#include "services/config_store_service.h"
#include "services/config_wire.h"
#include "services/config_validate_service.h"
#include "services/driver_catalog.h"
#include "drivers/config_transaction_backend.h"
#include "drivers/flash_storage_driver.h"
#include "pico/unique_id.h"

#include <assert.h>
#include <string.h>

static device_profile_config_t g_persisted_config;
static bool g_have_persisted_config;
static unsigned g_save_calls;

config_validate_service_result_t config_validate_service_validate_candidate(
    const device_profile_config_t *candidate,
    char *message,
    size_t message_size)
{
    (void)message;
    (void)message_size;
    return candidate != NULL ? CONFIG_VALIDATE_RESULT_OK :
                               CONFIG_VALIDATE_RESULT_INTERNAL_ERROR;
}

bool config_store_load_active(void *buffer,
                              size_t buffer_size,
                              size_t *out_payload_size,
                              config_store_record_info_t *out_info)
{
    if (!g_have_persisted_config || buffer == NULL ||
        buffer_size < sizeof(g_persisted_config) || out_payload_size == NULL ||
        out_info == NULL)
    {
        return false;
    }

    memcpy(buffer, &g_persisted_config, sizeof(g_persisted_config));
    *out_payload_size = sizeof(g_persisted_config);
    *out_info = (config_store_record_info_t){
        .payload_format = CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
        .schema_version = CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
        .generation = 7u,
    };
    return true;
}

bool config_store_save_payload(const void *payload,
                               size_t payload_size,
                               uint16_t payload_format,
                               uint32_t schema_version,
                               config_store_record_info_t *out_info)
{
    (void)payload;
    (void)payload_size;
    (void)payload_format;
    (void)schema_version;
    (void)out_info;
    g_save_calls++;
    return true;
}

bool config_store_get_active_info(config_store_record_info_t *out_info)
{
    (void)out_info;
    return false;
}

bool config_store_reset(void)
{
    return false;
}

/* v0.5.0 T4: config_service_init() consults the recovery classification and
 * discards any leftover staged record on every boot -- this test does not
 * exercise a power-loss scenario itself (see static/tests/config_transaction/
 * for that), so a fixed CONFIG_TXN_RECOVERY_OK / no-op discard is enough to
 * satisfy the call without changing this test's own persisted-identity
 * assertions. */
config_transaction_recovery_state_t config_store_get_recovery_state(void)
{
    return CONFIG_TXN_RECOVERY_OK;
}

bool config_store_discard_staged(void)
{
    return true;
}

bool config_store_recover_fallback_to_last_known_good(config_store_record_info_t *out_info)
{
    (void)out_info;
    return true;
}

/* v0.5.0 T4: config_service_apply()/config_service_replace_and_save() now
 * drive the two-phase stage/commit primitive directly; this test never
 * calls either (it only exercises config_service_init()), but both symbols
 * are still referenced by config_service.c and must resolve at link time. */
bool config_store_stage_payload(const void *payload,
                                size_t payload_size,
                                uint16_t payload_format,
                                uint32_t schema_version,
                                config_store_record_info_t *out_info)
{
    (void)payload;
    (void)payload_size;
    (void)payload_format;
    (void)schema_version;
    (void)out_info;
    return true;
}

bool config_store_commit_staged(config_store_record_info_t *out_info)
{
    (void)out_info;
    return true;
}

bool flash_storage_driver_load(void *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return false;
}

bool flash_storage_driver_load_partial(void *buffer,
                                       size_t buffer_size,
                                       size_t *out_payload_size)
{
    (void)buffer;
    (void)buffer_size;
    (void)out_payload_size;
    return false;
}

const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id)
{
    (void)driver_id;
    return NULL;
}

const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name)
{
    (void)name;
    return NULL;
}

size_t driver_catalog_count(void)
{
    return 0u;
}

const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index)
{
    (void)index;
    return NULL;
}

/* v0.5.0 T7: config_service_run_commit_extensions() references these, but
 * this test only exercises config_service_init() (never apply()/
 * replace_and_save()), so they are never actually invoked -- present solely
 * to satisfy the link, like config_store_stage_payload()/commit_staged()
 * above. */
bool history_service_clear_channel(const char *channel_id)
{
    (void)channel_id;
    return false;
}

bool alarm_service_clear_channel(const char *channel_id)
{
    (void)channel_id;
    return false;
}

void pico_get_unique_board_id(pico_unique_board_id_t *id_out)
{
    memset(id_out, 0, sizeof(*id_out));
}

int main(void)
{
    const device_profile_config_t compiled_profile = {
        .fw_revision = "0.3.3",
        .build_time = "2026-07-30T12:00:00Z",
        .git_hash = "3d456948abcd",
        .opcua_port = 4840u,
    };
    const device_profile_config_t *running;
    const device_profile_config_t *staged;

    g_persisted_config = (device_profile_config_t){
        .manufacturer = "Persisted manufacturer",
        .serial_number = "persisted-serial",
        .fw_revision = "0.3.0",
        .build_time = "2025-01-01T00:00:00Z",
        .git_hash = "old-build",
        .device_name = "persisted-device",
        .asset_id = "persisted-asset",
        .opcua_port = 51234u,
        .network_mode = NETWORK_MODE_STATIC,
        .static_ip = {192u, 0u, 2u, 44u},
    };
    g_have_persisted_config = true;

    config_service_init(&compiled_profile);
    running = config_service_get();
    staged = config_service_get_staged();

    assert(strcmp(running->fw_revision, compiled_profile.fw_revision) == 0);
    assert(strcmp(running->build_time, compiled_profile.build_time) == 0);
    assert(strcmp(running->git_hash, compiled_profile.git_hash) == 0);
    assert(strcmp(staged->fw_revision, compiled_profile.fw_revision) == 0);
    assert(strcmp(staged->build_time, compiled_profile.build_time) == 0);
    assert(strcmp(staged->git_hash, compiled_profile.git_hash) == 0);

    assert(strcmp(running->manufacturer, g_persisted_config.manufacturer) == 0);
    assert(strcmp(running->serial_number, g_persisted_config.serial_number) == 0);
    assert(strcmp(running->device_name, g_persisted_config.device_name) == 0);
    assert(strcmp(running->asset_id, g_persisted_config.asset_id) == 0);
    assert(running->opcua_port == g_persisted_config.opcua_port);
    assert(running->network_mode == g_persisted_config.network_mode);
    assert(memcmp(running->static_ip,
                  g_persisted_config.static_ip,
                  sizeof(running->static_ip)) == 0);
    assert(g_save_calls == 0u);
    return 0;
}

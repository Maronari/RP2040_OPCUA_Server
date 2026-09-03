#include "services/config_service.h"
#include "services/config_store_service.h"
#include "services/config_wire.h"
#include "services/config_validate_service.h"
#include "services/driver_catalog.h"
#include "drivers/config_transaction_backend.h"
#include "pico/unique_id.h"

#include <assert.h>
#include <string.h>

/* Regresses two review findings about the LAST_KNOWN_GOOD rollback path:
 *
 * 1. A naive config_service_rollback_running_to_last_known_good() that only
 *    restores Flash RUNNING and config_service's own g_running_config/
 *    g_staged_config, without also re-invoking the registered runtime-apply
 *    callback, leaves whatever runtime_apply last activated (channel_manager's
 *    live registry, active drivers) still on the pre-rollback candidate --
 *    so a caller that rebuilds the OPC UA namespace afterwards would mix
 *    recovered device-level fields with never-reverted channel/driver
 *    runtime state. Scenarios 2/3 below prove the fix actually reactivates
 *    the recovered configuration through runtime_apply.
 * 2. On a genuinely fresh device, config_service_init() used to seed
 *    RUNNING from compiled defaults only in RAM, never persisting them --
 *    so config_transaction_backend_commit()'s promote-RUNNING-to-
 *    LAST_KNOWN_GOOD step had nothing to promote on the very first real
 *    config_service_apply() transaction, leaving LAST_KNOWN_GOOD empty
 *    through that first transaction. If it then failed downstream, rollback
 *    had nothing to recover to even though the compiled defaults were a
 *    perfectly good, recently-running configuration. Scenario 0 below
 *    proves config_service_init() now persists them.
 *
 * Also regresses the FLASH_CHANGED_INCOMPLETE/UNAVAILABLE distinction: only
 * a rollback that already wrote Flash before failing later should report
 * FLASH_CHANGED_INCOMPLETE (the caller must force a reboot to reconcile),
 * never UNAVAILABLE (nothing changed, no reboot warranted). */

static device_profile_config_t g_lkg_config;
static bool g_recover_fallback_succeeds;
static bool g_serve_lkg_payload;

static bool g_runtime_apply_should_succeed;
static unsigned g_runtime_apply_calls;
static device_profile_config_t g_last_runtime_apply_candidate;
static uint32_t g_last_runtime_apply_revision;

static unsigned g_save_payload_calls;
static device_profile_config_t g_last_saved_payload;

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
    if (!g_serve_lkg_payload || buffer == NULL ||
        buffer_size < sizeof(g_lkg_config) || out_payload_size == NULL ||
        out_info == NULL)
    {
        return false;
    }

    memcpy(buffer, &g_lkg_config, sizeof(g_lkg_config));
    *out_payload_size = sizeof(g_lkg_config);
    *out_info = (config_store_record_info_t){
        .payload_format = CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
        .schema_version = CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY,
        .generation = 42u,
    };
    return true;
}

bool config_store_save_payload(const void *payload,
                               size_t payload_size,
                               uint16_t payload_format,
                               uint32_t schema_version,
                               config_store_record_info_t *out_info)
{
    (void)payload_format;
    (void)schema_version;
    (void)out_info;
    g_save_payload_calls++;
    if (payload != NULL && payload_size == sizeof(g_last_saved_payload))
    {
        memcpy(&g_last_saved_payload, payload, sizeof(g_last_saved_payload));
    }
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
    return g_recover_fallback_succeeds;
}

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

static bool fake_runtime_apply(const device_profile_config_t *candidate, uint32_t candidate_revision)
{
    g_runtime_apply_calls++;
    g_last_runtime_apply_candidate = *candidate;
    g_last_runtime_apply_revision = candidate_revision;
    return g_runtime_apply_should_succeed;
}

int main(void)
{
    const device_profile_config_t compiled_profile = {
        .fw_revision = "0.5.0",
        .build_time = "2026-09-02T00:00:00Z",
        .git_hash = "test-build",
        .opcua_port = 4840u,
    };
    const device_profile_config_t *running;

    /* --- Scenario 0: cold boot on compiled defaults, no persisted record --
     * config_service_init() must now persist those defaults to Flash RUNNING
     * itself, so a first-ever real transaction has something to promote to
     * LAST_KNOWN_GOOD. --- */
    g_serve_lkg_payload = false;
    g_save_payload_calls = 0u;
    config_service_init(&compiled_profile);
    config_service_set_runtime_apply_callback(fake_runtime_apply);

    running = config_service_get();
    assert(strcmp(running->device_name, "") == 0 || running->device_name[0] == '\0');
    assert(g_save_payload_calls == 1u);
    assert(strcmp(g_last_saved_payload.fw_revision, compiled_profile.fw_revision) == 0);
    assert(g_last_saved_payload.opcua_port == compiled_profile.opcua_port);

    /* --- Scenario 1: LAST_KNOWN_GOOD unusable (e.g. first-ever transaction
     * on a device with no prior commit) -- rollback must fail cleanly,
     * report UNAVAILABLE (not FLASH_CHANGED_INCOMPLETE: nothing was
     * touched), and never call runtime_apply. --- */
    g_recover_fallback_succeeds = false;
    g_serve_lkg_payload = false;
    g_runtime_apply_calls = 0u;
    assert(config_service_rollback_running_to_last_known_good() ==
           CONFIG_SERVICE_ROLLBACK_UNAVAILABLE);
    assert(g_runtime_apply_calls == 0u);

    /* --- Scenario 2: LAST_KNOWN_GOOD usable and runtime_apply accepts the
     * recovered candidate -- rollback must report OK and reactivate runtime
     * state with the *recovered* configuration, not silently leave it on
     * whatever the pre-rollback candidate had activated. --- */
    g_lkg_config = compiled_profile;
    strncpy(g_lkg_config.device_name, "lkg-device", sizeof(g_lkg_config.device_name) - 1u);
    g_lkg_config.opcua_port = 51234u;
    g_recover_fallback_succeeds = true;
    g_serve_lkg_payload = true;
    g_runtime_apply_calls = 0u;
    g_runtime_apply_should_succeed = true;

    assert(config_service_rollback_running_to_last_known_good() ==
           CONFIG_SERVICE_ROLLBACK_OK);
    assert(g_runtime_apply_calls == 1u);
    assert(strcmp(g_last_runtime_apply_candidate.device_name, "lkg-device") == 0);
    assert(g_last_runtime_apply_candidate.opcua_port == 51234u);
    assert(g_last_runtime_apply_revision == 42u);

    running = config_service_get();
    assert(strcmp(running->device_name, "lkg-device") == 0);
    assert(running->opcua_port == 51234u);

    /* --- Scenario 3: LAST_KNOWN_GOOD usable but runtime_apply rejects the
     * recovered candidate (e.g. a driver that worked before no longer
     * initializes) -- rollback must report FLASH_CHANGED_INCOMPLETE (Flash
     * was already overwritten before this failure), not silently claim
     * success, and must not advance config_service's own running/staged
     * state to the rejected candidate. --- */
    g_lkg_config.opcua_port = 9999u;
    g_runtime_apply_calls = 0u;
    g_runtime_apply_should_succeed = false;

    assert(config_service_rollback_running_to_last_known_good() ==
           CONFIG_SERVICE_ROLLBACK_FLASH_CHANGED_INCOMPLETE);
    assert(g_runtime_apply_calls == 1u);

    /* config_service's own running/staged state must not have advanced to
     * the rejected candidate -- it still reflects scenario 2's result. */
    running = config_service_get();
    assert(strcmp(running->device_name, "lkg-device") == 0);
    assert(running->opcua_port == 51234u);

    return 0;
}

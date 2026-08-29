#include "services/channel_manager.h"
#include "services/config_service.h"
#include "services/config_store_service.h"
#include "services/config_validate_service.h"
#include "services/driver_catalog.h"
#include "services/history_service.h"
#include "services/sensor_manager.h"
#include "psram_allocator.h"
#include "drivers/native_channel_driver.h"
#include "drivers/config_transaction_backend.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "pico/unique_id.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_events[192];
static size_t g_event_length;
static uint8_t g_fail_source;
static char g_fail_step;
static bool g_fail_stage;
static bool g_fail_commit;
static config_transaction_recovery_state_t g_recovery_state = CONFIG_TXN_RECOVERY_OK;
static size_t g_discard_staged_calls;
static size_t g_recover_fallback_calls;

void *psram_calloc(size_t count, size_t size) { return calloc(count, size); }
bool psram_allocator_is_ready(void) { return true; }
TickType_t xTaskGetTickCount(void) { return 1u; }
void vTaskDelayUntil(TickType_t *last_wake_time, TickType_t period)
{ (void)last_wake_time; (void)period; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void pico_get_unique_board_id(pico_unique_board_id_t *id_out) { memset(id_out, 0, sizeof(*id_out)); }
bool time_service_get_utc_time_us(uint64_t *timestamp) { *timestamp = 1u; return true; }
bool history_service_record_channel_sample(const char *id, telemetry_channel_value_type_t value_type,
                                           const history_service_value_t *value, service_quality_t quality,
                                           uint64_t timestamp, uint32_t interval)
{ (void)id; (void)value_type; (void)value; (void)quality; (void)timestamp; (void)interval; return true; }

/* Single-threaded simulation of a FreeRTOS static mutex, mirroring the
 * pattern already used by static/tests/history_service/. sensor_manager.c
 * now takes g_activate_mutex around the whole of
 * sensor_manager_activate_config() (v0.5.0 T4). */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{ storage->locked = 0; return storage; }
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{ (void)ticks; if (semaphore->locked) { return pdFALSE; } semaphore->locked = 1; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t semaphore)
{ semaphore->locked = 0; return pdTRUE; }

static uint8_t source(const telemetry_channel_t *channel)
{ return ((const native_channel_source_t *)channel->driver_context)->source_index; }
static void event(char step, uint8_t value)
{ assert(g_event_length + 2u < sizeof(g_events)); g_events[g_event_length++] = step; g_events[g_event_length++] = (char)('0' + value); g_events[g_event_length] = '\0'; }
static telemetry_driver_result_t fake_start(telemetry_channel_t *channel)
{ event('S', source(channel)); return source(channel) == g_fail_source && g_fail_step == 'S' ? TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE : TELEMETRY_DRIVER_RESULT_OK; }
static telemetry_driver_result_t fake_stop(telemetry_channel_t *channel)
{ event('O', source(channel)); return source(channel) == g_fail_source && g_fail_step == 'O' ? TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE : TELEMETRY_DRIVER_RESULT_OK; }
static telemetry_driver_result_t fake_configure(telemetry_channel_t *channel, const void *config)
{ (void)config; event('C', source(channel)); return source(channel) == g_fail_source && g_fail_step == 'C' ? TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE : TELEMETRY_DRIVER_RESULT_OK; }
static telemetry_driver_result_t fake_self_test(telemetry_channel_t *channel)
{ event('T', source(channel)); return source(channel) == g_fail_source && g_fail_step == 'T' ? TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE : TELEMETRY_DRIVER_RESULT_OK; }
static telemetry_driver_result_t fake_read(telemetry_channel_t *channel, uint32_t now)
{ (void)now; event('R', source(channel)); return source(channel) == g_fail_source && g_fail_step == 'R' ? TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE : TELEMETRY_DRIVER_RESULT_OK; }
static bool fake_validate_source(uint8_t source_index, uint8_t source_flags)
{ return source_index > 0u && source_flags == 0u; }
static const telemetry_driver_vtbl_t g_driver = {
    .start = fake_start, .stop = fake_stop, .read = fake_read,
    .configure = fake_configure, .self_test = fake_self_test,
};
static const driver_catalog_entry_t g_entry = {
    .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
    .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT),
    .validate_source = fake_validate_source, .driver = &g_driver,
};
const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t id) { return id == g_entry.driver_id ? &g_entry : NULL; }
const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name) { (void)name; return NULL; }
size_t driver_catalog_count(void) { return 1u; }
const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index) { return index == 0u ? &g_entry : NULL; }
bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry, telemetry_channel_value_type_t type)
{ return entry == &g_entry && type == TELEMETRY_CHANNEL_VALUE_FLOAT; }
bool driver_catalog_get_resource_claims(uint8_t id, uint8_t source_index, uint8_t source_flags,
                                        driver_catalog_resource_claim_t *claims, size_t capacity, size_t *out_count)
{ (void)source_index; (void)source_flags; if (id != g_entry.driver_id || capacity == 0u || claims == NULL || out_count == NULL) return false; claims[0] = (driver_catalog_resource_claim_t){ DRIVER_CATALOG_RESOURCE_GPIO, 1u, true }; *out_count = 1u; return true; }
bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims, size_t count)
{ (void)claims; return count <= 1u; }

config_validate_service_result_t config_validate_service_validate_candidate(const device_profile_config_t *candidate, char *message, size_t message_size)
{ (void)message; (void)message_size; return candidate != NULL ? CONFIG_VALIDATE_RESULT_OK : CONFIG_VALIDATE_RESULT_INTERNAL_ERROR; }
bool config_store_load_active(void *buffer, size_t size, size_t *payload_size, config_store_record_info_t *info)
{ (void)buffer; (void)size; (void)payload_size; (void)info; return false; }
bool flash_storage_driver_load_partial(void *buffer, size_t size, size_t *payload_size)
{ (void)buffer; (void)size; (void)payload_size; return false; }
bool config_store_save_payload(const void *payload, size_t size, uint16_t format, uint32_t schema, config_store_record_info_t *info)
{ (void)payload; (void)size; (void)format; (void)schema; (void)info; return true; }

/* v0.5.0 T4: the two-phase primitive config_service.c's transaction
 * pipeline now drives directly, faked here at the same config_store_service.h
 * boundary the atomic wrapper above already was -- see
 * services/config_store_service.h's file-level doc comment. */
bool config_store_stage_payload(const void *payload, size_t size, uint16_t format, uint32_t schema, config_store_record_info_t *info)
{ (void)payload; (void)size; (void)format; (void)schema; (void)info; return !g_fail_stage; }
bool config_store_commit_staged(config_store_record_info_t *info)
{ (void)info; return !g_fail_commit; }
bool config_store_discard_staged(void)
{ ++g_discard_staged_calls; return true; }
config_transaction_recovery_state_t config_store_get_recovery_state(void)
{ return g_recovery_state; }
bool config_store_recover_fallback_to_last_known_good(config_store_record_info_t *info)
{ (void)info; ++g_recover_fallback_calls; return true; }

static device_profile_config_t profile(uint8_t source_index)
{
    device_profile_config_t value = { .opcua_port = 4840u, .channels = {{
        .in_use = 1u, .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
        .source_index = source_index, .value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .flags = TELEMETRY_CHANNEL_FLAG_ENABLED, .id = "channel.temperature.1",
        .name = "temperature", .settings = {
            .enabled = true, .sample_period_ms = 1u,
            /* Finite high-only migration sentinel: normalization must retain
             * the equal pair rather than clamping it to a live low alarm. */
            .low_threshold = DBL_MAX, .low_low_threshold = DBL_MAX,
        },
    }}};
    return value;
}

static void expect_event(const char *expected)
{
    if (strcmp(g_events, expected) != 0)
    {
        fprintf(stderr, "expected lifecycle %s, got %s\n", expected, g_events);
        assert(false);
    }
    g_event_length = 0u;
    g_events[0] = '\0';
}
static void stage_source(uint8_t source_index)
{ config_channel_definition_t channel; assert(config_service_stage_copy_channel(0u, &channel)); channel.source_index = source_index; assert(config_service_stage_set_channel(0u, &channel)); }

int main(void)
{
    const device_profile_config_t defaults = profile(1u);
    device_profile_config_t candidate;
    uint32_t old_revision;

    assert(config_service_init(&defaults));
    assert(config_service_get()->channels[0].settings.low_threshold == DBL_MAX);
    assert(config_service_get()->channels[0].settings.low_low_threshold == DBL_MAX);
    assert(channel_manager_init());
    assert(sensor_manager_init());
    /* Boot activation: no prior bank to diff against, so the sole channel is
     * always "new" and the v0.5.0 T4 verify gate runs (one read, R1). */
    expect_event("S1C1T1R1");

    for (const char *step = "SCT"; *step != '\0'; ++step)
    {
        stage_source(2u); old_revision = config_service_get_running_revision();
        g_fail_source = 2u; g_fail_step = *step;
        assert(!config_service_apply());
        assert(config_service_get_running_revision() == old_revision);
        assert(config_service_get()->channels[0].source_index == 1u);
        assert(config_service_get_staged()->channels[0].source_index == 2u);
        /* Failures inside start_candidates() never reach the verify gate. */
        expect_event(*step == 'S' ? "O1S2O2S1C1T1" :
                     *step == 'C' ? "O1S2C2O2S1C1T1" :
                                    "O1S2C2T2O2S1C1T1");
        g_fail_source = 0u;
    }

    assert(config_service_apply());
    expect_event("O1S2C2T2R2");
    assert(config_service_get()->channels[0].source_index == 2u);

    /* v0.5.0 T4 ordering fix, proven directly: a Flash stage failure must
     * abort the transaction before any driver is touched at all -- the
     * opposite of the pre-T4 activate-before-persist bug. */
    stage_source(4u); old_revision = config_service_get_running_revision();
    g_fail_stage = true;
    assert(!config_service_apply());
    assert(config_service_get_running_revision() == old_revision);
    assert(config_service_get()->channels[0].source_index == 2u);
    expect_event("");
    g_fail_stage = false;

    /* Driver stop() failure (the coverage gap T1/T9 identified: fake_stop
     * previously always returned OK). The old driver's stop() is attempted
     * and fails; activation must abort before starting any candidate driver
     * at all, restoring the old driver via sensor_manager_restart_active()
     * -- not proceeding to start_candidates(). */
    stage_source(6u); old_revision = config_service_get_running_revision();
    g_fail_source = 2u; g_fail_step = 'O';
    assert(!config_service_apply());
    assert(config_service_get_running_revision() == old_revision);
    assert(config_service_get()->channels[0].source_index == 2u);
    assert(config_service_get_staged()->channels[0].source_index == 6u);
    expect_event("O2S2C2T2");
    g_fail_source = 0u;

    /* Verify-gate (read) failure: driver init/start/configure/self_test all
     * succeed and the candidate is published to channel_manager, but every
     * read attempt within the bounded per-channel retry budget fails --
     * activation must roll back exactly like a registration failure
     * (restore the old registry, restart the old drivers), never commit. */
    stage_source(7u); old_revision = config_service_get_running_revision();
    g_fail_source = 7u; g_fail_step = 'R';
    assert(!config_service_apply());
    assert(config_service_get_running_revision() == old_revision);
    assert(config_service_get()->channels[0].source_index == 2u);
    assert(config_service_get_staged()->channels[0].source_index == 7u);
    expect_event("O2S7C7T7R7R7R7O7S2C2T2");
    g_fail_source = 0u;

    candidate = profile(3u); old_revision = config_service_get_running_revision();
    g_fail_commit = true;
    assert(!config_service_replace_and_save(&candidate, true));
    assert(config_service_get_running_revision() == old_revision);
    assert(config_service_get()->channels[0].source_index == 2u);
    /* A commit failure never touches the staged configuration (mirrors the
     * pre-T4 Flash-write-failure behavior) -- still whatever the preceding
     * verify-failure scenario staged (7), not this call's own candidate. */
    assert(config_service_get_staged()->channels[0].source_index == 7u);
    expect_event("O2S3C3T3R3O3S2C2T2R2");
    g_fail_commit = false;

    /* v0.5.0 T4(d) reboot recovery: config_service_init() must consult
     * config_transaction_backend's recovery classification (via
     * config_store_get_recovery_state()) and react per the mapping
     * documented next to that call site in config_service.c. */
    {
        const device_profile_config_t recovery_defaults = profile(9u);
        size_t discard_before;
        size_t fallback_before;

        /* CONFIG_TXN_RECOVERY_OK (and, by the same code path, _EMPTY/
         * _DEGRADED): normal boot; a leftover staged record is discarded as
         * hygiene, no last-known-good fallback is attempted. */
        g_recovery_state = CONFIG_TXN_RECOVERY_OK;
        discard_before = g_discard_staged_calls;
        fallback_before = g_recover_fallback_calls;
        assert(config_service_init(&recovery_defaults));
        assert(g_discard_staged_calls == discard_before + 1u);
        assert(g_recover_fallback_calls == fallback_before);

        /* CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED: RUNNING unusable but
         * LAST_KNOWN_GOOD committed -- must self-heal RUNNING from it. */
        g_recovery_state = CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED;
        discard_before = g_discard_staged_calls;
        fallback_before = g_recover_fallback_calls;
        assert(config_service_init(&recovery_defaults));
        assert(g_discard_staged_calls == discard_before + 1u);
        assert(g_recover_fallback_calls == fallback_before + 1u);

        /* CONFIG_TXN_RECOVERY_FATAL (and, by the same code path,
         * _INDETERMINATE): nothing usable / mutations blocked -- no
         * fallback is attempted (nothing usable to fall back to); boot
         * still succeeds, falling through to compiled defaults exactly as
         * when no persisted record exists at all. */
        g_recovery_state = CONFIG_TXN_RECOVERY_FATAL;
        discard_before = g_discard_staged_calls;
        fallback_before = g_recover_fallback_calls;
        assert(config_service_init(&recovery_defaults));
        assert(g_discard_staged_calls == discard_before + 1u);
        assert(g_recover_fallback_calls == fallback_before);
        assert(config_service_get()->channels[0].source_index == 9u);
    }
    return 0;
}

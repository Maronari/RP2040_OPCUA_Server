#include "services/alarm_service.h"
#include "services/channel_manager.h"
#include "services/config_presets.h"
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
/* v0.5.0 T9: proves the activate-phase-failure -> Flash-untouched claim
 * T4's own doc comment calls out (config_service.c's commit-step doc
 * comment) -- config_store_commit_staged() (the only call that ever touches
 * RUNNING/LAST_KNOWN_GOOD) must never be reached when stop/start/configure/
 * self_test/verify fails. */
static size_t g_commit_staged_calls;

/* v0.5.0 T6: config_service_run_commit_extensions() must invoke the
 * registered restart callback exactly once per successful commit -- never on
 * a rolled-back/failed transaction -- passing the just-committed and
 * previous configs. The real restart decision (model_config_requires_server_reboot())
 * and the actual UA_Server teardown/rebuild live in opcua_model, which this
 * host suite does not link (no open62541); this fake only proves the
 * config_service-side wiring: registered/invoked correctly, with the right
 * arguments, on commit only. */
static size_t g_restart_calls;
static device_profile_config_t g_restart_last_new_config;
static device_profile_config_t g_restart_last_previous_config;

/* v0.5.0 T7: config_service_run_commit_extensions() must call
 * history_service_clear_channel()/alarm_service_clear_channel() for every
 * deleted-or-modified channel strictly before it invokes the restart
 * callback above (see the ordering rule in config_service.c's doc comment).
 * g_extension_sequence is a shared call-order counter stamped by every one
 * of these three fakes; comparing the stamps a test captured for the same
 * commit proves the relative order without relying on wall-clock/thread
 * timing. */
static unsigned g_extension_sequence;
static unsigned g_restart_seq;
#define TEST_MAX_CLEAR_RECORDS 8u
static char g_history_clear_ids[TEST_MAX_CLEAR_RECORDS][CONFIG_SERVICE_CHANNEL_ID_MAX_LEN];
static unsigned g_history_clear_seq[TEST_MAX_CLEAR_RECORDS];
static size_t g_history_clear_calls;
static char g_alarm_clear_ids[TEST_MAX_CLEAR_RECORDS][CONFIG_SERVICE_CHANNEL_ID_MAX_LEN];
static unsigned g_alarm_clear_seq[TEST_MAX_CLEAR_RECORDS];
static size_t g_alarm_clear_calls;

static void fake_restart_callback(const device_profile_config_t *new_config,
                                  const device_profile_config_t *previous_config)
{
    ++g_restart_calls;
    g_restart_seq = ++g_extension_sequence;
    if (new_config != NULL) { g_restart_last_new_config = *new_config; }
    if (previous_config != NULL) { g_restart_last_previous_config = *previous_config; }
}

bool history_service_clear_channel(const char *channel_id)
{
    assert(g_history_clear_calls < TEST_MAX_CLEAR_RECORDS);
    (void)snprintf(g_history_clear_ids[g_history_clear_calls], sizeof(g_history_clear_ids[0]), "%s",
                   channel_id != NULL ? channel_id : "");
    g_history_clear_seq[g_history_clear_calls] = ++g_extension_sequence;
    ++g_history_clear_calls;
    return true;
}

bool alarm_service_clear_channel(const char *channel_id)
{
    assert(g_alarm_clear_calls < TEST_MAX_CLEAR_RECORDS);
    (void)snprintf(g_alarm_clear_ids[g_alarm_clear_calls], sizeof(g_alarm_clear_ids[0]), "%s",
                   channel_id != NULL ? channel_id : "");
    g_alarm_clear_seq[g_alarm_clear_calls] = ++g_extension_sequence;
    ++g_alarm_clear_calls;
    return true;
}

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
/* v0.5.0 T8: source_index == 0 must validate too -- the real
 * CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP/CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE
 * source validators both accept index 0 (see
 * drivers/src/internal_temperature_channel_driver.c and
 * drivers/src/adc_voltage_channel_driver.c), and the "sensor_node" preset
 * (services/config_presets.c) uses index 0 for both. No existing test
 * above relies on index 0 being rejected. */
static bool fake_validate_source(uint8_t source_index, uint8_t source_flags)
{ (void)source_index; return source_flags == 0u; }
static const telemetry_driver_vtbl_t g_driver = {
    .start = fake_start, .stop = fake_stop, .read = fake_read,
    .configure = fake_configure, .self_test = fake_self_test,
};
static const driver_catalog_entry_t g_entry = {
    .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
    .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT),
    .validate_source = fake_validate_source, .driver = &g_driver,
};
/* v0.5.0 T7: a second, distinct driver id so tests can exercise a "modified
 * channel" (same channel id, different driver_id) without also having to
 * change value_type -- reuses the same fake vtbl, since events are tracked
 * by source_index, not driver_id. */
static const driver_catalog_entry_t g_entry_b = {
    .driver_id = CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE,
    .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_FLOAT),
    .validate_source = fake_validate_source, .driver = &g_driver,
};
/* v0.5.0 T8: a third catalog entry (BOOLEAN-typed, like the real
 * CONFIG_CHANNEL_DRIVER_GPIO_INPUT) so the "sensor_node" preset's three
 * channels can all activate through this fixture. */
static const driver_catalog_entry_t g_entry_c = {
    .driver_id = CONFIG_CHANNEL_DRIVER_GPIO_INPUT,
    .supported_value_types = TELEMETRY_CHANNEL_VALUE_TYPE_BIT(TELEMETRY_CHANNEL_VALUE_BOOLEAN),
    .validate_source = fake_validate_source, .driver = &g_driver,
};
const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t id)
{
    if (id == g_entry.driver_id) return &g_entry;
    if (id == g_entry_b.driver_id) return &g_entry_b;
    if (id == g_entry_c.driver_id) return &g_entry_c;
    return NULL;
}
const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name) { (void)name; return NULL; }
size_t driver_catalog_count(void) { return 3u; }
const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index)
{ return index == 0u ? &g_entry : index == 1u ? &g_entry_b : index == 2u ? &g_entry_c : NULL; }
bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry, telemetry_channel_value_type_t type)
{ return (entry == &g_entry && type == TELEMETRY_CHANNEL_VALUE_FLOAT) ||
         (entry == &g_entry_b && type == TELEMETRY_CHANNEL_VALUE_FLOAT) ||
         (entry == &g_entry_c && type == TELEMETRY_CHANNEL_VALUE_BOOLEAN); }
bool driver_catalog_get_resource_claims(uint8_t id, uint8_t source_index, uint8_t source_flags,
                                        driver_catalog_resource_claim_t *claims, size_t capacity, size_t *out_count)
{ (void)source_flags; if ((id != g_entry.driver_id && id != g_entry_b.driver_id && id != g_entry_c.driver_id) ||
      capacity == 0u || claims == NULL || out_count == NULL) return false;
  /* Keyed by source_index so two simultaneously-active channels (distinct
   * source_index by construction, see fake_validate_source) never appear to
   * conflict over the same fake GPIO resource. */
  claims[0] = (driver_catalog_resource_claim_t){ DRIVER_CATALOG_RESOURCE_GPIO, source_index, true }; *out_count = 1u; return true; }
/* v0.5.0 T8: raised from the original 8 to PLATFORM_CHANNEL_LIMIT so the
 * "remote_io" (14 channels) and "dev_benchmark" (16 channels, filling
 * PLATFORM_CHANNEL_LIMIT exactly) preset-apply scenarios below don't trip
 * this arbitrary sanity ceiling -- no existing scenario above ever built a
 * candidate with more than two channels, so nothing here relied on the old
 * cap actually rejecting anything. */
bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims, size_t count)
{ (void)claims; return count <= (size_t)PLATFORM_CHANNEL_LIMIT; }

config_validate_service_result_t config_validate_service_validate_candidate(const device_profile_config_t *candidate, char *message, size_t message_size)
{ (void)message; (void)message_size; return candidate != NULL ? CONFIG_VALIDATE_RESULT_OK : CONFIG_VALIDATE_RESULT_INTERNAL_ERROR; }
bool config_store_load_active(void *buffer, size_t size, size_t *payload_size, config_store_record_info_t *info)
{ (void)buffer; (void)size; (void)payload_size; (void)info; return false; }
bool config_store_save_payload(const void *payload, size_t size, uint16_t format, uint32_t schema, config_store_record_info_t *info)
{ (void)payload; (void)size; (void)format; (void)schema; (void)info; return true; }

/* v0.5.0 T4: the two-phase primitive config_service.c's transaction
 * pipeline now drives directly, faked here at the same config_store_service.h
 * boundary the atomic wrapper above already was -- see
 * services/config_store_service.h's file-level doc comment. */
bool config_store_stage_payload(const void *payload, size_t size, uint16_t format, uint32_t schema, config_store_record_info_t *info)
{ (void)payload; (void)size; (void)format; (void)schema; (void)info; return !g_fail_stage; }
bool config_store_commit_staged(config_store_record_info_t *info)
{ (void)info; ++g_commit_staged_calls; return !g_fail_commit; }
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

/* v0.5.0 T7 helpers: build one channel definition / a one- or two-channel
 * profile with an explicit id/driver_id, so tests can construct a
 * delete/modify/untouched scenario across a commit. */
static config_channel_definition_t channel_def(uint8_t source_index, const char *channel_id, uint8_t driver_id,
                                                const char *name)
{
    config_channel_definition_t channel = {
        .in_use = 1u, .driver_id = driver_id,
        .source_index = source_index, .value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .flags = TELEMETRY_CHANNEL_FLAG_ENABLED,
        .settings = {
            .enabled = true, .sample_period_ms = 1u,
            .low_threshold = DBL_MAX, .low_low_threshold = DBL_MAX,
        },
    };
    (void)snprintf(channel.id, sizeof(channel.id), "%s", channel_id);
    (void)snprintf(channel.name, sizeof(channel.name), "%s", name);
    return channel;
}
static device_profile_config_t profile_one(config_channel_definition_t a)
{
    device_profile_config_t value = { .opcua_port = 4840u };
    value.channels[0] = a;
    return value;
}
static device_profile_config_t profile_two(config_channel_definition_t a, config_channel_definition_t b)
{
    device_profile_config_t value = { .opcua_port = 4840u };
    value.channels[0] = a;
    value.channels[1] = b;
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
    config_service_set_restart_callback(fake_restart_callback);
    /* Boot activation: no prior bank to diff against, so the sole channel is
     * always "new" and the v0.5.0 T4 verify gate runs (one read, R1). */
    expect_event("S1C1T1R1");
    /* Boot activation runs sensor_manager_activate_running_config() directly,
     * never through config_service_apply()/config_service_run_commit_extensions()
     * -- the restart callback must not fire for it. */
    assert(g_restart_calls == 0u);

    for (const char *step = "SCT"; *step != '\0'; ++step)
    {
        size_t commit_before = g_commit_staged_calls;
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
        /* v0.5.0 T6: a rolled-back transaction never reaches the commit
         * step, so the restart callback must not fire. */
        assert(g_restart_calls == 0u);
        /* v0.5.0 T9 point 6: an activate-phase failure must never reach
         * config_store_commit_staged() -- the only call that ever touches
         * RUNNING/LAST_KNOWN_GOOD -- so Flash state is provably untouched. */
        assert(g_commit_staged_calls == commit_before);
        g_fail_source = 0u;
    }

    {
        size_t commit_before = g_commit_staged_calls;
        assert(config_service_apply());
        assert(g_commit_staged_calls == commit_before + 1u);
    }
    expect_event("O1S2C2T2R2");
    assert(config_service_get()->channels[0].source_index == 2u);
    /* v0.5.0 T6: the first successful commit must invoke the restart
     * callback exactly once, with the just-committed and previous configs. */
    assert(g_restart_calls == 1u);
    assert(g_restart_last_new_config.channels[0].source_index == 2u);
    assert(g_restart_last_previous_config.channels[0].source_index == 1u);

    /* v0.5.0 T4 ordering fix, proven directly: a Flash stage failure must
     * abort the transaction before any driver is touched at all -- the
     * opposite of the pre-T4 activate-before-persist bug. */
    stage_source(4u); old_revision = config_service_get_running_revision();
    g_fail_stage = true;
    {
        size_t commit_before = g_commit_staged_calls;
        assert(!config_service_apply());
        assert(g_commit_staged_calls == commit_before);
    }
    assert(config_service_get_running_revision() == old_revision);
    assert(config_service_get()->channels[0].source_index == 2u);
    expect_event("");
    assert(g_restart_calls == 1u);
    g_fail_stage = false;

    /* Driver stop() failure (the coverage gap T1/T9 identified: fake_stop
     * previously always returned OK). The old driver's stop() is attempted
     * and fails; activation must abort before starting any candidate driver
     * at all, restoring the old driver via sensor_manager_restart_active()
     * -- not proceeding to start_candidates(). */
    stage_source(6u); old_revision = config_service_get_running_revision();
    g_fail_source = 2u; g_fail_step = 'O';
    {
        size_t commit_before = g_commit_staged_calls;
        assert(!config_service_apply());
        /* v0.5.0 T9 point 6: staging (Flash) already succeeded by this
         * point -- proving the activate-phase failure alone, not merely a
         * persist failure, still never reaches commit_staged(). */
        assert(g_commit_staged_calls == commit_before);
    }
    assert(config_service_get_running_revision() == old_revision);
    assert(config_service_get()->channels[0].source_index == 2u);
    assert(config_service_get_staged()->channels[0].source_index == 6u);
    expect_event("O2S2C2T2");
    assert(g_restart_calls == 1u);
    g_fail_source = 0u;

    /* Verify-gate (read) failure: driver init/start/configure/self_test all
     * succeed and the candidate is published to channel_manager, but every
     * read attempt within the bounded per-channel retry budget fails --
     * activation must roll back exactly like a registration failure
     * (restore the old registry, restart the old drivers), never commit. */
    stage_source(7u); old_revision = config_service_get_running_revision();
    g_fail_source = 7u; g_fail_step = 'R';
    {
        size_t commit_before = g_commit_staged_calls;
        assert(!config_service_apply());
        assert(g_commit_staged_calls == commit_before);
    }
    assert(config_service_get_running_revision() == old_revision);
    assert(config_service_get()->channels[0].source_index == 2u);
    assert(config_service_get_staged()->channels[0].source_index == 7u);
    expect_event("O2S7C7T7R7R7R7O7S2C2T2");
    assert(g_restart_calls == 1u);
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
    assert(g_restart_calls == 1u);
    g_fail_commit = false;

    /* v0.5.0 T6: config_service_replace_and_save()'s commit path must also
     * invoke the registered restart callback exactly like
     * config_service_apply() does -- both call
     * config_service_run_commit_extensions() from the same place. */
    candidate = profile(5u);
    assert(config_service_replace_and_save(&candidate, true));
    expect_event("O2S5C5T5R5");
    assert(config_service_get()->channels[0].source_index == 5u);
    assert(g_restart_calls == 2u);
    assert(g_restart_last_new_config.channels[0].source_index == 5u);
    assert(g_restart_last_previous_config.channels[0].source_index == 2u);

    /* v0.5.0 T7: per-channel history/alarm cleanup on delete/modify, wired
     * into config_service_run_commit_extensions() strictly before the
     * restart callback. Every commit above only ever changed source_index
     * on the same id/driver_id/value_type ("channel.temperature.1"), so
     * neither clear function should have fired yet. */
    assert(g_history_clear_calls == 0u);
    assert(g_alarm_clear_calls == 0u);
    {
        config_channel_definition_t channel_a =
            channel_def(5u, "channel.temperature.1", CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP, "temperature");
        config_channel_definition_t channel_b =
            channel_def(8u, "channel.pressure.1", CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP, "pressure");
        device_profile_config_t two_channel = profile_two(channel_a, channel_b);
        size_t history_before;
        size_t alarm_before;

        /* Adding a genuinely new channel (B) alongside an untouched one (A,
         * same id/driver/value_type as already running) must not clear A's
         * state -- A was never in the "deleted or modified" set. (B's
         * verify pass polls every registered channel, including unchanged
         * A -- R5 alongside R8 -- but that read alone must not trigger a
         * clear for A.) */
        assert(config_service_replace_and_save(&two_channel, true));
        expect_event("O5S5C5T5S8C8T8R5R8");
        assert(g_history_clear_calls == 0u);
        assert(g_alarm_clear_calls == 0u);

        /* Modify B only (same id, different driver_id): B must be cleared
         * exactly once by each service, strictly before the restart
         * callback fires for this same commit; A (still unchanged) must
         * never be cleared. */
        channel_b = channel_def(8u, "channel.pressure.1", CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE, "pressure");
        two_channel = profile_two(channel_a, channel_b);
        history_before = g_history_clear_calls;
        alarm_before = g_alarm_clear_calls;
        assert(config_service_replace_and_save(&two_channel, true));
        expect_event("O5O8S5C5T5S8C8T8R5R8");
        assert(g_history_clear_calls == history_before + 1u);
        assert(g_alarm_clear_calls == alarm_before + 1u);
        assert(strcmp(g_history_clear_ids[g_history_clear_calls - 1u], "channel.pressure.1") == 0);
        assert(strcmp(g_alarm_clear_ids[g_alarm_clear_calls - 1u], "channel.pressure.1") == 0);
        assert(g_history_clear_seq[g_history_clear_calls - 1u] < g_restart_seq);
        assert(g_alarm_clear_seq[g_alarm_clear_calls - 1u] < g_restart_seq);

        /* Delete B entirely (drop it from the candidate): B must be cleared
         * exactly once more; A (still unchanged) must never be cleared. An
         * untouched channel's history/alarm state survives a transaction
         * that changes/removes a sibling channel. */
        {
            device_profile_config_t one_channel = profile_one(channel_a);

            history_before = g_history_clear_calls;
            alarm_before = g_alarm_clear_calls;
            assert(config_service_replace_and_save(&one_channel, true));
            expect_event("O5O8S5C5T5");
            assert(g_history_clear_calls == history_before + 1u);
            assert(g_alarm_clear_calls == alarm_before + 1u);
            assert(strcmp(g_history_clear_ids[g_history_clear_calls - 1u], "channel.pressure.1") == 0);
            assert(strcmp(g_alarm_clear_ids[g_alarm_clear_calls - 1u], "channel.pressure.1") == 0);
            assert(g_history_clear_seq[g_history_clear_calls - 1u] < g_restart_seq);
            assert(g_alarm_clear_seq[g_alarm_clear_calls - 1u] < g_restart_seq);
        }

        /* Across all three commits above, "channel.temperature.1" (A) was
         * never passed to either clear function. */
        for (size_t i = 0u; i < g_history_clear_calls; ++i)
        {
            assert(strcmp(g_history_clear_ids[i], "channel.temperature.1") != 0);
        }
        for (size_t i = 0u; i < g_alarm_clear_calls; ++i)
        {
            assert(strcmp(g_alarm_clear_ids[i], "channel.temperature.1") != 0);
        }
    }

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

        /* v0.5.0 T9: config_service.c's own doc comment groups _EMPTY/_OK/
         * _DEGRADED as behaving identically -- proven directly here rather
         * than only asserted in prose, since the boot logic is a simple
         * binary branch (only _FALLBACK_REQUIRED is special-cased) and a
         * future refactor could easily split that branch incorrectly
         * without a test pinning every named outcome, not just one
         * representative of the group. */
        g_recovery_state = CONFIG_TXN_RECOVERY_EMPTY;
        discard_before = g_discard_staged_calls;
        fallback_before = g_recover_fallback_calls;
        assert(config_service_init(&recovery_defaults));
        assert(g_discard_staged_calls == discard_before + 1u);
        assert(g_recover_fallback_calls == fallback_before);

        g_recovery_state = CONFIG_TXN_RECOVERY_DEGRADED;
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

        /* v0.5.0 T9: _INDETERMINATE must group with _FATAL, not with the
         * _OK-like group -- same reasoning as the _EMPTY/_DEGRADED pinning
         * above. */
        g_recovery_state = CONFIG_TXN_RECOVERY_INDETERMINATE;
        discard_before = g_discard_staged_calls;
        fallback_before = g_recover_fallback_calls;
        assert(config_service_init(&recovery_defaults));
        assert(g_discard_staged_calls == discard_before + 1u);
        assert(g_recover_fallback_calls == fallback_before);
        assert(config_service_get()->channels[0].source_index == 9u);
    }

    /* v0.5.0 T8: applying the "sensor_node" runtime configuration preset
     * (services/config_presets.c) through the existing transaction
     * pipeline. config_presets_apply() must resolve the preset by id and
     * feed the resulting candidate straight into
     * config_service_replace_and_save() -- the exact same apply path every
     * other candidate configuration in this file goes through; there is no
     * parallel apply path to test separately. The recovery block above
     * re-ran config_service_init() (which unregisters both callbacks), so
     * both must be re-registered first, exactly as app_init_task() does at
     * boot. */
    {
        uint32_t revision_before;
        const config_preset_catalog_entry_t *preset;

        config_service_set_runtime_apply_callback(sensor_manager_activate_config);
        config_service_set_restart_callback(fake_restart_callback);
        /* g_fail_step is left set (never reset to a no-op value) by the last
         * injected-failure scenario above; g_fail_source == 0u would
         * otherwise spuriously match this preset's source_index == 0
         * channels. */
        g_fail_source = 0xFFu; g_fail_step = '\0';

        preset = config_presets_get_by_id((uint8_t)CONFIG_PRESET_ID_SENSOR_NODE);
        assert(preset != NULL);
        assert(strcmp(preset->name, "sensor_node") == 0);
        assert(config_presets_get_by_name("sensor_node") == preset);
        assert(config_presets_get_by_name("no_such_preset") == NULL);
        assert(config_presets_get_by_id(99u) == NULL);

        revision_before = config_service_get_running_revision();
        g_event_length = 0u; g_events[0] = '\0';

        assert(config_presets_apply((uint8_t)CONFIG_PRESET_ID_SENSOR_NODE));
        /* Proves activation actually ran through sensor_manager (stop/start/
         * configure/self_test/read of the real driver lifecycle), not a
         * trivial no-op success. */
        assert(g_event_length > 0u);
        assert(config_service_get_running_revision() == revision_before + 1u);

        {
            const device_profile_config_t *running = config_service_get();
            size_t in_use_count = 0u;

            for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
            {
                if (running->channels[index].in_use != 0u)
                {
                    ++in_use_count;
                }
            }
            assert(in_use_count == 3u);
            assert(strcmp(running->channels[0].id, "channel.temperature.1") == 0);
            assert(running->channels[0].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP);
            assert(strcmp(running->channels[1].id, "channel.adc.1") == 0);
            assert(running->channels[1].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE);
            assert(strcmp(running->channels[2].id, "channel.gpio.1") == 0);
            assert(running->channels[2].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT);
        }

        /* Unknown preset id: resolved before any Flash/activation attempt,
         * so the running configuration must be left untouched. */
        assert(!config_presets_apply(99u));
        assert(config_service_get_running_revision() == revision_before + 1u);
    }

    /* v0.5.0 T8: "remote_io" (services/config_presets.c) -- the
     * discrete-I/O-heavy composition (12 GPIO inputs + 2 supervisory ADC
     * inputs) applied through the exact same config_presets_apply() ->
     * config_service_replace_and_save() path proven above for
     * "sensor_node". Proves the second catalog entry resolves, activates
     * through the real sensor_manager lifecycle, and commits. */
    {
        uint32_t revision_before;
        const config_preset_catalog_entry_t *preset;

        g_fail_source = 0xFFu; g_fail_step = '\0';

        preset = config_presets_get_by_id((uint8_t)CONFIG_PRESET_ID_REMOTE_IO);
        assert(preset != NULL);
        assert(strcmp(preset->name, "remote_io") == 0);
        assert(config_presets_get_by_name("remote_io") == preset);

        revision_before = config_service_get_running_revision();
        g_event_length = 0u; g_events[0] = '\0';

        assert(config_presets_apply((uint8_t)CONFIG_PRESET_ID_REMOTE_IO));
        assert(g_event_length > 0u);
        assert(config_service_get_running_revision() == revision_before + 1u);

        {
            const device_profile_config_t *running = config_service_get();
            size_t in_use_count = 0u;

            for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
            {
                if (running->channels[index].in_use != 0u)
                {
                    ++in_use_count;
                }
            }
            assert(in_use_count == 14u);
            assert(strcmp(running->channels[0].id, "channel.gpio.1") == 0);
            assert(running->channels[0].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT);
            assert(strcmp(running->channels[11].id, "channel.gpio.12") == 0);
            assert(running->channels[11].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT);
            assert(strcmp(running->channels[12].id, "channel.adc.1") == 0);
            assert(running->channels[12].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE);
            assert(strcmp(running->channels[13].id, "channel.adc.2") == 0);
            assert(running->channels[13].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE);
            assert(running->channels[14].in_use == 0u);
            assert(running->channels[15].in_use == 0u);
        }
    }

    /* v0.5.0 T8: "dev_benchmark" (services/config_presets.c) -- the
     * maximum-density mix (1 internal temperature + 4 ADC + 11 GPIO ==
     * PLATFORM_CHANNEL_LIMIT) applied through the same pipeline. Proves the
     * catalog's densest entry also activates and commits cleanly, and that
     * every one of PLATFORM_CHANNEL_LIMIT slots really does get filled. */
    {
        uint32_t revision_before;
        const config_preset_catalog_entry_t *preset;

        g_fail_source = 0xFFu; g_fail_step = '\0';

        preset = config_presets_get_by_id((uint8_t)CONFIG_PRESET_ID_DEV_BENCHMARK);
        assert(preset != NULL);
        assert(strcmp(preset->name, "dev_benchmark") == 0);
        assert(config_presets_get_by_name("dev_benchmark") == preset);

        revision_before = config_service_get_running_revision();
        g_event_length = 0u; g_events[0] = '\0';

        assert(config_presets_apply((uint8_t)CONFIG_PRESET_ID_DEV_BENCHMARK));
        assert(g_event_length > 0u);
        assert(config_service_get_running_revision() == revision_before + 1u);

        {
            const device_profile_config_t *running = config_service_get();
            size_t in_use_count = 0u;

            for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
            {
                if (running->channels[index].in_use != 0u)
                {
                    ++in_use_count;
                }
            }
            assert(in_use_count == (size_t)PLATFORM_CHANNEL_LIMIT);
            assert(strcmp(running->channels[0].id, "channel.temperature.1") == 0);
            assert(running->channels[0].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP);
            assert(strcmp(running->channels[1].id, "channel.adc.1") == 0);
            assert(running->channels[1].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE);
            assert(strcmp(running->channels[4].id, "channel.adc.4") == 0);
            assert(running->channels[4].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE);
            assert(strcmp(running->channels[5].id, "channel.gpio.1") == 0);
            assert(running->channels[5].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT);
            assert(strcmp(running->channels[15].id, "channel.gpio.11") == 0);
            assert(running->channels[15].driver_id == (uint8_t)CONFIG_CHANNEL_DRIVER_GPIO_INPUT);
        }
    }

    /* v0.5.0 T8: ListConfigPresets' backing enumeration
     * (config_presets_count()/config_presets_get_by_index()) must report all
     * three catalog entries once remote_io/dev_benchmark are appended --
     * the OPC UA method itself is untested here (no open62541 in this host
     * suite), but this proves the catalog it iterates is catalog-generic and
     * genuinely has three entries, not still hardcoded to one. */
    {
        assert(config_presets_count() == 3u);
        assert(config_presets_get_by_index(0u) != NULL &&
               strcmp(config_presets_get_by_index(0u)->name, "sensor_node") == 0);
        assert(config_presets_get_by_index(1u) != NULL &&
               strcmp(config_presets_get_by_index(1u)->name, "remote_io") == 0);
        assert(config_presets_get_by_index(2u) != NULL &&
               strcmp(config_presets_get_by_index(2u)->name, "dev_benchmark") == 0);
        assert(config_presets_get_by_index(3u) == NULL);
    }
    return 0;
}

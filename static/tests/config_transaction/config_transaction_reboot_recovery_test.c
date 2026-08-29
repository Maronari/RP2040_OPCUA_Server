/**
 * @file
 * @brief v0.5.0 T9 host suite: reboot-recovery after a simulated power-loss
 * abort, driven end-to-end through the REAL production chain --
 * services/config_service.c -> services/config_store_service.c ->
 * drivers/config_transaction_backend.c -- against one simulated Flash media
 * array, plus real services/sensor_manager.c driver activation against a
 * fake telemetry driver.
 *
 * This complements two existing, narrower suites rather than duplicating
 * them: static/tests/config_transaction_backend/config_transaction_backend_test.c
 * (T3) already exhaustively fault-injects every physical Flash write inside
 * config_transaction_backend_stage()/_commit() in isolation (every program
 * call, both halves of promotion); static/tests/config_transaction/
 * config_transaction_test.c (T4) already proves config_service.c's own
 * phase ordering and its reaction to each config_transaction_recovery_state_t
 * value, but fakes services/config_store_service.h entirely -- neither
 * proves that a real reboot (a fresh config_transaction_backend_t recovering
 * the SAME underlying Flash bytes) actually drives config_service_init()
 * to the documented outcome. This file closes that gap for the six
 * scenarios docs/releases/v0.5.0.md's T9 section enumerates:
 *   1. power loss before the staged blob's commit-page write starts;
 *   2. power loss during/right after that commit-page write, before
 *      promotion to running;
 *   3. power loss during commit's staged->running promotion, both in the
 *      last-known-good half and the running half;
 *   4. power loss after commit fully completes;
 *   5. a corrupted running region independent of any in-flight transaction;
 *   6. an activate-phase (driver) failure leaving Flash state untouched.
 *
 * "Reboot" is simulated by calling config_store_service_init_with_storage()
 * again against the same simulated_media_t byte array (a fresh
 * config_transaction_backend_t re-recovering real Flash content, exactly as
 * config_store_backend()'s lazy init would on a real boot) followed by a
 * fresh config_service_init()/channel_manager_init()/sensor_manager_init()
 * -- mirroring app_init_task()'s real boot sequence.
 */
#include "services/alarm_service.h"
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

/* ---- Simulated Flash media, mirroring config_transaction_backend_test.c's simulated_media_t. ---- */
typedef struct
{
    uint8_t bytes[CONFIG_TXN_ROLE_SIZE_BYTES * CONFIG_TXN_ROLE_COUNT];
    unsigned program_count;
    unsigned fail_program_number; /* 0 = never fail. */
    bool apply_failed_program;
    bool unreadable;
} simulated_media_t;

static bool media_read(void *context, uint32_t offset, uint8_t *destination, size_t size)
{
    simulated_media_t *media = context;
    if (media->unreadable || offset > sizeof(media->bytes) || size > sizeof(media->bytes) - offset)
    {
        return false;
    }
    memcpy(destination, &media->bytes[offset], size);
    return true;
}

static bool media_program(void *context, uint32_t offset, const uint8_t page[CONFIG_TXN_PAGE_SIZE_BYTES])
{
    simulated_media_t *media = context;
    ++media->program_count;
    if (offset > sizeof(media->bytes) - CONFIG_TXN_PAGE_SIZE_BYTES)
    {
        return false;
    }
    if (media->fail_program_number == media->program_count && !media->apply_failed_program)
    {
        return false;
    }
    for (size_t i = 0u; i < CONFIG_TXN_PAGE_SIZE_BYTES; ++i)
    {
        media->bytes[offset + i] &= page[i];
    }
    return media->fail_program_number != media->program_count;
}

static bool media_erase(void *context, uint32_t offset)
{
    simulated_media_t *media = context;
    if (offset > sizeof(media->bytes) - CONFIG_TXN_SECTOR_SIZE_BYTES || (offset % CONFIG_TXN_SECTOR_SIZE_BYTES) != 0u)
    {
        return false;
    }
    memset(&media->bytes[offset], 0xFF, CONFIG_TXN_SECTOR_SIZE_BYTES);
    return true;
}

static config_transaction_storage_t storage(simulated_media_t *media)
{
    config_transaction_storage_t s = {.context = media, .read = media_read, .program_page = media_program, .erase_sector = media_erase};
    return s;
}

static void init_media(simulated_media_t *media)
{
    memset(media->bytes, 0xFF, sizeof(media->bytes));
    media->program_count = 0u;
    media->fail_program_number = 0u;
    media->apply_failed_program = false;
    media->unreadable = false;
}

/* ---- Fake single-channel driver + event log, mirroring config_transaction_test.c. ---- */
static char g_events[192];
static size_t g_event_length;
static uint8_t g_fail_source;
static char g_fail_step;

void *psram_calloc(size_t count, size_t size) { return calloc(count, size); }
bool psram_allocator_is_ready(void) { return true; }
TickType_t xTaskGetTickCount(void) { return 1u; }
void vTaskDelayUntil(TickType_t *last_wake_time, TickType_t period) { (void)last_wake_time; (void)period; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void pico_get_unique_board_id(pico_unique_board_id_t *id_out) { memset(id_out, 0, sizeof(*id_out)); }
bool time_service_get_utc_time_us(uint64_t *timestamp) { *timestamp = 1u; return true; }
bool history_service_record_channel_sample(const char *id, telemetry_channel_value_type_t value_type,
                                           const history_service_value_t *value, service_quality_t quality,
                                           uint64_t timestamp, uint32_t interval)
{ (void)id; (void)value_type; (void)value; (void)quality; (void)timestamp; (void)interval; return true; }
bool history_service_clear_channel(const char *channel_id) { (void)channel_id; return true; }
bool alarm_service_clear_channel(const char *channel_id) { (void)channel_id; return true; }
bool flash_storage_driver_load_partial(void *buffer, size_t buffer_size, size_t *out_payload_size)
{ (void)buffer; (void)buffer_size; (void)out_payload_size; return false; }
config_validate_service_result_t config_validate_service_validate_candidate(const device_profile_config_t *candidate, char *message, size_t message_size)
{ (void)message; (void)message_size; return candidate != NULL ? CONFIG_VALIDATE_RESULT_OK : CONFIG_VALIDATE_RESULT_INTERNAL_ERROR; }

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage_arg)
{ storage_arg->locked = 0; return storage_arg; }
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
const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t id)
{ return id == g_entry.driver_id ? &g_entry : NULL; }
const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name) { (void)name; return NULL; }
size_t driver_catalog_count(void) { return 1u; }
const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index)
{ return index == 0u ? &g_entry : NULL; }
bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry, telemetry_channel_value_type_t type)
{ return entry == &g_entry && type == TELEMETRY_CHANNEL_VALUE_FLOAT; }
bool driver_catalog_get_resource_claims(uint8_t id, uint8_t source_index, uint8_t source_flags,
                                        driver_catalog_resource_claim_t *claims, size_t capacity, size_t *out_count)
{ (void)source_flags; if (id != g_entry.driver_id || capacity == 0u || claims == NULL || out_count == NULL) return false;
  claims[0] = (driver_catalog_resource_claim_t){ DRIVER_CATALOG_RESOURCE_GPIO, source_index, true }; *out_count = 1u; return true; }
bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims, size_t count)
{ (void)claims; return count <= 8u; }

static device_profile_config_t profile(uint8_t source_index)
{
    device_profile_config_t value = { .opcua_port = 4840u, .channels = {{
        .in_use = 1u, .driver_id = CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP,
        .source_index = source_index, .value_type = TELEMETRY_CHANNEL_VALUE_FLOAT,
        .flags = TELEMETRY_CHANNEL_FLAG_ENABLED, .id = "channel.temperature.1",
        .name = "temperature", .settings = {
            .enabled = true, .sample_period_ms = 1u,
            .low_threshold = DBL_MAX, .low_low_threshold = DBL_MAX,
        },
    }}};
    return value;
}

static void stage_source(uint8_t source_index)
{
    config_channel_definition_t channel;
    assert(config_service_stage_copy_channel(0u, &channel));
    channel.source_index = source_index;
    assert(config_service_stage_set_channel(0u, &channel));
}

/* ---- Boot/reboot helpers. ---- */

/** @brief (Re)installs the storage backend against @p media without touching config_service/sensor_manager/channel_manager state -- lets a test inspect config_store_get_recovery_state() at the exact moment a real boot would, before config_service_init()'s self-heal (if any) mutates it. */
static void reboot_storage_only(simulated_media_t *media)
{
    config_transaction_storage_t io = storage(media);
    config_store_service_init_with_storage(&io);
}

/** @brief Full simulated reboot: storage, then config_service/channel_manager/sensor_manager, mirroring app_init_task()'s real boot sequence. sensor_manager_init() re-registers itself as the runtime-apply callback. */
static void reboot_full(simulated_media_t *media, const device_profile_config_t *defaults)
{
    reboot_storage_only(media);
    assert(config_service_init(defaults));
    assert(channel_manager_init());
    assert(sensor_manager_init());
    g_event_length = 0u;
    g_events[0] = '\0';
}

/** @brief Number of program_page() calls one config_store_stage_payload() takes for a device_profile_config_t-sized payload (payload-size-invariant, so probed once and reused everywhere below). */
static unsigned probe_stage_program_call_count(void)
{
    simulated_media_t media;
    device_profile_config_t payload;
    unsigned count;

    memset(&payload, 0, sizeof(payload));
    init_media(&media);
    reboot_storage_only(&media);
    assert(config_store_stage_payload(&payload, sizeof(payload), CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                      CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY, NULL));
    count = media.program_count;
    assert(count >= 3u);
    return count;
}

/** @brief Number of program_page() calls one config_store_commit_staged() takes when it must promote an existing RUNNING into LAST_KNOWN_GOOD as well as write the new RUNNING (both halves equal-sized, since every payload here is the same fixed device_profile_config_t size). */
static unsigned probe_commit_program_call_count_with_promotion(void)
{
    simulated_media_t media;
    device_profile_config_t payload;
    unsigned before;

    memset(&payload, 0, sizeof(payload));
    init_media(&media);
    reboot_storage_only(&media);
    assert(config_store_save_payload(&payload, sizeof(payload), CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                     CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY, NULL)); /* gen1: no promotion yet */
    assert(config_store_stage_payload(&payload, sizeof(payload), CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY,
                                      CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY, NULL));
    before = media.program_count;
    assert(config_store_commit_staged(NULL)); /* gen2: promotion runs */
    assert(media.program_count > before);
    assert((media.program_count - before) % 2u == 0u);
    return media.program_count - before;
}

/* ---- Point 1: power loss before the staged blob's commit-page write starts. ---- */
static void test_point1_power_loss_before_stage_commit_page(unsigned stage_calls)
{
    simulated_media_t media;
    const device_profile_config_t defaults = profile(1u);

    (void)stage_calls;
    init_media(&media);
    reboot_full(&media, &defaults);
    assert(config_service_apply()); /* establish a real Flash RUNNING record (gen1). */
    g_event_length = 0u; g_events[0] = '\0';

    stage_source(2u);
    media.fail_program_number = media.program_count + 1u; /* the very first program call: the header page. */
    media.apply_failed_program = false;
    assert(!config_service_apply());
    media.fail_program_number = 0u;
    /* The stage failed before any Flash commit-page write was even attempted
     * -- activation must never have been reached. */
    assert(g_event_length == 0u);
    assert(config_service_get()->channels[0].source_index == 1u);

    reboot_full(&media, &defaults);
    assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_OK);
    assert(config_service_get()->channels[0].source_index == 1u);
}

/* ---- Point 2: power loss during/right after the stage commit-page write, before promotion. ---- */
static void test_point2_power_loss_at_stage_commit_page(unsigned stage_calls)
{
    simulated_media_t media;
    const device_profile_config_t defaults = profile(1u);

    init_media(&media);
    reboot_full(&media, &defaults);
    assert(config_service_apply());
    g_event_length = 0u; g_events[0] = '\0';

    stage_source(3u);
    media.fail_program_number = media.program_count + stage_calls; /* stage()'s own commit page, the last of its calls. */
    media.apply_failed_program = false; /* cleanly rejected -> erased -> NOT_COMMITTED, never a "lying success". */
    assert(!config_service_apply());
    media.fail_program_number = 0u;
    assert(g_event_length == 0u); /* torn stage still precedes activation entirely. */
    assert(config_service_get()->channels[0].source_index == 1u);

    reboot_full(&media, &defaults);
    assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_OK);
    assert(config_service_get()->channels[0].source_index == 1u);
}

/* ---- Point 3 (both halves of promotion) + point 4 (old running lands correctly in last-known-good). ---- */
static void test_point3_and_4_promotion_ordering_and_last_known_good(unsigned stage_calls, unsigned commit_calls_with_promotion)
{
    const unsigned lkg_calls = commit_calls_with_promotion / 2u;
    const device_profile_config_t defaults = profile(1u);

    /* Sub-case A: fail inside the LAST_KNOWN_GOOD half. RUNNING must be left
     * completely untouched by this commit() attempt -- still the pre-attempt
     * value, recovery still OK. */
    {
        simulated_media_t media;
        unsigned before;

        init_media(&media);
        reboot_full(&media, &defaults);
        assert(config_service_apply()); /* gen1 RUNNING = profile(1), no promotion yet. */
        stage_source(2u);
        assert(config_service_apply()); /* gen2 RUNNING = profile(2), LAST_KNOWN_GOOD = profile(1). */
        assert(config_service_get()->channels[0].source_index == 2u);
        g_event_length = 0u; g_events[0] = '\0';

        stage_source(3u);
        before = media.program_count;
        media.fail_program_number = before + stage_calls + 1u; /* first call of the LKG half. */
        media.apply_failed_program = false;
        assert(!config_service_apply());
        media.fail_program_number = 0u;
        /* Activation ran (stop old, start/verify candidate) before the Flash
         * commit -- per the frozen order -- then must have been rolled back
         * by config_service_apply()'s own restore-previous-drivers path. */
        assert(config_service_get()->channels[0].source_index == 2u);

        reboot_full(&media, &defaults);
        assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_OK);
        assert(config_service_get()->channels[0].source_index == 2u);
    }

    /* Sub-case B: fail inside the RUNNING half (LAST_KNOWN_GOOD already
     * landed). Recovery must report FALLBACK_REQUIRED; config_service_init()
     * must self-heal RUNNING from LAST_KNOWN_GOOD, landing back on the
     * pre-attempt value -- simultaneously proving point 4 (the OLD running
     * config, profile(2), really is what LAST_KNOWN_GOOD now holds). */
    {
        simulated_media_t media;
        unsigned before;

        init_media(&media);
        reboot_full(&media, &defaults);
        assert(config_service_apply()); /* gen1 RUNNING = profile(1). */
        stage_source(2u);
        assert(config_service_apply()); /* gen2 RUNNING = profile(2), LAST_KNOWN_GOOD = profile(1). */
        g_event_length = 0u; g_events[0] = '\0';

        stage_source(4u);
        before = media.program_count;
        media.fail_program_number = before + stage_calls + commit_calls_with_promotion; /* RUNNING half's own commit page, the very last call. */
        media.apply_failed_program = false;
        assert(!config_service_apply());
        media.fail_program_number = 0u;
        assert(config_service_get()->channels[0].source_index == 2u); /* RAM already rolled back. */

        reboot_storage_only(&media);
        assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED);
        assert(config_service_init(&defaults));
        assert(channel_manager_init());
        assert(sensor_manager_init());
        g_event_length = 0u; g_events[0] = '\0';
        /* Self-healed: RUNNING now holds what LAST_KNOWN_GOOD held, i.e.
         * profile(2) -- the OLD running config from before this failed
         * attempt, never profile(1) and never the torn profile(4). */
        assert(config_service_get()->channels[0].source_index == 2u);
        assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_OK);
    }
    (void)lkg_calls;
}

/* ---- Point 5: a corrupted running region independent of any in-flight transaction. ---- */
static void test_point5_corrupted_running_falls_back_to_last_known_good(void)
{
    simulated_media_t media;
    const device_profile_config_t defaults = profile(1u);
    uint32_t running_offset;

    init_media(&media);
    reboot_full(&media, &defaults);
    assert(config_service_apply()); /* gen1 RUNNING = profile(1). */
    stage_source(2u);
    assert(config_service_apply()); /* gen2 RUNNING = profile(2), LAST_KNOWN_GOOD = profile(1). */

    /* Simulated bit-rot, not a transaction: corrupt RUNNING's payload directly. */
    running_offset = (uint32_t)CONFIG_TXN_ROLE_RUNNING * CONFIG_TXN_ROLE_SIZE_BYTES;
    media.bytes[running_offset + CONFIG_TXN_PAGE_SIZE_BYTES] ^= 0xFFu;

    reboot_storage_only(&media);
    assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED);
    assert(config_service_init(&defaults));
    assert(channel_manager_init());
    assert(sensor_manager_init());
    assert(config_service_get()->channels[0].source_index == 1u); /* LAST_KNOWN_GOOD's value. */
    assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_OK);
}

/* ---- Point 6: an activate-phase (driver) failure leaves Flash state untouched. ---- */
static void test_point6_activate_failure_leaves_flash_untouched(void)
{
    simulated_media_t media;
    const device_profile_config_t defaults = profile(1u);

    init_media(&media);
    reboot_full(&media, &defaults);
    assert(config_service_apply()); /* gen1 RUNNING = profile(1). */
    g_event_length = 0u; g_events[0] = '\0';

    stage_source(2u);
    g_fail_source = 2u;
    g_fail_step = 'S'; /* driver start failure -- staging (Flash) already succeeded by this point. */
    assert(!config_service_apply());
    g_fail_source = 0u;
    g_fail_step = '\0';
    assert(config_service_get()->channels[0].source_index == 1u); /* RAM rolled back by sensor_manager itself. */

    reboot_storage_only(&media);
    /* The torn/discarded STAGED leftover must not have corrupted RUNNING's
     * own classification -- still a clean OK, never FALLBACK_REQUIRED/FATAL. */
    assert(config_store_get_recovery_state() == CONFIG_TXN_RECOVERY_OK);
    assert(config_service_init(&defaults));
    assert(channel_manager_init());
    assert(sensor_manager_init());
    assert(config_service_get()->channels[0].source_index == 1u); /* Flash-confirmed: never changed. */
}

int main(void)
{
    const unsigned stage_calls = probe_stage_program_call_count();
    const unsigned commit_calls_with_promotion = probe_commit_program_call_count_with_promotion();

    test_point1_power_loss_before_stage_commit_page(stage_calls);
    test_point2_power_loss_at_stage_commit_page(stage_calls);
    test_point3_and_4_promotion_ordering_and_last_known_good(stage_calls, commit_calls_with_promotion);
    test_point5_corrupted_running_falls_back_to_last_known_good();
    test_point6_activate_failure_leaves_flash_untouched();
    return 0;
}

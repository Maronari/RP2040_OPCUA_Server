#include "services/alarm_service.h"
#include "services/channel_manager.h"
#include "services/device_channels.h"

#include "FreeRTOS.h"
#include "task.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

/* Must be >= ALARM_SERVICE_MAX_CONDITIONS (== PLATFORM_CHANNEL_LIMIT, up to
 * 16) so the v0.5.0 T7 allocator-reclamation regression test below can fill
 * every condition slot in one round regardless of which PLATFORM_CHANNEL_LIMIT
 * this translation unit was built with (see test_alarm_service.py's 16/4
 * matrix). */
#define TEST_MAX_CHANNELS 16u

static telemetry_channel_snapshot_t g_test_channels[TEST_MAX_CHANNELS];
static size_t g_test_channel_count;
static unsigned g_critical_entries;
static const char *g_ack_interleave_channel;
static bool g_ack_interleave_fired;

void alarm_service_test_enter_critical(void) { g_critical_entries++; }
void alarm_service_test_exit_critical(void) { g_critical_entries++; }

/* Called by alarm_service.c only in this test build after it has copied a
 * condition to local state and before it commits that local state. This is a
 * deterministic stand-in for an independent OPC UA task acknowledging the
 * condition in precisely the formerly lossy interleaving window. */
void alarm_service_test_before_commit(const char *channel_id)
{
    if (!g_ack_interleave_fired && g_ack_interleave_channel != NULL &&
        strcmp(channel_id, g_ack_interleave_channel) == 0)
    {
        g_ack_interleave_fired = true;
        assert(alarm_service_acknowledge_channel(channel_id));
    }
}

TickType_t xTaskGetTickCount(void) { return 0u; }
void vTaskDelayUntil(TickType_t *previous_wake_time, TickType_t time_increment)
{
    (void)previous_wake_time;
    (void)time_increment;
}

size_t channel_manager_count(void)
{
    return g_test_channel_count;
}

bool channel_manager_copy_snapshot_by_index(size_t index, telemetry_channel_snapshot_t *out_snapshot)
{
    if (index >= g_test_channel_count || out_snapshot == NULL)
    {
        return false;
    }
    memcpy(out_snapshot, &g_test_channels[index], sizeof(*out_snapshot));
    out_snapshot->channel.metadata.id = out_snapshot->id;
    out_snapshot->channel.metadata.name = out_snapshot->name;
    out_snapshot->channel.metadata.description = out_snapshot->description;
    out_snapshot->channel.metadata.unit = out_snapshot->unit;
    return true;
}

static void set_numeric_channel(size_t index, const char *id, const char *name,
                                telemetry_channel_value_type_t value_type,
                                double warning, double critical, double low, double low_low,
                                double hysteresis, uint32_t debounce_ms,
                                double value, service_quality_t quality, bool enabled)
{
    telemetry_channel_snapshot_t *snapshot = &g_test_channels[index];

    memset(snapshot, 0, sizeof(*snapshot));
    (void)snprintf(snapshot->id, sizeof(snapshot->id), "%s", id);
    (void)snprintf(snapshot->name, sizeof(snapshot->name), "%s", name);
    snapshot->channel.metadata.id = snapshot->id;
    snapshot->channel.metadata.name = snapshot->name;
    snapshot->channel.metadata.value_type = value_type;
    snapshot->channel.metadata.flags = enabled ? TELEMETRY_CHANNEL_FLAG_ENABLED : 0u;
    snapshot->channel.settings.warning_threshold = warning;
    snapshot->channel.settings.critical_threshold = critical;
    snapshot->channel.settings.low_threshold = low;
    snapshot->channel.settings.low_low_threshold = low_low;
    snapshot->channel.settings.alarm_hysteresis = hysteresis;
    snapshot->channel.settings.alarm_debounce_ms = debounce_ms;
    snapshot->channel.runtime.quality = quality;
    switch (value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
        snapshot->channel.runtime.value.f32 = (float)value;
        break;
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        snapshot->channel.runtime.value.f64 = value;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        snapshot->channel.runtime.value.i32 = (int32_t)value;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        snapshot->channel.runtime.value.u32 = (uint32_t)value;
        break;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        snapshot->channel.runtime.value.i64 = (int64_t)value;
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        snapshot->channel.runtime.value.u64 = (uint64_t)value;
        break;
    default:
        break;
    }
}

static void set_discrete_channel(size_t index, const char *id, const char *name,
                                 uint32_t debounce_ms, bool value, service_quality_t quality, bool enabled)
{
    telemetry_channel_snapshot_t *snapshot = &g_test_channels[index];

    memset(snapshot, 0, sizeof(*snapshot));
    (void)snprintf(snapshot->id, sizeof(snapshot->id), "%s", id);
    (void)snprintf(snapshot->name, sizeof(snapshot->name), "%s", name);
    snapshot->channel.metadata.id = snapshot->id;
    snapshot->channel.metadata.name = snapshot->name;
    snapshot->channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_BOOLEAN;
    snapshot->channel.metadata.flags = enabled ? TELEMETRY_CHANNEL_FLAG_ENABLED : 0u;
    snapshot->channel.settings.alarm_debounce_ms = debounce_ms;
    snapshot->channel.runtime.quality = quality;
    snapshot->channel.runtime.value.boolean = value;
}

static void set_string_channel(size_t index, const char *id)
{
    telemetry_channel_snapshot_t *snapshot = &g_test_channels[index];

    memset(snapshot, 0, sizeof(*snapshot));
    (void)snprintf(snapshot->id, sizeof(snapshot->id), "%s", id);
    snapshot->channel.metadata.id = snapshot->id;
    snapshot->channel.metadata.name = snapshot->name;
    snapshot->channel.metadata.value_type = TELEMETRY_CHANNEL_VALUE_STRING;
    snapshot->channel.metadata.flags = TELEMETRY_CHANNEL_FLAG_ENABLED;
    snapshot->channel.runtime.quality = SERVICE_QUALITY_GOOD;
}

static alarm_condition_status_t must_find(const char *channel_id)
{
    alarm_condition_status_t condition = {0};
    assert(alarm_service_get_condition_by_channel_id(channel_id, &condition));
    return condition;
}

int main(void)
{
    assert(alarm_service_init());

    /* Migrated pre-v0.4.3 high-only configurations use DBL_MAX for the
     * absent lower side.  A negative, good-quality value must stay NORMAL;
     * an explicitly configured native v0.4.3 zero lower threshold remains
     * active when it is coherently ordered with the high threshold. */
    g_test_channel_count = 2u;
    set_numeric_channel(0u, "chan.migrated-high-only", "Migrated", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, DBL_MAX, DBL_MAX, 0.0, 0u,
                        -42.0, SERVICE_QUALITY_GOOD, true);
    set_numeric_channel(1u, "chan.native-zero-low", "Native", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u,
                        -42.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(900u);
    assert(must_find("chan.migrated-high-only").process_state == ALARM_CONDITION_STATE_NORMAL);
    assert(!must_find("chan.migrated-high-only").active);
    assert(must_find("chan.native-zero-low").process_state == ALARM_CONDITION_STATE_LOW_LOW);
    assert(must_find("chan.native-zero-low").active);

    /* --- Eligibility: numeric/Boolean get a condition, String does not --- */
    assert(alarm_service_init());
    g_test_channel_count = 3u;
    set_numeric_channel(0u, "chan.a", "A", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 10.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
    set_discrete_channel(1u, "chan.eligible-bool", "B", 0u, false, SERVICE_QUALITY_GOOD, true);
    set_string_channel(2u, "chan.c");
    alarm_service_evaluate_all(1000u);
    assert(alarm_service_get_condition_count() == 2u);
    {
        alarm_condition_status_t missing = {0};
        assert(!alarm_service_get_condition_by_channel_id("chan.c", &missing));
    }

    /* --- Independent simultaneous alarms on two channels --- */
    set_numeric_channel(0u, "chan.a", "A", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 10.0, 0.0, 0.0, 0u, 80.0, SERVICE_QUALITY_GOOD, true);
    set_numeric_channel(1u, "chan.d", "D", TELEMETRY_CHANNEL_VALUE_INT32,
                        60.0, 75.0, 10.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
    g_test_channel_count = 2u;
    alarm_service_evaluate_all(2000u);
    {
        alarm_condition_status_t a = must_find("chan.a");
        alarm_condition_status_t d = must_find("chan.d");
        assert(a.process_state == ALARM_CONDITION_STATE_HIGH_HIGH);
        assert(a.active);
        assert(d.process_state == ALARM_CONDITION_STATE_NORMAL);
        assert(!d.active);
        assert(a.trip_count == 1u);
        assert(!a.acknowledged);
    }

    /* --- Hysteresis: dropping just below warning but within the hysteresis
     * band must not clear HIGH; dropping below (warning - hysteresis) must. */
    assert(alarm_service_init());
    set_numeric_channel(0u, "chan.h", "H", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        /*warning*/ 60.0, /*critical*/ 75.0, /*low*/ 0.0, /*low_low*/ 0.0,
                        /*hysteresis*/ 5.0, /*debounce*/ 0u, /*value*/ 65.0, SERVICE_QUALITY_GOOD, true);
    g_test_channel_count = 1u;
    alarm_service_evaluate_all(3000u);
    assert(must_find("chan.h").process_state == ALARM_CONDITION_STATE_HIGH);

    set_numeric_channel(0u, "chan.h", "H", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 5.0, 0u, 57.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(3100u);
    assert(must_find("chan.h").process_state == ALARM_CONDITION_STATE_HIGH); /* inside hysteresis band, stays HIGH */

    set_numeric_channel(0u, "chan.h", "H", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 5.0, 0u, 54.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(3200u);
    assert(must_find("chan.h").process_state == ALARM_CONDITION_STATE_NORMAL); /* below warning - hysteresis, clears */

    /* --- Debounce: a transient crossing shorter than alarm_debounce_ms must
     * not confirm; a sustained crossing for >= alarm_debounce_ms must. */
    assert(alarm_service_init());
    set_numeric_channel(0u, "chan.debounce", "B", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 1000u, 20.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(10000u);
    assert(must_find("chan.debounce").process_state == ALARM_CONDITION_STATE_NORMAL);

    set_numeric_channel(0u, "chan.debounce", "B", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 1000u, 65.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(10100u); /* candidate observed, debounce timer starts at 10100+1000=11100 */
    assert(must_find("chan.debounce").process_state == ALARM_CONDITION_STATE_NORMAL); /* not yet confirmed */
    assert(must_find("chan.debounce").trip_count == 0u);

    /* Transient: value reverts before debounce elapses -- must never confirm. */
    set_numeric_channel(0u, "chan.debounce", "B", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 1000u, 20.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(10500u);
    assert(must_find("chan.debounce").process_state == ALARM_CONDITION_STATE_NORMAL);
    assert(must_find("chan.debounce").trip_count == 0u);

    /* Sustained: crosses again and stays until debounce elapses. */
    set_numeric_channel(0u, "chan.debounce", "B", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 1000u, 65.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(20000u); /* candidate restarts pending, deadline 21000 */
    assert(must_find("chan.debounce").process_state == ALARM_CONDITION_STATE_NORMAL);
    alarm_service_evaluate_all(20500u); /* still pending, same candidate, deadline unchanged */
    assert(must_find("chan.debounce").process_state == ALARM_CONDITION_STATE_NORMAL);
    alarm_service_evaluate_all(21000u); /* deadline reached -- confirmed */
    assert(must_find("chan.debounce").process_state == ALARM_CONDITION_STATE_HIGH);
    assert(must_find("chan.debounce").trip_count == 1u);
    assert(!must_find("chan.debounce").acknowledged);

    /* Zero debounce reproduces the v0.4.2 instantaneous behavior. */
    set_numeric_channel(0u, "chan.z", "Z", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 65.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(30000u);
    assert(must_find("chan.z").process_state == ALARM_CONDITION_STATE_HIGH);

    /* --- Quality fault entry/exit: sticky process_state, distinct fault --- */
    assert(alarm_service_init());
    set_numeric_channel(0u, "chan.f", "F", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 65.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(40000u);
    assert(must_find("chan.f").process_state == ALARM_CONDITION_STATE_HIGH);
    assert(must_find("chan.f").quality_fault == ALARM_QUALITY_FAULT_NONE);

    set_numeric_channel(0u, "chan.f", "F", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 65.0, SERVICE_QUALITY_BAD, true);
    alarm_service_evaluate_all(40500u);
    {
        alarm_condition_status_t f = must_find("chan.f");
        assert(f.quality_fault == ALARM_QUALITY_FAULT_BAD);
        assert(f.process_state == ALARM_CONDITION_STATE_HIGH); /* sticky: not overwritten */
        assert(f.active); /* active for either reason */
    }

    set_numeric_channel(0u, "chan.f", "F", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 65.0, SERVICE_QUALITY_UNCERTAIN, true);
    alarm_service_evaluate_all(41000u);
    assert(must_find("chan.f").quality_fault == ALARM_QUALITY_FAULT_UNCERTAIN);
    assert(must_find("chan.f").process_state == ALARM_CONDITION_STATE_HIGH);

    set_numeric_channel(0u, "chan.f", "F", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(41500u);
    {
        alarm_condition_status_t f = must_find("chan.f");
        assert(f.quality_fault == ALARM_QUALITY_FAULT_NONE);
        assert(f.process_state == ALARM_CONDITION_STATE_NORMAL);
        assert(!f.active);
    }

    /* Disabled channel (FLAG_ENABLED clear) is treated as a quality fault
     * (uncertain), not silently dropped and not treated as GOOD. */
    set_numeric_channel(0u, "chan.g", "G", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, false);
    alarm_service_evaluate_all(42000u);
    assert(must_find("chan.g").quality_fault == ALARM_QUALITY_FAULT_UNCERTAIN);

    /* --- Discrete Boolean channel alarm + debounce --- */
    assert(alarm_service_init());
    set_discrete_channel(0u, "chan.disc", "Disc", 500u, false, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(50000u);
    assert(!must_find("chan.disc").discrete_active);
    set_discrete_channel(0u, "chan.disc", "Disc", 500u, true, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(50100u);
    assert(!must_find("chan.disc").discrete_active); /* pending, not yet confirmed */
    alarm_service_evaluate_all(50600u);
    assert(must_find("chan.disc").discrete_active);
    assert(must_find("chan.disc").trip_count == 1u);
    assert(must_find("chan.disc").is_discrete);

    /* --- Acknowledgement independence across conditions --- */
    assert(alarm_service_init());
    g_test_channel_count = 2u;
    set_numeric_channel(0u, "chan.ack1", "Ack1", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 80.0, SERVICE_QUALITY_GOOD, true);
    set_numeric_channel(1u, "chan.ack2", "Ack2", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 80.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(60000u);
    assert(!must_find("chan.ack1").acknowledged);
    assert(!must_find("chan.ack2").acknowledged);
    assert(alarm_service_acknowledge_channel("chan.ack1"));
    assert(must_find("chan.ack1").acknowledged);
    assert(!must_find("chan.ack2").acknowledged); /* independent */
    assert(!alarm_service_acknowledge_channel("chan.does-not-exist"));
    alarm_service_acknowledge_all();
    assert(must_find("chan.ack1").acknowledged);
    assert(must_find("chan.ack2").acknowledged);

    /* Acknowledge after evaluator snapshot/before its commit. The old
     * copy-evaluate-whole-struct commit lost this acknowledgement; the
     * revision merge must preserve it. */
    set_numeric_channel(0u, "chan.ack1", "Ack1", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(60025u);
    set_numeric_channel(0u, "chan.ack1", "Ack1", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 80.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(60050u);
    assert(!must_find("chan.ack1").acknowledged);
    g_ack_interleave_channel = "chan.ack1";
    g_ack_interleave_fired = false;
    alarm_service_evaluate_all(60075u);
    assert(g_ack_interleave_fired);
    assert(must_find("chan.ack1").acknowledged);
    g_ack_interleave_channel = NULL;

    /* A fresh trip re-clears acknowledged independently of the other
     * channel's already-acknowledged state. */
    set_numeric_channel(0u, "chan.ack1", "Ack1", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(60100u); /* clear */
    set_numeric_channel(0u, "chan.ack1", "Ack1", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 80.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(60200u); /* re-trip */
    assert(!must_find("chan.ack1").acknowledged);
    assert(must_find("chan.ack2").acknowledged);

    /* --- Legacy singleton shim: falls back cleanly when Temperature1 is
     * absent, and mirrors the per-condition state once present. --- */
    assert(alarm_service_init());
    g_test_channel_count = 0u;
    {
        alarm_service_status_t legacy = {0};
        alarm_service_get_status(&legacy);
        assert(legacy.state == TELEMETRY_ALARM_STATE_INVALID);
    }
    set_numeric_channel(0u, DEVICE_CHANNEL_ID_TEMPERATURE_1, DEVICE_CHANNEL_NAME_TEMPERATURE_1,
                        TELEMETRY_CHANNEL_VALUE_FLOAT, 60.0, 75.0, 0.0, 0.0, 0.0, 0u,
                        80.0, SERVICE_QUALITY_GOOD, true);
    g_test_channel_count = 1u;
    alarm_service_evaluate_all(70000u);
    {
        alarm_service_status_t legacy = {0};
        alarm_service_get_status(&legacy);
        assert(legacy.state == TELEMETRY_ALARM_STATE_CRITICAL);
        assert(legacy.warning_active && legacy.critical_active);
        assert(strcmp(legacy.channel_id, DEVICE_CHANNEL_ID_TEMPERATURE_1) == 0);
    }

    /* --- Resource limit: more alarm-eligible channels than
     * ALARM_SERVICE_MAX_CONDITIONS must not overflow; extra channels simply
     * get no condition instance. --- */
    assert(alarm_service_init());
    g_test_channel_count = TEST_MAX_CHANNELS;
    for (size_t index = 0u; index < TEST_MAX_CHANNELS; ++index)
    {
        char id[TELEMETRY_CHANNEL_ID_MAX_LEN];
        (void)snprintf(id, sizeof(id), "chan.limit.%u", (unsigned)index);
        set_numeric_channel(index, id, id, TELEMETRY_CHANNEL_VALUE_FLOAT,
                            60.0, 75.0, 0.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
    }
    alarm_service_evaluate_all(80000u);
    assert(alarm_service_get_condition_count() <= ALARM_SERVICE_MAX_CONDITIONS);
    assert(alarm_service_get_condition_count() == (TEST_MAX_CHANNELS < ALARM_SERVICE_MAX_CONDITIONS
                                                        ? TEST_MAX_CHANNELS
                                                        : ALARM_SERVICE_MAX_CONDITIONS));
    if (TEST_MAX_CHANNELS > ALARM_SERVICE_MAX_CONDITIONS)
    {
        alarm_condition_status_t overflow = {0};
        char last_id[TELEMETRY_CHANNEL_ID_MAX_LEN];
        (void)snprintf(last_id, sizeof(last_id), "chan.limit.%u", (unsigned)(TEST_MAX_CHANNELS - 1u));
        assert(!alarm_service_get_condition_by_channel_id(last_id, &overflow));
    }

    /* alarm_service_get_condition_by_index bounds check. */
    {
        alarm_condition_status_t out_of_range = {0};
        assert(!alarm_service_get_condition_by_index(alarm_service_get_condition_count(), &out_of_range));
    }

    /* --- v0.5.0 T7: alarm_service_clear_channel() basic contract --- */
    assert(alarm_service_init());
    g_test_channel_count = 1u;
    set_numeric_channel(0u, "chan.clear", "Clear", TELEMETRY_CHANNEL_VALUE_FLOAT,
                        60.0, 75.0, 0.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
    alarm_service_evaluate_all(90000u);
    assert(alarm_service_get_condition_count() == 1u);
    /* Bad arguments never crash and never report success. */
    assert(!alarm_service_clear_channel(NULL));
    assert(!alarm_service_clear_channel(""));
    /* Clearing an id that was never bound reports false, no-op. */
    assert(!alarm_service_clear_channel("chan.never-bound"));
    assert(alarm_service_get_condition_count() == 1u);
    /* Clearing a bound channel succeeds and its condition becomes
     * unreachable by channel id afterward. */
    assert(alarm_service_clear_channel("chan.clear"));
    {
        alarm_condition_status_t after_clear = {0};
        assert(!alarm_service_get_condition_by_channel_id("chan.clear", &after_clear));
    }
    /* Clearing twice in a row is a safe, reported no-op the second time. */
    assert(!alarm_service_clear_channel("chan.clear"));

    /* --- v0.5.0 T7 regression: alarm_service_clear_channel() must reclaim
     * its slot for reuse, not just mark it unused, or repeated delete/
     * modify cycles on reused channel ids would permanently shrink
     * available condition capacity (the exact bug this release's plan
     * (docs/releases/v0.5.0.md T7) found in the original allocator, which
     * only ever extended g_condition_count and never reclaimed a freed
     * slot). Bind ALARM_SERVICE_MAX_CONDITIONS distinct channels to fill
     * every slot, clear all of them, then repeat with an entirely
     * different set of ids for several rounds. A naive non-reclaiming
     * allocator exhausts capacity after the very first round -- every
     * channel in round 2+ would silently fail to bind; the fix keeps
     * every round fully bindable. --- */
    assert(alarm_service_init());
    {
        const unsigned rounds = 5u;

        for (unsigned round = 0u; round < rounds; ++round)
        {
            g_test_channel_count = ALARM_SERVICE_MAX_CONDITIONS;
            for (size_t index = 0u; index < ALARM_SERVICE_MAX_CONDITIONS; ++index)
            {
                char id[TELEMETRY_CHANNEL_ID_MAX_LEN];

                (void)snprintf(id, sizeof(id), "chan.round%u.%u", round, (unsigned)index);
                set_numeric_channel(index, id, id, TELEMETRY_CHANNEL_VALUE_FLOAT,
                                    60.0, 75.0, 0.0, 0.0, 0.0, 0u, 20.0, SERVICE_QUALITY_GOOD, true);
            }
            alarm_service_evaluate_all(100000u + round);

            /* Every channel this round got a real, reachable condition slot
             * -- proof capacity was not already exhausted by an earlier
             * round's clears. */
            assert(alarm_service_get_condition_count() == ALARM_SERVICE_MAX_CONDITIONS);
            for (size_t index = 0u; index < ALARM_SERVICE_MAX_CONDITIONS; ++index)
            {
                char id[TELEMETRY_CHANNEL_ID_MAX_LEN];
                alarm_condition_status_t status = {0};

                (void)snprintf(id, sizeof(id), "chan.round%u.%u", round, (unsigned)index);
                assert(alarm_service_get_condition_by_channel_id(id, &status));
                assert(status.in_use);

                /* Clear every slot so the next round's entirely different
                 * ids must reclaim them. */
                assert(alarm_service_clear_channel(id));
                assert(!alarm_service_get_condition_by_channel_id(id, &status));
            }
            g_test_channel_count = 0u;
        }
    }

    assert(g_critical_entries > 0u);
    printf("alarm service host suite: PASS\n");
    return 0;
}

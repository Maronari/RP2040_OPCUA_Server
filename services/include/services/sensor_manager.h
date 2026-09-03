/**
 * @file
 * @brief Sensor channel lifecycle manager: activates channel driver
 *        configuration, drives periodic read polling, and records history
 *        samples.
 *
 * Reads sensor channels indirectly via the `telemetry_driver_vtbl_t`
 * dispatch table (`channel_manager_poll_all()`), never touching hardware
 * directly. Owns a double-buffered candidate/active configuration bank so a
 * failed reconfiguration cannot corrupt the currently running channel set
 * (see docs/code/state-ownership.md, "Активация конфигурации датчиков").
 */
#ifndef SERVICES_SENSOR_MANAGER_H
#define SERVICES_SENSOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "services/config_service.h"
#include "services/service_types.h"

/**
 * @brief Legacy Temperature1-channel compatibility snapshot.
 *
 * v0.4.0 temperature compatibility only: this is a projection read from the
 * channel registry's `Temperature1` entry (see
 * DEVICE_CHANNEL_ID_TEMPERATURE_1), not an independent runtime sample or a
 * selected "primary channel". New code should read the channel registry
 * directly instead of this struct.
 */
typedef struct
{
    bool enabled;
    float temperature_c;
    service_quality_t quality;
    /* Deprecated compatibility projection of source_timestamp_unix_us.
     * Zero means unknown in this legacy field; use microseconds for new code. */
    uint32_t source_timestamp_epoch_s;
    uint64_t source_timestamp_unix_us;
    uint32_t sample_count;
    uint32_t error_count;
    uint32_t last_update_ms;
} sensor_manager_snapshot_t;

/**
 * @brief One-time startup init: verifies prerequisites, allocates the
 *        double-buffered runtime storage, and activates the running config.
 *
 * @pre `channel_manager_is_ready()` and `psram_allocator_is_ready()` must
 *      already hold; both are dependencies this function checks but does
 *      not itself establish.
 * @return true if PSRAM storage allocation and the initial
 *         sensor_manager_activate_config() call both succeed; false
 *         otherwise (leaves the manager uninitialized).
 * @note Registers itself as `config_service`'s runtime apply callback
 *       (`config_service_set_runtime_apply_callback()`), so subsequent
 *       config-service transactions invoke sensor_manager_activate_config()
 *       synchronously.
 */
bool sensor_manager_init(void);

/**
 * @brief Runtime-owner half of the configuration transaction: validates,
 *        stops the old configuration, starts and verifies a candidate
 *        channel/driver configuration, and publishes it.
 *
 * Performs all driver lifecycle work (validate -> stop active -> start
 * candidate -> publish to `channel_manager` -> verify at least one valid
 * read from every new/changed enabled channel, each bounded by a short
 * per-channel retry budget -- see `SENSOR_MANAGER_VERIFY_MAX_ATTEMPTS_PER_CHANNEL`
 * in sensor_manager.c) synchronously and preserves the prior registry and
 * hardware on any failure path (including a stop() failure or a failed
 * verify), restarting the previously-active bank's drivers before
 * returning. If that restart itself fails, the manager is left
 * uninitialized (`g_initialized = false`) rather than reporting success
 * with an inconsistent hardware/registry state.
 *
 * Deliberately does **not** touch the OPC UA namespace -- that remains
 * `config_service`'s commit-step extension point's job (v0.5.0 T6),
 * invoked only after this call's caller has also persisted/committed the
 * candidate to Flash.
 *
 * @param candidate New configuration to activate; validated internally
 *        (bounded strings, driver/source compatibility, resource-claim
 *        conflicts) before any hardware/driver call is made.
 * @param candidate_revision Config revision number to record as applied on
 *        success.
 * @return true if the candidate was validated, started, verified, and
 *         published as the active bank; false on any validation,
 *         driver-lifecycle, or verify failure (previous configuration
 *         remains active where possible).
 * @note Invoked either from sensor_manager_init()/sensor_manager_task()
 *       (activating the current running config) or synchronously as
 *       `config_service`'s runtime apply callback during a config
 *       transaction. As of v0.5.0 T4, internally serialized by an explicit
 *       bounded-timeout mutex (`g_activate_mutex`,
 *       `SENSOR_MANAGER_ACTIVATE_LOCK_TIMEOUT_MS`) -- safe to call from
 *       multiple tasks, though `config_service`'s own transaction lock
 *       already prevents concurrent calls reached through it.
 * @note Touches hardware indirectly through the channel drivers' `start`/
 *       `stop`/`configure`/`self_test`/`read` vtable entries.
 */
bool sensor_manager_activate_config(const device_profile_config_t *candidate,
                                    uint32_t candidate_revision);

/**
 * @brief FreeRTOS task entry point ("SensorTask") that reapplies config
 *        changes, polls all channels, and records history samples.
 *
 * Runs forever; never returns. Each iteration: re-activates the running
 * config if its revision changed since last applied, polls all channels
 * via `channel_manager_poll_all()` (which dispatches to driver `read`
 * operations), records historized-channel samples, then sleeps for the
 * shortest enabled channel's `sample_period_ms` (or a 1000 ms default).
 *
 * @param argument Unused (FreeRTOS task signature).
 */
void sensor_manager_task(void *argument);

/**
 * @brief Copies the legacy Temperature1 compatibility snapshot to @p out_snapshot.
 *
 * Always writes a fully-initialized struct: if the manager is not
 * initialized or the `Temperature1` channel is absent, quality is set to
 * SERVICE_QUALITY_BAD and the timestamp to
 * TELEMETRY_CHANNEL_SOURCE_TIMESTAMP_INVALID rather than leaving stale/
 * uninitialized data.
 *
 * @param out_snapshot Destination; no-op if NULL.
 */
void sensor_manager_get_snapshot(sensor_manager_snapshot_t *out_snapshot);

#endif

#ifndef SERVICES_CHANNEL_MANAGER_H
#define SERVICES_CHANNEL_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/channel_config.h"
#include "services/platform_limits.h"
#include "services/telemetry_driver.h"

/**
 * @brief Stable reference to one registry slot: index + generation.
 *
 * @c generation increments every time the slot's identity/settings change
 * (register, channel_manager_update_settings()), so a handle captured before
 * a reconfiguration can be detected as stale by comparing generations --
 * this is the basis of the acquire/publish/release driver-operation lease
 * pattern (see channel_manager_acquire_driver_operation()). Never persisted
 * across a channel_manager_reset().
 */
typedef struct
{
    uint8_t index;
    uint32_t generation;
} channel_manager_handle_t;

/**
 * @brief Typed result of channel_manager_write_value().
 *
 * CHANNEL_MANAGER_WRITE_RESULT_STALE specifically means a concurrent
 * reconfiguration (generation bump) invalidated the write's lease before it
 * could publish -- the write was neither applied nor corrupted, just
 * discarded; callers should treat it as "retry against current state" if a
 * retry is desired. The other RESULT_* values map from
 * telemetry_driver_result_t (see channel_manager_map_driver_result() in
 * channel_manager.c) or from channel_manager's own pre-driver-call
 * validation (NOT_FOUND, DISABLED, READ_ONLY, TYPE_MISMATCH).
 * CHANNEL_MANAGER_WRITE_RESULT_OUT_OF_RANGE means the driver refused a
 * value it cannot produce; nothing was applied or published.
 */
typedef enum
{
    CHANNEL_MANAGER_WRITE_RESULT_OK = 0,
    CHANNEL_MANAGER_WRITE_RESULT_NOT_FOUND,
    CHANNEL_MANAGER_WRITE_RESULT_DISABLED,
    CHANNEL_MANAGER_WRITE_RESULT_READ_ONLY,
    CHANNEL_MANAGER_WRITE_RESULT_TYPE_MISMATCH,
    CHANNEL_MANAGER_WRITE_RESULT_UNSUPPORTED,
    CHANNEL_MANAGER_WRITE_RESULT_STALE,
    CHANNEL_MANAGER_WRITE_RESULT_INVALID_LIFECYCLE,
    CHANNEL_MANAGER_WRITE_RESULT_TIMEOUT,
    CHANNEL_MANAGER_WRITE_RESULT_HARDWARE_FAILURE,
    CHANNEL_MANAGER_WRITE_RESULT_RESOURCE_FAILURE,
    CHANNEL_MANAGER_WRITE_RESULT_OUT_OF_RANGE
} channel_manager_write_result_t;

/**
 * @brief A leased driver operation: the slot handle, a one-shot token, and a
 *        detached channel snapshot the driver call may read/mutate.
 *
 * Returned by channel_manager_acquire_driver_operation() while the registry
 * critical section is *not* held, so the caller may invoke a driver's
 * read()/write() against @c snapshot for an arbitrary (potentially blocking)
 * duration. @c operation_token is bumped on every acquire and must match the
 * slot's current token at publish/release time; combined with @c
 * handle.generation, this rejects a publish against a slot that was reset,
 * reconfigured, or re-leased since this operation was acquired
 * (channel_manager.c:463-501). Never share one instance across concurrent
 * operations.
 */
typedef struct
{
    channel_manager_handle_t handle;
    uint32_t operation_token;
    telemetry_channel_snapshot_t snapshot;
} channel_manager_driver_operation_t;

/**
 * @brief Allocate and zero the bounded channel registry.
 *
 * Allocates the registry from PSRAM on the reference target. Failure leaves
 * the registry unavailable; it never falls back to internal SRAM.
 * Call once during platform init, before any other channel_manager_* call.
 * Not thread-safe against concurrent channel_manager calls.
 *
 * @return true on success; false if the PSRAM allocation failed.
 */
bool channel_manager_init(void);

/** @brief Whether channel_manager_init() has completed successfully. */
bool channel_manager_is_ready(void);

/**
 * @brief Clear every registered channel slot.
 *
 * Refuses while any driver operation is acquired (see
 * channel_manager_acquire_driver_operation()), which prevents a driver from
 * publishing into a recycled slot after a configuration change. Safe to call
 * from any task; internally synchronized with a global critical section.
 *
 * @return true if the registry was cleared; false if a driver operation was
 *         still in flight.
 */
bool channel_manager_reset(void);

/**
 * @brief Register one channel into a free registry slot.
 *
 * Copies @p channel, including its id/name/description/unit strings, into
 * owned registry storage — the caller's pointer is not retained past this
 * call.
 *
 * @param channel Channel definition to copy in. Not retained.
 * @param out_handle Optional; receives the slot handle (index + generation)
 *        on success.
 * @return true on success; false if the registry is full or @p channel is
 *         invalid (e.g. duplicate id).
 */
bool channel_manager_register(const telemetry_channel_t *channel,
                              channel_manager_handle_t *out_handle);

/**
 * @brief Copy the current snapshot of one channel by its stable string id.
 *
 * @param id Channel id to look up.
 * @param out_snapshot Receives an owned copy (its own id/name/description/
 *        unit buffers) of the channel's current value, quality, and source
 *        timestamp. Never a live pointer into registry storage.
 * @return true if @p id was found; false otherwise.
 */
bool channel_manager_copy_snapshot_by_id(const char *id, telemetry_channel_snapshot_t *out_snapshot);

/**
 * @brief Copy the current snapshot of one channel by registry slot index.
 *
 * @param index Registry slot index in [0, channel_manager_count()).
 * @param out_snapshot Receives an owned copy; see
 *        channel_manager_copy_snapshot_by_id().
 * @return true if @p index names an occupied slot; false otherwise.
 */
bool channel_manager_copy_snapshot_by_index(size_t index, telemetry_channel_snapshot_t *out_snapshot);

/**
 * @brief Replace a channel's mutable settings and bump its generation.
 *
 * Bumping the generation invalidates any in-flight driver operation lease
 * acquired against the previous generation (see
 * channel_manager_acquire_driver_operation()), so a stale publish is
 * rejected rather than silently applied.
 *
 * @param id Channel id to update.
 * @param settings New settings to apply (sample/publish period, offset,
 *        scale, alarm thresholds, etc.).
 * @return true on success; false if @p id was not found.
 */
bool channel_manager_update_settings(const char *id, const telemetry_channel_settings_t *settings);

/**
 * @brief Write one exact-type value to a writable channel through its driver.
 *
 * Performs one exact-type write through the configured driver. The driver
 * call happens outside the registry critical section — this function may
 * block for the duration of the driver's write() call. @p out_snapshot
 * receives a caller-owned coherent copy only after a successful
 * current-generation publication; on any failure the prior registry sample
 * is left unchanged.
 *
 * @param id Channel id to write.
 * @param type Exact value type being supplied; must match the channel's
 *        configured type or the call fails with
 *        CHANNEL_MANAGER_WRITE_RESULT_TYPE_MISMATCH.
 * @param value Value to write.
 * @param out_snapshot Optional; receives the post-write snapshot on success.
 * @return Typed result; CHANNEL_MANAGER_WRITE_RESULT_STALE means a
 *         concurrent reconfiguration invalidated this write before it could
 *         publish.
 */
channel_manager_write_result_t channel_manager_write_value(const char *id,
                                                            telemetry_channel_value_type_t type,
                                                            const telemetry_channel_value_t *value,
                                                            telemetry_channel_snapshot_t *out_snapshot);

/**
 * @brief Register a whole configuration's worth of channels in one call.
 *
 * Used by the configuration-apply runtime path after channel_manager_reset().
 *
 * @param configs Array of channel configuration entries.
 * @param config_count Number of entries in @p configs.
 * @return Number of channels successfully registered (may be less than
 *         @p config_count if an entry is invalid).
 */
size_t channel_manager_register_from_config(const channel_config_t *configs, size_t config_count);

/** @brief Number of currently registered channel slots. */
size_t channel_manager_count(void);

/**
 * @brief Acquire a generation-stamped lease on one channel slot for a driver
 *        operation.
 *
 * Takes a snapshot of the slot under the registry critical section, then
 * releases the lock — the caller may then invoke the channel's driver
 * without holding any registry lock. Pair with
 * channel_manager_publish_driver_operation() or
 * channel_manager_release_driver_operation().
 *
 * @param index Registry slot index.
 * @param out_operation Receives the leased operation (handle + token +
 *        snapshot) on success.
 * @return true if the slot was available to lease; false otherwise.
 */
bool channel_manager_acquire_driver_operation(size_t index,
                                              channel_manager_driver_operation_t *out_operation);

/**
 * @brief Publish the result of a driver operation acquired via
 *        channel_manager_acquire_driver_operation().
 *
 * Writes @p operation's snapshot back into the registry only if the slot's
 * generation and operation token still match what was leased — a concurrent
 * reconfiguration (bumped generation) causes this to be silently rejected
 * rather than corrupting state.
 *
 * @param operation The operation previously returned by
 *        channel_manager_acquire_driver_operation(), with its snapshot
 *        updated by the driver call.
 * @return true if the publish was accepted; false if the lease had gone
 *         stale.
 */
bool channel_manager_publish_driver_operation(const channel_manager_driver_operation_t *operation);

/**
 * @brief Release a driver-operation lease without publishing a result.
 *
 * Use when the driver call failed and no snapshot update should be
 * committed.
 *
 * @param operation The operation to release.
 */
void channel_manager_release_driver_operation(const channel_manager_driver_operation_t *operation);

/**
 * @brief Poll every registered channel's driver once.
 *
 * Iterates every occupied slot, acquiring/releasing the per-slot lease
 * around each driver read() call (never while the registry lock is held).
 * Intended to be called once per sampling period from sensor_manager_task().
 *
 * @param now_ms Current tick timestamp passed through to each driver's
 *        read() call.
 * @return true if at least one channel was polled; false if the registry is
 *         empty or not ready.
 */
bool channel_manager_poll_all(uint32_t now_ms);

/**
 * Observer invoked after a successful sample has been published to the
 * registry.  The supplied channel remains valid only for the call.
 */
typedef void (*channel_manager_sample_observer_t)(size_t channel_index,
                                                  const telemetry_channel_t *channel);

/**
 * Polls channels as channel_manager_poll_all(), optionally observing each
 * successfully published sample.  The observer runs outside registry locks
 * and must remain bounded; it must not retain @p channel.
 */
bool channel_manager_poll_all_observed(uint32_t now_ms,
                                       channel_manager_sample_observer_t observer);

#endif

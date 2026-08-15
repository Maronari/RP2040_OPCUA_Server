#ifndef SERVICES_CHANNEL_MANAGER_H
#define SERVICES_CHANNEL_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/channel_config.h"
#include "services/platform_limits.h"
#include "services/telemetry_driver.h"

typedef struct
{
    uint8_t index;
    uint32_t generation;
} channel_manager_handle_t;

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
    CHANNEL_MANAGER_WRITE_RESULT_RESOURCE_FAILURE
} channel_manager_write_result_t;

typedef struct
{
    channel_manager_handle_t handle;
    uint32_t operation_token;
    telemetry_channel_snapshot_t snapshot;
} channel_manager_driver_operation_t;

/* Allocates the bounded registry from PSRAM on the reference target.  Failure
 * leaves the registry unavailable; it never falls back to internal SRAM. */
bool channel_manager_init(void);
bool channel_manager_is_ready(void);
/* Reset refuses while a driver operation is acquired; this prevents a driver
 * from publishing into a recycled slot after a configuration change. */
bool channel_manager_reset(void);
bool channel_manager_register(const telemetry_channel_t *channel,
                              channel_manager_handle_t *out_handle);
bool channel_manager_copy_snapshot_by_id(const char *id, telemetry_channel_snapshot_t *out_snapshot);
bool channel_manager_copy_snapshot_by_index(size_t index, telemetry_channel_snapshot_t *out_snapshot);
bool channel_manager_update_settings(const char *id, const telemetry_channel_settings_t *settings);
/* Performs one exact-type write through the configured driver.  The driver is
 * called outside the registry critical section.  out_snapshot receives a
 * caller-owned coherent copy only after a successful current-generation
 * publication; failure never changes the prior registry sample. */
channel_manager_write_result_t channel_manager_write_value(const char *id,
                                                            telemetry_channel_value_type_t type,
                                                            const telemetry_channel_value_t *value,
                                                            telemetry_channel_snapshot_t *out_snapshot);
size_t channel_manager_register_from_config(const channel_config_t *configs, size_t config_count);
size_t channel_manager_count(void);
bool channel_manager_acquire_driver_operation(size_t index,
                                              channel_manager_driver_operation_t *out_operation);
bool channel_manager_publish_driver_operation(const channel_manager_driver_operation_t *operation);
void channel_manager_release_driver_operation(const channel_manager_driver_operation_t *operation);
bool channel_manager_poll_all(uint32_t now_ms);

#endif

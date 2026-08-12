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

typedef struct
{
    channel_manager_handle_t handle;
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
size_t channel_manager_register_from_config(const channel_config_t *configs, size_t config_count);
size_t channel_manager_count(void);
bool channel_manager_acquire_driver_operation(size_t index,
                                              channel_manager_driver_operation_t *out_operation);
bool channel_manager_publish_driver_operation(const channel_manager_driver_operation_t *operation);
void channel_manager_release_driver_operation(const channel_manager_driver_operation_t *operation);
bool channel_manager_poll_all(uint32_t now_ms);

#endif

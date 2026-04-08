#ifndef SERVICES_CHANNEL_MANAGER_H
#define SERVICES_CHANNEL_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/channel_config.h"

#define CHANNEL_MANAGER_MAX_CHANNELS 16u

void channel_manager_init(void);
void channel_manager_reset(void);
bool channel_manager_register(telemetry_channel_t *channel);
bool channel_manager_copy_channel(const char *id, telemetry_channel_t *out_channel);
bool channel_manager_update_settings(const char *id, const telemetry_channel_settings_t *settings);
size_t channel_manager_register_from_config(const channel_config_t *configs,
                                            size_t config_count,
                                            telemetry_channel_t *channel_storage,
                                            size_t channel_capacity);
size_t channel_manager_count(void);
telemetry_channel_t *channel_manager_get_by_index(size_t index);
telemetry_channel_t *channel_manager_find_by_id(const char *id);
telemetry_channel_t *channel_manager_find_by_name(const char *name);
bool channel_manager_poll_all(uint32_t now_ms);

#endif

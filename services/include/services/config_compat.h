#ifndef SERVICES_CONFIG_COMPAT_H
#define SERVICES_CONFIG_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/config_service.h"

#define CONFIG_COMPAT_V040_CHANNEL_LIMIT 4u

/* Decode the native current layout, the frozen v0.4.1/v0.4.2 layout (before
 * the v0.4.3 alarm-tuning settings fields), or a frozen four-channel v0.4.0
 * layout. */
bool config_compat_decode_binary(const uint8_t *payload,
                                 size_t payload_size,
                                 device_profile_config_t *out_config,
                                 bool *out_migrated);

size_t config_compat_v040_payload_size(void);
size_t config_compat_v042_payload_size(void);

#endif

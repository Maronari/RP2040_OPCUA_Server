#ifndef SERVICES_CHANNEL_CONFIG_H
#define SERVICES_CHANNEL_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "services/telemetry_channel.h"

typedef struct
{
    telemetry_channel_metadata_t metadata; /* copied by channel_manager */
    telemetry_channel_settings_t settings;
    const telemetry_driver_vtbl_t *driver;
    void *driver_context;
} channel_config_t;

typedef struct
{
    const channel_config_t *channels;
    size_t channel_count;
} telemetry_device_config_t;

#endif

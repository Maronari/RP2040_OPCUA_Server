#ifndef SERVICES_TELEMETRY_DRIVER_H
#define SERVICES_TELEMETRY_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "services/telemetry_channel.h"

typedef struct telemetry_driver_vtbl
{
    bool (*init)(telemetry_channel_t *channel);
    bool (*poll)(telemetry_channel_t *channel, uint32_t now_ms);
    bool (*configure)(telemetry_channel_t *channel, const void *config);
    bool (*self_test)(telemetry_channel_t *channel);
} telemetry_driver_vtbl_t;

#endif

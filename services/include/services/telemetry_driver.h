#ifndef SERVICES_TELEMETRY_DRIVER_H
#define SERVICES_TELEMETRY_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "services/telemetry_channel.h"

typedef enum
{
    TELEMETRY_DRIVER_RESULT_OK = 0,
    TELEMETRY_DRIVER_RESULT_UNSUPPORTED,
    TELEMETRY_DRIVER_RESULT_INVALID_ARGUMENT,
    TELEMETRY_DRIVER_RESULT_INVALID_STATE,
    TELEMETRY_DRIVER_RESULT_TIMEOUT,
    TELEMETRY_DRIVER_RESULT_HARDWARE_FAILURE,
    TELEMETRY_DRIVER_RESULT_NO_MEMORY
} telemetry_driver_result_t;

typedef enum
{
    TELEMETRY_DRIVER_CAPABILITY_START = (1u << 0u),
    TELEMETRY_DRIVER_CAPABILITY_STOP = (1u << 1u),
    TELEMETRY_DRIVER_CAPABILITY_READ = (1u << 2u),
    TELEMETRY_DRIVER_CAPABILITY_WRITE = (1u << 3u),
    TELEMETRY_DRIVER_CAPABILITY_CONFIGURE = (1u << 4u),
    TELEMETRY_DRIVER_CAPABILITY_SELF_TEST = (1u << 5u)
} telemetry_driver_capability_t;

typedef struct telemetry_driver_vtbl
{
    telemetry_driver_result_t (*start)(telemetry_channel_t *channel);
    telemetry_driver_result_t (*stop)(telemetry_channel_t *channel);
    telemetry_driver_result_t (*read)(telemetry_channel_t *channel, uint32_t now_ms);
    telemetry_driver_result_t (*write)(telemetry_channel_t *channel,
                                       const telemetry_channel_value_t *value);
    telemetry_driver_result_t (*configure)(telemetry_channel_t *channel, const void *config);
    telemetry_driver_result_t (*self_test)(telemetry_channel_t *channel);
} telemetry_driver_vtbl_t;

#endif

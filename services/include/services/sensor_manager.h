#ifndef SERVICES_SENSOR_MANAGER_H
#define SERVICES_SENSOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "services/service_types.h"

typedef struct
{
    bool enabled;
    float temperature_c;
    service_quality_t quality;
    uint32_t source_timestamp_epoch_s;
    uint32_t sample_count;
    uint32_t error_count;
    uint32_t last_update_ms;
} sensor_manager_snapshot_t;

bool sensor_manager_init(void);
void sensor_manager_task(void *argument);
void sensor_manager_get_snapshot(sensor_manager_snapshot_t *out_snapshot);

#endif

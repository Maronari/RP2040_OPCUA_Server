#ifndef SERVICES_ALARM_SERVICE_H
#define SERVICES_ALARM_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/service_types.h"

typedef struct
{
    bool high_temperature_active;
    bool high_high_temperature_active;
    bool acknowledged;
    service_quality_t quality;
    float high_threshold_c;
    float high_high_threshold_c;
    temperature_alarm_state_t alarm_state;
    uint32_t evaluation_count;
    uint32_t trip_count;
} alarm_service_status_t;

bool alarm_service_init(void);
void alarm_service_task(void *argument);
void alarm_service_get_status(alarm_service_status_t *out_status);
void alarm_service_acknowledge_all(void);

#endif

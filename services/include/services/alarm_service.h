#ifndef SERVICES_ALARM_SERVICE_H
#define SERVICES_ALARM_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "services/service_types.h"

typedef struct
{
    const char *channel_id;
    const char *channel_name;
    bool warning_active;
    bool critical_active;
    bool acknowledged;
    service_quality_t quality;
    float warning_threshold;
    float critical_threshold;
    telemetry_alarm_state_t state;
    uint32_t evaluation_count;
    uint32_t trip_count;
} alarm_service_status_t;

bool alarm_service_init(void);
void alarm_service_task(void *argument);
void alarm_service_get_status(alarm_service_status_t *out_status);
void alarm_service_acknowledge_all(void);

#endif

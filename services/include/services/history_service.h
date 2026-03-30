#ifndef SERVICES_HISTORY_SERVICE_H
#define SERVICES_HISTORY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/service_types.h"

#define HISTORY_SERVICE_TEMPERATURE_CAPACITY 512u

typedef struct
{
    float value;
    service_quality_t quality;
    uint32_t source_timestamp_epoch_s;
} history_service_temperature_sample_t;

bool history_service_init(void);
void history_service_reset(void);
bool history_service_record_temperature_sample(float value,
                                               service_quality_t quality,
                                               uint32_t source_timestamp_epoch_s);
size_t history_service_read_temperature_samples(uint32_t start_epoch_s,
                                                uint32_t end_epoch_s,
                                                bool descending,
                                                size_t max_samples,
                                                history_service_temperature_sample_t *out_samples,
                                                size_t out_capacity,
                                                bool *out_truncated);

#endif

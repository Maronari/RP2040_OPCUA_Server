#ifndef SERVICES_HISTORY_SERVICE_H
#define SERVICES_HISTORY_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/service_types.h"
#include "services/telemetry_channel.h"

#define HISTORY_SERVICE_CHANNEL_CAPACITY 512u
#define HISTORY_SERVICE_CHANNEL_ID_MAX_LEN TELEMETRY_CHANNEL_ID_MAX_LEN
typedef struct
{
    char channel_id[HISTORY_SERVICE_CHANNEL_ID_MAX_LEN];
    float value;
    service_quality_t quality;
    uint64_t source_timestamp_unix_us;
    uint32_t recorded_at_ms;
} history_service_channel_sample_t;

bool history_service_init(void);
void history_service_reset(void);
bool history_service_record_channel_sample(const char *channel_id,
                                          float value,
                                          service_quality_t quality,
                                          uint64_t source_timestamp_unix_us,
                                          uint32_t record_interval_ms);
size_t history_service_read_channel_samples(const char *channel_id,
                                           uint32_t start_epoch_s,
                                           uint32_t end_epoch_s,
                                           bool descending,
                                           size_t max_samples,
                                           history_service_channel_sample_t *out_samples,
                                           size_t out_capacity,
                                           bool *out_truncated);

#endif

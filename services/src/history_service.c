#include "services/history_service.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "services/config_service.h"

typedef struct
{
    history_service_temperature_sample_t samples[HISTORY_SERVICE_TEMPERATURE_CAPACITY];
    size_t count;
    size_t write_index;
    uint32_t last_recorded_ms;
} history_service_temperature_ring_t;

static history_service_temperature_ring_t g_temperature_history;
static bool g_initialized = false;

static size_t history_service_oldest_index(void)
{
    if (g_temperature_history.count >= HISTORY_SERVICE_TEMPERATURE_CAPACITY)
    {
        return g_temperature_history.write_index;
    }

    return 0u;
}

static bool history_service_sample_in_range(uint32_t sample_epoch_s,
                                            uint32_t start_epoch_s,
                                            uint32_t end_epoch_s)
{
    uint32_t lower_bound = 0u;
    uint32_t upper_bound = 0u;

    if (start_epoch_s == 0u && end_epoch_s == 0u)
    {
        return true;
    }

    if (start_epoch_s == 0u)
    {
        return sample_epoch_s <= end_epoch_s;
    }

    if (end_epoch_s == 0u)
    {
        return sample_epoch_s >= start_epoch_s;
    }

    lower_bound = start_epoch_s < end_epoch_s ? start_epoch_s : end_epoch_s;
    upper_bound = start_epoch_s > end_epoch_s ? start_epoch_s : end_epoch_s;
    return sample_epoch_s >= lower_bound && sample_epoch_s <= upper_bound;
}

bool history_service_init(void)
{
    history_service_reset();
    g_initialized = true;
    return true;
}

void history_service_reset(void)
{
    taskENTER_CRITICAL();
    memset(&g_temperature_history, 0, sizeof(g_temperature_history));
    taskEXIT_CRITICAL();
}

bool history_service_record_temperature_sample(float value,
                                               service_quality_t quality,
                                               uint32_t source_timestamp_epoch_s)
{
    const device_profile_config_t *config = config_service_get();
    const uint32_t history_interval_ms = config->temperature_history_interval_ms;
    const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool recorded = false;

    if (!g_initialized || history_interval_ms == 0u ||
        quality != SERVICE_QUALITY_GOOD || source_timestamp_epoch_s == 0u)
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (g_temperature_history.count == 0u ||
        (uint32_t)(now_ms - g_temperature_history.last_recorded_ms) >= history_interval_ms)
    {
        g_temperature_history.samples[g_temperature_history.write_index].value = value;
        g_temperature_history.samples[g_temperature_history.write_index].quality = quality;
        g_temperature_history.samples[g_temperature_history.write_index].source_timestamp_epoch_s =
            source_timestamp_epoch_s;
        g_temperature_history.write_index =
            (g_temperature_history.write_index + 1u) % HISTORY_SERVICE_TEMPERATURE_CAPACITY;
        if (g_temperature_history.count < HISTORY_SERVICE_TEMPERATURE_CAPACITY)
        {
            g_temperature_history.count++;
        }
        g_temperature_history.last_recorded_ms = now_ms;
        recorded = true;
    }
    taskEXIT_CRITICAL();

    return recorded;
}

size_t history_service_read_temperature_samples(uint32_t start_epoch_s,
                                                uint32_t end_epoch_s,
                                                bool descending,
                                                size_t max_samples,
                                                history_service_temperature_sample_t *out_samples,
                                                size_t out_capacity,
                                                bool *out_truncated)
{
    size_t collected = 0u;
    size_t scan_limit = 0u;
    size_t oldest_index = 0u;
    size_t requested_limit = max_samples;
    bool truncated = false;

    if (out_truncated != NULL)
    {
        *out_truncated = false;
    }

    if (!g_initialized || out_samples == NULL || out_capacity == 0u)
    {
        return 0u;
    }

    requested_limit = (requested_limit == 0u || requested_limit > out_capacity)
                          ? out_capacity
                          : requested_limit;

    taskENTER_CRITICAL();
    scan_limit = g_temperature_history.count;
    oldest_index = history_service_oldest_index();

    for (size_t offset = 0u; offset < scan_limit; ++offset)
    {
        size_t sample_index = 0u;
        history_service_temperature_sample_t sample;

        if (descending)
        {
            sample_index = (oldest_index + scan_limit - 1u - offset) % HISTORY_SERVICE_TEMPERATURE_CAPACITY;
        }
        else
        {
            sample_index = (oldest_index + offset) % HISTORY_SERVICE_TEMPERATURE_CAPACITY;
        }

        sample = g_temperature_history.samples[sample_index];
        if (!history_service_sample_in_range(sample.source_timestamp_epoch_s, start_epoch_s, end_epoch_s))
        {
            continue;
        }

        if (collected >= requested_limit)
        {
            truncated = true;
            continue;
        }

        out_samples[collected++] = sample;
    }
    taskEXIT_CRITICAL();

    if (out_truncated != NULL)
    {
        *out_truncated = truncated;
    }

    return collected;
}

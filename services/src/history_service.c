#include "services/history_service.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

typedef struct
{
    history_service_channel_sample_t samples[HISTORY_SERVICE_CHANNEL_CAPACITY];
    size_t count;
    size_t write_index;
} history_service_channel_ring_t;

static history_service_channel_ring_t g_channel_history;
static bool g_initialized = false;

static size_t history_service_oldest_index(void)
{
    if (g_channel_history.count >= HISTORY_SERVICE_CHANNEL_CAPACITY)
    {
        return g_channel_history.write_index;
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

static bool history_service_channel_ids_equal(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL)
    {
        return false;
    }

    return strcmp(lhs, rhs) == 0;
}

static uint32_t history_service_find_last_recorded_ms(const char *channel_id)
{
    const size_t oldest_index = history_service_oldest_index();

    for (size_t offset = 0u; offset < g_channel_history.count; ++offset)
    {
        const size_t sample_index =
            (oldest_index + g_channel_history.count - 1u - offset) % HISTORY_SERVICE_CHANNEL_CAPACITY;
        const history_service_channel_sample_t *sample = &g_channel_history.samples[sample_index];

        if (history_service_channel_ids_equal(sample->channel_id, channel_id))
        {
            return sample->recorded_at_ms;
        }
    }

    return 0u;
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
    memset(&g_channel_history, 0, sizeof(g_channel_history));
    taskEXIT_CRITICAL();
}

bool history_service_record_channel_sample(const char *channel_id,
                                          float value,
                                          service_quality_t quality,
                                          uint32_t source_timestamp_epoch_s,
                                          uint32_t record_interval_ms)
{
    const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool recorded = false;

    if (!g_initialized || channel_id == NULL || channel_id[0] == '\0' ||
        record_interval_ms == 0u || quality != SERVICE_QUALITY_GOOD || source_timestamp_epoch_s == 0u)
    {
        return false;
    }

    taskENTER_CRITICAL();
    {
        const uint32_t last_recorded_ms = history_service_find_last_recorded_ms(channel_id);

        if (g_channel_history.count == 0u ||
            last_recorded_ms == 0u ||
            (uint32_t)(now_ms - last_recorded_ms) >= record_interval_ms)
        {
            history_service_channel_sample_t *sample = &g_channel_history.samples[g_channel_history.write_index];

            sample->channel_id = channel_id;
            sample->value = value;
            sample->quality = quality;
            sample->source_timestamp_epoch_s = source_timestamp_epoch_s;
            sample->recorded_at_ms = now_ms;

            g_channel_history.write_index =
                (g_channel_history.write_index + 1u) % HISTORY_SERVICE_CHANNEL_CAPACITY;
            if (g_channel_history.count < HISTORY_SERVICE_CHANNEL_CAPACITY)
            {
                g_channel_history.count++;
            }
            recorded = true;
        }
    }
    taskEXIT_CRITICAL();

    return recorded;
}

size_t history_service_read_channel_samples(const char *channel_id,
                                           uint32_t start_epoch_s,
                                           uint32_t end_epoch_s,
                                           bool descending,
                                           size_t max_samples,
                                           history_service_channel_sample_t *out_samples,
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

    if (!g_initialized || channel_id == NULL || out_samples == NULL || out_capacity == 0u)
    {
        return 0u;
    }

    requested_limit = (requested_limit == 0u || requested_limit > out_capacity)
                          ? out_capacity
                          : requested_limit;

    taskENTER_CRITICAL();
    scan_limit = g_channel_history.count;
    oldest_index = history_service_oldest_index();

    for (size_t offset = 0u; offset < scan_limit; ++offset)
    {
        size_t sample_index = 0u;
        history_service_channel_sample_t sample;

        if (descending)
        {
            sample_index = (oldest_index + scan_limit - 1u - offset) % HISTORY_SERVICE_CHANNEL_CAPACITY;
        }
        else
        {
            sample_index = (oldest_index + offset) % HISTORY_SERVICE_CHANNEL_CAPACITY;
        }

        sample = g_channel_history.samples[sample_index];
        if (!history_service_channel_ids_equal(sample.channel_id, channel_id) ||
            !history_service_sample_in_range(sample.source_timestamp_epoch_s, start_epoch_s, end_epoch_s))
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

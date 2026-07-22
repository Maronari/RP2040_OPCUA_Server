#include "services/device_health_service.h"

#include <stdio.h>
#include <string.h>

#include "services/config_import_service.h"
#include "services/diag_service.h"
#include "services/firmware_update_service.h"
#include "pico.h"
#include "ua_port_alloc.h"

#define DEVICE_HEALTH_FREE_HEAP_WARN_BYTES 32768u
#define DEVICE_HEALTH_LARGEST_HEAP_BLOCK_WARN_BYTES 16384u
#define DEVICE_HEALTH_PSRAM_FREE_DIVISOR 16u
#define DEVICE_HEALTH_PSRAM_LARGEST_BLOCK_DIVISOR 32u

static void device_health_append_reason(char *buffer,
                                        size_t buffer_size,
                                        const char *reason)
{
    size_t used;

    if (buffer == NULL || buffer_size == 0u || reason == NULL)
    {
        return;
    }

    used = strlen(buffer);
    if (used >= buffer_size - 1u)
    {
        return;
    }

    (void)snprintf(buffer + used,
                   buffer_size - used,
                   "%s%s",
                   used == 0u ? "" : "; ",
                   reason);
}

void device_health_service_get_report(device_health_report_t *out_report)
{
    diag_service_heap_stats_t heap = {0};
    diag_service_system_stats_t system = {0};
    firmware_update_service_status_t firmware = {0};
    config_import_service_status_t config_import = {0};
    ua_port_alloc_stats_t psram = {0};
    uint32_t failure_reasons = 0u;
    uint32_t check_function_reasons = 0u;
    uint32_t off_spec_reasons = 0u;
    uint32_t maintenance_required_reasons = 0u;

    if (out_report == NULL)
    {
        return;
    }

    memset(out_report, 0, sizeof(*out_report));
    diag_service_get_heap_stats(&heap);
    diag_service_get_system_stats(&system);
    firmware_update_service_get_status(&firmware);
    config_import_service_get_status(&config_import);
    ua_port_alloc_get_stats(&psram);

    if (system.last_fault_code != DIAG_FAULT_CODE_NONE)
    {
        failure_reasons |= DEVICE_HEALTH_REASON_RUNTIME_FAULT;
    }

#if defined(WAVESHARE_CORE2350B_16MB_PSRAM8MB)
    if (!psram.psram_ready)
    {
        failure_reasons |= DEVICE_HEALTH_REASON_PSRAM_UNAVAILABLE;
    }
#endif

    if (firmware.session_active)
    {
        check_function_reasons |= DEVICE_HEALTH_REASON_FIRMWARE_UPDATE;
    }
    if (config_import.session_active)
    {
        check_function_reasons |= DEVICE_HEALTH_REASON_CONFIG_IMPORT;
    }

    if (heap.available_heap_bytes < DEVICE_HEALTH_FREE_HEAP_WARN_BYTES)
    {
        off_spec_reasons |= DEVICE_HEALTH_REASON_LOW_FREE_HEAP;
    }
    if (heap.largest_free_block_bytes < DEVICE_HEALTH_LARGEST_HEAP_BLOCK_WARN_BYTES)
    {
        off_spec_reasons |= DEVICE_HEALTH_REASON_FRAGMENTED_HEAP;
    }
    if (psram.psram_ready && psram.total_bytes > 0u)
    {
        if (psram.free_bytes < (psram.total_bytes / DEVICE_HEALTH_PSRAM_FREE_DIVISOR))
        {
            off_spec_reasons |= DEVICE_HEALTH_REASON_LOW_FREE_PSRAM;
        }
        if (psram.largest_free_block_bytes <
            (psram.total_bytes / DEVICE_HEALTH_PSRAM_LARGEST_BLOCK_DIVISOR))
        {
            off_spec_reasons |= DEVICE_HEALTH_REASON_FRAGMENTED_PSRAM;
        }
    }

    out_report->last_fault_code = system.last_fault_code;
    out_report->failure_reasons = failure_reasons;
    out_report->check_function_reasons = check_function_reasons;
    out_report->off_spec_reasons = off_spec_reasons;
    out_report->maintenance_required_reasons = maintenance_required_reasons;
    out_report->reasons = failure_reasons | check_function_reasons |
                          off_spec_reasons | maintenance_required_reasons;
    out_report->free_heap_bytes = heap.available_heap_bytes;
    out_report->largest_heap_block_bytes = heap.largest_free_block_bytes;
    out_report->free_psram_bytes = (uint32_t)psram.free_bytes;
    out_report->largest_psram_block_bytes = (uint32_t)psram.largest_free_block_bytes;
    out_report->psram_ready = psram.psram_ready;

    if (failure_reasons != 0u)
    {
        out_report->state = DEVICE_HEALTH_FAILURE;
    }
    else if (check_function_reasons != 0u)
    {
        out_report->state = DEVICE_HEALTH_CHECK_FUNCTION;
    }
    else if (off_spec_reasons != 0u)
    {
        out_report->state = DEVICE_HEALTH_OFF_SPEC;
    }
    else if (maintenance_required_reasons != 0u)
    {
        out_report->state = DEVICE_HEALTH_MAINTENANCE_REQUIRED;
    }
    else
    {
        out_report->state = DEVICE_HEALTH_NORMAL;
    }
}

uint32_t device_health_service_reasons_for_state(const device_health_report_t *report,
                                                 device_health_state_t state)
{
    if (report == NULL)
    {
        return DEVICE_HEALTH_REASON_NONE;
    }

    switch (state)
    {
    case DEVICE_HEALTH_FAILURE:
        return report->failure_reasons;
    case DEVICE_HEALTH_CHECK_FUNCTION:
        return report->check_function_reasons;
    case DEVICE_HEALTH_OFF_SPEC:
        return report->off_spec_reasons;
    case DEVICE_HEALTH_MAINTENANCE_REQUIRED:
        return report->maintenance_required_reasons;
    default:
        return DEVICE_HEALTH_REASON_NONE;
    }
}

const char *device_health_service_state_to_string(device_health_state_t state)
{
    switch (state)
    {
    case DEVICE_HEALTH_NORMAL:
        return "NORMAL";
    case DEVICE_HEALTH_FAILURE:
        return "FAILURE";
    case DEVICE_HEALTH_CHECK_FUNCTION:
        return "CHECK_FUNCTION";
    case DEVICE_HEALTH_OFF_SPEC:
        return "OFF_SPEC";
    case DEVICE_HEALTH_MAINTENANCE_REQUIRED:
        return "MAINTENANCE_REQUIRED";
    default:
        return "UNKNOWN";
    }
}

void device_health_service_format_summary(const device_health_report_t *report,
                                          char *buffer,
                                          size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u)
    {
        return;
    }

    buffer[0] = '\0';
    if (report == NULL || report->reasons == DEVICE_HEALTH_REASON_NONE)
    {
        (void)snprintf(buffer, buffer_size, "Device health normal");
        return;
    }

    if ((report->reasons & DEVICE_HEALTH_REASON_RUNTIME_FAULT) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "runtime fault recorded");
    }
    if ((report->reasons & DEVICE_HEALTH_REASON_PSRAM_UNAVAILABLE) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "required PSRAM unavailable");
    }
    if ((report->reasons & DEVICE_HEALTH_REASON_LOW_FREE_HEAP) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "SRAM capacity near limit");
    }
    if ((report->reasons & DEVICE_HEALTH_REASON_FRAGMENTED_HEAP) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "SRAM fragmented");
    }
    if ((report->reasons & DEVICE_HEALTH_REASON_LOW_FREE_PSRAM) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "PSRAM capacity near limit");
    }
    if ((report->reasons & DEVICE_HEALTH_REASON_FRAGMENTED_PSRAM) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "PSRAM fragmented");
    }
    if ((report->reasons & DEVICE_HEALTH_REASON_FIRMWARE_UPDATE) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "firmware update in progress");
    }
    if ((report->reasons & DEVICE_HEALTH_REASON_CONFIG_IMPORT) != 0u)
    {
        device_health_append_reason(buffer, buffer_size, "configuration import in progress");
    }
}

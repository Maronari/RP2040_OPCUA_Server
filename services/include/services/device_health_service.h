#ifndef SERVICES_DEVICE_HEALTH_SERVICE_H
#define SERVICES_DEVICE_HEALTH_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Values match OPC UA DI DeviceHealthEnumeration (OPC 10000-100). */
typedef enum
{
    DEVICE_HEALTH_NORMAL = 0,
    DEVICE_HEALTH_FAILURE = 1,
    DEVICE_HEALTH_CHECK_FUNCTION = 2,
    DEVICE_HEALTH_OFF_SPEC = 3,
    DEVICE_HEALTH_MAINTENANCE_REQUIRED = 4
} device_health_state_t;

typedef enum
{
    DEVICE_HEALTH_REASON_NONE = 0u,
    DEVICE_HEALTH_REASON_RUNTIME_FAULT = 1u << 0,
    DEVICE_HEALTH_REASON_PSRAM_UNAVAILABLE = 1u << 1,
    DEVICE_HEALTH_REASON_LOW_FREE_HEAP = 1u << 2,
    DEVICE_HEALTH_REASON_FRAGMENTED_HEAP = 1u << 3,
    DEVICE_HEALTH_REASON_LOW_FREE_PSRAM = 1u << 4,
    DEVICE_HEALTH_REASON_FRAGMENTED_PSRAM = 1u << 5,
    DEVICE_HEALTH_REASON_FIRMWARE_UPDATE = 1u << 6,
    DEVICE_HEALTH_REASON_CONFIG_IMPORT = 1u << 7
} device_health_reason_t;

typedef struct
{
    device_health_state_t state;
    uint32_t reasons;
    uint32_t failure_reasons;
    uint32_t check_function_reasons;
    uint32_t off_spec_reasons;
    uint32_t maintenance_required_reasons;
    uint32_t last_fault_code;
    uint32_t free_heap_bytes;
    uint32_t largest_heap_block_bytes;
    uint32_t free_psram_bytes;
    uint32_t largest_psram_block_bytes;
    bool psram_ready;
} device_health_report_t;

void device_health_service_get_report(device_health_report_t *out_report);
uint32_t device_health_service_reasons_for_state(const device_health_report_t *report,
                                                 device_health_state_t state);
const char *device_health_service_state_to_string(device_health_state_t state);
void device_health_service_format_summary(const device_health_report_t *report,
                                          char *buffer,
                                          size_t buffer_size);

#endif

#ifndef SERVICES_DEVICE_HEALTH_SERVICE_H
#define SERVICES_DEVICE_HEALTH_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Aggregate device health state. Values match OPC UA DI
 *        DeviceHealthEnumeration (OPC 10000-100) exactly, so this enum can
 *        be cast directly into the DI NodeSet's DeviceHealth variable.
 *
 * device_health_service_get_report() picks the *worst* applicable state:
 * FAILURE > CHECK_FUNCTION > OFF_SPEC > MAINTENANCE_REQUIRED > NORMAL (see
 * the priority chain in device_health_service.c).
 */
typedef enum
{
    DEVICE_HEALTH_NORMAL = 0,
    DEVICE_HEALTH_FAILURE = 1,
    DEVICE_HEALTH_CHECK_FUNCTION = 2,
    DEVICE_HEALTH_OFF_SPEC = 3,
    DEVICE_HEALTH_MAINTENANCE_REQUIRED = 4
} device_health_state_t;

/**
 * @brief Bitmask of individual contributing reasons behind a
 *        device_health_state_t, firmware-local (no OPC UA DI equivalent).
 *
 * Each bit maps to exactly one device_health_state_t bucket -- see
 * device_health_service_reasons_for_state() and the
 * failure_reasons/check_function_reasons/off_spec_reasons/
 * maintenance_required_reasons fields of device_health_report_t.
 */
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

/**
 * @brief One point-in-time device health snapshot, freshly computed on every
 *        device_health_service_get_report() call (this service holds no
 *        state of its own between calls).
 *
 * @c reasons is the union of the four per-bucket reason fields; @c state is
 * the worst bucket that has any reason bit set (NORMAL if none do).
 */
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

/**
 * @brief Compute a fresh device health report by aggregating diag_service,
 *        firmware_update_service, config_import_service, and PSRAM
 *        allocator statistics.
 *
 * Stateless aggregator: recomputes everything from those services' current
 * state on every call; does not itself touch hardware or retain state.
 * Intended to back the OPC UA DI DeviceHealth-family nodes.
 *
 * @param out_report Must not be NULL; fully overwritten.
 */
void device_health_service_get_report(device_health_report_t *out_report);
/**
 * @brief Look up the reason bitmask contributing to a specific
 *        device_health_state_t bucket within an already-computed report.
 *
 * @param report Report previously filled by device_health_service_get_report().
 * @param state Bucket to query.
 * @return The matching reason field (e.g. report->failure_reasons for
 *         DEVICE_HEALTH_FAILURE); DEVICE_HEALTH_REASON_NONE if @p report is
 *         NULL or @p state is not one of the four bucket states.
 */
uint32_t device_health_service_reasons_for_state(const device_health_report_t *report,
                                                 device_health_state_t state);
/** @brief Human-readable name for a device_health_state_t (e.g. "FAILURE"); "UNKNOWN" for an unrecognized value. Never NULL. */
const char *device_health_service_state_to_string(device_health_state_t state);
/**
 * @brief Format a semicolon-separated, human-readable summary of every
 *        reason bit set in @p report into @p buffer.
 *
 * Writes "Device health normal" if @p report is NULL or has no reasons set.
 * Truncates silently at @p buffer_size (each appended reason is skipped
 * once it would no longer fit) rather than overflowing.
 *
 * @param report Report to summarize; may be NULL.
 * @param buffer Destination buffer; NUL-terminated on return.
 * @param buffer_size Size of @p buffer in bytes.
 */
void device_health_service_format_summary(const device_health_report_t *report,
                                          char *buffer,
                                          size_t buffer_size);

#endif

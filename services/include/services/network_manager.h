/**
 * @file
 * @brief Ethernet/network bring-up and status reporting service, layered on
 *        `drivers/w5x00_ethernet_driver` and lwIP.
 *
 * Responsibilities:
 * - One-time driver init + DHCP/static IPv4 configuration at boot
 *   (network_manager_init()).
 * - Periodic driver polling and status refresh from the "NetworkTask"
 *   FreeRTOS task (network_manager_task()).
 * - Thread-safe status snapshot for other tasks/OPC UA adapter reads
 *   (network_manager_get_status()).
 *
 * `network_manager_task()` calling `w5x00_ethernet_driver_poll()` directly
 * from a services-layer task is the intended/documented driver-call
 * direction (see docs/code/hardware.md, "W5500 / SPI / networking chain"),
 * not a layering violation.
 */
#ifndef SERVICES_NETWORK_MANAGER_H
#define SERVICES_NETWORK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "lwip/netif.h"

#include "services/service_types.h"

/**
 * @brief Point-in-time snapshot of network link/IP/DHCP status.
 *
 * Populated by `network_manager_update_status()` (network_manager.c) from
 * driver/lwIP state and published to the single owned `g_status` under a
 * critical section; obtained by callers via network_manager_get_status().
 * `quality` is derived: GOOD if an IPv4 address is assigned, UNCERTAIN if
 * the link is up but no IPv4 yet, BAD otherwise.
 */
typedef struct
{
    bool initialized;
    bool dhcp_enabled;
    bool link_up;
    bool has_ipv4;
    service_quality_t quality;
    uint32_t reconnect_count;
    uint32_t rx_packets;
    uint32_t rx_errors;
    uint32_t input_errors;
    uint32_t link_state_changes;
    char mac_address[18];
    char ip_mode[8];
    char ipv4_address[16];
    char netmask[16];
    char gateway[16];
    char dhcp_state[24];
} network_manager_status_t;

/**
 * @brief Initializes the W5500 Ethernet driver and starts DHCP or applies
 *        static IPv4 configuration, per `config_service_get()`.
 *
 * Must be called once at boot before network_manager_task() is started.
 * Not safe to call concurrently with itself or with network_manager_task().
 *
 * @return true if the underlying driver initialized successfully; false if
 *         `w5x00_ethernet_driver_init()` failed (DHCP start failure alone
 *         does not fail this call - it only logs).
 *
 * @note Reads network configuration via `config_service_get()`; does not
 *       read hardware/config directly.
 */
bool network_manager_init(void);

/**
 * @brief FreeRTOS task entry point ("NetworkTask") that polls the Ethernet
 *        driver and refreshes published status every 10 ms.
 *
 * Runs forever; never returns. No-ops (skips polling) until
 * network_manager_init() has completed successfully. Drives
 * `w5x00_ethernet_driver_poll()`, which feeds received frames into lwIP.
 *
 * @param argument Unused (FreeRTOS task signature).
 */
void network_manager_task(void *argument);

/**
 * @brief Copies the current network status snapshot to @p out_status.
 *
 * Thread-safe: reads the shared status under `taskENTER_CRITICAL`/
 * `taskEXIT_CRITICAL`. No-op if @p out_status is NULL.
 *
 * @param out_status Destination for the copied status; must be non-NULL.
 */
void network_manager_get_status(network_manager_status_t *out_status);

/**
 * @brief Returns the lwIP netif owned by the underlying Ethernet driver.
 *
 * @return Pointer to the driver-owned `struct netif`; never allocates or
 *         transfers ownership. Valid only after network_manager_init().
 */
struct netif *network_manager_get_netif(void);

/**
 * @brief Reports whether the network interface currently has an IPv4 address.
 *
 * Convenience wrapper over network_manager_get_status(); thread-safe for
 * the same reason.
 *
 * @return true if the last-known status has `has_ipv4` set.
 */
bool network_manager_is_ready(void);

#endif

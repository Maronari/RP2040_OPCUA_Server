/**
 * @file
 * @brief Ethernet/network bring-up and status polling; see
 *        services/include/services/network_manager.h for the public API
 *        contract.
 *
 * Responsibilities:
 * - Drive `drivers/w5x00_ethernet_driver` init/poll and reflect its state
 *   plus lwIP DHCP state into a published `network_manager_status_t`.
 * - Own DHCP-state-change/IPv4-assignment console logging.
 *
 * Ownership:
 * `g_driver` (the W5500 driver instance) and `g_status` (the published
 * status snapshot) are both owned exclusively by this module.
 *
 * Concurrency:
 * `g_status` is written by `network_manager_update_status()` (called only
 * from "NetworkTask" via network_manager_task(), and once synchronously
 * from network_manager_init()) and read by network_manager_get_status()
 * from any task; both sides use `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`.
 * `g_driver` and the remaining link-tracking statics are touched only from
 * "NetworkTask" and from network_manager_init() before the task starts, so
 * they need no separate lock.
 */
#include "services/network_manager.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "drivers/w5x00_ethernet_driver.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/dhcp.h"
#include "services/config_service.h"

static w5x00_ethernet_driver_t g_driver;
static network_manager_status_t g_status;
static bool g_initialized = false;
static bool g_last_link_up = false;
static bool g_link_state_initialized = false;
static uint32_t g_link_state_changes = 0;
static uint32_t g_reconnect_count = 0;

/** @brief Maps an lwIP `DHCP_STATE_*` value to a human-readable string for status reporting/logging. */
static const char *network_manager_dhcp_state_to_string(uint8_t state)
{
    switch (state)
    {
    case DHCP_STATE_OFF:
        return "Off";
    case DHCP_STATE_INIT:
        return "Init";
    case DHCP_STATE_SELECTING:
        return "Selecting";
    case DHCP_STATE_REQUESTING:
        return "Requesting";
    case DHCP_STATE_BOUND:
        return "Bound";
    case DHCP_STATE_RENEWING:
        return "Renewing";
    case DHCP_STATE_REBINDING:
        return "Rebinding";
    case DHCP_STATE_REBOOTING:
        return "Rebooting";
    case DHCP_STATE_CHECKING:
        return "Checking";
    case DHCP_STATE_INFORMING:
        return "Informing";
    case DHCP_STATE_PERMANENT:
        return "Permanent";
    case DHCP_STATE_RELEASING:
        return "Releasing";
    case DHCP_STATE_BACKING_OFF:
        return "BackingOff";
    default:
        return "Unknown";
    }
}

/**
 * @brief Formats a 6-byte MAC address as "XX:XX:XX:XX:XX:XX" into @p out_buffer.
 * @param out_buffer Destination buffer; no-op if NULL.
 * @param out_buffer_size Size of @p out_buffer; no-op if 0.
 * @param mac 6-byte MAC address; no-op if NULL.
 */
static void network_manager_format_mac(char *out_buffer, size_t out_buffer_size, const uint8_t mac[6])
{
    if (out_buffer == NULL || out_buffer_size == 0u || mac == NULL)
    {
        return;
    }

    snprintf(out_buffer,
             out_buffer_size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

/**
 * @brief Rebuilds the network status snapshot from driver/lwIP state and
 *        publishes it to `g_status`, logging DHCP state/IPv4 transitions.
 *
 * Builds a local `status` from `g_driver`/lwIP (unlocked - only ever called
 * from "NetworkTask" or synchronously from network_manager_init(), before
 * the task starts), tracks link up/down transitions to update
 * `g_link_state_changes`/`g_reconnect_count`, then publishes atomically to
 * `g_status` under `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`. Logs to stdout
 * on DHCP state change or new DHCP IPv4 assignment (comparison against the
 * previous `g_status`, read outside the lock after publish).
 *
 * @note Not safe to call concurrently with itself; relies on being the only
 *       writer of `g_status` and the link-tracking statics.
 */
static void network_manager_update_status(void)
{
    const device_profile_config_t *config = config_service_get();
    network_manager_status_t status = {0};
    struct netif *netif = &g_driver.netif;
    const struct dhcp *dhcp = netif_dhcp_data(netif);
    bool dhcp_state_changed;
    bool dhcp_ipv4_assigned;

    status.initialized = g_initialized;
    status.dhcp_enabled = config->network_mode == NETWORK_MODE_DHCP;
    status.link_up = w5x00_ethernet_driver_is_link_up(&g_driver);
    status.has_ipv4 = w5x00_ethernet_driver_has_ipv4(&g_driver);
    status.rx_packets = g_driver.rx_packets;
    status.rx_errors = g_driver.rx_errors;
    status.input_errors = g_driver.input_errors;
    status.reconnect_count = g_reconnect_count;
    network_manager_format_mac(status.mac_address, sizeof(status.mac_address), g_driver.mac);
    strncpy(status.ip_mode, status.dhcp_enabled ? "DHCP" : "STATIC", sizeof(status.ip_mode));
    status.ip_mode[sizeof(status.ip_mode) - 1] = '\0';

    if (!g_link_state_initialized)
    {
        g_last_link_up = status.link_up;
        g_link_state_initialized = true;
    }
    else if (status.link_up != g_last_link_up)
    {
        g_link_state_changes++;
        if (status.link_up)
        {
            g_reconnect_count++;
        }
        g_last_link_up = status.link_up;
    }
    status.link_state_changes = g_link_state_changes;
    status.reconnect_count = g_reconnect_count;

    if (status.has_ipv4)
    {
        ip4addr_ntoa_r(netif_ip4_addr(netif), status.ipv4_address, sizeof(status.ipv4_address));
    }
    else
    {
        strncpy(status.ipv4_address, "0.0.0.0", sizeof(status.ipv4_address));
        status.ipv4_address[sizeof(status.ipv4_address) - 1] = '\0';
    }

    ip4addr_ntoa_r(netif_ip4_netmask(netif), status.netmask, sizeof(status.netmask));
    ip4addr_ntoa_r(netif_ip4_gw(netif), status.gateway, sizeof(status.gateway));

    if (status.dhcp_enabled)
    {
        strncpy(status.dhcp_state,
                dhcp != NULL ? network_manager_dhcp_state_to_string(dhcp->state) : "Initializing",
                sizeof(status.dhcp_state));
    }
    else
    {
        strncpy(status.dhcp_state, "Disabled", sizeof(status.dhcp_state));
    }
    status.dhcp_state[sizeof(status.dhcp_state) - 1] = '\0';

    if (status.has_ipv4)
    {
        status.quality = SERVICE_QUALITY_GOOD;
    }
    else if (status.link_up)
    {
        status.quality = SERVICE_QUALITY_UNCERTAIN;
    }
    else
    {
        status.quality = SERVICE_QUALITY_BAD;
    }

    /* g_status is the single previous network observation; no extra history is kept. */
    dhcp_state_changed = status.dhcp_enabled &&
                         (!g_status.dhcp_enabled ||
                          strcmp(status.dhcp_state, g_status.dhcp_state) != 0);
    dhcp_ipv4_assigned = status.dhcp_enabled && status.has_ipv4 &&
                         (!g_status.has_ipv4 ||
                          strcmp(status.ipv4_address, g_status.ipv4_address) != 0);

    taskENTER_CRITICAL();
    g_status = status;
    taskEXIT_CRITICAL();

    if (dhcp_state_changed)
    {
        printf("[NET]\t\tDHCP state: %s\n", status.dhcp_state);
    }
    if (dhcp_ipv4_assigned)
    {
        printf("[NET]\t\tDHCP IPv4 assigned: %s netmask=%s gateway=%s\n",
               status.ipv4_address,
               status.netmask,
               status.gateway);
    }
}

bool network_manager_init(void)
{
    const device_profile_config_t *config = config_service_get();

    memset(&g_status, 0, sizeof(g_status));
    g_link_state_initialized = false;
    g_link_state_changes = 0;
    g_reconnect_count = 0;
    strncpy(g_status.mac_address, "00:00:00:00:00:00", sizeof(g_status.mac_address));
    g_status.mac_address[sizeof(g_status.mac_address) - 1] = '\0';
    strncpy(g_status.ip_mode, config->network_mode == NETWORK_MODE_DHCP ? "DHCP" : "STATIC", sizeof(g_status.ip_mode));
    g_status.ip_mode[sizeof(g_status.ip_mode) - 1] = '\0';
    strncpy(g_status.ipv4_address, "0.0.0.0", sizeof(g_status.ipv4_address));
    g_status.ipv4_address[sizeof(g_status.ipv4_address) - 1] = '\0';
    strncpy(g_status.netmask, "0.0.0.0", sizeof(g_status.netmask));
    g_status.netmask[sizeof(g_status.netmask) - 1] = '\0';
    strncpy(g_status.gateway, "0.0.0.0", sizeof(g_status.gateway));
    g_status.gateway[sizeof(g_status.gateway) - 1] = '\0';
    strncpy(g_status.dhcp_state, "Off", sizeof(g_status.dhcp_state));
    g_status.dhcp_state[sizeof(g_status.dhcp_state) - 1] = '\0';

    if (!w5x00_ethernet_driver_init(&g_driver, config->mac))
    {
        return false;
    }

    if (config->network_mode == NETWORK_MODE_DHCP)
    {
        if (!w5x00_ethernet_driver_start_dhcp(&g_driver))
        {
            printf("[NET]\t\tFailed to start DHCP\n");
        }
    }
    else
    {
        ip4_addr_t dns_server;
        w5x00_ethernet_driver_set_static_ipv4(&g_driver, config->static_ip, config->static_netmask, config->static_gateway);
        IP4_ADDR(&dns_server, config->static_dns[0], config->static_dns[1], config->static_dns[2], config->static_dns[3]);
        dns_setserver(0, &dns_server);
    }

    g_initialized = true;
    network_manager_update_status();
    return true;
}

void network_manager_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();

    while (1)
    {
        if (g_initialized)
        {
            w5x00_ethernet_driver_poll(&g_driver);
            network_manager_update_status();
        }

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(10));
    }
}

void network_manager_get_status(network_manager_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *out_status = g_status;
    taskEXIT_CRITICAL();
}

struct netif *network_manager_get_netif(void)
{
    return w5x00_ethernet_driver_get_netif(&g_driver);
}

bool network_manager_is_ready(void)
{
    network_manager_status_t status = {0};
    network_manager_get_status(&status);
    return status.has_ipv4;
}

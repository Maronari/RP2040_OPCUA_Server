#include "drivers/w5x00_ethernet_driver.h"

#include <stdio.h>
#include <string.h>

#include "socket.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"

#include "lwip/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
#include "pico/lwip_freertos.h"

#define W5X00_MACRAW_SOCKET 0

static bool configure_netif(w5x00_ethernet_driver_t *driver)
{
    if (netif_add(&driver->netif, IP_ADDR_ANY, IP_ADDR_ANY, IP_ADDR_ANY, NULL, netif_initialize, tcpip_input) == NULL)
    {
        printf("[NET]\t\tFailed to add lwIP netif\n");
        return false;
    }

    driver->netif.name[0] = 'e';
    driver->netif.name[1] = '0';

    netif_set_link_callback(&driver->netif, netif_link_callback);
    netif_set_status_callback(&driver->netif, netif_status_callback);
    netif_set_default(&driver->netif);
    netif_set_link_up(&driver->netif);
    netif_set_up(&driver->netif);

    if (socket(W5X00_MACRAW_SOCKET, Sn_MR_MACRAW, 0, 0x00) < 0)
    {
        printf("[NET]\t\tFailed to open MACRAW socket\n");
        return false;
    }

    return true;
}

bool w5x00_ethernet_driver_init(w5x00_ethernet_driver_t *driver, const uint8_t mac[6])
{
    if (driver == NULL)
    {
        return false;
    }

    memset(driver, 0, sizeof(*driver));

    if (mac != NULL)
    {
        memcpy(driver->mac, mac, sizeof(driver->mac));
    }

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_soft_reset();
    wizchip_initialize();
    wizchip_check();

    w5x00_lwip_set_mac_address(driver->mac);
    setSHAR(driver->mac);
    ctlwizchip(CW_RESET_PHY, NULL);

    async_context_freertos_init_with_defaults(&driver->async_context_freertos);
    lwip_freertos_init(&driver->async_context_freertos.core);

    if (!configure_netif(driver))
    {
        return false;
    }

    driver->initialized = true;
    return true;
}

bool w5x00_ethernet_driver_start_dhcp(w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return false;
    }

    return dhcp_start(&driver->netif) == ERR_OK;
}

void w5x00_ethernet_driver_set_static_ipv4(w5x00_ethernet_driver_t *driver,
                                           const uint8_t ip[4],
                                           const uint8_t netmask[4],
                                           const uint8_t gateway[4])
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask_addr;
    ip4_addr_t gateway_addr;

    if (driver == NULL || !driver->initialized)
    {
        return;
    }

    IP4_ADDR(&ipaddr, ip[0], ip[1], ip[2], ip[3]);
    IP4_ADDR(&netmask_addr, netmask[0], netmask[1], netmask[2], netmask[3]);
    IP4_ADDR(&gateway_addr, gateway[0], gateway[1], gateway[2], gateway[3]);
    netif_set_addr(&driver->netif, &ipaddr, &netmask_addr, &gateway_addr);
}

void w5x00_ethernet_driver_poll(w5x00_ethernet_driver_t *driver)
{
    uint16_t packet_length = 0;
    struct pbuf *packet = NULL;

    if (driver == NULL || !driver->initialized)
    {
        return;
    }

    getsockopt(W5X00_MACRAW_SOCKET, SO_RECVBUF, &packet_length);
    if (packet_length == 0)
    {
        return;
    }

    packet_length = (uint16_t)recv_lwip(W5X00_MACRAW_SOCKET, driver->packet_buffer, packet_length);
    if (packet_length == 0)
    {
        driver->rx_errors++;
        return;
    }

    packet = pbuf_alloc(PBUF_RAW, packet_length, PBUF_POOL);
    if (packet == NULL)
    {
        driver->rx_errors++;
        return;
    }

    if (pbuf_take(packet, driver->packet_buffer, packet_length) != ERR_OK)
    {
        pbuf_free(packet);
        driver->rx_errors++;
        return;
    }

    LINK_STATS_INC(link.recv);

    if (driver->netif.input(packet, &driver->netif) != ERR_OK)
    {
        pbuf_free(packet);
        driver->input_errors++;
        return;
    }

    driver->rx_packets++;
}

bool w5x00_ethernet_driver_is_link_up(const w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return false;
    }

    return netif_is_link_up((struct netif *)&driver->netif) != 0;
}

bool w5x00_ethernet_driver_has_ipv4(const w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return false;
    }

    return !ip4_addr_isany_val(*netif_ip4_addr((struct netif *)&driver->netif));
}

struct netif *w5x00_ethernet_driver_get_netif(w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return NULL;
    }

    return &driver->netif;
}



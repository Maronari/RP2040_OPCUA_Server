/**
 * Copyright (c) 2022 WIZnet Co.,Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * ----------------------------------------------------------------------------------------------------
 * Includes
 * ----------------------------------------------------------------------------------------------------
 */
#include <stdio.h>

#include "w5x00_lwip.h"

#include "socket.h"

#include "netif/etharp.h"
#include "lwip/pbuf.h"
#include <string.h>
/**
 * ----------------------------------------------------------------------------------------------------
 * Macros
 * ----------------------------------------------------------------------------------------------------
 */

/**
 * ----------------------------------------------------------------------------------------------------
 * Variables
 * ----------------------------------------------------------------------------------------------------
 */
static uint8_t g_mac[6] = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56};

/**
 * ----------------------------------------------------------------------------------------------------
 * Functions
 * ----------------------------------------------------------------------------------------------------
 */
err_t netif_output(struct netif *netif, struct pbuf *p)
{
    uint8_t *tx_frame;
    uint16_t tx_capacity;
    uint16_t tot_len = 0;
    int32_t send_len;

    if (p == NULL)
    {
        return ERR_BUF;
    }

    if (w5x00_lwip_tx_begin(netif, &tx_frame, &tx_capacity) != ERR_OK)
    {
        return ERR_IF;
    }

    memset(tx_frame, 0x00, tx_capacity);
    for (struct pbuf *q = p; q != NULL; q = q->next)
    {
        if (q->len > (uint16_t)(tx_capacity - tot_len))
        {
            w5x00_lwip_tx_end(netif, false);
            return ERR_BUF;
        }

        memcpy(tx_frame + tot_len, q->payload, q->len);
        tot_len = (uint16_t)(tot_len + q->len);

        if (q->len == q->tot_len)
        {
            break;
        }
    }

    if (tot_len < 60)
    {
        // pad
        tot_len = 60u;
    }

    send_len = send_lwip(netif, tx_frame, tot_len);
    if (send_len == (int32_t)tot_len)
    {
        w5x00_lwip_tx_end(netif, true);
        return ERR_OK;
    }

    w5x00_lwip_tx_end(netif, false);
    return send_len == W5X00_LWIP_SEND_BUFFER ? ERR_BUF : ERR_IF;
}

void netif_link_callback(struct netif *netif)
{
    printf("[LWIP]\t\tNetif link status changed %s\n", netif_is_link_up(netif) ? "up" : "down");
}

void netif_status_callback(struct netif *netif)
{
    printf("[LWIP]\t\tNetif status changed %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));
}

err_t netif_initialize(struct netif *netif)
{
    netif->linkoutput = netif_output;
    netif->output = etharp_output;
    netif->mtu = ETHERNET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;
    SMEMCPY(netif->hwaddr, g_mac, sizeof(netif->hwaddr));
    netif->hwaddr_len = sizeof(netif->hwaddr);
    return ERR_OK;
}

void w5x00_lwip_set_mac_address(const uint8_t mac_addr[6])
{
    memcpy(g_mac, mac_addr, sizeof(g_mac));
}

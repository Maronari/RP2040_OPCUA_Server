/**
 * Copyright (c) 2022 WIZnet Co.,Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _W5x00_LWIP_H_
#define _W5x00_LWIP_H_

/**
 * ----------------------------------------------------------------------------------------------------
 * Includes
 * ----------------------------------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdbool.h>

#include "lwip/netif.h"

/**
 * ----------------------------------------------------------------------------------------------------
 * Macros
 * ----------------------------------------------------------------------------------------------------
 */
/* LWIP */
#define ETHERNET_MTU 1500u
/* W5500 MACRAW exposes the Ethernet header but not the FCS. */
#define W5X00_LWIP_MAX_FRAME_LEN (ETHERNET_MTU + 14u)
#define W5X00_LWIP_SEND_BUFFER (-1)
#define W5X00_LWIP_SEND_FAILURE (-2)
#define W5X00_LWIP_SEND_INTERFACE (-3)

/**
 * ----------------------------------------------------------------------------------------------------
 * Variables
 * ----------------------------------------------------------------------------------------------------
 */

/**
 * ----------------------------------------------------------------------------------------------------
 * Functions
 * ----------------------------------------------------------------------------------------------------
 */
/*! \brief send an ethernet packet
 *  \ingroup w5x00_lwip
 *
 *  It is used to send outgoing data to the socket.
 *
 *  \param sn socket number
 *  \param buf a pointer to the data to send
 *  \param len the length of data in packet
 *  \return he sent data size
 */
/*
 * The netif state is a driver-owned w5x00_ethernet_driver_t.  The functions
 * below deliberately keep that type opaque to the lwIP port: the driver owns
 * the socket-0 mutex, buffers and counters.
 */
int32_t send_lwip(struct netif *netif, const uint8_t *buf, uint16_t len);

/*! \brief read an ethernet packet
 *  \ingroup w5x00_lwip
 *
 *  It is used to read incoming data from the socket.
 *
 *  \param sn socket number
 *  \param buf a pointer buffer to read incoming data
 *  \param len the length of the data in the packet
 *  \return the real received data size
 */
int32_t recv_lwip(struct netif *netif, uint8_t *buf, uint16_t capacity);

/*
 * A TX transaction includes pbuf-chain copying and the W5500 SEND command so
 * two lwIP producers cannot overwrite the driver-owned frame buffer.  No
 * lwIP callback may run between begin and end.
 */
err_t w5x00_lwip_tx_begin(struct netif *netif, uint8_t **frame, uint16_t *capacity);
void w5x00_lwip_tx_end(struct netif *netif, bool success);

/*! \brief callback function
 *  \ingroup w5x00_lwip
 *
 *  This function is called by ethernet_output() when it wants
 *  to send a packet on the interface. This function outputs
 *  the pbuf as-is on the link medium.
 *
 *  \param netif a pre-allocated netif structure
 *  \param p main packet buffer struct
 *  \return ERR_OK if data was sent.
 */
err_t netif_output(struct netif *netif, struct pbuf *p);

/*! \brief callback function
 *  \ingroup w5x00_lwip
 *
 *  Callback function for link.
 *
 *  \param netif a pre-allocated netif structure
 */
void netif_link_callback(struct netif *netif);

/*! \brief callback function
 *  \ingroup w5x00_lwip
 *
 *   Callback function for status.
 *
 *   \param netif a pre-allocated netif structure
 */
void netif_status_callback(struct netif *netif);

/*! \brief callback function
 *  \ingroup w5x00_lwip
 *
 *  Callback function that initializes the interface.
 *
 *  \param netif a pre-allocated netif structure
 *  \return ERR_OK if Network interface initialized
 */
err_t netif_initialize(struct netif *netif);

/*! \brief Set the MAC address used by the lwIP netif wrapper
 *  \ingroup w5x00_lwip
 *
 *  \param mac_addr a pointer to a 6-byte MAC address
 */
void w5x00_lwip_set_mac_address(const uint8_t mac_addr[6]);

#endif /* _W5x00_LWIP_H_ */

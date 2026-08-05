#ifndef TEST_LWIP_TCPIP_H
#define TEST_LWIP_TCPIP_H
#include "lwip/netif.h"
err_t tcpip_input(struct pbuf *pbuf, struct netif *netif);
#endif

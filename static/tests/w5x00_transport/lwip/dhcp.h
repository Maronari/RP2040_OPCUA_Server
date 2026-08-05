#ifndef TEST_LWIP_DHCP_H
#define TEST_LWIP_DHCP_H
#include "lwip/netif.h"
err_t dhcp_start(struct netif *netif);
#endif

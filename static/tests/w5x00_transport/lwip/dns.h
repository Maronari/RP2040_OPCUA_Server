#ifndef TEST_LWIP_DNS_H
#define TEST_LWIP_DNS_H
#include "lwip/netif.h"
/* LWIP_IPV6 is 0 in this firmware, so lwIP's ip_addr_t is ip4_addr_t. */
void dns_setserver(uint8_t numdns, const ip4_addr_t *dnsserver);
#endif

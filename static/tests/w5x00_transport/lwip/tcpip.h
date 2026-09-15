#ifndef TEST_LWIP_TCPIP_H
#define TEST_LWIP_TCPIP_H
#include "lwip/netif.h"
err_t tcpip_input(struct pbuf *pbuf, struct netif *netif);

/* Host stand-in for lwIP's core lock (pico-sdk: the driver's async_context
 * lock); the test tracks nesting and lock order. */
void w5x00_test_lock_tcpip_core(void);
void w5x00_test_unlock_tcpip_core(void);
#define LOCK_TCPIP_CORE() w5x00_test_lock_tcpip_core()
#define UNLOCK_TCPIP_CORE() w5x00_test_unlock_tcpip_core()
#endif

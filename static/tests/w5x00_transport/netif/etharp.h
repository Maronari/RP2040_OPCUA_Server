#ifndef TEST_ETHARP_H
#define TEST_ETHARP_H
#include "lwip/netif.h"
err_t etharp_output(struct netif *, const void *, const void *);
#endif

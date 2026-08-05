#ifndef TEST_LWIP_PBUF_H
#define TEST_LWIP_PBUF_H

#include "lwip/netif.h"

#define PBUF_RAW 0
#define PBUF_POOL 0
struct pbuf {
    struct pbuf *next;
    void *payload;
    u16_t len;
    u16_t tot_len;
};
struct pbuf *pbuf_alloc(int layer, u16_t length, int type);
err_t pbuf_take(struct pbuf *pbuf, const void *source, u16_t length);
void pbuf_free(struct pbuf *pbuf);

#endif

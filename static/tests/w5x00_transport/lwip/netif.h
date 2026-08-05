#ifndef TEST_LWIP_NETIF_H
#define TEST_LWIP_NETIF_H

#include <stdint.h>

typedef int err_t;
typedef uint16_t u16_t;
typedef struct { uint32_t addr; } ip4_addr_t;
struct netif;
struct pbuf;
typedef err_t (*netif_input_fn)(struct pbuf *, struct netif *);
typedef err_t (*netif_linkoutput_fn)(struct netif *, struct pbuf *);
typedef err_t (*netif_output_fn)(struct netif *, const void *, const void *);
typedef err_t (*netif_init_fn)(struct netif *);
struct netif {
    char name[2];
    void *state;
    netif_input_fn input;
    netif_linkoutput_fn linkoutput;
    netif_output_fn output;
    uint16_t mtu;
    uint8_t flags;
    uint8_t hwaddr[6];
    uint8_t hwaddr_len;
    int link_up;
    ip4_addr_t ip_addr;
};

#define ERR_OK 0
#define ERR_IF -12
#define ERR_BUF -13
#define IP_ADDR_ANY NULL
#define NETIF_FLAG_BROADCAST 0x01u
#define NETIF_FLAG_ETHARP 0x02u
#define NETIF_FLAG_ETHERNET 0x04u
#define NETIF_FLAG_IGMP 0x08u
#define NETIF_FLAG_MLD6 0x10u
#define SMEMCPY memcpy
#define IP4_ADDR(address, a, b, c, d) ((address)->addr = (uint32_t)(a) + (uint32_t)(b) + (uint32_t)(c) + (uint32_t)(d))

struct netif *netif_add(struct netif *, const void *, const void *, const void *, void *, netif_init_fn, netif_input_fn);
void netif_set_link_callback(struct netif *, void (*)(struct netif *));
void netif_set_status_callback(struct netif *, void (*)(struct netif *));
void netif_set_default(struct netif *);
void netif_set_link_up(struct netif *);
void netif_set_up(struct netif *);
int netif_is_link_up(struct netif *);
const ip4_addr_t *netif_ip4_addr(struct netif *);
void netif_set_addr(struct netif *, const ip4_addr_t *, const ip4_addr_t *, const ip4_addr_t *);
const char *ip4addr_ntoa(const ip4_addr_t *);
#define ip4_addr_isany_val(value) ((value).addr == 0u)

#endif

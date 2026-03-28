#ifndef DRIVERS_W5X00_ETHERNET_DRIVER_H
#define DRIVERS_W5X00_ETHERNET_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "lwip/netif.h"
#include "pico/async_context_freertos.h"
#include "w5x00_lwip.h"

typedef struct
{
    struct netif netif;
    async_context_freertos_t async_context_freertos;
    uint8_t mac[6];
    uint8_t packet_buffer[ETHERNET_MTU];
    bool initialized;
    uint32_t rx_packets;
    uint32_t rx_errors;
    uint32_t input_errors;
} w5x00_ethernet_driver_t;

bool w5x00_ethernet_driver_init(w5x00_ethernet_driver_t *driver, const uint8_t mac[6]);
bool w5x00_ethernet_driver_start_dhcp(w5x00_ethernet_driver_t *driver);
void w5x00_ethernet_driver_set_static_ipv4(w5x00_ethernet_driver_t *driver,
                                           const uint8_t ip[4],
                                           const uint8_t netmask[4],
                                           const uint8_t gateway[4]);
void w5x00_ethernet_driver_poll(w5x00_ethernet_driver_t *driver);
bool w5x00_ethernet_driver_is_link_up(const w5x00_ethernet_driver_t *driver);
bool w5x00_ethernet_driver_has_ipv4(const w5x00_ethernet_driver_t *driver);
struct netif *w5x00_ethernet_driver_get_netif(w5x00_ethernet_driver_t *driver);

#endif

#ifndef SERVICES_NETWORK_MANAGER_H
#define SERVICES_NETWORK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "lwip/netif.h"

#include "services/service_types.h"

typedef struct
{
    bool initialized;
    bool dhcp_enabled;
    bool link_up;
    bool has_ipv4;
    service_quality_t quality;
    uint32_t reconnect_count;
    uint32_t rx_packets;
    uint32_t rx_errors;
    uint32_t input_errors;
    uint32_t link_state_changes;
    char mac_address[18];
    char ip_mode[8];
    char ipv4_address[16];
    char netmask[16];
    char gateway[16];
    char dhcp_state[24];
} network_manager_status_t;

bool network_manager_init(void);
void network_manager_task(void *argument);
void network_manager_get_status(network_manager_status_t *out_status);
struct netif *network_manager_get_netif(void);
bool network_manager_is_ready(void);

#endif

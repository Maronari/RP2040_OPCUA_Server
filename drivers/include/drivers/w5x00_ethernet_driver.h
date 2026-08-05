#ifndef DRIVERS_W5X00_ETHERNET_DRIVER_H
#define DRIVERS_W5X00_ETHERNET_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include <FreeRTOS.h>
#include <semphr.h>

#include "lwip/netif.h"
#include "pico/async_context_freertos.h"
#include "w5x00_lwip.h"

/* Eight frames cap one 10 ms network-manager poll without starving peers. */
#define W5X00_ETHERNET_RX_POLL_BUDGET 8u

typedef enum
{
    W5X00_MACRAW_RECOVERY_IDLE = 0,
    W5X00_MACRAW_RECOVERY_WAIT_CLOSED,
    W5X00_MACRAW_RECOVERY_WAIT_MACRAW
} w5x00_macraw_recovery_phase_t;

typedef enum
{
    W5X00_MACRAW_DIAGNOSTIC_NONE = 0,
    W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED,
    W5X00_MACRAW_DIAGNOSTIC_RX_OVERFLOW,
    W5X00_MACRAW_DIAGNOSTIC_TX_ADMISSION,
    W5X00_MACRAW_DIAGNOSTIC_TX_SEND,
    W5X00_MACRAW_DIAGNOSTIC_TX_INTERFACE
} w5x00_macraw_diagnostic_t;

typedef struct w5x00_ethernet_driver
{
    struct netif netif;
    async_context_freertos_t async_context_freertos;
    uint8_t mac[6];
    /* MACRAW data includes a 14-byte Ethernet header; the W5500 appends no FCS. */
    uint8_t packet_buffer[W5X00_LWIP_MAX_FRAME_LEN];
    uint8_t tx_frame[W5X00_LWIP_MAX_FRAME_LEN];
    StaticSemaphore_t macraw_mutex_storage;
    SemaphoreHandle_t macraw_mutex;
    bool macraw_quarantined;
    w5x00_macraw_recovery_phase_t macraw_recovery_phase;
    bool initialized;
    uint32_t rx_packets;
    uint32_t rx_errors;
    uint32_t input_errors;
    uint32_t rx_overflow_frames;
    uint32_t rx_malformed_frames;
    /* Bounded one-poll deferrals for incomplete MACRAW record observation. */
    uint32_t rx_short_deferrals;
    uint32_t macraw_timeouts;
    uint32_t pbuf_failures;
    uint32_t macraw_contention;
    uint32_t tx_failures;
    uint32_t tx_admission_failures;
    uint32_t tx_send_failures;
    uint32_t tx_interface_failures;
    bool rx_drop_diagnostic_reported;
    bool tx_admission_diagnostic_reported;
    bool tx_send_diagnostic_reported;
    bool tx_interface_diagnostic_reported;
    bool macraw_diagnostic_pending;
    w5x00_macraw_diagnostic_t macraw_diagnostic;
    uint16_t macraw_diagnostic_length;
    uint16_t macraw_diagnostic_value;
    bool rx_short_deferred;
    uint8_t rx_short_deferral_count;
    uint16_t rx_short_deferred_pointer;
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

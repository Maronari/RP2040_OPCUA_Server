/**
 * @file
 * @brief W5500 MACRAW-mode Ethernet driver: lwIP netif glue over the
 *        WIZnet ioLibrary_Driver SPI transport.
 *
 * This header declares the driver instance struct and the public lifecycle/
 * poll API. The MACRAW TX/RX state machine implementation (send_lwip(),
 * recv_lwip(), the socket recovery FSM) lives in
 * drivers/src/w5x00_ethernet_driver.c; see that file's file-level doc for
 * the concurrency contract between lwIP's calling task and NetworkTask's
 * poll loop.
 */
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

/**
 * @brief MACRAW socket recovery state machine phase.
 *
 * Recovery (drivers/src/w5x00_ethernet_driver.c's
 * w5x00_macraw_recover_locked()) is resumable across calls: each phase
 * represents "waiting for the W5500 to reach a specific socket status
 * after issuing a command", so a poll that times out mid-recovery picks up
 * from the same phase on the next call rather than restarting.
 */
typedef enum
{
    W5X00_MACRAW_RECOVERY_IDLE = 0,       /**< Not recovering; socket assumed healthy or about to be closed. */
    W5X00_MACRAW_RECOVERY_WAIT_CLOSED,    /**< CLOSE issued; waiting for SOCK_CLOSED before reopening MACRAW. */
    W5X00_MACRAW_RECOVERY_WAIT_MACRAW     /**< OPEN issued; waiting for SOCK_MACRAW before recovery completes. */
} w5x00_macraw_recovery_phase_t;

/**
 * @brief Which class of MACRAW fault the pending one-shot diagnostic
 *        (macraw_diagnostic_pending) describes.
 *
 * Each value is reported at most once per condition via
 * w5x00_record_diagnostic_once()'s "reported" latch (e.g.
 * rx_drop_diagnostic_reported) to avoid flooding the diagnostic log while a
 * persistent fault continues to recur every poll.
 */
typedef enum
{
    W5X00_MACRAW_DIAGNOSTIC_NONE = 0,
    W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED,   /**< Dropped an unparseable/undersized MACRAW record. */
    W5X00_MACRAW_DIAGNOSTIC_RX_OVERFLOW,    /**< Dropped a record too large for the RX buffer/lwIP frame limit. */
    W5X00_MACRAW_DIAGNOSTIC_TX_ADMISSION,   /**< TX free-space poll timed out before the socket admitted the frame. */
    W5X00_MACRAW_DIAGNOSTIC_TX_SEND,        /**< SEND command did not complete (no SENDOK before timeout, or TIMEOUT interrupt). */
    W5X00_MACRAW_DIAGNOSTIC_TX_INTERFACE    /**< Socket left MACRAW mode and could not be recovered before a send. */
} w5x00_macraw_diagnostic_t;

/**
 * @brief One W5500 MACRAW Ethernet driver instance.
 *
 * Ownership: allocated and owned by services/network_manager.c (one
 * instance for the whole firmware; the W5500/SPI bus is a singleton).
 *
 * Synchronization: `macraw_mutex` (backed by `macraw_mutex_storage`,
 * created in w5x00_ethernet_driver_init()) serializes every access to the
 * W5500 MACRAW socket registers and to `packet_buffer`/`tx_frame` between
 * lwIP's calling task (via send_lwip()/w5x00_lwip_tx_begin()/_tx_end()) and
 * NetworkTask's w5x00_ethernet_driver_poll() -> recv_lwip() path. All
 * `uint32_t` counters below are updated only while holding that mutex
 * except where noted (w5x00_counter_inc() uses `__atomic_fetch_add` so
 * counters remain race-free even for any future non-mutex-holding
 * incrementer).
 */
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

/**
 * @brief Initialize the W5500 chip, its lwIP netif, and driver state.
 *
 * Resets and reinitializes the W5500 over SPI (wizchip_reset/soft_reset/
 * initialize), sets the MAC address, creates the MACRAW mutex, starts a
 * FreeRTOS async context for lwIP, registers and brings up the lwIP netif,
 * and quarantines+recovers the MACRAW socket so it starts in a known-open
 * state.
 *
 * @param driver Instance to initialize; zeroed internally, so any prior
 *        content is discarded. Must not be NULL.
 * @param mac 6-byte MAC address to program; if NULL, the driver's `mac`
 *        field stays zeroed (all-zero MAC).
 * @return false if @p driver is NULL, mutex creation fails, the async
 *         context fails to initialize, or netif registration/MACRAW socket
 *         acquisition fails; true otherwise.
 * @post On success, `driver->initialized` is true and the netif is up
 *       (link state pending PHY negotiation).
 * @note Hardware side effect: full W5500 SPI reset/reinitialize sequence.
 * @note Not safe to call concurrently, and not safe to call twice on the
 *       same instance without re-zeroing (mutex/async-context are recreated
 *       unconditionally).
 */
bool w5x00_ethernet_driver_init(w5x00_ethernet_driver_t *driver, const uint8_t mac[6]);

/**
 * @brief Start lwIP's DHCP client on the driver's netif.
 * @return false if @p driver is NULL or not initialized, or if
 *         `dhcp_start()` reports an error; true otherwise.
 * @note Does not block for a lease; DHCP progresses asynchronously via
 *       lwIP's own timers/callbacks.
 */
bool w5x00_ethernet_driver_start_dhcp(w5x00_ethernet_driver_t *driver);

/**
 * @brief Configure a static IPv4 address/netmask/gateway on the driver's netif.
 *
 * No-op if @p driver is NULL or not initialized. Callers should not also
 * run DHCP on the same netif.
 *
 * @param ip 4-byte IPv4 address, network byte order (most-significant octet first).
 * @param netmask 4-byte subnet mask, same byte order.
 * @param gateway 4-byte default gateway, same byte order.
 */
void w5x00_ethernet_driver_set_static_ipv4(w5x00_ethernet_driver_t *driver,
                                           const uint8_t ip[4],
                                           const uint8_t netmask[4],
                                           const uint8_t gateway[4]);

/**
 * @brief Drain up to W5X00_ETHERNET_RX_POLL_BUDGET (8) queued received
 *        frames into lwIP.
 *
 * Calls recv_lwip() in a loop, allocating a pbuf per frame and handing it
 * to `netif.input()`. Stops early once the RX queue is empty (recv_lwip()
 * returns 0) or a receive error occurs; a malformed-header error is not
 * retried within the same poll (the queue position is left as-is since it
 * cannot be safely skipped).
 *
 * @pre Must be called from a single task (documented as NetworkTask, see
 *      docs/code/hardware.md) at a bounded interval (documented as every
 *      10 ms) so RX queue occupancy stays bounded between polls.
 * @note Hardware side effect: reads/consumes W5500 MACRAW socket registers
 *       and RX buffer memory over SPI, under the internal MACRAW mutex.
 */
void w5x00_ethernet_driver_poll(w5x00_ethernet_driver_t *driver);

/**
 * @brief Report the lwIP netif's cached link-up state.
 * @return false if @p driver is NULL or not initialized; the netif's
 *         `netif_is_link_up()` value otherwise.
 */
bool w5x00_ethernet_driver_is_link_up(const w5x00_ethernet_driver_t *driver);

/**
 * @brief Report whether the netif currently holds a non-zero IPv4 address.
 * @return false if @p driver is NULL or not initialized, or the netif's
 *         IPv4 address is the "any" address; true otherwise.
 */
bool w5x00_ethernet_driver_has_ipv4(const w5x00_ethernet_driver_t *driver);

/**
 * @brief Borrow the driver's lwIP netif pointer.
 *
 * @return NULL if @p driver is NULL or not initialized; otherwise a
 *         pointer valid for the driver instance's lifetime. Ownership is
 *         not transferred -- callers must not free or reinitialize it.
 */
struct netif *w5x00_ethernet_driver_get_netif(w5x00_ethernet_driver_t *driver);

#endif

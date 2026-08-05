#include "drivers/w5x00_ethernet_driver.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <semphr.h>

#include "socket.h"
#include "w5500.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"

#include "lwip/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
#include "pico/lwip_freertos.h"

#ifdef W5X00_DIAGNOSTIC_PRINTF
extern int W5X00_DIAGNOSTIC_PRINTF(const char *format, ...);
#else
#define W5X00_DIAGNOSTIC_PRINTF printf
#endif

#define W5X00_MACRAW_SOCKET 0
#define W5X00_MACRAW_MUTEX_WAIT_MS 5u
#define W5X00_MACRAW_COMMAND_POLL_LIMIT 1024u
#define W5X00_MACRAW_STATUS_POLL_LIMIT 1024u
#define W5X00_MACRAW_SEND_POLL_LIMIT 8192u
#define W5X00_MACRAW_TX_ADMISSION_POLL_LIMIT 128u
#define W5X00_MACRAW_TX_FSR_STABLE_POLL_LIMIT 4u
#define W5X00_MACRAW_RX_HEADER_LEN 2u
#define W5X00_MACRAW_MIN_ETHERNET_FRAME_LEN 60u
#define W5X00_MACRAW_MIN_RECORD_LEN (W5X00_MACRAW_RX_HEADER_LEN + W5X00_MACRAW_MIN_ETHERNET_FRAME_LEN)
/* A short RSR sample may precede the rest of one MACRAW record. */
#define W5X00_MACRAW_RX_SHORT_DEFERRAL_LIMIT 1u
#define W5X00_MACRAW_RX_SNAPSHOT_BYTES 16u

#define W5X00_LWIP_RX_MALFORMED (-1)
#define W5X00_LWIP_RX_OVERFLOW (-2)
#define W5X00_LWIP_RX_TIMEOUT (-3)
#define W5X00_LWIP_RX_CONTENTION (-4)

#if configSUPPORT_STATIC_ALLOCATION
static StackType_t g_w5x00_async_context_stack[ASYNC_CONTEXT_DEFAULT_FREERTOS_TASK_STACK_SIZE];
#endif

static bool w5x00_macraw_lock(w5x00_ethernet_driver_t *driver);
static void w5x00_macraw_unlock(w5x00_ethernet_driver_t *driver);
static void w5x00_macraw_unlock_and_report(w5x00_ethernet_driver_t *driver);
static bool w5x00_macraw_recover_locked(w5x00_ethernet_driver_t *driver);
static void w5x00_counter_inc(uint32_t *counter);

static void w5x00_macraw_reset_rx_short_deferral(w5x00_ethernet_driver_t *driver)
{
    driver->rx_short_deferred = false;
    driver->rx_short_deferral_count = 0u;
    driver->rx_short_deferred_pointer = 0u;
}

/* Called while the MACRAW mutex is held.  It never waits or issues RECV. */
static bool w5x00_macraw_defer_short_record_locked(w5x00_ethernet_driver_t *driver,
                                                    uint16_t rx_read_pointer)
{
    if (!driver->rx_short_deferred ||
        driver->rx_short_deferred_pointer != rx_read_pointer)
    {
        w5x00_macraw_reset_rx_short_deferral(driver);
        driver->rx_short_deferred = true;
        driver->rx_short_deferred_pointer = rx_read_pointer;
    }

    if (driver->rx_short_deferral_count >= W5X00_MACRAW_RX_SHORT_DEFERRAL_LIMIT)
    {
        return false;
    }

    driver->rx_short_deferral_count++;
    w5x00_counter_inc(&driver->rx_short_deferrals);
    return true;
}

typedef struct
{
    bool valid;
    w5x00_macraw_diagnostic_t diagnostic;
    uint16_t length;
    uint16_t value;
} w5x00_macraw_diagnostic_record_t;

/* Stack-only, one-shot evidence for a persistent malformed MACRAW prefix. */
typedef struct
{
    bool valid;
    uint8_t mode;
    uint8_t status;
    uint8_t rx_buffer_size;
    uint8_t tx_buffer_size;
    uint8_t interrupt_status;
    uint16_t rx_read_first;
    uint16_t rx_write_first;
    uint16_t rx_size_first;
    uint16_t rx_read_second;
    uint16_t rx_write_second;
    uint16_t rx_size_second;
    uint8_t raw_first[W5X00_MACRAW_RX_SNAPSHOT_BYTES];
    uint8_t raw_second[W5X00_MACRAW_RX_SNAPSHOT_BYTES];
} w5x00_macraw_rx_snapshot_t;

static uint32_t w5x00_macraw_rx_buffer_address(uint16_t rx_read_pointer)
{
    return ((uint32_t)rx_read_pointer << 8u) +
           (WIZCHIP_RXBUF_BLOCK(W5X00_MACRAW_SOCKET) << 3u);
}

/*
 * MACRAW records are consumed atomically from the saved pointer.  Do not use
 * wiz_recv_data() for a header followed by payload: it re-reads Sn_RX_RD for
 * each piece and therefore depends on immediate register-write visibility.
 */
static void w5x00_macraw_read_record_payload(uint16_t rx_read_pointer,
                                              uint8_t *buffer,
                                              uint16_t length)
{
    WIZCHIP_READ_BUF(w5x00_macraw_rx_buffer_address(
                         (uint16_t)(rx_read_pointer + W5X00_MACRAW_RX_HEADER_LEN)),
                     buffer,
                     length);
}

static void w5x00_macraw_consume_record(uint16_t rx_read_pointer,
                                        uint16_t wire_length)
{
    setSn_RX_RD(W5X00_MACRAW_SOCKET, (uint16_t)(rx_read_pointer + wire_length));
}

/* Diagnostic sampling must never inherit ioLibrary's unbounded stability loop. */
static uint16_t w5x00_read_rx_size_once(void)
{
    return (uint16_t)(((uint16_t)WIZCHIP_READ(Sn_RX_RSR(W5X00_MACRAW_SOCKET)) << 8u) |
                      WIZCHIP_READ(WIZCHIP_OFFSET_INC(Sn_RX_RSR(W5X00_MACRAW_SOCKET), 1u)));
}

/* Called with the MACRAW mutex held. It only performs W5500 register reads. */
static void w5x00_capture_rx_snapshot_locked(w5x00_macraw_rx_snapshot_t *snapshot,
                                              uint16_t rx_read_pointer)
{
    const uint32_t address = w5x00_macraw_rx_buffer_address(rx_read_pointer);

    snapshot->valid = true;
    snapshot->mode = getSn_MR(W5X00_MACRAW_SOCKET);
    snapshot->status = getSn_SR(W5X00_MACRAW_SOCKET);
    snapshot->rx_buffer_size = getSn_RXBUF_SIZE(W5X00_MACRAW_SOCKET);
    snapshot->tx_buffer_size = getSn_TXBUF_SIZE(W5X00_MACRAW_SOCKET);
    snapshot->interrupt_status = getSn_IR(W5X00_MACRAW_SOCKET);
    snapshot->rx_read_first = getSn_RX_RD(W5X00_MACRAW_SOCKET);
    snapshot->rx_write_first = getSn_RX_WR(W5X00_MACRAW_SOCKET);
    snapshot->rx_size_first = w5x00_read_rx_size_once();
    WIZCHIP_READ_BUF(address, snapshot->raw_first, sizeof(snapshot->raw_first));
    snapshot->rx_read_second = getSn_RX_RD(W5X00_MACRAW_SOCKET);
    snapshot->rx_write_second = getSn_RX_WR(W5X00_MACRAW_SOCKET);
    snapshot->rx_size_second = w5x00_read_rx_size_once();
    WIZCHIP_READ_BUF(address, snapshot->raw_second, sizeof(snapshot->raw_second));
}

static void w5x00_emit_rx_snapshot(const w5x00_macraw_rx_snapshot_t *snapshot)
{
    if (!snapshot->valid)
    {
        return;
    }

    W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW RX snapshot: mr=0x%02x sr=0x%02x rxbuf=%u txbuf=%u ir=0x%02x\n",
                            (unsigned)snapshot->mode,
                            (unsigned)snapshot->status,
                            (unsigned)snapshot->rx_buffer_size,
                            (unsigned)snapshot->tx_buffer_size,
                            (unsigned)snapshot->interrupt_status);
    W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW RX snapshot: rd=%u wr=%u rsr=%u; rd2=%u wr2=%u rsr2=%u\n",
                            (unsigned)snapshot->rx_read_first,
                            (unsigned)snapshot->rx_write_first,
                            (unsigned)snapshot->rx_size_first,
                            (unsigned)snapshot->rx_read_second,
                            (unsigned)snapshot->rx_write_second,
                            (unsigned)snapshot->rx_size_second);
    W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW RX snapshot raw1:"
                            " %02x %02x %02x %02x %02x %02x %02x %02x"
                            " %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            snapshot->raw_first[0], snapshot->raw_first[1],
                            snapshot->raw_first[2], snapshot->raw_first[3],
                            snapshot->raw_first[4], snapshot->raw_first[5],
                            snapshot->raw_first[6], snapshot->raw_first[7],
                            snapshot->raw_first[8], snapshot->raw_first[9],
                            snapshot->raw_first[10], snapshot->raw_first[11],
                            snapshot->raw_first[12], snapshot->raw_first[13],
                            snapshot->raw_first[14], snapshot->raw_first[15]);
    W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW RX snapshot raw2:"
                            " %02x %02x %02x %02x %02x %02x %02x %02x"
                            " %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            snapshot->raw_second[0], snapshot->raw_second[1],
                            snapshot->raw_second[2], snapshot->raw_second[3],
                            snapshot->raw_second[4], snapshot->raw_second[5],
                            snapshot->raw_second[6], snapshot->raw_second[7],
                            snapshot->raw_second[8], snapshot->raw_second[9],
                            snapshot->raw_second[10], snapshot->raw_second[11],
                            snapshot->raw_second[12], snapshot->raw_second[13],
                            snapshot->raw_second[14], snapshot->raw_second[15]);
}

/* All MACRAW diagnostics are recorded while serialized and printed after unlock. */
static void w5x00_record_diagnostic_once(w5x00_ethernet_driver_t *driver,
                                         bool *reported,
                                         w5x00_macraw_diagnostic_t diagnostic,
                                         uint16_t length,
                                         uint16_t value)
{
    if (!*reported)
    {
        *reported = true;
        driver->macraw_diagnostic = diagnostic;
        driver->macraw_diagnostic_length = length;
        driver->macraw_diagnostic_value = value;
        driver->macraw_diagnostic_pending = true;
    }
}

/* Called while the MACRAW mutex is held; ownership transfers to caller. */
static w5x00_macraw_diagnostic_record_t w5x00_take_pending_diagnostic_locked(
    w5x00_ethernet_driver_t *driver)
{
    w5x00_macraw_diagnostic_record_t record = {0};

    if (!driver->macraw_diagnostic_pending)
    {
        return record;
    }

    record.valid = true;
    record.diagnostic = driver->macraw_diagnostic;
    record.length = driver->macraw_diagnostic_length;
    record.value = driver->macraw_diagnostic_value;
    driver->macraw_diagnostic_pending = false;
    return record;
}

static void w5x00_emit_diagnostic(w5x00_ethernet_driver_t *driver,
                                  const w5x00_macraw_diagnostic_record_t *record)
{
    if (!record->valid)
    {
        return;
    }

    switch (record->diagnostic)
    {
    case W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED:
        W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW RX drop: malformed frame=%u overflow=%lu malformed=%lu\n",
               (unsigned)record->length,
               (unsigned long)driver->rx_overflow_frames,
               (unsigned long)driver->rx_malformed_frames);
        break;
    case W5X00_MACRAW_DIAGNOSTIC_RX_OVERFLOW:
        W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW RX drop: oversize frame=%u overflow=%lu malformed=%lu\n",
               (unsigned)record->length,
               (unsigned long)driver->rx_overflow_frames,
               (unsigned long)driver->rx_malformed_frames);
        break;
    case W5X00_MACRAW_DIAGNOSTIC_TX_ADMISSION:
        W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW TX admission timeout: len=%u free=%u count=%lu\n",
               (unsigned)record->length,
               (unsigned)record->value,
               (unsigned long)driver->tx_admission_failures);
        break;
    case W5X00_MACRAW_DIAGNOSTIC_TX_SEND:
        W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW SEND failed: len=%u ir=0x%02x count=%lu\n",
               (unsigned)record->length,
               (unsigned)record->value,
               (unsigned long)driver->tx_send_failures);
        break;
    case W5X00_MACRAW_DIAGNOSTIC_TX_INTERFACE:
        W5X00_DIAGNOSTIC_PRINTF("[NET]\t\tMACRAW socket recovery failed: len=%u sr=0x%02x count=%lu\n",
               (unsigned)record->length,
               (unsigned)record->value,
               (unsigned long)driver->tx_interface_failures);
        break;
    case W5X00_MACRAW_DIAGNOSTIC_NONE:
    default:
        break;
    }
}

static bool configure_netif(w5x00_ethernet_driver_t *driver)
{
    if (netif_add(&driver->netif, IP_ADDR_ANY, IP_ADDR_ANY, IP_ADDR_ANY, driver, netif_initialize, tcpip_input) == NULL)
    {
        printf("[NET]\t\tFailed to add lwIP netif\n");
        return false;
    }

    driver->netif.name[0] = 'e';
    driver->netif.name[1] = '0';

    netif_set_link_callback(&driver->netif, netif_link_callback);
    netif_set_status_callback(&driver->netif, netif_status_callback);
    netif_set_default(&driver->netif);
    netif_set_link_up(&driver->netif);
    netif_set_up(&driver->netif);

    driver->macraw_quarantined = true;
    if (!w5x00_macraw_lock(driver))
    {
        printf("[NET]\t\tFailed to acquire MACRAW socket\n");
        return false;
    }
    w5x00_macraw_unlock(driver);

    return true;
}

static void w5x00_counter_inc(uint32_t *counter)
{
    (void)__atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
}

static void w5x00_macraw_quarantine(w5x00_ethernet_driver_t *driver)
{
    driver->macraw_quarantined = true;
}

static bool w5x00_macraw_lock(w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || driver->macraw_mutex == NULL ||
        xSemaphoreTake(driver->macraw_mutex,
                       pdMS_TO_TICKS(W5X00_MACRAW_MUTEX_WAIT_MS)) != pdTRUE)
    {
        if (driver != NULL)
        {
            w5x00_counter_inc(&driver->macraw_contention);
        }
        return false;
    }

    if (!w5x00_macraw_recover_locked(driver))
    {
        w5x00_macraw_unlock(driver);
        return false;
    }

    return true;
}

static void w5x00_macraw_unlock(w5x00_ethernet_driver_t *driver)
{
    (void)xSemaphoreGive(driver->macraw_mutex);
}

static void w5x00_macraw_unlock_and_report(w5x00_ethernet_driver_t *driver)
{
    const w5x00_macraw_diagnostic_record_t record = w5x00_take_pending_diagnostic_locked(driver);

    w5x00_macraw_unlock(driver);
    w5x00_emit_diagnostic(driver, &record);
}

static bool w5x00_macraw_complete_command(w5x00_ethernet_driver_t *driver)
{
    for (uint32_t poll = 0; poll < W5X00_MACRAW_COMMAND_POLL_LIMIT; ++poll)
    {
        if (getSn_CR(W5X00_MACRAW_SOCKET) == 0u)
        {
            return true;
        }
    }

    w5x00_counter_inc(&driver->macraw_timeouts);
    w5x00_macraw_quarantine(driver);
    return false;
}

static bool w5x00_macraw_issue_command(w5x00_ethernet_driver_t *driver, uint8_t command)
{
    /* Never overwrite an in-flight command register. */
    if (getSn_CR(W5X00_MACRAW_SOCKET) != 0u)
    {
        w5x00_counter_inc(&driver->macraw_timeouts);
        w5x00_macraw_quarantine(driver);
        return false;
    }
    setSn_CR(W5X00_MACRAW_SOCKET, command);
    return w5x00_macraw_complete_command(driver);
}

static bool w5x00_macraw_wait_status(w5x00_ethernet_driver_t *driver, uint8_t expected_status)
{
    for (uint32_t poll = 0; poll < W5X00_MACRAW_STATUS_POLL_LIMIT; ++poll)
    {
        if (getSn_SR(W5X00_MACRAW_SOCKET) == expected_status)
        {
            return true;
        }
    }

    w5x00_counter_inc(&driver->macraw_timeouts);
    w5x00_macraw_quarantine(driver);
    return false;
}

/*
 * The vendor getSn_TX_FSR() retries forever until two snapshots agree.  The
 * driver owns the MACRAW deadline, so read two snapshots here and retry a
 * finite number of times instead.  A zero value is valid and is returned once
 * stable; it is handled by TX admission below.
 */
static bool w5x00_macraw_read_tx_free_bounded(uint16_t *free_space)
{
    for (uint32_t poll = 0; poll < W5X00_MACRAW_TX_FSR_STABLE_POLL_LIMIT; ++poll)
    {
        const uint16_t first = (uint16_t)(((uint16_t)WIZCHIP_READ(Sn_TX_FSR(W5X00_MACRAW_SOCKET)) << 8u) |
                                          WIZCHIP_READ(WIZCHIP_OFFSET_INC(Sn_TX_FSR(W5X00_MACRAW_SOCKET), 1u)));
        const uint16_t second = (uint16_t)(((uint16_t)WIZCHIP_READ(Sn_TX_FSR(W5X00_MACRAW_SOCKET)) << 8u) |
                                           WIZCHIP_READ(WIZCHIP_OFFSET_INC(Sn_TX_FSR(W5X00_MACRAW_SOCKET), 1u)));

        if (first == second)
        {
            *free_space = first;
            return true;
        }
    }

    return false;
}

/* Called with the driver-owned MACRAW mutex held. */
static int32_t w5x00_macraw_wait_tx_admission_locked(w5x00_ethernet_driver_t *driver,
                                                      uint16_t len)
{
    uint16_t free_space = 0u;

    for (uint32_t poll = 0; poll < W5X00_MACRAW_TX_ADMISSION_POLL_LIMIT; ++poll)
    {
        const uint8_t status = getSn_SR(W5X00_MACRAW_SOCKET);

        if (status != SOCK_MACRAW)
        {
            /* A transient socket state must not discard a DHCP Discover. */
            w5x00_macraw_quarantine(driver);
            if (!w5x00_macraw_recover_locked(driver))
            {
                w5x00_counter_inc(&driver->tx_interface_failures);
                w5x00_record_diagnostic_once(driver,
                                             &driver->tx_interface_diagnostic_reported,
                                             W5X00_MACRAW_DIAGNOSTIC_TX_INTERFACE,
                                             len,
                                             status);
                return W5X00_LWIP_SEND_INTERFACE;
            }
        }

        if (w5x00_macraw_read_tx_free_bounded(&free_space) && free_space >= len)
        {
            return (int32_t)len;
        }
    }

    w5x00_counter_inc(&driver->macraw_timeouts);
    w5x00_counter_inc(&driver->tx_admission_failures);
    w5x00_record_diagnostic_once(driver,
                                 &driver->tx_admission_diagnostic_reported,
                                 W5X00_MACRAW_DIAGNOSTIC_TX_ADMISSION,
                                 len,
                                 free_space);
    return W5X00_LWIP_SEND_BUFFER;
}

/* Called with the driver-owned MACRAW mutex held. */
static bool w5x00_macraw_recover_locked(w5x00_ethernet_driver_t *driver)
{
    if (!driver->macraw_quarantined)
    {
        return true;
    }

    /* Socket recovery invalidates a saved RX_RD observation. */
    w5x00_macraw_reset_rx_short_deferral(driver);

    if (driver->macraw_recovery_phase == W5X00_MACRAW_RECOVERY_WAIT_MACRAW)
    {
        if (!w5x00_macraw_wait_status(driver, SOCK_MACRAW))
        {
            return false;
        }
        driver->macraw_recovery_phase = W5X00_MACRAW_RECOVERY_IDLE;
        driver->macraw_quarantined = false;
        return true;
    }

    if (driver->macraw_recovery_phase == W5X00_MACRAW_RECOVERY_WAIT_CLOSED)
    {
        if (!w5x00_macraw_wait_status(driver, SOCK_CLOSED))
        {
            return false;
        }
    }
    else if (getSn_CR(W5X00_MACRAW_SOCKET) != 0u)
    {
        /* Do not reprogram an unresolved command; retain quarantine and retry later. */
        w5x00_counter_inc(&driver->macraw_timeouts);
        return false;
    }

    if (driver->macraw_recovery_phase == W5X00_MACRAW_RECOVERY_IDLE)
    {
        setSn_CR(W5X00_MACRAW_SOCKET, Sn_CR_CLOSE);
        driver->macraw_recovery_phase = W5X00_MACRAW_RECOVERY_WAIT_CLOSED;
        if (!w5x00_macraw_complete_command(driver) ||
            !w5x00_macraw_wait_status(driver, SOCK_CLOSED))
        {
            return false;
        }
    }

    setSn_MR(W5X00_MACRAW_SOCKET, Sn_MR_MACRAW);
    setSn_PORT(W5X00_MACRAW_SOCKET, 0u);
    setSn_IR(W5X00_MACRAW_SOCKET, Sn_IR_SENDOK | Sn_IR_TIMEOUT);
    if (getSn_CR(W5X00_MACRAW_SOCKET) != 0u)
    {
        w5x00_counter_inc(&driver->macraw_timeouts);
        w5x00_macraw_quarantine(driver);
        return false;
    }
    setSn_CR(W5X00_MACRAW_SOCKET, Sn_CR_OPEN);
    driver->macraw_recovery_phase = W5X00_MACRAW_RECOVERY_WAIT_MACRAW;
    if (!w5x00_macraw_complete_command(driver) ||
        !w5x00_macraw_wait_status(driver, SOCK_MACRAW))
    {
        return false;
    }

    driver->macraw_recovery_phase = W5X00_MACRAW_RECOVERY_IDLE;
    driver->macraw_quarantined = false;
    return true;
}

/* Called with the driver-owned MACRAW mutex held. */
static int32_t w5x00_macraw_send_locked(w5x00_ethernet_driver_t *driver,
                                        const uint8_t *buf,
                                        uint16_t len)
{
    const int32_t admission = w5x00_macraw_wait_tx_admission_locked(driver, len);

    if (admission != (int32_t)len)
    {
        return admission;
    }

    /* A stale completion must not acknowledge the new SEND. */
    setSn_IR(W5X00_MACRAW_SOCKET, Sn_IR_SENDOK | Sn_IR_TIMEOUT);
    wiz_send_data(W5X00_MACRAW_SOCKET, (uint8_t *)buf, len);
    if (!w5x00_macraw_issue_command(driver, Sn_CR_SEND))
    {
        w5x00_counter_inc(&driver->tx_send_failures);
        w5x00_record_diagnostic_once(driver,
                                     &driver->tx_send_diagnostic_reported,
                                     W5X00_MACRAW_DIAGNOSTIC_TX_SEND,
                                     len,
                                     getSn_IR(W5X00_MACRAW_SOCKET));
        return W5X00_LWIP_SEND_FAILURE;
    }

    for (uint32_t poll = 0; poll < W5X00_MACRAW_SEND_POLL_LIMIT; ++poll)
    {
        const uint8_t interrupt_status = getSn_IR(W5X00_MACRAW_SOCKET);

        if ((interrupt_status & Sn_IR_SENDOK) != 0u)
        {
            setSn_IR(W5X00_MACRAW_SOCKET, Sn_IR_SENDOK);
            return (int32_t)len;
        }
        if ((interrupt_status & Sn_IR_TIMEOUT) != 0u)
        {
            setSn_IR(W5X00_MACRAW_SOCKET, Sn_IR_TIMEOUT);
            w5x00_counter_inc(&driver->macraw_timeouts);
            w5x00_counter_inc(&driver->tx_send_failures);
            w5x00_macraw_quarantine(driver);
            w5x00_record_diagnostic_once(driver,
                                         &driver->tx_send_diagnostic_reported,
                                         W5X00_MACRAW_DIAGNOSTIC_TX_SEND,
                                         len,
                                         interrupt_status);
            return W5X00_LWIP_SEND_FAILURE;
        }
    }

    w5x00_counter_inc(&driver->macraw_timeouts);
    w5x00_counter_inc(&driver->tx_send_failures);
    w5x00_macraw_quarantine(driver);
    w5x00_record_diagnostic_once(driver,
                                 &driver->tx_send_diagnostic_reported,
                                 W5X00_MACRAW_DIAGNOSTIC_TX_SEND,
                                 len,
                                 getSn_IR(W5X00_MACRAW_SOCKET));
    return W5X00_LWIP_SEND_FAILURE;
}

err_t w5x00_lwip_tx_begin(struct netif *netif, uint8_t **frame, uint16_t *capacity)
{
    w5x00_ethernet_driver_t *driver = netif != NULL ? netif->state : NULL;

    if (frame == NULL || capacity == NULL || driver == NULL || !driver->initialized)
    {
        return ERR_IF;
    }
    if (!w5x00_macraw_lock(driver))
    {
        w5x00_counter_inc(&driver->tx_failures);
        return ERR_IF;
    }

    *frame = driver->tx_frame;
    *capacity = (uint16_t)sizeof(driver->tx_frame);
    return ERR_OK;
}

void w5x00_lwip_tx_end(struct netif *netif, bool success)
{
    w5x00_ethernet_driver_t *driver = netif != NULL ? netif->state : NULL;

    if (driver == NULL)
    {
        return;
    }
    if (!success)
    {
        w5x00_counter_inc(&driver->tx_failures);
    }
    w5x00_macraw_unlock_and_report(driver);
}

int32_t send_lwip(struct netif *netif, const uint8_t *buf, uint16_t len)
{
    w5x00_ethernet_driver_t *driver = netif != NULL ? netif->state : NULL;

    if (driver == NULL || buf == NULL || len == 0u || len > sizeof(driver->tx_frame))
    {
        return W5X00_LWIP_SEND_BUFFER;
    }

    return w5x00_macraw_send_locked(driver, buf, len);
}

int32_t recv_lwip(struct netif *netif, uint8_t *buf, uint16_t capacity)
{
    w5x00_ethernet_driver_t *driver = netif != NULL ? netif->state : NULL;
    uint8_t header[W5X00_MACRAW_RX_HEADER_LEN];
    uint16_t queued;
    uint16_t rx_read_pointer;
    uint16_t wire_length;
    uint16_t frame_length;
    w5x00_macraw_rx_snapshot_t snapshot = {0};

    if (driver == NULL || buf == NULL || capacity == 0u)
    {
        return W5X00_LWIP_RX_MALFORMED;
    }
    if (!w5x00_macraw_lock(driver))
    {
        return W5X00_LWIP_RX_CONTENTION;
    }

    queued = getSn_RX_RSR(W5X00_MACRAW_SOCKET);
    if (queued == 0u)
    {
        w5x00_macraw_reset_rx_short_deferral(driver);
        w5x00_macraw_unlock_and_report(driver);
        return 0;
    }

    rx_read_pointer = getSn_RX_RD(W5X00_MACRAW_SOCKET);
    if (queued < sizeof(header))
    {
        if (w5x00_macraw_defer_short_record_locked(driver, rx_read_pointer))
        {
            w5x00_macraw_unlock_and_report(driver);
            return 0;
        }

        /* No bounded record length is available, so retain the old fail-closed recovery path. */
        if (!driver->rx_drop_diagnostic_reported)
        {
            w5x00_capture_rx_snapshot_locked(&snapshot, rx_read_pointer);
        }
        w5x00_counter_inc(&driver->rx_malformed_frames);
        w5x00_record_diagnostic_once(driver,
                                     &driver->rx_drop_diagnostic_reported,
                                     W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED,
                                     queued,
                                     0u);
        w5x00_macraw_quarantine(driver);
        w5x00_macraw_reset_rx_short_deferral(driver);
        w5x00_macraw_unlock_and_report(driver);
        w5x00_emit_rx_snapshot(&snapshot);
        return W5X00_LWIP_RX_MALFORMED;
    }

    /* Read the prefix without changing Sn_RX_RD until its record is classified. */
    WIZCHIP_READ_BUF(w5x00_macraw_rx_buffer_address(rx_read_pointer),
                     header,
                     sizeof(header));
    wire_length = (uint16_t)(((uint16_t)header[0] << 8u) | header[1]);

    /*
     * RSR is queue occupancy, not the first-record length.  A 00 02 prefix
     * can therefore be followed by a whole DHCP Offer already in the queue.
     * Restore the pointer before the one-poll defer, then consume only the
     * declared short record when that observation remains unchanged.
     */
    if (queued < W5X00_MACRAW_MIN_RECORD_LEN ||
        wire_length < W5X00_MACRAW_MIN_RECORD_LEN)
    {
        if (w5x00_macraw_defer_short_record_locked(driver, rx_read_pointer))
        {
            w5x00_macraw_unlock_and_report(driver);
            return 0;
        }

        if (wire_length >= sizeof(header) && wire_length <= queued)
        {
            const uint16_t short_frame_length = (uint16_t)(wire_length - sizeof(header));

            if (!driver->rx_drop_diagnostic_reported)
            {
                w5x00_capture_rx_snapshot_locked(&snapshot, rx_read_pointer);
            }
            w5x00_counter_inc(&driver->rx_malformed_frames);
            w5x00_record_diagnostic_once(driver,
                                         &driver->rx_drop_diagnostic_reported,
                                         W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED,
                                         short_frame_length,
                                         0u);
            /* Prefix read was non-mutating; consume this complete short record once. */
            w5x00_macraw_consume_record(rx_read_pointer, wire_length);
            w5x00_macraw_reset_rx_short_deferral(driver);
            if (!w5x00_macraw_issue_command(driver, Sn_CR_RECV))
            {
                w5x00_macraw_unlock_and_report(driver);
                w5x00_emit_rx_snapshot(&snapshot);
                return W5X00_LWIP_RX_TIMEOUT;
            }
            w5x00_macraw_unlock_and_report(driver);
            w5x00_emit_rx_snapshot(&snapshot);
            return W5X00_LWIP_RX_MALFORMED;
        }

        /* A prefix without a complete declared record is not safely skippable. */
        if (!driver->rx_drop_diagnostic_reported)
        {
            w5x00_capture_rx_snapshot_locked(&snapshot, rx_read_pointer);
        }
        w5x00_counter_inc(&driver->rx_malformed_frames);
        w5x00_record_diagnostic_once(driver,
                                     &driver->rx_drop_diagnostic_reported,
                                     W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED,
                                     wire_length,
                                     0u);
        w5x00_macraw_quarantine(driver);
        w5x00_macraw_reset_rx_short_deferral(driver);
        w5x00_macraw_unlock_and_report(driver);
        w5x00_emit_rx_snapshot(&snapshot);
        return W5X00_LWIP_RX_MALFORMED;
    }

    if (wire_length < sizeof(header) || wire_length > queued)
    {
        if (!driver->rx_drop_diagnostic_reported)
        {
            w5x00_capture_rx_snapshot_locked(&snapshot, rx_read_pointer);
        }
        w5x00_counter_inc(&driver->rx_malformed_frames);
        w5x00_record_diagnostic_once(driver,
                                     &driver->rx_drop_diagnostic_reported,
                                     W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED,
                                     wire_length,
                                     0u);
        w5x00_macraw_quarantine(driver);
        w5x00_macraw_reset_rx_short_deferral(driver);
        w5x00_macraw_unlock_and_report(driver);
        w5x00_emit_rx_snapshot(&snapshot);
        return W5X00_LWIP_RX_MALFORMED;
    }

    frame_length = (uint16_t)(wire_length - sizeof(header));
    if (frame_length < W5X00_MACRAW_MIN_ETHERNET_FRAME_LEN)
    {
        if (!driver->rx_drop_diagnostic_reported)
        {
            w5x00_capture_rx_snapshot_locked(&snapshot, rx_read_pointer);
        }
        w5x00_counter_inc(&driver->rx_malformed_frames);
        w5x00_record_diagnostic_once(driver,
                                     &driver->rx_drop_diagnostic_reported,
                                     W5X00_MACRAW_DIAGNOSTIC_RX_MALFORMED,
                                     frame_length,
                                     0u);
        w5x00_macraw_consume_record(rx_read_pointer, wire_length);
        w5x00_macraw_reset_rx_short_deferral(driver);
        if (!w5x00_macraw_issue_command(driver, Sn_CR_RECV))
        {
            w5x00_macraw_unlock_and_report(driver);
            w5x00_emit_rx_snapshot(&snapshot);
            return W5X00_LWIP_RX_TIMEOUT;
        }
        w5x00_macraw_unlock_and_report(driver);
        w5x00_emit_rx_snapshot(&snapshot);
        return W5X00_LWIP_RX_MALFORMED;
    }

    if (frame_length > capacity || frame_length > W5X00_LWIP_MAX_FRAME_LEN)
    {
        if (!driver->rx_drop_diagnostic_reported)
        {
            w5x00_capture_rx_snapshot_locked(&snapshot, rx_read_pointer);
        }
        w5x00_counter_inc(&driver->rx_overflow_frames);
        w5x00_record_diagnostic_once(driver,
                                     &driver->rx_drop_diagnostic_reported,
                                     W5X00_MACRAW_DIAGNOSTIC_RX_OVERFLOW,
                                     frame_length,
                                     0u);
        w5x00_macraw_consume_record(rx_read_pointer, wire_length);
        w5x00_macraw_reset_rx_short_deferral(driver);
        if (!w5x00_macraw_issue_command(driver, Sn_CR_RECV))
        {
            w5x00_macraw_unlock_and_report(driver);
            w5x00_emit_rx_snapshot(&snapshot);
            return W5X00_LWIP_RX_TIMEOUT;
        }
        w5x00_macraw_unlock_and_report(driver);
        w5x00_emit_rx_snapshot(&snapshot);
        return W5X00_LWIP_RX_OVERFLOW;
    }

    w5x00_macraw_read_record_payload(rx_read_pointer, buf, frame_length);
    w5x00_macraw_consume_record(rx_read_pointer, wire_length);
    w5x00_macraw_reset_rx_short_deferral(driver);
    if (!w5x00_macraw_issue_command(driver, Sn_CR_RECV))
    {
        w5x00_macraw_unlock_and_report(driver);
        return W5X00_LWIP_RX_TIMEOUT;
    }
    w5x00_macraw_unlock_and_report(driver);
    return (int32_t)frame_length;
}

bool w5x00_ethernet_driver_init(w5x00_ethernet_driver_t *driver, const uint8_t mac[6])
{
    async_context_freertos_config_t async_config = async_context_freertos_default_config();

    if (driver == NULL)
    {
        return false;
    }

    memset(driver, 0, sizeof(*driver));

    driver->macraw_mutex = xSemaphoreCreateMutexStatic(&driver->macraw_mutex_storage);
    if (driver->macraw_mutex == NULL)
    {
        printf("[NET]\t\tFailed to create MACRAW mutex\n");
        return false;
    }

    if (mac != NULL)
    {
        memcpy(driver->mac, mac, sizeof(driver->mac));
    }

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_soft_reset();
    wizchip_initialize();
    wizchip_check();

    w5x00_lwip_set_mac_address(driver->mac);
    setSHAR(driver->mac);
    ctlwizchip(CW_RESET_PHY, NULL);

/*
 * Pico SDK switches all async-context objects to static allocation when
 * configSUPPORT_STATIC_ALLOCATION is enabled.  The default helper leaves the
 * required stack pointer unset, so provide the driver's bounded permanent
 * stack explicitly.
 */
#if configSUPPORT_STATIC_ALLOCATION
    async_config.task_stack = g_w5x00_async_context_stack;
#endif
    if (!async_context_freertos_init(&driver->async_context_freertos, &async_config))
    {
        printf("[NET]\t\tFailed to initialize FreeRTOS async context\n");
        return false;
    }
    lwip_freertos_init(&driver->async_context_freertos.core);

    if (!configure_netif(driver))
    {
        return false;
    }

    driver->initialized = true;
    return true;
}

bool w5x00_ethernet_driver_start_dhcp(w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return false;
    }

    return dhcp_start(&driver->netif) == ERR_OK;
}

void w5x00_ethernet_driver_set_static_ipv4(w5x00_ethernet_driver_t *driver,
                                           const uint8_t ip[4],
                                           const uint8_t netmask[4],
                                           const uint8_t gateway[4])
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask_addr;
    ip4_addr_t gateway_addr;

    if (driver == NULL || !driver->initialized)
    {
        return;
    }

    IP4_ADDR(&ipaddr, ip[0], ip[1], ip[2], ip[3]);
    IP4_ADDR(&netmask_addr, netmask[0], netmask[1], netmask[2], netmask[3]);
    IP4_ADDR(&gateway_addr, gateway[0], gateway[1], gateway[2], gateway[3]);
    netif_set_addr(&driver->netif, &ipaddr, &netmask_addr, &gateway_addr);
}

void w5x00_ethernet_driver_poll(w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return;
    }

    for (uint32_t frame = 0; frame < W5X00_ETHERNET_RX_POLL_BUDGET; ++frame)
    {
        const int32_t received = recv_lwip(&driver->netif,
                                           driver->packet_buffer,
                                           (uint16_t)sizeof(driver->packet_buffer));
        struct pbuf *packet;

        if (received == 0)
        {
            break;
        }
        if (received < 0)
        {
            w5x00_counter_inc(&driver->rx_errors);
            /* A malformed header remains queued deliberately; retrying it in this poll is useless. */
            break;
        }

        packet = pbuf_alloc(PBUF_RAW, (u16_t)received, PBUF_POOL);
        if (packet == NULL)
        {
            w5x00_counter_inc(&driver->pbuf_failures);
            w5x00_counter_inc(&driver->rx_errors);
            continue;
        }

        if (pbuf_take(packet, driver->packet_buffer, (u16_t)received) != ERR_OK)
        {
            pbuf_free(packet);
            w5x00_counter_inc(&driver->pbuf_failures);
            w5x00_counter_inc(&driver->rx_errors);
            continue;
        }

        LINK_STATS_INC(link.recv);

        /* The MACRAW mutex was released by recv_lwip before invoking lwIP. */
        if (driver->netif.input(packet, &driver->netif) != ERR_OK)
        {
            pbuf_free(packet);
            w5x00_counter_inc(&driver->input_errors);
            continue;
        }

        w5x00_counter_inc(&driver->rx_packets);
    }
}

bool w5x00_ethernet_driver_is_link_up(const w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return false;
    }

    return netif_is_link_up((struct netif *)&driver->netif) != 0;
}

bool w5x00_ethernet_driver_has_ipv4(const w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return false;
    }

    return !ip4_addr_isany_val(*netif_ip4_addr((struct netif *)&driver->netif));
}

struct netif *w5x00_ethernet_driver_get_netif(w5x00_ethernet_driver_t *driver)
{
    if (driver == NULL || !driver->initialized)
    {
        return NULL;
    }

    return &driver->netif;
}



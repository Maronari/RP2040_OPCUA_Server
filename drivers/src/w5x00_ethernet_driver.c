/**
 * @file
 * @brief W5500 MACRAW socket state machine: TX/RX framing, timeout-bounded
 *        register polling, and socket-recovery FSM.
 *
 * Responsibilities:
 * - Implement lwIP's low-level TX (w5x00_lwip_tx_begin/_end, send_lwip) and
 *   RX (recv_lwip) hooks against the W5500's single MACRAW socket
 *   (W5X00_MACRAW_SOCKET == 0).
 * - Replace ioLibrary's unbounded retry loops (e.g. getSn_TX_FSR()) with
 *   finite, counted polls so a stuck/unresponsive W5500 cannot hang a
 *   FreeRTOS task indefinitely.
 * - Detect and recover a MACRAW socket that has left MACRAW mode (closed,
 *   errored) via a resumable close/reopen state machine
 *   (w5x00_macraw_recovery_phase_t), rather than assuming the socket is
 *   always healthy.
 * - Rate-limit diagnostic printf output: each fault class is recorded once
 *   under the MACRAW mutex and printed once after the mutex is released
 *   (never while holding the SPI-serializing lock), and each condition is
 *   latched so it is not reprinted every poll while it persists.
 *
 * Ownership: all mutable state lives in the caller-owned
 * w5x00_ethernet_driver_t instance; this file has no static/global state
 * of its own (aside from the FreeRTOS async-context stack when static
 * allocation is configured).
 *
 * Concurrency: driver->macraw_mutex serializes every W5500 register/buffer
 * access. lwIP's calling task acquires it via w5x00_lwip_tx_begin()/
 * send_lwip(); NetworkTask acquires it via w5x00_ethernet_driver_poll() ->
 * recv_lwip(). recv_lwip() releases the mutex before handing a decoded
 * frame to `netif.input()`, so lwIP's own processing of a received frame
 * never runs while the MACRAW mutex is held.
 *
 * lwIP core lock: the netif, DHCP and DNS configuration calls
 * (configure_netif(), w5x00_ethernet_driver_start_dhcp(),
 * w5x00_ethernet_driver_set_static_ipv4()) run from InitTask after
 * lwip_freertos_init() has already started the tcpip thread, whose timers
 * walk the netif list, so each runs under LOCK_TCPIP_CORE(). pico-sdk maps
 * that to this driver's own async_context lock -- a recursive FreeRTOS
 * mutex. Nothing in this firmware registers async_context workers on that
 * context, so the lock's only other holders are the tcpip thread and
 * task-context lwIP API users (sockets, time_service's SNTP start).
 * Lock order is core lock, then MACRAW mutex: a transmit issued under the
 * core lock (a gratuitous ARP from netif_set_up()/netif_set_addr(), a DHCP
 * DISCOVER from dhcp_start()) reaches netif_output() -> send_lwip() while
 * holding it, exactly like every other lwIP transmit. Nothing may therefore
 * take the core lock while holding the MACRAW mutex, and recv_lwip() never
 * does. Until 2026-09-15 these calls ran unlocked.
 */
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
#include "lwip/dns.h"
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

/** @brief Clear any in-progress short-record deferral bookkeeping (see w5x00_macraw_defer_short_record_locked()). */
static void w5x00_macraw_reset_rx_short_deferral(w5x00_ethernet_driver_t *driver)
{
    driver->rx_short_deferred = false;
    driver->rx_short_deferral_count = 0u;
    driver->rx_short_deferred_pointer = 0u;
}

/**
 * @brief Allow one extra poll for a MACRAW record whose header/length bytes
 *        have not fully arrived yet, instead of treating it as malformed.
 *
 * Called while the MACRAW mutex is held. It never waits or issues RECV --
 * it only decides whether the caller should return "no data yet" (0) one
 * more time for the exact same `rx_read_pointer`, bounded to
 * W5X00_MACRAW_RX_SHORT_DEFERRAL_LIMIT deferrals before giving up and
 * falling through to the malformed-record path.
 *
 * @return true if the caller should defer (return 0, retry next poll);
 *         false once the deferral budget for this pointer is exhausted.
 */
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

/** @brief One diagnostic event handed off from w5x00_take_pending_diagnostic_locked() to w5x00_emit_diagnostic() after the MACRAW mutex is released. */
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

/** @brief Translate a raw Sn_RX_RD offset into the W5500's internal RX buffer memory address. */
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
/** @brief Read @p length payload bytes (after the 2-byte length header) at @p rx_read_pointer into @p buffer. */
static void w5x00_macraw_read_record_payload(uint16_t rx_read_pointer,
                                              uint8_t *buffer,
                                              uint16_t length)
{
    WIZCHIP_READ_BUF(w5x00_macraw_rx_buffer_address(
                         (uint16_t)(rx_read_pointer + W5X00_MACRAW_RX_HEADER_LEN)),
                     buffer,
                     length);
}

/** @brief Advance Sn_RX_RD past one fully-consumed @p wire_length-byte MACRAW record. */
static void w5x00_macraw_consume_record(uint16_t rx_read_pointer,
                                        uint16_t wire_length)
{
    setSn_RX_RD(W5X00_MACRAW_SOCKET, (uint16_t)(rx_read_pointer + wire_length));
}

/* Diagnostic sampling must never inherit ioLibrary's unbounded stability loop. */
/** @brief Read Sn_RX_RSR exactly once (no ioLibrary-style stability retry loop); for diagnostic snapshots only. */
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

/** @brief Print a captured RX register/buffer snapshot via W5X00_DIAGNOSTIC_PRINTF; no-op if the snapshot was never captured (snapshot->valid == false). Must be called after the MACRAW mutex is released. */
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

/**
 * @brief Latch one diagnostic event as pending, but only the first time
 *        @p reported is false, to avoid re-recording (and later
 *        re-printing) the same persistent condition every poll.
 *
 * All MACRAW diagnostics are recorded while serialized (MACRAW mutex held)
 * and printed after unlock via w5x00_macraw_unlock_and_report(). Called
 * with the mutex held.
 *
 * @param reported Caller-owned per-condition latch (e.g.
 *        driver->rx_drop_diagnostic_reported); set true on first record.
 */
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

/**
 * @brief Take and clear the pending diagnostic (if any) for later printing.
 *
 * Called while the MACRAW mutex is held; ownership of the returned record
 * transfers to the caller, which must print it only after releasing the
 * mutex (see w5x00_macraw_unlock_and_report()).
 */
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

/**
 * @brief Print one taken diagnostic record's fault-specific message.
 *
 * No-op if @p record->valid is false (nothing was pending). Must be called
 * without the MACRAW mutex held (uses printf, which must not run under the
 * SPI-serializing lock).
 */
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

/**
 * @brief Register and bring up the lwIP netif, then quarantine and
 *        immediately recover the MACRAW socket into a known-open state.
 *
 * Called once from w5x00_ethernet_driver_init(). netif_initialize()
 * (port/lwip/w5x00_lwip.c) wires send_lwip/recv_lwip as the netif's
 * low-level TX/RX hooks. Deliberately forces `macraw_quarantined = true`
 * before the first lock so the very first send/receive always goes through
 * the recovery FSM rather than assuming the socket happens to already be
 * in MACRAW mode after chip reset.
 *
 * @return false if netif_add() or the initial MACRAW socket
 *         acquisition/recovery fails; true otherwise.
 */
static bool configure_netif(w5x00_ethernet_driver_t *driver)
{
    struct netif *added;

    /* The tcpip thread is already running (lwip_freertos_init()), so the
     * netif API needs the core lock; see the file header for lock order. The
     * MACRAW recovery below is not lwIP and runs after the lock is released. */
    LOCK_TCPIP_CORE();
    added = netif_add(&driver->netif, IP_ADDR_ANY, IP_ADDR_ANY, IP_ADDR_ANY, driver, netif_initialize, tcpip_input);
    if (added != NULL)
    {
        driver->netif.name[0] = 'e';
        driver->netif.name[1] = '0';

        netif_set_link_callback(&driver->netif, netif_link_callback);
        netif_set_status_callback(&driver->netif, netif_status_callback);
        netif_set_default(&driver->netif);
        netif_set_link_up(&driver->netif);
        netif_set_up(&driver->netif);
    }
    UNLOCK_TCPIP_CORE();

    if (added == NULL)
    {
        printf("[NET]\t\tFailed to add lwIP netif\n");
        return false;
    }

    driver->macraw_quarantined = true;
    if (!w5x00_macraw_lock(driver))
    {
        printf("[NET]\t\tFailed to acquire MACRAW socket\n");
        return false;
    }
    w5x00_macraw_unlock(driver);

    return true;
}

/** @brief Atomically increment a diagnostic counter; safe even without the MACRAW mutex held. */
static void w5x00_counter_inc(uint32_t *counter)
{
    (void)__atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
}

/** @brief Mark the MACRAW socket as needing recovery before its next use (checked/cleared by w5x00_macraw_recover_locked()). */
static void w5x00_macraw_quarantine(w5x00_ethernet_driver_t *driver)
{
    driver->macraw_quarantined = true;
}

/**
 * @brief Acquire the MACRAW mutex (bounded wait) and ensure the socket is
 *        out of quarantine before returning success.
 *
 * @return false if @p driver/its mutex is NULL, the mutex could not be
 *         taken within W5X00_MACRAW_MUTEX_WAIT_MS (counts
 *         macraw_contention), or socket recovery fails after acquiring the
 *         mutex (mutex is released again in that case). true only once the
 *         mutex is held and the socket is confirmed in MACRAW mode.
 */
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

/** @brief Release the MACRAW mutex without touching any pending diagnostic. */
static void w5x00_macraw_unlock(w5x00_ethernet_driver_t *driver)
{
    (void)xSemaphoreGive(driver->macraw_mutex);
}

/** @brief Take any pending diagnostic while still locked, release the mutex, then print it -- the standard unlock path used by every MACRAW operation. */
static void w5x00_macraw_unlock_and_report(w5x00_ethernet_driver_t *driver)
{
    const w5x00_macraw_diagnostic_record_t record = w5x00_take_pending_diagnostic_locked(driver);

    w5x00_macraw_unlock(driver);
    w5x00_emit_diagnostic(driver, &record);
}

/**
 * @brief Poll Sn_CR until the W5500 clears it to 0 (command consumed), up
 *        to W5X00_MACRAW_COMMAND_POLL_LIMIT times.
 * @return true once Sn_CR reads 0; false on timeout (counts
 *         macraw_timeouts and quarantines the socket for recovery).
 */
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

/**
 * @brief Issue one Sn_CR command register write and wait for it to complete.
 * @return false without writing the register if a previous command is
 *         still in flight (Sn_CR != 0, quarantines the socket); otherwise
 *         the result of w5x00_macraw_complete_command().
 */
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

/**
 * @brief Poll Sn_SR until it equals @p expected_status, up to
 *        W5X00_MACRAW_STATUS_POLL_LIMIT times.
 * @return true once matched; false on timeout (counts macraw_timeouts and
 *         quarantines the socket).
 */
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
/**
 * @brief Read Sn_TX_FSR twice and accept the value only if both reads
 *        agree, retrying up to W5X00_MACRAW_TX_FSR_STABLE_POLL_LIMIT times.
 * @param free_space Set to the stable free-space value on success.
 * @return false if the two reads never agreed within the poll budget
 *         (caller must not trust @p free_space in that case). A stable
 *         value of 0 is a valid result (TX buffer momentarily full).
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

/**
 * @brief Wait for the MACRAW TX buffer to have at least @p len bytes free,
 *        recovering the socket first if it has left MACRAW mode.
 *
 * Called with the driver-owned MACRAW mutex held. A non-MACRAW status is
 * deliberately not treated as a hard failure on its own -- it triggers an
 * inline recovery attempt so a transient state does not silently drop a
 * time-sensitive frame (e.g. a DHCP Discover), only failing once recovery
 * itself fails.
 *
 * @return (int32_t)len once admission is confirmed;
 *         W5X00_LWIP_SEND_INTERFACE if the socket left MACRAW mode and
 *         could not be recovered; W5X00_LWIP_SEND_BUFFER if free space
 *         never reached @p len within W5X00_MACRAW_TX_ADMISSION_POLL_LIMIT
 *         polls.
 */
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

/**
 * @brief Resumable close/reopen recovery for a quarantined MACRAW socket.
 *
 * Called with the driver-owned MACRAW mutex held. No-op (returns true
 * immediately) if the socket is not currently quarantined. Otherwise walks
 * `macraw_recovery_phase` (w5x00_macraw_recovery_phase_t) forward one step
 * at a time: issue CLOSE and wait SOCK_CLOSED, then reprogram MACRAW mode
 * and issue OPEN and wait SOCK_MACRAW. Each wait is bounded
 * (w5x00_macraw_wait_status()'s poll limit); a timed-out wait leaves
 * `macraw_recovery_phase` at its current step so the next call to this
 * function resumes from there rather than restarting the whole sequence.
 * Resets any in-progress short-record RX deferral first, since recovery
 * invalidates any saved RX_RD observation.
 *
 * @return true once the socket is confirmed back in SOCK_MACRAW (quarantine
 *         cleared); false if any step is still pending/timed out.
 * @note Hardware side effect: up to 2 Sn_CR command issues plus register
 *       writes (Sn_MR, Sn_PORT, Sn_IR) over SPI.
 */
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

/**
 * @brief Transmit one MACRAW frame: wait for TX admission, clear stale
 *        completion interrupts, write the frame, issue SEND, and wait for
 *        SENDOK.
 *
 * Called with the driver-owned MACRAW mutex held. This is send_lwip()'s
 * core implementation.
 *
 * @return (int32_t)len on success; the admission failure code
 *         (W5X00_LWIP_SEND_INTERFACE/W5X00_LWIP_SEND_BUFFER) if admission
 *         fails; W5X00_LWIP_SEND_FAILURE if the SEND command does not
 *         complete, or SENDOK is not observed before
 *         W5X00_MACRAW_SEND_POLL_LIMIT polls or a TIMEOUT interrupt fires
 *         first (quarantines the socket in both failure cases).
 * @note Hardware side effect: one Sn_IR clear, one frame write via
 *       `wiz_send_data()`, one Sn_CR_SEND command, and Sn_IR polling.
 */
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

/**
 * @brief lwIP low-level TX hook (start of frame): lock the MACRAW socket
 *        and hand back the driver's TX scratch buffer for lwIP to fill.
 *
 * Declared in the vendor/port header port/lwip/w5x00_lwip.h (not owned by
 * this module) and wired as the netif's TX-begin hook by
 * netif_initialize() in port/lwip/w5x00_lwip.c. Invoked by lwIP's own
 * output path on whichever task is currently transmitting.
 *
 * @param netif The driver's netif; `netif->state` must be the owning
 *        w5x00_ethernet_driver_t.
 * @param frame Out: set to `driver->tx_frame` on success.
 * @param capacity Out: set to `sizeof(driver->tx_frame)` on success.
 * @return ERR_IF if arguments are invalid, the driver is not initialized,
 *         or the MACRAW mutex could not be acquired (counts tx_failures);
 *         ERR_OK otherwise.
 * @post On ERR_OK, the MACRAW mutex is held until a matching
 *       w5x00_lwip_tx_end() call.
 */
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

/**
 * @brief lwIP low-level TX hook (end of frame): release the MACRAW socket
 *        lock acquired by w5x00_lwip_tx_begin().
 *
 * Declared in port/lwip/w5x00_lwip.h; must be called exactly once for every
 * successful w5x00_lwip_tx_begin() call, regardless of @p success.
 *
 * @param success false if lwIP failed to fill the frame buffer (counted as
 *        tx_failures); the frame is not sent by this call either way --
 *        actual transmission happens via send_lwip().
 */
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

/**
 * @brief lwIP low-level TX hook: transmit @p len bytes from @p buf (which
 *        must be the buffer previously handed out by w5x00_lwip_tx_begin())
 *        over the MACRAW socket.
 *
 * Declared in port/lwip/w5x00_lwip.h. Delegates to
 * w5x00_macraw_send_locked() while the MACRAW mutex acquired by
 * w5x00_lwip_tx_begin() is still held.
 *
 * @return W5X00_LWIP_SEND_BUFFER if arguments are invalid or @p len exceeds
 *         the driver's TX frame capacity; otherwise the result of
 *         w5x00_macraw_send_locked() (frame length on success, or a
 *         negative W5X00_LWIP_SEND_* failure code).
 */
int32_t send_lwip(struct netif *netif, const uint8_t *buf, uint16_t len)
{
    w5x00_ethernet_driver_t *driver = netif != NULL ? netif->state : NULL;

    if (driver == NULL || buf == NULL || len == 0u || len > sizeof(driver->tx_frame))
    {
        return W5X00_LWIP_SEND_BUFFER;
    }

    return w5x00_macraw_send_locked(driver, buf, len);
}

/**
 * @brief lwIP low-level RX hook: dequeue and validate one MACRAW record
 *        from the W5500 into @p buf.
 *
 * Declared in port/lwip/w5x00_lwip.h; called by
 * w5x00_ethernet_driver_poll() in a loop (NetworkTask). Acquires and
 * releases the MACRAW mutex internally for the duration of one record's
 * classification (never leaves it held across the return to the caller).
 *
 * Validation order, each stage consuming/dropping the record and
 * quarantining the socket on failure except where noted:
 * 1. If fewer than W5X00_MACRAW_RX_HEADER_LEN bytes are queued, defer once
 *    (see w5x00_macraw_defer_short_record_locked()) before treating it as
 *    malformed.
 * 2. Read the 2-byte length prefix without advancing Sn_RX_RD. If the
 *    declared record is shorter than the minimum legal Ethernet-frame
 *    record, defer once; if a complete short record is nonetheless fully
 *    queued once the deferral is exhausted, consume just that record as
 *    malformed (bounded skip) rather than quarantining on an
 *    unboundedly-sized unknown remainder.
 * 3. Reject (and quarantine) if the declared wire_length is inconsistent
 *    with queued bytes, or if the derived Ethernet frame length is below
 *    the minimum legal frame size.
 * 4. Reject as overflow (consuming the record, no quarantine) if the frame
 *    is larger than @p capacity or the lwIP frame limit.
 * 5. Otherwise, copy the payload, advance Sn_RX_RD past the whole record,
 *    and issue RECV.
 *
 * @return >0: number of payload bytes copied into @p buf (a valid frame).
 *          0: no complete record available yet (queue empty or a bounded
 *             short-record deferral).
 *         W5X00_LWIP_RX_MALFORMED / _OVERFLOW / _TIMEOUT / _CONTENTION on
 *         the corresponding failure (see the `#define`s above).
 * @note Hardware side effect: W5500 RX buffer/register reads and, on a
 *       consumed record, an Sn_RX_RD update plus an Sn_CR_RECV command.
 */
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
    err_t result;

    if (driver == NULL || !driver->initialized)
    {
        return false;
    }

    /* `initialized` implies lwip_freertos_init() ran, so the core lock exists. */
    LOCK_TCPIP_CORE();
    result = dhcp_start(&driver->netif);
    UNLOCK_TCPIP_CORE();
    return result == ERR_OK;
}

void w5x00_ethernet_driver_set_static_ipv4(w5x00_ethernet_driver_t *driver,
                                           const uint8_t ip[4],
                                           const uint8_t netmask[4],
                                           const uint8_t gateway[4],
                                           const uint8_t dns[4])
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask_addr;
    ip4_addr_t gateway_addr;
    ip4_addr_t dns_addr;

    if (driver == NULL || !driver->initialized)
    {
        return;
    }

    IP4_ADDR(&ipaddr, ip[0], ip[1], ip[2], ip[3]);
    IP4_ADDR(&netmask_addr, netmask[0], netmask[1], netmask[2], netmask[3]);
    IP4_ADDR(&gateway_addr, gateway[0], gateway[1], gateway[2], gateway[3]);
    IP4_ADDR(&dns_addr, dns[0], dns[1], dns[2], dns[3]);
    LOCK_TCPIP_CORE();
    netif_set_addr(&driver->netif, &ipaddr, &netmask_addr, &gateway_addr);
    dns_setserver(0, &dns_addr);
    UNLOCK_TCPIP_CORE();
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



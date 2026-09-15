#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/w5x00_ethernet_driver.h"
#include "lwip/pbuf.h"
#include "socket.h"
#include "w5500.h"

static uint8_t fake_rx[8192];
static uint16_t fake_rx_length;
static uint16_t fake_rx_rd;
static uint16_t fake_rx_rsr_override;
static unsigned fake_rx_rsr_override_reads;
static uint8_t fake_tx[2048];
static uint16_t fake_tx_length;
static uint16_t fake_tx_free = sizeof(fake_tx);
static unsigned fake_tx_zero_samples;
static unsigned fake_tx_unstable_samples;
static unsigned fake_tx_fsr_read_phase;
static int fake_reject_legacy_tx_fsr;
static uint8_t fake_ir;
static int fake_command_stuck;
static int fake_send_timeout;
static uint8_t fake_socket_status = SOCK_MACRAW;
static unsigned fake_close_delay;
static unsigned fake_open_delay;
static int fake_close_pending;
static int fake_open_pending;
static int fake_reconfigure_before_closed;
static unsigned fake_close_commands;
static unsigned fake_open_commands;
static int fake_pbuf_fail;
static int input_count;
static u16_t last_input_length;
static uint8_t last_input_payload[W5X00_LWIP_MAX_FRAME_LEN];
static SemaphoreHandle_t fake_console_mutex;
static unsigned fake_console_diagnostics;
static unsigned fake_console_admission_diagnostics;
static unsigned fake_console_send_diagnostics;
static unsigned fake_console_snapshot_diagnostics;
static unsigned fake_rx_rd_set_calls;
static unsigned fake_command_calls;
static unsigned fake_snapshot_raw_reads;
static uint32_t fake_snapshot_raw_addresses[2];
static unsigned fake_snapshot_raw_rd_set_calls[2];
static unsigned fake_snapshot_raw_command_calls[2];
static uint8_t fake_snapshot_raw_first[16];
static uint8_t fake_snapshot_raw_second[16];
static int fake_snapshot_raw_second_differs;
static void (*fake_unlock_hook)(void);
static int fake_unlock_hook_active;
static w5x00_ethernet_driver_t *fake_interleaved_driver;
static struct pbuf *fake_interleaved_packet;

int w5x00_test_printf(const char *format, ...)
{
    if (strncmp(format, "[NET]\t\tMACRAW", 12u) == 0)
    {
        assert(fake_console_mutex == NULL || fake_console_mutex->locked == 0);
        fake_console_diagnostics++;
        if (strstr(format, "TX admission") != NULL)
        {
            fake_console_admission_diagnostics++;
        }
        if (strstr(format, "SEND failed") != NULL)
        {
            fake_console_send_diagnostics++;
        }
        if (strstr(format, "RX snapshot") != NULL)
        {
            fake_console_snapshot_diagnostics++;
        }
    }
    return 0;
}

static StaticSemaphore_t *fake_last_mutex;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { storage->locked = 0; fake_last_mutex = storage; return storage; }
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) { (void)ticks; if (semaphore->locked) return pdFALSE; semaphore->locked = 1; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t semaphore) { semaphore->locked = 0; if (fake_unlock_hook != NULL && !fake_unlock_hook_active) { fake_unlock_hook_active = 1; fake_unlock_hook(); fake_unlock_hook_active = 0; } return pdTRUE; }

/* lwIP core lock. The netif/DHCP/DNS configuration API is core-locked in
 * lwIP, and the driver calls it after lwip_freertos_init() has started the
 * tcpip thread, so each stub below asserts the lock is held. */
static int fake_core_lock_depth;
static unsigned fake_core_lock_takes;
static unsigned fake_core_locked_calls;
static ip4_addr_t fake_dns_server;
void w5x00_test_lock_tcpip_core(void)
{
    /* Lock order is core lock, then MACRAW mutex: never the reverse. */
    assert(fake_last_mutex == NULL || fake_last_mutex->locked == 0);
    ++fake_core_lock_depth;
    ++fake_core_lock_takes;
}
void w5x00_test_unlock_tcpip_core(void) { assert(fake_core_lock_depth > 0); --fake_core_lock_depth; }
static void core_locked_call(void) { assert(fake_core_lock_depth > 0); ++fake_core_locked_calls; }

struct netif *netif_add(struct netif *n, const void *a, const void *b, const void *c, void *state, netif_init_fn init, netif_input_fn input) { (void)a; (void)b; (void)c; core_locked_call(); n->state = state; n->input = input; return init(n) == ERR_OK ? n : NULL; }
void netif_set_link_callback(struct netif *n, void (*callback)(struct netif *)) { (void)n; (void)callback; core_locked_call(); }
void netif_set_status_callback(struct netif *n, void (*callback)(struct netif *)) { (void)n; (void)callback; core_locked_call(); }
void netif_set_default(struct netif *n) { (void)n; core_locked_call(); }
void netif_set_link_up(struct netif *n) { core_locked_call(); n->link_up = 1; }
void netif_set_up(struct netif *n) { (void)n; core_locked_call(); }
int netif_is_link_up(struct netif *n) { return n->link_up; }
const ip4_addr_t *netif_ip4_addr(struct netif *n) { return &n->ip_addr; }
void netif_set_addr(struct netif *n, const ip4_addr_t *a, const ip4_addr_t *b, const ip4_addr_t *c) { (void)b; (void)c; core_locked_call(); n->ip_addr = *a; }
void dns_setserver(uint8_t numdns, const ip4_addr_t *dnsserver) { assert(numdns == 0u); core_locked_call(); fake_dns_server = *dnsserver; }
const char *ip4addr_ntoa(const ip4_addr_t *a) { (void)a; return "0.0.0.0"; }
err_t dhcp_start(struct netif *n) { (void)n; core_locked_call(); return ERR_OK; }
err_t tcpip_input(struct pbuf *p, struct netif *n) { (void)p; (void)n; return ERR_OK; }
err_t etharp_output(struct netif *n, const void *a, const void *b) { (void)n; (void)a; (void)b; return ERR_OK; }
struct pbuf *pbuf_alloc(int layer, u16_t length, int type) { struct pbuf *p; (void)layer; (void)type; if (fake_pbuf_fail) return NULL; p = calloc(1, sizeof(*p)); if (p != NULL) { p->payload = malloc(length); if (p->payload == NULL) { free(p); return NULL; } p->len = length; p->tot_len = length; } return p; }
err_t pbuf_take(struct pbuf *p, const void *data, u16_t length) { if (length != p->len) return ERR_BUF; memcpy(p->payload, data, length); return ERR_OK; }
void pbuf_free(struct pbuf *p) { free(p->payload); free(p); }
async_context_freertos_config_t async_context_freertos_default_config(void) { async_context_freertos_config_t config = {0}; return config; }
bool async_context_freertos_init(async_context_freertos_t *a, async_context_freertos_config_t *c) { (void)a; (void)c; return true; }
void lwip_freertos_init(void *core) { (void)core; }
void wizchip_spi_initialize(void) {}
void wizchip_cris_initialize(void) {}
void wizchip_reset(void) {}
void wizchip_soft_reset(void) {}
void wizchip_initialize(void) {}
void wizchip_check(void) {}
void setSHAR(uint8_t *mac) { (void)mac; }
void ctlwizchip(uint8_t command, void *argument) { (void)command; (void)argument; }
int8_t socket(uint8_t a, uint8_t b, uint16_t c, uint8_t d) { (void)a; (void)b; (void)c; (void)d; return 0; }
uint8_t getSn_SR(uint8_t sn) { (void)sn; if (fake_close_pending) { if (fake_close_delay > 0u) { fake_close_delay--; } else { fake_close_pending = 0; fake_socket_status = SOCK_CLOSED; } } else if (fake_open_pending) { if (fake_open_delay > 0u) { fake_open_delay--; } else { fake_open_pending = 0; fake_socket_status = SOCK_MACRAW; } } return fake_socket_status; }
uint16_t getSn_TX_FSR(uint8_t sn) { (void)sn; assert(fake_reject_legacy_tx_fsr == 0); return fake_tx_free; }
uint8_t WIZCHIP_READ(uint32_t address)
{
    if (address == Sn_RX_RSR(0))
    {
        return (uint8_t)(getSn_RX_RSR(0) >> 8u);
    }
    if (address == WIZCHIP_OFFSET_INC(Sn_RX_RSR(0), 1u))
    {
        return (uint8_t)getSn_RX_RSR(0);
    }
    uint16_t value = fake_tx_zero_samples != 0u ? 0u : fake_tx_free;
    const uint32_t expected_address = fake_tx_fsr_read_phase == 0u || fake_tx_fsr_read_phase == 2u ?
                                      Sn_TX_FSR(0) : WIZCHIP_OFFSET_INC(Sn_TX_FSR(0), 1u);

    assert(address == expected_address);
    if (fake_tx_unstable_samples != 0u && fake_tx_fsr_read_phase >= 2u)
    {
        value = (uint16_t)(value ^ 0x0001u);
    }
    const uint8_t result = (fake_tx_fsr_read_phase == 0u || fake_tx_fsr_read_phase == 2u) ?
                           (uint8_t)(value >> 8u) : (uint8_t)value;

    (void)address;
    fake_tx_fsr_read_phase = (fake_tx_fsr_read_phase + 1u) % 4u;
    if (fake_tx_fsr_read_phase == 0u && fake_tx_zero_samples != 0u)
    {
        fake_tx_zero_samples--;
    }
    if (fake_tx_fsr_read_phase == 0u && fake_tx_unstable_samples != 0u)
    {
        fake_tx_unstable_samples--;
    }
    return result;
}
uint16_t getSn_RX_RSR(uint8_t sn) { (void)sn; if (fake_rx_rsr_override_reads != 0u) { fake_rx_rsr_override_reads--; return fake_rx_rsr_override; } return (uint16_t)(fake_rx_length - fake_rx_rd); }
uint16_t getSn_RX_RD(uint8_t sn) { (void)sn; return fake_rx_rd; }
void setSn_RX_RD(uint8_t sn, uint16_t value) { (void)sn; fake_rx_rd_set_calls++; fake_rx_rd = value; }
uint8_t getSn_MR(uint8_t sn) { (void)sn; return Sn_MR_MACRAW; }
uint8_t getSn_RXBUF_SIZE(uint8_t sn) { (void)sn; return 8u; }
uint8_t getSn_TXBUF_SIZE(uint8_t sn) { (void)sn; return 8u; }
uint16_t getSn_RX_WR(uint8_t sn) { (void)sn; return fake_rx_length; }
void WIZCHIP_READ_BUF(uint32_t address, uint8_t *buffer, uint16_t length)
{
    const uint16_t pointer = (uint16_t)((address - (WIZCHIP_RXBUF_BLOCK(0) << 3u)) >> 8u);

    assert(pointer <= sizeof(fake_rx) && length <= sizeof(fake_rx) - pointer);
    memcpy(buffer, fake_rx + pointer, length);
    if (length == 16u)
    {
        assert(fake_snapshot_raw_reads < 2u);
        fake_snapshot_raw_addresses[fake_snapshot_raw_reads] = address;
        fake_snapshot_raw_rd_set_calls[fake_snapshot_raw_reads] = fake_rx_rd_set_calls;
        fake_snapshot_raw_command_calls[fake_snapshot_raw_reads] = fake_command_calls;
        if (fake_snapshot_raw_reads == 0u)
        {
            memcpy(fake_snapshot_raw_first, buffer, sizeof(fake_snapshot_raw_first));
        }
        else
        {
            if (fake_snapshot_raw_second_differs)
            {
                buffer[0] ^= 0xffu;
            }
            memcpy(fake_snapshot_raw_second, buffer, sizeof(fake_snapshot_raw_second));
        }
        fake_snapshot_raw_reads++;
    }
}
void setSn_MR(uint8_t sn, uint8_t mode) { (void)sn; (void)mode; if (fake_socket_status != SOCK_CLOSED) fake_reconfigure_before_closed++; }
void setSn_PORT(uint8_t sn, uint16_t port) { (void)sn; (void)port; if (fake_socket_status != SOCK_CLOSED) fake_reconfigure_before_closed++; }
uint8_t getSn_CR(uint8_t sn) { (void)sn; return fake_command_stuck ? Sn_CR_RECV : 0u; }
void setSn_CR(uint8_t sn, uint8_t command) { (void)sn; fake_command_calls++; if (command == Sn_CR_CLOSE) { fake_close_commands++; fake_rx_length = 0; fake_rx_rd = 0; fake_ir = 0; fake_close_pending = 1; } if (command == Sn_CR_OPEN) { fake_open_commands++; if (fake_socket_status != SOCK_CLOSED) fake_reconfigure_before_closed++; fake_open_pending = 1; } if (command == Sn_CR_SEND && !fake_command_stuck) fake_ir |= fake_send_timeout ? Sn_IR_TIMEOUT : Sn_IR_SENDOK; }
uint8_t getSn_IR(uint8_t sn) { (void)sn; return fake_ir; }
void setSn_IR(uint8_t sn, uint8_t status) { (void)sn; fake_ir &= (uint8_t)~status; }
void wiz_send_data(uint8_t sn, uint8_t *data, uint16_t length) { (void)sn; memcpy(fake_tx, data, length); fake_tx_length = length; }
void wiz_recv_data(uint8_t sn, uint8_t *data, uint16_t length) { (void)sn; memcpy(data, fake_rx + fake_rx_rd, length); fake_rx_rd = (uint16_t)(fake_rx_rd + length); }
void wiz_recv_ignore(uint8_t sn, uint16_t length) { (void)sn; fake_rx_rd = (uint16_t)(fake_rx_rd + length); }

static err_t test_input(struct pbuf *p, struct netif *n) { (void)n; input_count++; last_input_length = p->tot_len; assert(p->tot_len <= sizeof(last_input_payload)); memcpy(last_input_payload, p->payload, p->tot_len); pbuf_free(p); return ERR_OK; }
static void reset_fake(void) { memset(fake_rx, 0, sizeof(fake_rx)); fake_rx_length = 0; fake_rx_rd = 0; fake_rx_rsr_override = 0; fake_rx_rsr_override_reads = 0; fake_tx_length = 0; fake_tx_free = sizeof(fake_tx); fake_tx_zero_samples = 0; fake_tx_unstable_samples = 0; fake_tx_fsr_read_phase = 0; fake_reject_legacy_tx_fsr = 0; fake_ir = Sn_IR_SENDOK; fake_command_stuck = 0; fake_send_timeout = 0; fake_socket_status = SOCK_MACRAW; fake_close_delay = 0; fake_open_delay = 0; fake_close_pending = 0; fake_open_pending = 0; fake_reconfigure_before_closed = 0; fake_close_commands = 0; fake_open_commands = 0; fake_pbuf_fail = 0; input_count = 0; last_input_length = 0; memset(last_input_payload, 0, sizeof(last_input_payload)); fake_console_diagnostics = 0; fake_console_admission_diagnostics = 0; fake_console_send_diagnostics = 0; fake_console_snapshot_diagnostics = 0; fake_rx_rd_set_calls = 0; fake_command_calls = 0; fake_snapshot_raw_reads = 0; memset(fake_snapshot_raw_addresses, 0, sizeof(fake_snapshot_raw_addresses)); memset(fake_snapshot_raw_rd_set_calls, 0, sizeof(fake_snapshot_raw_rd_set_calls)); memset(fake_snapshot_raw_command_calls, 0, sizeof(fake_snapshot_raw_command_calls)); memset(fake_snapshot_raw_first, 0, sizeof(fake_snapshot_raw_first)); memset(fake_snapshot_raw_second, 0, sizeof(fake_snapshot_raw_second)); fake_snapshot_raw_second_differs = 0; fake_unlock_hook = NULL; fake_unlock_hook_active = 0; fake_interleaved_driver = NULL; fake_interleaved_packet = NULL; }
static void enqueue_frame(const uint8_t *frame, uint16_t length) { fake_rx[fake_rx_length++] = (uint8_t)((length + 2u) >> 8u); fake_rx[fake_rx_length++] = (uint8_t)(length + 2u); memcpy(fake_rx + fake_rx_length, frame, length); fake_rx_length = (uint16_t)(fake_rx_length + length); }
static void prepare_driver(w5x00_ethernet_driver_t *driver) { memset(driver, 0, sizeof(*driver)); driver->initialized = true; driver->netif.state = driver; driver->netif.input = test_input; driver->macraw_mutex = xSemaphoreCreateMutexStatic(&driver->macraw_mutex_storage); fake_console_mutex = driver->macraw_mutex; }

static void test_rx_canary_and_oversize_recovery(void) { w5x00_ethernet_driver_t driver; uint8_t oversized[1515] = {0}; uint8_t valid[60] = {0}; struct { uint8_t before; uint8_t frame[W5X00_LWIP_MAX_FRAME_LEN]; uint8_t after; } guarded = {0xa5, {0}, 0x5a}; prepare_driver(&driver); reset_fake(); enqueue_frame(oversized, sizeof(oversized)); enqueue_frame(valid, sizeof(valid)); assert(recv_lwip(&driver.netif, guarded.frame, sizeof(guarded.frame)) < 0); assert(guarded.before == 0xa5 && guarded.after == 0x5a); assert(driver.rx_overflow_frames == 1u); assert(fake_console_snapshot_diagnostics == 4u); assert(recv_lwip(&driver.netif, guarded.frame, sizeof(guarded.frame)) == (int32_t)sizeof(valid)); assert(guarded.before == 0xa5 && guarded.after == 0x5a); }
static void test_malformed_recovery(void) { w5x00_ethernet_driver_t driver; uint8_t out[64]; uint8_t valid[60] = {0}; prepare_driver(&driver); reset_fake(); fake_rx[0] = 0xff; fake_rx[1] = 0xff; fake_rx_length = 2; enqueue_frame(valid, sizeof(valid)); assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0); assert(fake_console_snapshot_diagnostics == 4u); assert(fake_rx_rd == 0u); assert(driver.macraw_quarantined); assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0); enqueue_frame(valid, sizeof(valid)); assert(recv_lwip(&driver.netif, out, sizeof(out)) == (int32_t)sizeof(valid)); }
static void test_rx_short_record_deferral_and_recovery(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t out[400];
    uint8_t offer[342] = {0};

    prepare_driver(&driver);
    reset_fake();
    enqueue_frame(offer, sizeof(offer));
    /* The first RSR sample exposes only the MACRAW prefix at the same RX_RD. */
    fake_rx_rsr_override = 2u;
    fake_rx_rsr_override_reads = 1u;
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    assert(fake_rx_rd == 0u);
    assert(driver.rx_short_deferrals == 1u);
    assert(driver.rx_malformed_frames == 0u);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == (int32_t)sizeof(offer));
    assert(driver.rx_malformed_frames == 0u);

    prepare_driver(&driver);
    reset_fake();
    fake_rx[0] = 0u;
    fake_rx[1] = 2u;
    fake_rx_length = 2u;
    enqueue_frame(offer, sizeof(offer));
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    assert(fake_rx_rd == 0u && driver.rx_short_deferrals == 1u);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0);
    assert(fake_rx_rd == 2u && driver.rx_malformed_frames == 1u);
    assert(fake_snapshot_raw_reads == 2u);
    assert(fake_snapshot_raw_addresses[0] == (WIZCHIP_RXBUF_BLOCK(0) << 3u));
    assert(fake_snapshot_raw_addresses[1] == fake_snapshot_raw_addresses[0]);
    assert(fake_snapshot_raw_rd_set_calls[0] == 0u && fake_snapshot_raw_rd_set_calls[1] == 0u);
    assert(fake_snapshot_raw_command_calls[0] == 0u && fake_snapshot_raw_command_calls[1] == 0u);
    assert(memcmp(fake_snapshot_raw_first, fake_snapshot_raw_second, sizeof(fake_snapshot_raw_first)) == 0);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == (int32_t)sizeof(offer));
}
static void test_rx_snapshot_observes_second_raw_read(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t out[400];
    uint8_t offer[342] = {0};

    prepare_driver(&driver);
    reset_fake();
    fake_rx[0] = 0u;
    fake_rx[1] = 2u;
    fake_rx_length = 2u;
    enqueue_frame(offer, sizeof(offer));
    fake_snapshot_raw_second_differs = 1;
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0);
    assert(fake_snapshot_raw_reads == 2u);
    assert(fake_snapshot_raw_first[0] != fake_snapshot_raw_second[0]);
}
static void test_rx_snapshot_for_persistent_prefix_only(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t out[64];

    prepare_driver(&driver);
    reset_fake();
    fake_rx[0] = 0u;
    fake_rx[1] = 2u;
    fake_rx_length = 2u;
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    assert(fake_rx_rd == 0u && driver.rx_short_deferrals == 1u);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0);
    assert(fake_snapshot_raw_reads == 2u);
    assert(fake_snapshot_raw_addresses[0] == (WIZCHIP_RXBUF_BLOCK(0) << 3u));
    assert(fake_snapshot_raw_addresses[1] == fake_snapshot_raw_addresses[0]);
    assert(fake_snapshot_raw_rd_set_calls[0] == 0u && fake_snapshot_raw_rd_set_calls[1] == 0u);
    assert(fake_snapshot_raw_command_calls[0] == 0u && fake_snapshot_raw_command_calls[1] == 0u);
    assert(fake_console_snapshot_diagnostics == 4u);
}
static void test_rx_snapshot_after_recv_command_failure(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t out[64];

    prepare_driver(&driver);
    reset_fake();
    fake_rx[0] = 0u;
    fake_rx[1] = 2u;
    fake_rx_length = 2u;
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    fake_command_stuck = 1;
    assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0);
    assert(fake_snapshot_raw_reads == 2u);
    assert(fake_snapshot_raw_rd_set_calls[0] == 0u && fake_snapshot_raw_rd_set_calls[1] == 0u);
    assert(fake_snapshot_raw_command_calls[0] == 0u && fake_snapshot_raw_command_calls[1] == 0u);
    assert(fake_console_snapshot_diagnostics == 4u);
}
static void test_rx_snapshot_is_one_shot_for_two_prefixes(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t out[64];

    prepare_driver(&driver);
    reset_fake();
    fake_rx[0] = 0u;
    fake_rx[1] = 2u;
    fake_rx[2] = 0u;
    fake_rx[3] = 2u;
    fake_rx_length = 4u;
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0);
    assert(fake_snapshot_raw_reads == 2u);
    assert(fake_console_snapshot_diagnostics == 4u);
}
static void test_rx_runt_payload_recovery(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t out[400];
    uint8_t runt[59] = {0};
    uint8_t offer[342] = {0};

    prepare_driver(&driver);
    reset_fake();
    enqueue_frame(runt, sizeof(runt));
    enqueue_frame(offer, sizeof(offer));
    /* The one-poll short-record defer precedes the first malformed drop. */
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0);
    assert(fake_rx_rd == 61u && driver.rx_malformed_frames == 1u);
    assert(fake_console_snapshot_diagnostics == 4u);
    assert(recv_lwip(&driver.netif, out, sizeof(out)) == (int32_t)sizeof(offer));
}
static void test_rx_timeout_recovery(void) { w5x00_ethernet_driver_t driver; uint8_t frame[60] = {0}; uint8_t out[64]; prepare_driver(&driver); reset_fake(); enqueue_frame(frame, sizeof(frame)); fake_command_stuck = 1; assert(recv_lwip(&driver.netif, out, sizeof(out)) < 0); assert(driver.macraw_quarantined); fake_command_stuck = 0; assert(recv_lwip(&driver.netif, out, sizeof(out)) == 0); enqueue_frame(frame, sizeof(frame)); assert(recv_lwip(&driver.netif, out, sizeof(out)) == (int32_t)sizeof(frame)); }
static void test_rx_saved_pointer_delivers_payload_and_commits_full_record(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t frame[60];

    for (uint16_t index = 0u; index < sizeof(frame); ++index)
    {
        frame[index] = (uint8_t)(0xa0u + index);
    }
    prepare_driver(&driver);
    reset_fake();
    enqueue_frame(frame, sizeof(frame));

    w5x00_ethernet_driver_poll(&driver);

    assert(input_count == 1);
    assert(last_input_length == sizeof(frame));
    assert(memcmp(last_input_payload, frame, sizeof(frame)) == 0);
    assert(fake_rx_rd == sizeof(frame) + 2u);
    assert(fake_rx_rd_set_calls == 1u);
}
static void test_bounded_poll_and_pbuf_failure(void) { w5x00_ethernet_driver_t driver; uint8_t frame[60] = {0}; prepare_driver(&driver); reset_fake(); for (unsigned i = 0; i < 10; ++i) enqueue_frame(frame, sizeof(frame)); w5x00_ethernet_driver_poll(&driver); assert(input_count == W5X00_ETHERNET_RX_POLL_BUDGET); w5x00_ethernet_driver_poll(&driver); assert(input_count == 10); enqueue_frame(frame, sizeof(frame)); fake_pbuf_fail = 1; w5x00_ethernet_driver_poll(&driver); assert(driver.pbuf_failures == 1u); }
static void test_tx_errors_and_mutex_serialization(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t data[60] = {0};
    uint8_t first[1500] = {0};
    uint8_t second[20] = {0};
    struct pbuf p = {.payload = data, .len = sizeof(data), .tot_len = sizeof(data)};
    struct pbuf chain_tail = {.payload = second, .len = sizeof(second), .tot_len = sizeof(second)};
    struct pbuf chain_head = {.next = &chain_tail, .payload = first, .len = sizeof(first), .tot_len = sizeof(first) + sizeof(second)};
    uint8_t *frame;
    uint16_t capacity;

    prepare_driver(&driver);
    reset_fake();
    fake_tx_free = 10;
    assert(netif_output(&driver.netif, &p) == ERR_BUF);
    assert(driver.tx_admission_failures == 1u);
    assert(driver.tx_interface_failures == 0u);
    assert(fake_console_diagnostics == 1u);
    fake_tx_free = sizeof(fake_tx);
    fake_command_stuck = 1;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(driver.macraw_quarantined);
    fake_command_stuck = 0;
    fake_close_delay = 3;
    fake_open_delay = 3;
    assert(netif_output(&driver.netif, &p) == ERR_OK);
    assert(fake_reconfigure_before_closed == 0);
    fake_send_timeout = 1;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(driver.macraw_quarantined);
    fake_send_timeout = 0;
    assert(netif_output(&driver.netif, &p) == ERR_OK);
    assert(netif_output(&driver.netif, &chain_head) == ERR_BUF);
    assert(w5x00_lwip_tx_begin(&driver.netif, &frame, &capacity) == ERR_OK);
    assert(capacity == W5X00_LWIP_MAX_FRAME_LEN);
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(driver.macraw_contention == 1u);
    w5x00_lwip_tx_end(&driver.netif, true);
    assert(netif_output(&driver.netif, &p) == ERR_OK);
    assert(fake_tx_length == sizeof(data));
}

static void test_tx_interface_admission_failure(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t data[60] = {0};
    struct pbuf p = {.payload = data, .len = sizeof(data), .tot_len = sizeof(data)};

    prepare_driver(&driver);
    reset_fake();
    fake_socket_status = SOCK_CLOSED;
    fake_command_stuck = 1;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(driver.tx_interface_failures == 1u);
    assert(driver.tx_admission_failures == 0u);
    assert(driver.macraw_quarantined);

    fake_command_stuck = 0;
    assert(netif_output(&driver.netif, &p) == ERR_OK);
}

static void test_dhcp_sized_tx_admission_and_offer_progress(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t discover[342];
    uint8_t offer[342];
    struct pbuf p = {.payload = discover, .len = sizeof(discover), .tot_len = sizeof(discover)};

    memset(discover, 0, sizeof(discover));
    memset(offer, 0, sizeof(offer));
    memset(discover, 0xff, 6u); /* DHCP Discover Ethernet broadcast destination. */
    prepare_driver(&driver);
    reset_fake();

    /* The first two current-FSR snapshots are transiently empty. */
    fake_tx_zero_samples = 2u;
    fake_reject_legacy_tx_fsr = 1;
    assert(netif_output(&driver.netif, &p) == ERR_OK);
    fake_reject_legacy_tx_fsr = 0;
    assert(fake_tx_length == sizeof(discover));
    assert(memcmp(fake_tx, discover, sizeof(discover)) == 0);
    assert(driver.tx_admission_failures == 0u);

    /* A DHCP Offer-sized MACRAW frame reaches the lwIP input path next poll. */
    enqueue_frame(offer, sizeof(offer));
    w5x00_ethernet_driver_poll(&driver);
    assert(input_count == 1);
    assert(last_input_length == sizeof(offer));

    /* A transient non-MACRAW socket is recovered within the same output call. */
    fake_socket_status = SOCK_CLOSED;
    assert(netif_output(&driver.netif, &p) == ERR_OK);
    assert(fake_close_commands == 1u && fake_open_commands == 1u);
    assert(driver.tx_interface_failures == 0u);
}

static void test_unstable_tx_fsr_exhaustion(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t data[60] = {0};
    struct pbuf p = {.payload = data, .len = sizeof(data), .tot_len = sizeof(data)};

    prepare_driver(&driver);
    reset_fake();
    /* 128 admission attempts x 4 stable-snapshot attempts. */
    fake_tx_unstable_samples = 512u;
    fake_reject_legacy_tx_fsr = 1;
    assert(netif_output(&driver.netif, &p) == ERR_BUF);
    fake_reject_legacy_tx_fsr = 0;
    assert(fake_tx_length == 0u);
    assert(driver.tx_admission_failures == 1u);
    assert(fake_console_diagnostics == 1u);
}

static void fake_interleaved_tx_after_unlock(void)
{
    fake_tx_free = 10u;
    fake_command_stuck = 0;
    assert(netif_output(&fake_interleaved_driver->netif, fake_interleaved_packet) == ERR_BUF);
}

static void test_diagnostic_interleaving(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t data[60] = {0};
    struct pbuf p = {.payload = data, .len = sizeof(data), .tot_len = sizeof(data)};

    prepare_driver(&driver);
    reset_fake();
    fake_command_stuck = 1;
    fake_interleaved_driver = &driver;
    fake_interleaved_packet = &p;
    fake_unlock_hook = fake_interleaved_tx_after_unlock;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    fake_unlock_hook = NULL;

    assert(driver.tx_send_failures == 1u);
    assert(driver.tx_admission_failures == 1u);
    assert(fake_console_send_diagnostics == 1u);
    assert(fake_console_admission_diagnostics == 1u);
    assert(driver.macraw_mutex->locked == 0);
}

static void test_status_timeout_recovery_phases(void)
{
    w5x00_ethernet_driver_t driver;
    uint8_t data[60] = {0};
    struct pbuf p = {.payload = data, .len = sizeof(data), .tot_len = sizeof(data)};

    prepare_driver(&driver);
    reset_fake();
    fake_command_stuck = 1;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    fake_command_stuck = 0;
    fake_close_delay = 2048u;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(driver.macraw_recovery_phase == W5X00_MACRAW_RECOVERY_WAIT_CLOSED);
    assert(fake_close_commands == 1u && fake_open_commands == 0u);
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(fake_close_commands == 1u && fake_open_commands == 0u);
    assert(netif_output(&driver.netif, &p) == ERR_OK);
    assert(fake_close_commands == 1u && fake_open_commands == 1u);
    assert(fake_reconfigure_before_closed == 0);

    prepare_driver(&driver);
    reset_fake();
    fake_command_stuck = 1;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    fake_command_stuck = 0;
    fake_open_delay = 2048u;
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(driver.macraw_recovery_phase == W5X00_MACRAW_RECOVERY_WAIT_MACRAW);
    assert(fake_close_commands == 1u && fake_open_commands == 1u);
    assert(netif_output(&driver.netif, &p) == ERR_IF);
    assert(fake_close_commands == 1u && fake_open_commands == 1u);
    assert(netif_output(&driver.netif, &p) == ERR_OK);
    assert(fake_close_commands == 1u && fake_open_commands == 1u);
}

static void reset_core_lock_fake(void) { fake_core_lock_depth = 0; fake_core_lock_takes = 0u; fake_core_locked_calls = 0u; fake_dns_server.addr = 0u; }

/*
 * Regression: configure_netif() ran netif_add()/netif_set_*() from InitTask
 * with no lwIP core lock, after lwip_freertos_init() had already started the
 * tcpip thread whose timers walk the netif list. The stubs assert the lock;
 * this drives the real w5x00_ethernet_driver_init() path through them.
 */
static void test_netif_bring_up_runs_under_the_core_lock(void)
{
    w5x00_ethernet_driver_t driver;
    const uint8_t mac[6] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};

    reset_fake();
    reset_core_lock_fake();
    fake_console_mutex = NULL; /* no earlier test's driver may be referenced */
    assert(w5x00_ethernet_driver_init(&driver, mac));
    assert(driver.initialized && driver.netif.link_up == 1);
    assert(fake_core_lock_depth == 0);
    assert(fake_core_lock_takes == 1u);   /* one hold covers the whole netif bring-up */
    assert(fake_core_locked_calls == 6u); /* add, link/status callbacks, default, link up, up */
    assert(driver.macraw_mutex->locked == 0);
}

/*
 * Regression: dhcp_start() and netif_set_addr() ran unlocked from InitTask,
 * and network_manager called dns_setserver() unlocked too. The DNS server is
 * now applied by the driver, under the same lock as the address.
 */
static void test_dhcp_and_static_configuration_run_under_the_core_lock(void)
{
    w5x00_ethernet_driver_t driver;
    const uint8_t ip[4] = {192u, 168u, 1u, 50u};
    const uint8_t netmask[4] = {255u, 255u, 255u, 0u};
    const uint8_t gateway[4] = {192u, 168u, 1u, 1u};
    const uint8_t dns[4] = {9u, 9u, 9u, 9u};
    ip4_addr_t expected;

    prepare_driver(&driver);
    reset_fake();
    reset_core_lock_fake();

    assert(w5x00_ethernet_driver_start_dhcp(&driver));
    assert(fake_core_lock_depth == 0 && fake_core_lock_takes == 1u && fake_core_locked_calls == 1u);

    w5x00_ethernet_driver_set_static_ipv4(&driver, ip, netmask, gateway, dns);
    assert(fake_core_lock_depth == 0 && fake_core_lock_takes == 2u && fake_core_locked_calls == 3u);
    IP4_ADDR(&expected, ip[0], ip[1], ip[2], ip[3]);
    assert(driver.netif.ip_addr.addr == expected.addr);
    IP4_ADDR(&expected, dns[0], dns[1], dns[2], dns[3]);
    assert(fake_dns_server.addr == expected.addr);

    /* Not initialized: no lwIP call and no lock taken. */
    driver.initialized = false;
    assert(!w5x00_ethernet_driver_start_dhcp(&driver));
    w5x00_ethernet_driver_set_static_ipv4(&driver, ip, netmask, gateway, dns);
    assert(fake_core_lock_takes == 2u && fake_core_locked_calls == 3u);
}

int main(void) { test_rx_canary_and_oversize_recovery(); test_malformed_recovery(); test_rx_short_record_deferral_and_recovery(); test_rx_snapshot_observes_second_raw_read(); test_rx_snapshot_for_persistent_prefix_only(); test_rx_snapshot_after_recv_command_failure(); test_rx_snapshot_is_one_shot_for_two_prefixes(); test_rx_runt_payload_recovery(); test_rx_timeout_recovery(); test_rx_saved_pointer_delivers_payload_and_commits_full_record(); test_bounded_poll_and_pbuf_failure(); test_tx_errors_and_mutex_serialization(); test_tx_interface_admission_failure(); test_dhcp_sized_tx_admission_and_offer_progress(); test_unstable_tx_fsr_exhaustion(); test_diagnostic_interleaving(); test_status_timeout_recovery_phases(); test_netif_bring_up_runs_under_the_core_lock(); test_dhcp_and_static_configuration_run_under_the_core_lock(); return 0; }

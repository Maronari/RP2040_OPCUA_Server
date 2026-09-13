#include "drivers/rp2350_spi_bus.h"
#include "hardware/spi.h"
#include "semphr.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>

static spi_inst_t instances[2] = {{0}, {1}};
spi_inst_t *spi0 = &instances[0];
spi_inst_t *spi1 = &instances[1];
static bool held[2];
static unsigned allocations, gpio_edges, hw_inits, format_calls;
static bool fail_allocation, fail_transfer;
/* Clock mode currently programmed into each peripheral (-1 = never set),
 * and the mode that was live when a select line last went low. */
static int format_mode[2] = {-1, -1};
static int mode_at_select = -1;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (fail_allocation) return NULL;
    assert(allocations < 2);
    return &held[allocations++];
}
int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    assert(ticks == 20);
    bool *lock = handle;
    if (*lock) return 0;
    *lock = true;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t handle)
{
    bool *lock = handle;
    assert(*lock);
    *lock = false;
    return pdTRUE;
}
void gpio_init(unsigned pin) { (void)pin; }
void gpio_set_dir(unsigned pin, int dir) { (void)pin; (void)dir; }
void gpio_set_function(unsigned pin, int function) { (void)pin; (void)function; }
void gpio_put(unsigned pin, int value)
{
    (void)pin;
    ++gpio_edges;
    /* Select lines are on instance 0 in every transfer below. */
    if (value == 0) mode_at_select = format_mode[0];
}
unsigned spi_init(spi_inst_t *spi, unsigned baud) { (void)spi; ++hw_inits; return baud; }
void spi_set_format(spi_inst_t *spi, unsigned bits, spi_cpol_t cpol, spi_cpha_t cpha, spi_order_t order)
{
    /* Reprogramming a shared peripheral is only safe under its lock. */
    assert(held[spi->id]);
    assert(bits == 8 && order == SPI_MSB_FIRST);
    format_mode[spi->id] = (int)cpol * 2 + (int)cpha;
    ++format_calls;
}
int spi_write_read_blocking(spi_inst_t *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
    assert(held[spi->id]);
    if (fail_transfer) return -1;
    memcpy(rx, tx, len);
    return (int)len;
}
int spi_write_blocking(spi_inst_t *spi, const uint8_t *tx, size_t len)
{ (void)tx; assert(held[spi->id]); return fail_transfer ? -1 : (int)len; }
int spi_read_blocking(spi_inst_t *spi, uint8_t fill, uint8_t *rx, size_t len)
{ assert(held[spi->id]); memset(rx, fill, len); return fail_transfer ? -1 : (int)len; }

int main(void)
{
    rp2350_spi_bus_config_t config = {.spi_instance = 0, .baudrate_hz = 1000000,
        .sck_gpio = 14, .mosi_gpio = 15, .miso_gpio = 12, .cs_gpio = 13, .cpol = 0, .cpha = 1};
    rp2350_spi_bus_state_t first, second, other;
    rp2350_spi_bus_config_t bad = config;
    bad.cpol = 2;
    assert(!rp2350_spi_bus_init(&first, &bad));
    bad = config;
    bad.cpha = 2;
    assert(!rp2350_spi_bus_init(&first, &bad));
    assert(hw_inits == 0 && gpio_edges == 0 && allocations == 0);
    fail_allocation = true;
    assert(!rp2350_spi_bus_init(&first, &config));
    assert(hw_inits == 0 && gpio_edges == 0);
    fail_allocation = false;
    assert(rp2350_spi_bus_init(&first, &config));
    /* Initialization programs no frame format: it belongs to each device. */
    assert(format_calls == 0);
    config.cs_gpio = 16;
    config.cpha = 0; /* mode 0 device sharing the mode 1 device's peripheral */
    assert(rp2350_spi_bus_init(&second, &config));
    assert(allocations == 1 && hw_inits == 1);
    config.spi_instance = 1;
    assert(rp2350_spi_bus_init(&other, &config));
    assert(allocations == 2);
    spi_bus_t b = {.vtbl = &rp2350_spi_bus_vtbl, .context = &second};
    spi_bus_t independent = {.vtbl = &rp2350_spi_bus_vtbl, .context = &other};
    uint8_t tx = 0x5A, rx = 0;
    assert(rp2350_spi_bus_vtbl.acquire(&first));
    unsigned edges = gpio_edges;
    assert(!spi_bus_transfer_framed(&b, &tx, &rx, 1));
    assert(gpio_edges == edges);
    assert(spi_bus_transfer_framed(&independent, &tx, &rx, 1));
    assert(rx == tx);
    rp2350_spi_bus_vtbl.release(&first);
    fail_transfer = true;
    assert(!spi_bus_transfer_framed(&b, &tx, &rx, 1));
    assert(!held[0]);
    fail_transfer = false;
    assert(spi_bus_transfer_framed(&b, &tx, &rx, 1));
    assert(!held[0] && rx == tx);

    /* Each device's own mode is live when its select line goes low, in
     * either order and repeatedly -- not whichever device initialized the
     * peripheral first. */
    spi_bus_t a = {.vtbl = &rp2350_spi_bus_vtbl, .context = &first};
    for (unsigned round = 0; round < 2; ++round)
    {
        assert(spi_bus_transfer_framed(&a, &tx, &rx, 1));
        assert(mode_at_select == 1);
        assert(spi_bus_transfer_framed(&b, &tx, &rx, 1));
        assert(mode_at_select == 0);
    }
    /* A lock that cannot be taken touches neither the format nor the pins. */
    assert(rp2350_spi_bus_vtbl.acquire(&first));
    unsigned calls = format_calls;
    edges = gpio_edges;
    assert(!spi_bus_transfer_framed(&b, &tx, &rx, 1));
    assert(format_calls == calls && gpio_edges == edges && format_mode[0] == 1);
    rp2350_spi_bus_vtbl.release(&first);
    return 0;
}

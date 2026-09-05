/**
 * @file
 * @brief Host tests for drivers/src/uart_transport.c's NULL-safe wrappers.
 */
#include "drivers/uart_transport.h"

#include <assert.h>
#include <string.h>

static uint8_t g_last_write[8];
static size_t g_last_write_length;
static int g_write_calls;
static bool g_write_result;

static uint8_t g_read_source[8];
static size_t g_read_available;
static int g_read_calls;

static bool fake_write(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    ++g_write_calls;
    g_last_write_length = length;
    memcpy(g_last_write, data, length);
    return g_write_result;
}

static size_t fake_read(void *context, uint8_t *data, size_t length)
{
    size_t copied = g_read_available < length ? g_read_available : length;

    (void)context;
    ++g_read_calls;
    memcpy(data, g_read_source, copied);
    return copied;
}

static const uart_bus_vtbl_t g_fake_vtbl = {
    .write = fake_write,
    .read = fake_read,
};

static void reset_counters(void)
{
    g_write_calls = 0;
    g_read_calls = 0;
    g_write_result = true;
    g_read_available = 0u;
}

static void test_null_and_incomplete_vtbl_rejected(void)
{
    uart_bus_t incomplete_bus;
    uart_bus_vtbl_t incomplete_vtbl = {.write = fake_write};
    uint8_t data[2] = {0};

    reset_counters();
    assert(!uart_bus_write(NULL, data, 1u));
    assert(uart_bus_read(NULL, data, 1u) == 0u);

    incomplete_bus.vtbl = NULL;
    incomplete_bus.context = NULL;
    assert(!uart_bus_write(&incomplete_bus, data, 1u));

    incomplete_bus.vtbl = &incomplete_vtbl; /* .read missing */
    assert(uart_bus_read(&incomplete_bus, data, 1u) == 0u);
    assert(g_write_calls == 0);
    assert(g_read_calls == 0);
}

static void test_write_dispatches_and_propagates_result(void)
{
    uart_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t data[3] = {0x01u, 0x02u, 0x03u};

    reset_counters();
    g_write_result = true;
    assert(uart_bus_write(&bus, data, sizeof(data)));
    assert(g_write_calls == 1 && g_last_write_length == sizeof(data));
    assert(memcmp(g_last_write, data, sizeof(data)) == 0);

    g_write_result = false;
    assert(!uart_bus_write(&bus, data, sizeof(data)));
}

static void test_read_returns_only_available_bytes(void)
{
    uart_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t data[8] = {0};

    reset_counters();
    g_read_source[0] = 0xAAu;
    g_read_source[1] = 0xBBu;
    g_read_available = 2u;
    assert(uart_bus_read(&bus, data, sizeof(data)) == 2u);
    assert(data[0] == 0xAAu && data[1] == 0xBBu);

    g_read_available = 0u;
    assert(uart_bus_read(&bus, data, sizeof(data)) == 0u);
}

int main(void)
{
    test_null_and_incomplete_vtbl_rejected();
    test_write_dispatches_and_propagates_result();
    test_read_returns_only_available_bytes();
    return 0;
}

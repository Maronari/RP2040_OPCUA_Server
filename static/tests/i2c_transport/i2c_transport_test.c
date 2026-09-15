/**
 * @file
 * @brief Host tests for drivers/src/i2c_transport.c's register read/write
 *        convenience wrappers.
 */
#include "drivers/i2c_transport.h"

#include <assert.h>
#include <string.h>

#define FAKE_ADDR 0x74u

static bool g_write_result;
static bool g_read_result;
static uint8_t g_last_write[16];
static size_t g_last_write_length;
static int g_write_calls;
static int g_read_calls;
static uint8_t g_read_echo[8];

static bool fake_write(void *context, uint8_t addr, const uint8_t *data, size_t length)
{
    (void)context;
    assert(addr == FAKE_ADDR);
    ++g_write_calls;
    g_last_write_length = length;
    memcpy(g_last_write, data, length);
    return g_write_result;
}

static bool fake_read(void *context, uint8_t addr, uint8_t *data, size_t length)
{
    (void)context;
    assert(addr == FAKE_ADDR);
    ++g_read_calls;
    if (g_read_result)
    {
        memcpy(data, g_read_echo, length);
    }
    return g_read_result;
}

static const i2c_bus_vtbl_t g_fake_vtbl = {
    .write = fake_write,
    .read = fake_read,
};

static void reset_counters(void)
{
    g_write_calls = 0;
    g_read_calls = 0;
    g_write_result = true;
    g_read_result = true;
}

static void test_null_and_incomplete_vtbl_rejected(void)
{
    i2c_bus_t incomplete_bus;
    i2c_bus_vtbl_t incomplete_vtbl = {.write = fake_write};
    uint8_t data[1] = {0};

    reset_counters();
    assert(!i2c_bus_read_register(NULL, FAKE_ADDR, 0x00u, data, 1u));
    assert(!i2c_bus_write_register(NULL, FAKE_ADDR, 0x00u, data, 1u));

    incomplete_bus.vtbl = NULL;
    incomplete_bus.context = NULL;
    assert(!i2c_bus_read_register(&incomplete_bus, FAKE_ADDR, 0x00u, data, 1u));

    incomplete_bus.vtbl = &incomplete_vtbl; /* .read missing */
    assert(!i2c_bus_read_register(&incomplete_bus, FAKE_ADDR, 0x00u, data, 1u));
    assert(g_write_calls == 0);
    assert(g_read_calls == 0);
}

static void test_read_register_writes_pointer_then_reads(void)
{
    i2c_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t data[2] = {0};

    reset_counters();
    g_read_echo[0] = 0xAAu;
    g_read_echo[1] = 0xBBu;
    assert(i2c_bus_read_register(&bus, FAKE_ADDR, 0x02u, data, 2u));
    assert(g_write_calls == 1 && g_last_write_length == 1u && g_last_write[0] == 0x02u);
    assert(g_read_calls == 1);
    assert(data[0] == 0xAAu && data[1] == 0xBBu);
}

static void test_read_register_fails_if_pointer_write_fails(void)
{
    i2c_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t data[1] = {0};

    reset_counters();
    g_write_result = false;
    assert(!i2c_bus_read_register(&bus, FAKE_ADDR, 0x00u, data, 1u));
    assert(g_write_calls == 1);
    assert(g_read_calls == 0); /* never attempts the read half after a failed pointer write */
}

static void test_write_register_concatenates_pointer_and_data(void)
{
    i2c_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t data[2] = {0x11u, 0x22u};

    reset_counters();
    assert(i2c_bus_write_register(&bus, FAKE_ADDR, 0x06u, data, 2u));
    assert(g_write_calls == 1 && g_last_write_length == 3u);
    assert(g_last_write[0] == 0x06u && g_last_write[1] == 0x11u && g_last_write[2] == 0x22u);
}

static void test_write_register_rejects_oversized_payload(void)
{
    i2c_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t data[16] = {0};

    reset_counters();
    assert(!i2c_bus_write_register(&bus, FAKE_ADDR, 0x00u, data, sizeof(data)));
    assert(g_write_calls == 0);
}

int main(void)
{
    test_null_and_incomplete_vtbl_rejected();
    test_read_register_writes_pointer_then_reads();
    test_read_register_fails_if_pointer_write_fails();
    test_write_register_concatenates_pointer_and_data();
    test_write_register_rejects_oversized_payload();
    return 0;
}

/**
 * @file
 * @brief Host tests for the real drivers/src/tcal9539_driver.c against a
 *        fake I2C device model (a small register file honoring the
 *        datasheet's "write sets pointer (+ optional data), read
 *        continues from the pointer" framing) -- proves the actual
 *        register-pointer write/read round trip, not just that calls
 *        dispatch.
 */
#include "drivers/tcal9539_driver.h"

#include <assert.h>
#include <string.h>

#define TEST_ADDR 0x74u
#define REGISTER_COUNT 8u

typedef struct
{
    uint8_t registers[REGISTER_COUNT];
    uint8_t pending_pointer;
    bool force_write_failure;
    bool force_read_failure;
} fake_device_t;

static fake_device_t g_device;

static void fake_device_reset(void)
{
    memset(&g_device, 0, sizeof(g_device));
}

static bool fake_write(void *context, uint8_t addr, const uint8_t *data, size_t length)
{
    size_t index;

    (void)context;
    assert(addr == TEST_ADDR);
    if (g_device.force_write_failure || length == 0u)
    {
        return false;
    }
    g_device.pending_pointer = data[0];
    for (index = 1u; index < length; ++index)
    {
        uint8_t reg = (uint8_t)(g_device.pending_pointer + (index - 1u));

        if (reg < REGISTER_COUNT)
        {
            g_device.registers[reg] = data[index];
        }
    }
    return true;
}

static bool fake_read(void *context, uint8_t addr, uint8_t *data, size_t length)
{
    size_t index;

    (void)context;
    assert(addr == TEST_ADDR);
    if (g_device.force_read_failure)
    {
        return false;
    }
    for (index = 0u; index < length; ++index)
    {
        uint8_t reg = (uint8_t)(g_device.pending_pointer + index);

        data[index] = reg < REGISTER_COUNT ? g_device.registers[reg] : 0u;
    }
    return true;
}

static const i2c_bus_vtbl_t g_fake_vtbl = {
    .write = fake_write,
    .read = fake_read,
};

static void test_unbound_fails(void)
{
    uint8_t value = 0u;

    tcal9539_bind_bus(NULL, TEST_ADDR);
    assert(!tcal9539_read_register(TCAL9539_REG_INPUT_PORT0, &value));
    assert(!tcal9539_write_register(TCAL9539_REG_OUTPUT_PORT0, 0x00u));
}

static void test_write_then_read_round_trip(void)
{
    i2c_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t value = 0u;

    fake_device_reset();
    tcal9539_bind_bus(&bus, TEST_ADDR);
    assert(tcal9539_write_register(TCAL9539_REG_OUTPUT_PORT0, 0xAAu));
    assert(g_device.registers[TCAL9539_REG_OUTPUT_PORT0] == 0xAAu);
    assert(tcal9539_read_register(TCAL9539_REG_OUTPUT_PORT0, &value));
    assert(value == 0xAAu);

    /* A different register is unaffected. */
    assert(tcal9539_read_register(TCAL9539_REG_CONFIG_PORT0, &value));
    assert(value == 0x00u);
}

static void test_transport_failure_propagates(void)
{
    i2c_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};
    uint8_t value = 0u;

    fake_device_reset();
    tcal9539_bind_bus(&bus, TEST_ADDR);
    g_device.force_write_failure = true;
    assert(!tcal9539_write_register(TCAL9539_REG_OUTPUT_PORT0, 0x01u));

    g_device.force_write_failure = false;
    g_device.force_read_failure = true;
    assert(!tcal9539_read_register(TCAL9539_REG_INPUT_PORT0, &value));
}

int main(void)
{
    test_unbound_fails();
    test_write_then_read_round_trip();
    test_transport_failure_propagates();
    return 0;
}

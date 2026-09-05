/**
 * @file
 * @brief Board-power-service implementation. See
 *        services/include/services/board_power_service.h for the full
 *        contract and docs/datasheets/TCAL9539-register-notes.md for the
 *        register/pin reference this file follows.
 *
 * Ownership: g_output_port0/g_output_port1 shadow this board's TCAL9539
 * Output Port registers so a single-bit change (one sensor-power port, one
 * isolation-enable line) can be applied with a read-modify-write against
 * the shadow and a single register write, without an extra I2C read
 * round-trip per call.
 */
#include "services/board_power_service.h"

#include "drivers/tcal9539_driver.h"

#include <stddef.h>

#define BOARD_POWER_SENSOR_PORT_COUNT 4u
#define BOARD_POWER_I2C_ISOLATION_BUS_COUNT 2u

/** Port0 config: bits 0-3 (SENSOR_PWR_EN0..3) output, bits 4-7 (FAULT0..3_N) input. */
#define BOARD_POWER_CONFIG_PORT0 0xF0u
/** Port1 config: bit0 (I2C0_ISOL_EN) output, bit1 (I2C0_READY) input, bit2 (I2C1_ISOL_EN) output, bit3 (I2C1_READY) input, bit4 (DI_READY_N) input, bits5-7 (reserved) input. */
#define BOARD_POWER_CONFIG_PORT1 0xFAu

static uint8_t g_output_port0;
static uint8_t g_output_port1;

typedef struct
{
    bool in_progress;
    uint32_t re_enable_at_ms;
} board_power_cycle_slot_t;

static board_power_cycle_slot_t g_cycle_slots[BOARD_POWER_SENSOR_PORT_COUNT];

void board_power_service_bind_bus(const i2c_bus_t *bus, uint8_t address)
{
    tcal9539_bind_bus(bus, address);
}

bool board_power_service_init(void)
{
    g_output_port0 = 0x00u;
    g_output_port1 = 0x00u;
    if (!tcal9539_write_register(TCAL9539_REG_OUTPUT_PORT0, g_output_port0))
    {
        return false;
    }
    if (!tcal9539_write_register(TCAL9539_REG_OUTPUT_PORT1, g_output_port1))
    {
        return false;
    }
    if (!tcal9539_write_register(TCAL9539_REG_CONFIG_PORT0, BOARD_POWER_CONFIG_PORT0))
    {
        return false;
    }
    return tcal9539_write_register(TCAL9539_REG_CONFIG_PORT1, BOARD_POWER_CONFIG_PORT1);
}

bool board_power_set_sensor_enable(board_power_sensor_port_t port, bool enable)
{
    uint8_t mask;

    if ((uint8_t)port >= BOARD_POWER_SENSOR_PORT_COUNT)
    {
        return false;
    }
    mask = (uint8_t)(1u << (uint8_t)port);
    g_output_port0 = enable ? (uint8_t)(g_output_port0 | mask) : (uint8_t)(g_output_port0 & (uint8_t)~mask);
    return tcal9539_write_register(TCAL9539_REG_OUTPUT_PORT0, g_output_port0);
}

bool board_power_get_sensor_fault(board_power_sensor_port_t port, bool *out_fault_active)
{
    uint8_t value;
    uint8_t bit_index;

    if ((uint8_t)port >= BOARD_POWER_SENSOR_PORT_COUNT || out_fault_active == NULL)
    {
        return false;
    }
    if (!tcal9539_read_register(TCAL9539_REG_INPUT_PORT0, &value))
    {
        return false;
    }
    bit_index = (uint8_t)(4u + (uint8_t)port);
    *out_fault_active = (value & (uint8_t)(1u << bit_index)) == 0u;
    return true;
}

bool board_power_start_sensor_power_cycle(board_power_sensor_port_t port, uint32_t now_ms)
{
    if ((uint8_t)port >= BOARD_POWER_SENSOR_PORT_COUNT)
    {
        return false;
    }
    if (!board_power_set_sensor_enable(port, false))
    {
        return false;
    }
    g_cycle_slots[port].in_progress = true;
    g_cycle_slots[port].re_enable_at_ms = now_ms + BOARD_POWER_CYCLE_OFF_MS;
    return true;
}

void board_power_poll(uint32_t now_ms)
{
    uint8_t port;

    for (port = 0u; port < BOARD_POWER_SENSOR_PORT_COUNT; ++port)
    {
        if (g_cycle_slots[port].in_progress &&
            (int32_t)(now_ms - g_cycle_slots[port].re_enable_at_ms) >= 0)
        {
            g_cycle_slots[port].in_progress = false;
            (void)board_power_set_sensor_enable((board_power_sensor_port_t)port, true);
        }
    }
}

bool board_power_set_i2c_isolation_enable(board_power_i2c_isolation_bus_t bus, bool enable)
{
    uint8_t bit_index;
    uint8_t mask;

    if ((uint8_t)bus >= BOARD_POWER_I2C_ISOLATION_BUS_COUNT)
    {
        return false;
    }
    bit_index = (uint8_t)((uint8_t)bus * 2u);
    mask = (uint8_t)(1u << bit_index);
    g_output_port1 = enable ? (uint8_t)(g_output_port1 | mask) : (uint8_t)(g_output_port1 & (uint8_t)~mask);
    return tcal9539_write_register(TCAL9539_REG_OUTPUT_PORT1, g_output_port1);
}

bool board_power_get_i2c_ready(board_power_i2c_isolation_bus_t bus, bool *out_ready)
{
    uint8_t value;
    uint8_t bit_index;

    if ((uint8_t)bus >= BOARD_POWER_I2C_ISOLATION_BUS_COUNT || out_ready == NULL)
    {
        return false;
    }
    if (!tcal9539_read_register(TCAL9539_REG_INPUT_PORT1, &value))
    {
        return false;
    }
    bit_index = (uint8_t)((uint8_t)bus * 2u + 1u);
    *out_ready = (value & (uint8_t)(1u << bit_index)) != 0u;
    return true;
}

bool board_power_get_di_ready(bool *out_ready)
{
    uint8_t value;

    if (out_ready == NULL)
    {
        return false;
    }
    if (!tcal9539_read_register(TCAL9539_REG_INPUT_PORT1, &value))
    {
        return false;
    }
    *out_ready = (value & (1u << 4)) == 0u;
    return true;
}

/**
 * @file
 * @brief Host tests for the real services/src/board_power_service.c against
 *        a fake TCAL9539 I2C register-file model -- proves the exact pin
 *        mapping (which Output Port bit each sensor-power/isolation-enable
 *        control touches, which Input Port bit each fault/ready read
 *        checks), the hardware-safe init ordering, and the bounded
 *        power-cycle sequencing's timing, not just that calls dispatch.
 */
#include "services/board_power_service.h"

#include <assert.h>
#include <string.h>

#define TEST_ADDR 0x74u
#define REGISTER_COUNT 8u
#define MAX_LOGGED_WRITES 16u

typedef struct
{
    uint8_t registers[REGISTER_COUNT];
    uint8_t pending_pointer;
    uint8_t write_log_reg[MAX_LOGGED_WRITES];
    uint8_t write_log_value[MAX_LOGGED_WRITES];
    size_t write_log_count;
} fake_device_t;

static fake_device_t g_device;

static void fake_device_reset(void)
{
    memset(&g_device, 0, sizeof(g_device));
}

static bool fake_write(void *context, uint8_t addr, const uint8_t *data, size_t length)
{
    (void)context;
    assert(addr == TEST_ADDR);
    g_device.pending_pointer = data[0];
    if (length > 1u)
    {
        uint8_t reg = data[0];
        uint8_t value = data[1];

        if (reg < REGISTER_COUNT)
        {
            g_device.registers[reg] = value;
        }
        if (g_device.write_log_count < MAX_LOGGED_WRITES)
        {
            g_device.write_log_reg[g_device.write_log_count] = reg;
            g_device.write_log_value[g_device.write_log_count] = value;
            g_device.write_log_count++;
        }
    }
    return true;
}

static bool fake_read(void *context, uint8_t addr, uint8_t *data, size_t length)
{
    size_t index;

    (void)context;
    assert(addr == TEST_ADDR);
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

static void bind_and_reset(void)
{
    static const i2c_bus_t bus = {.vtbl = &g_fake_vtbl, .context = NULL};

    fake_device_reset();
    board_power_service_bind_bus(&bus, TEST_ADDR);
}

static void test_init_writes_safe_state_before_direction(void)
{
    bind_and_reset();
    assert(board_power_service_init());
    assert(g_device.registers[0x02u] == 0x00u); /* Output Port 0: every sensor-power/enable pin off */
    assert(g_device.registers[0x03u] == 0x00u); /* Output Port 1 */
    assert(g_device.registers[0x06u] == 0xF0u); /* Configuration Port 0 */
    assert(g_device.registers[0x07u] == 0xFAu); /* Configuration Port 1 */

    /* Output ports must be written before Configuration ports, so no pin
     * is ever briefly driven high once it becomes an output. */
    assert(g_device.write_log_count == 4u);
    assert(g_device.write_log_reg[0] == 0x02u);
    assert(g_device.write_log_reg[1] == 0x03u);
    assert(g_device.write_log_reg[2] == 0x06u);
    assert(g_device.write_log_reg[3] == 0x07u);
}

static void test_sensor_enable_touches_only_its_own_bit(void)
{
    bind_and_reset();
    assert(board_power_service_init());
    assert(board_power_set_sensor_enable(BOARD_POWER_SENSOR_PORT_I2C1, true));
    assert(g_device.registers[0x02u] == 0x02u); /* bit 1 only */
    assert(board_power_set_sensor_enable(BOARD_POWER_SENSOR_PORT_SPI, true));
    assert(g_device.registers[0x02u] == 0x06u); /* bits 1 and 2 */
    assert(board_power_set_sensor_enable(BOARD_POWER_SENSOR_PORT_I2C1, false));
    assert(g_device.registers[0x02u] == 0x04u); /* only bit 2 remains */
}

static void test_sensor_fault_reads_correct_bit_and_polarity(void)
{
    bind_and_reset();
    board_power_service_init();
    g_device.registers[0x00u] = 0xF0u; /* all four fault bits idle (active-low -> no fault) */

    bool fault_active = true;
    assert(board_power_get_sensor_fault(BOARD_POWER_SENSOR_PORT_UART, &fault_active));
    assert(fault_active == false);

    g_device.registers[0x00u] = (uint8_t)(0xF0u & ~(1u << 6)); /* SPI port (bit 2 -> input bit 6) fault */
    assert(board_power_get_sensor_fault(BOARD_POWER_SENSOR_PORT_SPI, &fault_active));
    assert(fault_active == true);
    /* An unrelated port is unaffected. */
    assert(board_power_get_sensor_fault(BOARD_POWER_SENSOR_PORT_I2C0, &fault_active));
    assert(fault_active == false);
}

static void test_power_cycle_sequencing(void)
{
    bind_and_reset();
    board_power_service_init();
    assert(board_power_set_sensor_enable(BOARD_POWER_SENSOR_PORT_SPI, true));
    assert(g_device.registers[0x02u] == 0x04u);

    assert(board_power_start_sensor_power_cycle(BOARD_POWER_SENSOR_PORT_SPI, 1000u));
    assert(g_device.registers[0x02u] == 0x00u); /* disabled immediately */

    board_power_poll(1000u + BOARD_POWER_CYCLE_OFF_MS - 1u);
    assert(g_device.registers[0x02u] == 0x00u); /* not yet due */

    board_power_poll(1000u + BOARD_POWER_CYCLE_OFF_MS);
    assert(g_device.registers[0x02u] == 0x04u); /* re-enabled */

    /* A second poll after completion must not re-toggle anything. */
    size_t writes_before = g_device.write_log_count;
    board_power_poll(1000u + BOARD_POWER_CYCLE_OFF_MS + 100u);
    assert(g_device.write_log_count == writes_before);
}

static void test_i2c_isolation_enable_and_ready(void)
{
    bind_and_reset();
    board_power_service_init();
    assert(board_power_set_i2c_isolation_enable(BOARD_POWER_I2C_ISOLATION_BUS1, true));
    assert(g_device.registers[0x03u] == 0x04u); /* bit 2 only */

    g_device.registers[0x01u] = (uint8_t)(1u << 1); /* I2C0_READY (bit 1) asserted */
    bool ready = false;
    assert(board_power_get_i2c_ready(BOARD_POWER_I2C_ISOLATION_BUS0, &ready));
    assert(ready == true);
    assert(board_power_get_i2c_ready(BOARD_POWER_I2C_ISOLATION_BUS1, &ready));
    assert(ready == false);
}

static void test_di_ready(void)
{
    bind_and_reset();
    board_power_service_init();
    bool ready = false;

    g_device.registers[0x01u] = 0x00u; /* DI_READY_N (bit 4) asserted low -> ready */
    assert(board_power_get_di_ready(&ready));
    assert(ready == true);

    g_device.registers[0x01u] = (uint8_t)(1u << 4);
    assert(board_power_get_di_ready(&ready));
    assert(ready == false);
}

static void test_out_of_range_and_null_rejected(void)
{
    bool flag = false;

    bind_and_reset();
    board_power_service_init();
    assert(!board_power_set_sensor_enable((board_power_sensor_port_t)4, true));
    assert(!board_power_get_sensor_fault((board_power_sensor_port_t)4, &flag));
    assert(!board_power_get_sensor_fault(BOARD_POWER_SENSOR_PORT_I2C0, NULL));
    assert(!board_power_set_i2c_isolation_enable((board_power_i2c_isolation_bus_t)2, true));
    assert(!board_power_get_i2c_ready((board_power_i2c_isolation_bus_t)2, &flag));
    assert(!board_power_get_di_ready(NULL));
}

int main(void)
{
    test_init_writes_safe_state_before_direction();
    test_sensor_enable_touches_only_its_own_bit();
    test_sensor_fault_reads_correct_bit_and_polarity();
    test_power_cycle_sequencing();
    test_i2c_isolation_enable_and_ready();
    test_di_ready();
    test_out_of_range_and_null_rejected();
    return 0;
}

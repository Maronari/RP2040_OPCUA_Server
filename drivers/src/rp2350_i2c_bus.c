/**
 * @file
 * @brief Real RP2350 implementation of i2c_bus_vtbl_t
 *        (drivers/include/drivers/rp2350_i2c_bus.h). Not compiled into any
 *        host test target -- only the real firmware needs the actual
 *        hardware_i2c/hardware_gpio APIs; drivers depending only on
 *        rp2350_i2c_bus.h stay host testable without them.
 */
#include "drivers/rp2350_i2c_bus.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"

static bool g_i2c_peripheral_initialized[2];

static i2c_inst_t *i2c_instance_handle(uint8_t i2c_instance)
{
    return i2c_instance == 0u ? i2c0 : i2c1;
}

bool rp2350_i2c_bus_init(rp2350_i2c_bus_state_t *state, const rp2350_i2c_bus_config_t *config)
{
    if (state == NULL || config == NULL || config->i2c_instance > 1u)
    {
        return false;
    }
    state->config = *config;
    if (!g_i2c_peripheral_initialized[config->i2c_instance])
    {
        i2c_inst_t *instance = i2c_instance_handle(config->i2c_instance);

        (void)i2c_init(instance, config->baudrate_hz);
        gpio_set_function(config->sda_gpio, GPIO_FUNC_I2C);
        gpio_set_function(config->scl_gpio, GPIO_FUNC_I2C);
        /* Weak internal pull-ups as a defensive default alongside whatever
         * external bus pull-ups this board provides -- harmless if
         * external ones already exist, required if they don't. */
        gpio_pull_up(config->sda_gpio);
        gpio_pull_up(config->scl_gpio);
        g_i2c_peripheral_initialized[config->i2c_instance] = true;
    }
    return true;
}

static bool rp2350_i2c_bus_write(void *context, uint8_t addr, const uint8_t *data, size_t length)
{
    const rp2350_i2c_bus_state_t *state = (const rp2350_i2c_bus_state_t *)context;
    i2c_inst_t *instance = i2c_instance_handle(state->config.i2c_instance);
    int written = i2c_write_blocking(instance, addr, data, length, false);

    return written >= 0 && (size_t)written == length;
}

static bool rp2350_i2c_bus_read(void *context, uint8_t addr, uint8_t *data, size_t length)
{
    const rp2350_i2c_bus_state_t *state = (const rp2350_i2c_bus_state_t *)context;
    i2c_inst_t *instance = i2c_instance_handle(state->config.i2c_instance);
    int received = i2c_read_blocking(instance, addr, data, length, false);

    return received >= 0 && (size_t)received == length;
}

const i2c_bus_vtbl_t rp2350_i2c_bus_vtbl = {
    .write = rp2350_i2c_bus_write,
    .read = rp2350_i2c_bus_read,
};

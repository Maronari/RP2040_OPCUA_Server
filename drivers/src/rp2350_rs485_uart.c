/**
 * @file
 * @brief Real RP2350 implementation of uart_bus_vtbl_t
 *        (drivers/include/drivers/rp2350_rs485_uart.h). Not compiled into
 *        any host test target -- only the real firmware needs the actual
 *        hardware_uart/hardware_gpio APIs; drivers depending only on
 *        rp2350_rs485_uart.h stay host testable without them.
 */
#include "drivers/rp2350_rs485_uart.h"

#include "hardware/gpio.h"
#include "hardware/uart.h"

static uart_inst_t *uart_instance_handle(uint8_t uart_instance)
{
    return uart_instance == 0u ? uart0 : uart1;
}

bool rp2350_rs485_uart_init(rp2350_rs485_uart_state_t *state, const rp2350_rs485_uart_config_t *config)
{
    uart_inst_t *instance;

    if (state == NULL || config == NULL || config->uart_instance > 1u)
    {
        return false;
    }
    state->config = *config;
    instance = uart_instance_handle(config->uart_instance);
    (void)uart_init(instance, config->baudrate_hz);
    uart_set_format(instance, 8u, 1u, UART_PARITY_NONE);
    gpio_set_function(config->tx_gpio, GPIO_FUNC_UART);
    gpio_set_function(config->rx_gpio, GPIO_FUNC_UART);
    gpio_init(config->de_re_gpio);
    gpio_put(config->de_re_gpio, 0u); /* safe receive level, set before enabling the output driver */
    gpio_set_dir(config->de_re_gpio, GPIO_OUT);
    return true;
}

static bool rp2350_rs485_uart_write(void *context, const uint8_t *data, size_t length)
{
    const rp2350_rs485_uart_state_t *state = (const rp2350_rs485_uart_state_t *)context;
    uart_inst_t *instance = uart_instance_handle(state->config.uart_instance);

    gpio_put(state->config.de_re_gpio, 1u); /* assert DE/RE: transmit */
    uart_write_blocking(instance, data, length);
    /* Wait for the FIFO and shift register to fully drain -- releasing
     * DE/RE as soon as uart_write_blocking() returns would still leave
     * queued/in-flight bits truncated on the wire. */
    uart_tx_wait_blocking(instance);
    gpio_put(state->config.de_re_gpio, 0u); /* release DE/RE: back to receive */
    return true;
}

static size_t rp2350_rs485_uart_read(void *context, uint8_t *data, size_t length)
{
    const rp2350_rs485_uart_state_t *state = (const rp2350_rs485_uart_state_t *)context;
    uart_inst_t *instance = uart_instance_handle(state->config.uart_instance);
    size_t count = 0u;

    while (count < length && uart_is_readable(instance))
    {
        data[count] = (uint8_t)uart_getc(instance);
        ++count;
    }
    return count;
}

const uart_bus_vtbl_t rp2350_rs485_uart_vtbl = {
    .write = rp2350_rs485_uart_write,
    .read = rp2350_rs485_uart_read,
};

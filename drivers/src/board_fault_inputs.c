#include "drivers/board_fault_inputs.h"
#include "drivers/industrial_board_pins.h"
#include "hardware/gpio.h"

/* Compile/mock validated, not hardware qualified. No runtime allocation. */
static void configure_input(unsigned pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
}

void board_fault_inputs_init(void)
{
    configure_input(INDUSTRIAL_POWER_INPUT_FAULT_GPIO);
    configure_input(INDUSTRIAL_BOARD_FAULT_GPIO);
    configure_input(INDUSTRIAL_3V3_PGOOD_GPIO);
    configure_input(INDUSTRIAL_5V_PGOOD_GPIO);
}

board_fault_inputs_t board_fault_inputs_read(void)
{
    return (board_fault_inputs_t){
        .power_input_fault = !gpio_get(INDUSTRIAL_POWER_INPUT_FAULT_GPIO),
        .board_fault = !gpio_get(INDUSTRIAL_BOARD_FAULT_GPIO),
        .rail_3v3_fault = !gpio_get(INDUSTRIAL_3V3_PGOOD_GPIO),
        .rail_5v_fault = !gpio_get(INDUSTRIAL_5V_PGOOD_GPIO),
    };
}

#ifndef DRIVERS_BOARD_FAULT_INPUTS_H
#define DRIVERS_BOARD_FAULT_INPUTS_H

#include <stdbool.h>

/* Driver-owned GPIO binding; only call on the industrial board. */
typedef struct
{
    bool power_input_fault;
    bool board_fault;
    bool rail_3v3_fault;
    bool rail_5v_fault;
} board_fault_inputs_t;

void board_fault_inputs_init(void);
board_fault_inputs_t board_fault_inputs_read(void);

#endif

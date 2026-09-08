#ifndef DRIVERS_INDUSTRIAL_BOARD_PINS_H
#define DRIVERS_INDUSTRIAL_BOARD_PINS_H

/* RP2350_INDUSTRIAL_MVP_V1 schematic binding. Compile/mock validated,
 * not hardware qualified. Shared by drivers and resource validation so a
 * pin revision cannot silently invalidate conflict detection. */
#define INDUSTRIAL_UIC_BASE_GPIO 40u
#define INDUSTRIAL_DO_ENABLE_GPIO 10u
#define INDUSTRIAL_DO_COMMAND_BASE_GPIO 20u
#define INDUSTRIAL_DO_FAULT_BASE_GPIO 22u
#define INDUSTRIAL_POWER_INPUT_FAULT_GPIO 11u
#define INDUSTRIAL_BOARD_FAULT_GPIO 19u
#define INDUSTRIAL_3V3_PGOOD_GPIO 34u
#define INDUSTRIAL_5V_PGOOD_GPIO 35u

#endif

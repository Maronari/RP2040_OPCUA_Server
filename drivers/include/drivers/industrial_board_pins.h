#ifndef DRIVERS_INDUSTRIAL_BOARD_PINS_H
#define DRIVERS_INDUSTRIAL_BOARD_PINS_H

/* RP2350_INDUSTRIAL_MVP_V1 schematic binding. Compile/mock validated,
 * not hardware qualified. Shared by drivers and resource validation so a
 * pin revision cannot silently invalidate conflict detection. */
#define INDUSTRIAL_UIC_BASE_GPIO 40u
/* MAX22190 /READY (DI_READY_N), active low. Direct MCU input as of the
 * 2026-09-10 HARDWARE_MVP.md revision -- it previously reached the MCU
 * through TCAL9539 P14, which no longer exists. The board provides an
 * external 10k -> +3V3_DIG pull-up, so the driver disables internal pulls. */
#define INDUSTRIAL_DI_READY_GPIO 24u
/* Shared industrial SPI bus (HARDWARE_MVP.md section 6.3 frozen GPIO map).
 * The peripheral instance is not a board choice: GPIO12/14/15 carry spi1's
 * MISO/SCK/MOSI functions on RP2350 silicon, the same way GPIO36/37 fix
 * UART1 for the RS-485 binding. ADS8688, both AD5422 devices and MAX22190
 * share these three lines and differ only in their select line. */
#define INDUSTRIAL_BOARD_SPI_INSTANCE 1u
#define INDUSTRIAL_BOARD_SPI_MISO_GPIO 12u
#define INDUSTRIAL_BOARD_SPI_SCK_GPIO 14u
#define INDUSTRIAL_BOARD_SPI_MOSI_GPIO 15u
#define INDUSTRIAL_ADC_CS_GPIO 13u /**< ADS8688 /CS. */
#define INDUSTRIAL_DI_CS_GPIO 16u  /**< MAX22190 /CS. */
/** AD5422 daisy-chain LATCH, used as this binding's chip-select: idle high,
 *  and the deassert edge at the end of a transfer is the LOW->HIGH edge that
 *  latches both devices (HARDWARE_MVP.md section 3.2.2). */
#define INDUSTRIAL_AO_LATCH_GPIO 17u
#define INDUSTRIAL_DO_ENABLE_GPIO 10u
#define INDUSTRIAL_DO_COMMAND_BASE_GPIO 20u
#define INDUSTRIAL_DO_FAULT_BASE_GPIO 22u
#define INDUSTRIAL_POWER_INPUT_FAULT_GPIO 11u
#define INDUSTRIAL_BOARD_FAULT_GPIO 19u
#define INDUSTRIAL_3V3_PGOOD_GPIO 34u
#define INDUSTRIAL_5V_PGOOD_GPIO 35u

#endif

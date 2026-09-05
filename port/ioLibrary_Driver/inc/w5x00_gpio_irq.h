/**
 * Copyright (c) 2022 WIZnet Co.,Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _W5X00_GPIO_IRQ_H_
#define _W5X00_GPIO_IRQ_H_

/**
 * ----------------------------------------------------------------------------------------------------
 * Macros
 * ----------------------------------------------------------------------------------------------------
 */
/* GPIO
 *
 * Default matches a generic W5x00 reference module. RP2350_INDUSTRIAL_MVP_V1
 * assigns GPIO8 (ETH_INT_N) instead -- see FIRMWARE_ENABLE_INDUSTRIAL_W5500_PINS
 * in port/CMakeLists.txt (v0.5.1 T10). Not currently wired into any live
 * init path in this firmware (wizchip_gpio_interrupt_initialize() has no
 * caller), so this override has no observed behavioral effect yet -- kept
 * consistent with w5x00_spi.h's pins for completeness against
 * HARDWARE_MVP.md's frozen GPIO map. */
#ifndef PIN_INT
#define PIN_INT 21
#endif

/**
 * ----------------------------------------------------------------------------------------------------
 * Functions
 * ----------------------------------------------------------------------------------------------------
 */
/* GPIO */
/*! \brief Initialize w5x00 gpio interrupt callback function
 *  \ingroup w5x00_gpio_irq
 *
 *  Add a w5x00 interrupt callback.
 *
 *  \param socket socket number
 *  \param callback the gpio interrupt callback function
 */
void wizchip_gpio_interrupt_initialize(uint8_t socket, void (*callback)(void));

/*! \brief Assign gpio interrupt callback function
 *  \ingroup w5x00_gpio_irq
 *
 *  GPIO interrupt callback function.
 *
 *  \param gpio Which GPIO caused this interrupt
 *  \param events Which events caused this interrupt. See \ref gpio_set_irq_enabled for details.
 */
static void wizchip_gpio_interrupt_callback(uint gpio, uint32_t events);

#endif /* _W5X00_GPIO_IRQ_H_ */

/**
 * @file
 * @brief Host-test stand-in for the Pico SDK's hardware/timer.h: only
 *        busy_wait_us_32(), defined by the test so each delay is recorded
 *        in order with the SPI frames around it.
 */
#ifndef HARDWARE_TIMER_H
#define HARDWARE_TIMER_H

#include <stdint.h>

void busy_wait_us_32(uint32_t delay_us);

#endif

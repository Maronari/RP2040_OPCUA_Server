/**
 * @file
 * @brief Minimal host-test stand-in for the Pico SDK's hardware/adc.h,
 *        covering only the calls
 *        drivers/src/adc_voltage_channel_driver.c's
 *        native_adc_voltage_read_raw() makes (adc_init/adc_gpio_init/
 *        adc_select_input/adc_read). Models an 8-channel ADC (matching
 *        RP2350B's ADC0..ADC7) with a per-channel settable raw code.
 */
#ifndef HARDWARE_ADC_H
#define HARDWARE_ADC_H

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

#define ADC_BASE_PIN 40u
#define NUM_ADC_CHANNELS 8u
#define NUM_BANK0_GPIOS 48u

extern bool g_adc_stub_init_called;
extern unsigned int g_adc_stub_init_call_count;
extern bool g_adc_stub_gpio_init_called[NUM_BANK0_GPIOS];
extern unsigned int g_adc_stub_selected_input;
extern uint16_t g_adc_stub_channel_code[NUM_ADC_CHANNELS];

/** @brief Test-only helper (not part of the real Pico SDK): reset all stub state. */
void adc_stub_reset(void);

void adc_init(void);
void adc_gpio_init(uint gpio);
void adc_select_input(uint input);
uint16_t adc_read(void);

#endif

/**
 * @file
 * @brief Implementation of the host-test hardware/adc.h stand-in (see that
 *        header for rationale).
 */
#include "hardware/adc.h"

#include <string.h>

bool g_adc_stub_init_called;
unsigned int g_adc_stub_init_call_count;
bool g_adc_stub_gpio_init_called[NUM_BANK0_GPIOS];
unsigned int g_adc_stub_selected_input;
uint16_t g_adc_stub_channel_code[NUM_ADC_CHANNELS];

void adc_stub_reset(void)
{
    g_adc_stub_init_called = false;
    g_adc_stub_init_call_count = 0u;
    memset(g_adc_stub_gpio_init_called, 0, sizeof(g_adc_stub_gpio_init_called));
    g_adc_stub_selected_input = 0u;
    memset(g_adc_stub_channel_code, 0, sizeof(g_adc_stub_channel_code));
}

void adc_init(void)
{
    g_adc_stub_init_called = true;
    ++g_adc_stub_init_call_count;
}

void adc_gpio_init(uint gpio)
{
    g_adc_stub_gpio_init_called[gpio] = true;
}

void adc_select_input(uint input)
{
    g_adc_stub_selected_input = input;
}

uint16_t adc_read(void)
{
    return g_adc_stub_channel_code[g_adc_stub_selected_input];
}

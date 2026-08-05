#ifndef TEST_WIZCHIP_CONF_H
#define TEST_WIZCHIP_CONF_H
#include <stdint.h>
void wizchip_cris_initialize(void);
void wizchip_reset(void);
void wizchip_soft_reset(void);
void wizchip_initialize(void);
void wizchip_check(void);
void setSHAR(uint8_t *);
void ctlwizchip(uint8_t, void *);
#define CW_RESET_PHY 0u
#endif

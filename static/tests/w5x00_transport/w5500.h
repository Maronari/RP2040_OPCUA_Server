#ifndef TEST_W5500_H
#define TEST_W5500_H

#include <stdint.h>

/* Minimal W5500 register surface used by the bounded TX-FSR reader. */
#define WIZCHIP_SREG_BLOCK(N) (1u + (4u * (N)))
#define WIZCHIP_RXBUF_BLOCK(N) (3u + (4u * (N)))
#define Sn_TX_FSR(N) ((uint32_t)((0x0020u << 8u) + (WIZCHIP_SREG_BLOCK(N) << 3u)))
#define Sn_RX_RSR(N) ((uint32_t)((0x0026u << 8u) + (WIZCHIP_SREG_BLOCK(N) << 3u)))
#define WIZCHIP_OFFSET_INC(address, offset) ((uint32_t)((address) + ((offset) << 8u)))

uint8_t WIZCHIP_READ(uint32_t address);
void WIZCHIP_READ_BUF(uint32_t address, uint8_t *buffer, uint16_t length);
uint8_t getSn_MR(uint8_t sn);
uint8_t getSn_RXBUF_SIZE(uint8_t sn);
uint8_t getSn_TXBUF_SIZE(uint8_t sn);
uint16_t getSn_RX_WR(uint8_t sn);

#endif

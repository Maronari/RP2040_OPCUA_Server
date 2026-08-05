#ifndef TEST_SOCKET_H
#define TEST_SOCKET_H

#include <stdint.h>
#define Sn_MR_MACRAW 0x04u
#define SOCK_CLOSED 0x00u
#define SOCK_MACRAW 0x42u
#define Sn_CR_OPEN 0x01u
#define Sn_CR_CLOSE 0x10u
#define Sn_CR_SEND 0x20u
#define Sn_CR_RECV 0x40u
#define Sn_IR_SENDOK 0x10u
#define Sn_IR_TIMEOUT 0x08u

int8_t socket(uint8_t, uint8_t, uint16_t, uint8_t);
uint8_t getSn_SR(uint8_t);
uint16_t getSn_TX_FSR(uint8_t);
uint16_t getSn_RX_RSR(uint8_t);
uint16_t getSn_RX_RD(uint8_t);
void setSn_RX_RD(uint8_t, uint16_t);
void setSn_MR(uint8_t, uint8_t);
void setSn_PORT(uint8_t, uint16_t);
uint8_t getSn_CR(uint8_t);
void setSn_CR(uint8_t, uint8_t);
uint8_t getSn_IR(uint8_t);
void setSn_IR(uint8_t, uint8_t);
void wiz_send_data(uint8_t, uint8_t *, uint16_t);
void wiz_recv_data(uint8_t, uint8_t *, uint16_t);
void wiz_recv_ignore(uint8_t, uint16_t);

#endif

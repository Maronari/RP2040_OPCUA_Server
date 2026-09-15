/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Simon Goldschmidt
 *
 */
#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

#include <stdint.h>

extern void set_system_time(uint32_t sec);
extern unsigned char *g_lwip_ram_heap;

/* Prevent having to link sys_arch.c (we don't test the API layers in unit tests) */
#define NO_SYS                      0
#define MEM_ALIGNMENT               4
#define LWIP_RAW                    1
#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_ICMP                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_IPV6                   0
#define LWIP_IGMP                   1
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_HAVE_LOOPIF            1
#define LWIP_MULTICAST_TX_OPTIONS   1
/*
 * open62541 is configured with a 16 KiB secure send buffer.  ExportConfig's
 * bounded 16-channel response is sent through a blocking lwIP socket, so the
 * lwIP heap must be able to hold one secure response rather than failing to
 * queue it partway through.  Internal SRAM cannot hold this bound alongside
 * the required 360 KiB FreeRTOS heap, so startup reserves this fixed-size
 * arena from PSRAM before lwIP is initialized.
 */
#define LWIP_OPCUA_SECURE_SEND_MIN   16384u
#define MEM_SIZE                    LWIP_OPCUA_SECURE_SEND_MIN
#define LWIP_RAM_HEAP_POINTER       g_lwip_ram_heap

#define LWIP_TCP_KEEPALIVE          0

// disable ACD to avoid build errors
// http://lwip.100.n7.nabble.com/Build-issue-if-LWIP-DHCP-is-set-to-0-td33280.html
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define ETH_PAD_SIZE                0
#define LWIP_IP_ACCEPT_UDP_PORT(p)  ((p) == PP_NTOHS(67))

#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_STATUS_CALLBACK  1

#define TCP_MSS                     (1500 /*mtu*/ - 20 /*iphdr*/ - 20 /*tcphhr*/)
#define TCP_SND_BUF                 LWIP_OPCUA_SECURE_SEND_MIN

#if MEM_SIZE < LWIP_OPCUA_SECURE_SEND_MIN
#error "lwIP MEM_SIZE must carry one bounded OPC UA secure response"
#endif

/* The configured TCP queue must accept the same bounded secure chunk as the
 * lwIP heap. Otherwise a blocking socket write can wait forever for capacity
 * that the request-processing task itself must release. */
#define LWIP_OPCUA_CONFIG_EXPORT_SEND_MIN  LWIP_OPCUA_SECURE_SEND_MIN
#if TCP_SND_BUF < LWIP_OPCUA_CONFIG_EXPORT_SEND_MIN
#error "lwIP TCP_SND_BUF cannot carry the bounded OPC UA configuration export"
#endif

#define LWIP_HTTPD_CGI              0
#define LWIP_HTTPD_SSI              0
#define LWIP_HTTPD_SSI_INCLUDE_TAG  0

#define LWIP_RAND_WIZ()             ((u32_t)rand())

#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 2)
#define LWIP_ARP                    2

#ifdef W5X00_LWIP_ALL_DEBUG
#define LWIP_DEBUG                  1
#define NETIF_DEBUG                 LWIP_DBG_ON
#define TCP_DEBUG                   LWIP_DBG_ON
#define ETHARP_DEBUG                LWIP_DBG_ON
#define PBUF_DEBUG                  LWIP_DBG_ON
#define IP_DEBUG                    LWIP_DBG_ON
#define TCPIP_DEBUG                 LWIP_DBG_ON
#define DHCP_DEBUG                  LWIP_DBG_ON
#define UDP_DEBUG                   LWIP_DBG_ON
#define SNTP_DEBUG                  LWIP_DBG_ON
#endif
/* Keep upstream lwIP debug disabled: it consumes permanent internal SRAM. */

#define LWIP_COMPAT_SOCKETS         0
#define SO_REUSE                    1
#define LWIP_TIMEVAL_PRIVATE        0

#define LWIP_PROVIDE_ERRNO

#define TCPIP_MBOX_SIZE             16
#define TCPIP_THREAD_NAME           "TCPIP_Task"
#define TCPIP_THREAD_PRIO           2
#define TCPIP_THREAD_STACKSIZE      5*1024

#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define MEMP_NUM_NETCONN            10
#define PBUF_POOL_SIZE              7
#define MEMP_NUM_PBUF               16
/* MEM-01 fix: open62541's lwIP EventLoop self-pipe (UA_EventLoopLWIP_pipe(),
 * "Windows has no pipes, use a local TCP connection for the self-pipe
 * trick") permanently holds 2 established TCP PCBs for the life of the
 * server, on this embedded lwIP backend too, not just on Windows hosts.
 * The lwIP default (5) therefore only leaves 3 PCBs for real OPC UA client
 * connections even though opcua_model/src/model_core_lifecycle.c configures
 * maxSecureChannels/maxSessions = 4 -- the 4th concurrent client's TCP
 * connection was accepted at the listen socket but then had no PCB left to
 * become ESTABLISHED, so the client's handshake hung until its own timeout
 * (Windows WinError 121). Budget: 2 (self-pipe) + 4 (maxSecureChannels) + 2
 * spare (a departing client lingering in TIME_WAIT/CLOSE_WAIT must not
 * block a new one). If maxSecureChannels in model_core_lifecycle.c ever
 * changes, this must be revisited together with it. */
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_SEG            48
#define MEMP_NUM_TCP_WND            6
#define DEFAULT_UDP_RECVMBOX_SIZE   6
#define DEFAULT_TCP_RECVMBOX_SIZE   128
#define DEFAULT_ACCEPTMBOX_SIZE     128

//SNTP NETWORK TIME
#define SNTP_SERVER_DNS             1
#define SNTP_SERVER_ADDRESS         "ntp.msk-ix.ru"
#define SNTP_UPDATE_DELAY           600000
#define SNTP_SET_SYSTEM_TIME(s)     set_system_time((int32_t) s)

#endif /* __LWIPOPTS_H__ */

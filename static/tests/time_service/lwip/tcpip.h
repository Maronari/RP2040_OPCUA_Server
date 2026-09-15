#ifndef TIME_SERVICE_TEST_TCPIP_H
#define TIME_SERVICE_TEST_TCPIP_H

/* Host stand-in for lwIP's core lock: the test tracks nesting and asserts
 * the SNTP API is only reached while it is held. */
#define LWIP_TCPIP_CORE_LOCKING 1

void time_service_test_lock_tcpip_core(void);
void time_service_test_unlock_tcpip_core(void);
#define LOCK_TCPIP_CORE() time_service_test_lock_tcpip_core()
#define UNLOCK_TCPIP_CORE() time_service_test_unlock_tcpip_core()

#endif

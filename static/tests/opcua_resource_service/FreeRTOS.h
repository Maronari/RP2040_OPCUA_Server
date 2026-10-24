#ifndef OPCUA_RESOURCE_SERVICE_TEST_FREERTOS_H
#define OPCUA_RESOURCE_SERVICE_TEST_FREERTOS_H

void opcua_resource_service_test_enter_critical(void);
void opcua_resource_service_test_exit_critical(void);

#define taskENTER_CRITICAL() opcua_resource_service_test_enter_critical()
#define taskEXIT_CRITICAL() opcua_resource_service_test_exit_critical()

#endif

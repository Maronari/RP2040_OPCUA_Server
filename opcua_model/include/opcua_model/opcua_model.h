#ifndef OPCUA_MODEL_OPCUA_MODEL_H
#define OPCUA_MODEL_OPCUA_MODEL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    const char *application_name;
    const char *hostname;
    uint16_t port;
    bool enable_alarm_methods;
    bool enable_heap_diagnostics;
    bool enable_task_diagnostics;
} opcua_model_config_t;

bool opcua_model_init(const opcua_model_config_t *config);
void opcua_model_task(void *argument);

#endif

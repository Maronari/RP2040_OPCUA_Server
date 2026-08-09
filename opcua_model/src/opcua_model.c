#include "opcua_model/opcua_model.h"
#include "opcua_model_internal.h"

bool opcua_model_init(const opcua_model_config_t *config)
{
    return model_core_init(config);
}

void opcua_model_task(void *argument)
{
    model_core_task(argument);
}

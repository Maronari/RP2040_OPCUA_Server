/**
 * @file
 * @brief Public-API forwarding shim for the `opcua_model` component.
 *
 * Responsibilities:
 * - Implements the two functions declared in `opcua_model/opcua_model.h`
 *   (`opcua_model_init`, `opcua_model_task`) by forwarding verbatim to their
 *   `model_core_*` counterparts in `model_core_lifecycle.c`.
 *
 * Ownership:
 * Holds no state of its own; all adapter state lives in
 * `opcua_model_context_t` (`opcua_model_context.c`).
 *
 * Concurrency:
 * N/A - pure forwarding, no logic beyond the call.
 *
 * @note Full behavioral contracts (blocking behavior, PSRAM allocation,
 *       task lifetime) are documented on the public declarations in
 *       `opcua_model/opcua_model.h`, not duplicated here.
 */
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

#ifndef SERVICES_SECURITY_STATE_SERVICE_H
#define SERVICES_SECURITY_STATE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool commissioning_mode;
    uint32_t trust_list_version;
    uint32_t commissioning_entered_epoch_s;
    uint32_t commissioning_deadline_epoch_s;
    uint32_t active_identity_generation;
    uint32_t active_trust_generation;
    uint32_t pending_identity_generation;
    uint32_t pending_trust_generation;
    uint32_t last_security_fault_code;
} security_state_service_snapshot_t;

bool security_state_service_init(bool default_commissioning_mode,
                                 uint32_t default_trust_list_version);
bool security_state_service_get_snapshot(security_state_service_snapshot_t *out_snapshot);
bool security_state_service_get_commissioning_mode(bool *out_enabled);
bool security_state_service_set_commissioning_mode(bool enabled);
bool security_state_service_get_trust_list_version(uint32_t *out_version);
bool security_state_service_set_trust_list_version(uint32_t version);
bool security_state_service_set_trust_generations(uint32_t active_generation,
                                                  uint32_t pending_generation);
bool security_state_service_set_identity_generations(uint32_t active_generation,
                                                     uint32_t pending_generation);

#endif

#ifndef SERVICES_SECURITY_CERTIFICATE_GROUP_SERVICE_H
#define SERVICES_SECURITY_CERTIFICATE_GROUP_SERVICE_H

#include <stdbool.h>

#include "open62541.h"

UA_StatusCode security_certificate_group_service_configure(UA_CertificateGroup *cert_group,
                                                           const UA_NodeId *certificate_group_id,
                                                           const UA_Logger *logger);
UA_StatusCode security_certificate_group_service_refresh(UA_CertificateGroup *cert_group);
bool security_certificate_group_service_manages(const UA_CertificateGroup *cert_group);

#endif

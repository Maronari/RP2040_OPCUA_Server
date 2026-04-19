#include "services/security_certificate_group_service.h"

#include <string.h>

#include "services/security_service.h"

#define SECURITY_CERTIFICATE_GROUP_SERVICE_CONTEXT_MAGIC 0x53434731u /* "SCG1" */

typedef struct
{
    uint32_t magic;
    UA_CertificateGroup store;
} security_certificate_group_service_context_t;

static security_certificate_group_service_context_t *security_certificate_group_service_context_from_group(const UA_CertificateGroup *cert_group)
{
    security_certificate_group_service_context_t *context;

    if (cert_group == NULL || cert_group->context == NULL)
    {
        return NULL;
    }

    context = (security_certificate_group_service_context_t *)cert_group->context;
    if (context->magic != SECURITY_CERTIFICATE_GROUP_SERVICE_CONTEXT_MAGIC)
    {
        return NULL;
    }

    return context;
}

static UA_StatusCode security_certificate_group_service_build_flash_trust_list(UA_TrustListDataType *out_trust_list)
{
    security_service_blob_view_t trust_list_blob = {0};
    UA_ByteString source = UA_BYTESTRING_NULL;
    UA_StatusCode status;

    if (out_trust_list == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    UA_TrustListDataType_init(out_trust_list);
    out_trust_list->specifiedLists = UA_TRUSTLISTMASKS_TRUSTEDCERTIFICATES;

    if (!security_service_get_trust_list(&trust_list_blob) || trust_list_blob.length == 0u)
    {
        return UA_STATUSCODE_GOOD;
    }

    out_trust_list->trustedCertificates =
        (UA_ByteString *)UA_Array_new(1u, &UA_TYPES[UA_TYPES_BYTESTRING]);
    if (out_trust_list->trustedCertificates == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    out_trust_list->trustedCertificatesSize = 1u;
    source.data = (UA_Byte *)(uintptr_t)trust_list_blob.data;
    source.length = trust_list_blob.length;
    status = UA_ByteString_copy(&source, &out_trust_list->trustedCertificates[0]);
    if (status != UA_STATUSCODE_GOOD)
    {
        UA_TrustListDataType_clear(out_trust_list);
    }

    return status;
}

static UA_StatusCode security_certificate_group_service_reload_store(UA_CertificateGroup *cert_group)
{
    security_certificate_group_service_context_t *context =
        security_certificate_group_service_context_from_group(cert_group);
    UA_TrustListDataType trust_list;
    UA_StatusCode status;

    if (context == NULL || cert_group == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    status = security_certificate_group_service_build_flash_trust_list(&trust_list);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = UA_CertificateGroup_Memorystore(&context->store,
                                             &cert_group->certificateGroupId,
                                             &trust_list,
                                             cert_group->logging,
                                             NULL);
    UA_TrustListDataType_clear(&trust_list);
    return status;
}

static UA_StatusCode security_certificate_group_service_store_supported_trust_list(const UA_TrustListDataType *trust_list)
{
    const uint32_t unsupported_mask = UA_TRUSTLISTMASKS_ISSUERCERTIFICATES |
                                      UA_TRUSTLISTMASKS_TRUSTEDCRLS |
                                      UA_TRUSTLISTMASKS_ISSUERCRLS;
    const UA_ByteString *entry = NULL;

    if (trust_list == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if ((trust_list->specifiedLists & unsupported_mask) != 0u ||
        trust_list->issuerCertificatesSize != 0u ||
        trust_list->trustedCrlsSize != 0u ||
        trust_list->issuerCrlsSize != 0u)
    {
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    if (trust_list->trustedCertificatesSize == 0u)
    {
        return security_service_set_trust_list(NULL, 0u) ?
                   UA_STATUSCODE_GOOD :
                   UA_STATUSCODE_BADINTERNALERROR;
    }

    if (trust_list->trustedCertificatesSize != 1u)
    {
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    entry = &trust_list->trustedCertificates[0];
    if (entry->length > 0u && entry->data == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    return security_service_set_trust_list(entry->data, entry->length) ?
               UA_STATUSCODE_GOOD :
               UA_STATUSCODE_BADINTERNALERROR;
}

static UA_StatusCode security_certificate_group_service_get_trust_list(UA_CertificateGroup *cert_group,
                                                                       UA_TrustListDataType *trust_list)
{
    (void)cert_group;
    return security_certificate_group_service_build_flash_trust_list(trust_list);
}

static UA_StatusCode security_certificate_group_service_set_trust_list(UA_CertificateGroup *cert_group,
                                                                       const UA_TrustListDataType *trust_list)
{
    UA_StatusCode status = security_certificate_group_service_store_supported_trust_list(trust_list);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return security_certificate_group_service_refresh(cert_group);
}

static UA_StatusCode security_certificate_group_service_add_to_trust_list(UA_CertificateGroup *cert_group,
                                                                          const UA_TrustListDataType *trust_list)
{
    (void)cert_group;
    (void)trust_list;
    return UA_STATUSCODE_BADNOTSUPPORTED;
}

static UA_StatusCode security_certificate_group_service_remove_from_trust_list(UA_CertificateGroup *cert_group,
                                                                               const UA_TrustListDataType *trust_list)
{
    (void)cert_group;
    (void)trust_list;
    return UA_STATUSCODE_BADNOTSUPPORTED;
}

static UA_StatusCode security_certificate_group_service_get_rejected_list(UA_CertificateGroup *cert_group,
                                                                          UA_ByteString **rejected_list,
                                                                          size_t *rejected_list_size)
{
    security_certificate_group_service_context_t *context =
        security_certificate_group_service_context_from_group(cert_group);

    if (context == NULL || context->store.getRejectedList == NULL)
    {
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    return context->store.getRejectedList(&context->store, rejected_list, rejected_list_size);
}

static UA_StatusCode security_certificate_group_service_get_certificate_crls(UA_CertificateGroup *cert_group,
                                                                             const UA_ByteString *certificate,
                                                                             const UA_Boolean is_trusted,
                                                                             UA_ByteString **crls,
                                                                             size_t *crls_size)
{
    security_certificate_group_service_context_t *context =
        security_certificate_group_service_context_from_group(cert_group);

    if (context == NULL || context->store.getCertificateCrls == NULL)
    {
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    return context->store.getCertificateCrls(&context->store,
                                             certificate,
                                             is_trusted,
                                             crls,
                                             crls_size);
}

static UA_StatusCode security_certificate_group_service_verify_certificate(UA_CertificateGroup *cert_group,
                                                                           const UA_ByteString *certificate)
{
    security_certificate_group_service_context_t *context =
        security_certificate_group_service_context_from_group(cert_group);

    if (context == NULL || context->store.verifyCertificate == NULL)
    {
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    return context->store.verifyCertificate(&context->store, certificate);
}

static void security_certificate_group_service_clear(UA_CertificateGroup *cert_group)
{
    security_certificate_group_service_context_t *context =
        security_certificate_group_service_context_from_group(cert_group);

    if (cert_group == NULL)
    {
        return;
    }

    UA_NodeId_clear(&cert_group->certificateGroupId);

    if (context != NULL)
    {
        context->magic = 0u;
        if (context->store.clear != NULL)
        {
            context->store.clear(&context->store);
        }
        UA_free(context);
    }

    memset(cert_group, 0, sizeof(*cert_group));
}

UA_StatusCode security_certificate_group_service_configure(UA_CertificateGroup *cert_group,
                                                           const UA_NodeId *certificate_group_id,
                                                           const UA_Logger *logger)
{
    security_certificate_group_service_context_t *context;
    UA_StatusCode status;

    if (cert_group == NULL || certificate_group_id == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    if (cert_group->clear != NULL)
    {
        cert_group->clear(cert_group);
    }

    context = (security_certificate_group_service_context_t *)UA_calloc(1u, sizeof(*context));
    if (context == NULL)
    {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    context->magic = SECURITY_CERTIFICATE_GROUP_SERVICE_CONTEXT_MAGIC;

    status = UA_NodeId_copy(certificate_group_id, &cert_group->certificateGroupId);
    if (status != UA_STATUSCODE_GOOD)
    {
        UA_free(context);
        return status;
    }

    cert_group->context = context;
    cert_group->logging = logger;
    cert_group->getTrustList = security_certificate_group_service_get_trust_list;
    cert_group->setTrustList = security_certificate_group_service_set_trust_list;
    cert_group->addToTrustList = security_certificate_group_service_add_to_trust_list;
    cert_group->removeFromTrustList = security_certificate_group_service_remove_from_trust_list;
    cert_group->getRejectedList = security_certificate_group_service_get_rejected_list;
    cert_group->getCertificateCrls = security_certificate_group_service_get_certificate_crls;
    cert_group->verifyCertificate = security_certificate_group_service_verify_certificate;
    cert_group->clear = security_certificate_group_service_clear;

    status = security_certificate_group_service_refresh(cert_group);
    if (status != UA_STATUSCODE_GOOD)
    {
        cert_group->clear(cert_group);
        return status;
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode security_certificate_group_service_refresh(UA_CertificateGroup *cert_group)
{
    return security_certificate_group_service_reload_store(cert_group);
}

bool security_certificate_group_service_manages(const UA_CertificateGroup *cert_group)
{
    return security_certificate_group_service_context_from_group(cert_group) != NULL;
}

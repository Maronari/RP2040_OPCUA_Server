/**
 * @file
 * @brief Wires an open62541 `UA_CertificateGroup` (PKI store vtable) to
 * `security_service`'s Flash-resident trust list.
 *
 * Responsibilities:
 * - Implements the `UA_CertificateGroup` function-pointer table
 *   (`getTrustList`/`setTrustList`/`addToTrustList`/`removeFromTrustList`/
 *   `getRejectedList`/`getCertificateCrls`/`verifyCertificate`/`clear`) so
 *   that open62541's secure-channel and session PKI logic reads/writes
 *   through `security_service` instead of open62541's own filesystem-based
 *   trust list store.
 * - Delegates the actual trust-anchor verification logic to open62541's
 *   built-in `UA_CertificateGroup_Memorystore` (rebuilt on every refresh
 *   from the current `security_service` trust list) rather than
 *   reimplementing X.509 chain verification.
 *
 * @note Architecture deviation: this header/module exposes open62541 types
 * (`UA_CertificateGroup *`, `UA_StatusCode`, `UA_NodeId`,
 * `UA_TrustListDataType`) directly in a `services/` public API, which
 * departs from `AGENTS.md`'s goal of keeping services independent of
 * open62541 types where practical. See
 * `docs/code/architecture.md#confirmed-layering-observations` (item 3).
 * Not fixed as part of this documentation pass (comment-only scope).
 *
 * Ownership: allocates and owns a small heap-allocated context struct
 * (`security_certificate_group_service_context_t`) per configured
 * `UA_CertificateGroup`, stored in `cert_group->context` and freed by
 * `clear()`. The wrapped `UA_CertificateGroup_Memorystore` instance
 * (`context->store`) is itself rebuilt from scratch on every
 * `security_certificate_group_service_refresh()` call.
 *
 * Concurrency: no internal locking; expected to run in whatever task
 * context open62541 invokes trust-list callbacks from, or the
 * initialization task that calls `configure()`/`refresh()` (see
 * `opcua_model/src/model_core_lifecycle.c:518` and
 * `opcua_model/src/model_security.c:70`).
 */
#ifndef SERVICES_SECURITY_CERTIFICATE_GROUP_SERVICE_H
#define SERVICES_SECURITY_CERTIFICATE_GROUP_SERVICE_H

#include <stdbool.h>

#include "open62541.h"

/**
 * @brief Initializes @p cert_group's vtable to route trust-list operations
 * through `security_service`, and performs an initial load from Flash.
 *
 * If @p cert_group already has a `clear` callback set, it is invoked first
 * to release any prior state. Allocates a new context, copies
 * @p certificate_group_id into `cert_group->certificateGroupId`, installs
 * all vtable function pointers, then calls
 * `security_certificate_group_service_refresh()` to build the initial
 * memory-store trust list.
 *
 * @param cert_group Certificate group to configure in place (e.g.
 * `config->secureChannelPKI` or `config->sessionPKI`).
 * @param certificate_group_id NodeId identifying this certificate group;
 * copied (not aliased).
 * @param logger open62541 logger, stored by reference (not copied) into
 * `cert_group->logging` for the lifetime of the group.
 * @return `UA_STATUSCODE_GOOD` on success. On failure, @p cert_group's
 * `clear()` is invoked to unwind partial state before returning the error
 * (`UA_STATUSCODE_BADINVALIDARGUMENT`, `UA_STATUSCODE_BADOUTOFMEMORY`, or
 * whatever `refresh()` returned).
 * @pre `security_service_init()` must already have run so the trust list is
 * available to load.
 */
UA_StatusCode security_certificate_group_service_configure(UA_CertificateGroup *cert_group,
                                                           const UA_NodeId *certificate_group_id,
                                                           const UA_Logger *logger);
/**
 * @brief Rebuilds @p cert_group's internal `UA_CertificateGroup_Memorystore`
 * from the current `security_service` trust list.
 *
 * Call after any change to the underlying trust list (e.g. after
 * `security_service_set_trust_list()`) to make open62541's in-memory PKI
 * verifier reflect the new trusted certificates.
 *
 * @param cert_group A group previously configured via
 * `security_certificate_group_service_configure()`.
 * @return `UA_STATUSCODE_GOOD` on success; `UA_STATUSCODE_BADINVALIDARGUMENT`
 * if @p cert_group was not configured by this module;
 * `UA_STATUSCODE_BADOUTOFMEMORY` if the trust-list `UA_ByteString` array
 * could not be allocated; otherwise the status from
 * `UA_CertificateGroup_Memorystore()`.
 */
UA_StatusCode security_certificate_group_service_refresh(UA_CertificateGroup *cert_group);
/**
 * @brief Checks whether @p cert_group was configured by this module (i.e.
 * its `context` carries this module's magic tag).
 * @return false for NULL, an unconfigured group, or a group owned by a
 * different `UA_CertificateGroup` implementation.
 */
bool security_certificate_group_service_manages(const UA_CertificateGroup *cert_group);

#endif

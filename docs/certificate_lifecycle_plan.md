# Certificate Lifecycle And Trust Plan

## Goal

- Make OPC UA certificate and trust management operationally safe for long-lived deployed devices.
- Move trust list, commissioning state, and certificate rotation to explicit flash-backed lifecycle services.
- Use open62541 CertificateGroup plugin APIs as the architectural integration point for trust handling.
- Keep authorization grounded in the access-control plugin and explicit method-level checks instead of betting on experimental RBAC APIs.

## Current State

The codebase already has a usable first security baseline:

- device certificate and private key are persisted by `services/src/security_service.c`
- trust list bytes are persisted in the same store and exposed through `security_service_get_trust_list()` and `security_service_set_trust_list()`
- commissioning mode exists and currently switches the server between `AcceptAll` and `Memorystore`
- certificate expiry metadata is partially available via `security_service_get_device_certificate_not_after()`
- OPC UA maintenance methods already expose `SetTrustList`, `EnterCommissioningMode`, and `ExitCommissioningMode`

That baseline is not yet operationally robust enough for production lifecycle management because:

- security persistence is monolithic instead of split into identity, trust, and state domains
- the security flash region is still computed ad hoc from the end of flash instead of coming from `ota/cmake/ota_layout.cmake`
- trust list size is too small for realistic certificate chains and future CRL support
- trust list updates are direct replace operations instead of staged validate-and-apply workflows
- certificate rotation is not modeled as a first-class staged lifecycle
- certificate-group integration still uses temporary in-memory setup instead of a flash-backed backend

## Invariants

- `boards/include/boards/weact_studio_rp2350a_16mb.h` remains the source of board flash facts.
- `ota/cmake/ota_layout.cmake` remains the single source of truth for flash partitioning.
- Firmware OTA state, config state, security identity state, and trust state remain separate domains.
- The device must stay bootable even with corrupted trust list or certificate slots.
- Commissioning mode must be explicit state, not inferred only from an empty trust list.
- Private keys are never exported over the network.
- Access decisions for maintenance methods continue to use the access-control plugin plus explicit role checks in the application layer.

## Target Scope

The first lifecycle-management implementation should cover:

- flash-backed trust list storage with staged import and atomic activation
- explicit commissioning state with a persisted mode and optional timeout or deadline
- staged server certificate rotation with certificate and key validation before activation
- trust list export and import over OPC UA maintenance methods
- certificate metadata reporting including thumbprint, `notBefore`, `notAfter`, and time-to-expiry
- warnings and alarms for expired certificates and soon-to-expire certificates

The first implementation should explicitly exclude:

- full GDS enrollment flows
- automatic CSR submission to remote PKI systems
- private key export
- per-user fine-grained rights modeled through open62541 RBAC tables
- full CRL management if it materially delays the core trust-list and rotation workflow

## Flash Layout Direction

Security persistence must stop using a hard-coded tail-of-flash offset and move under the OTA layout generator.

Recommended direction:

- add dedicated security regions to `ota/cmake/ota_layout.cmake`
- reserve independent A/B slots for device identity material and trust-list material
- keep a small explicit security state region for commissioning mode and rotation metadata

Suggested 16 MB direction after the current OTA/config work:

- Bootloader: existing OTA layout
- App Bank A: existing OTA layout
- App Bank B: existing OTA layout
- Config Slot 0: existing OTA layout
- Config Slot 1: existing OTA layout
- Security Identity Slot 0: `64 KiB`
- Security Identity Slot 1: `64 KiB`
- Trust Slot 0: `64 KiB`
- Trust Slot 1: `64 KiB`
- Security State: `64 KiB`
- OTA State: `64 KiB`

Exact offsets should be generated, not hand-written.

## Persistent Model

Split security persistence into three independently versioned areas.

### Identity Store

Contains:

- device certificate
- private key
- certificate metadata cache
- generation counter
- payload CRC
- active and pending flags

### Trust Store

Contains:

- serialized trust list payload
- schema version
- generation counter
- payload CRC
- optional flags describing whether trusted certs, issuer certs, and CRLs are present

### Security State Store

Contains:

- commissioning mode
- commissioning entered timestamp
- commissioning expiry deadline if configured
- pending identity activation generation
- active identity generation
- active trust generation
- last security maintenance result or fault code

Each area should use an A/B slot model with validated headers and generation-based selection, following the same operational principles already used for firmware and configuration.

## Service Split

Do not keep growing `services/src/security_service.c` as one monolith.

Recommended split:

- `services/include/services/security_identity_store_service.h`
- `services/src/security_identity_store_service.c`
- `services/include/services/security_trust_store_service.h`
- `services/src/security_trust_store_service.c`
- `services/include/services/security_state_service.h`
- `services/src/security_state_service.c`
- `services/include/services/security_certinfo_service.h`
- `services/src/security_certinfo_service.c`
- `services/include/services/security_trust_import_service.h`
- `services/src/security_trust_import_service.c`
- `services/include/services/security_identity_import_service.h`
- `services/src/security_identity_import_service.c`
- `services/include/services/security_certificate_group_service.h`
- `services/src/security_certificate_group_service.c`

Responsibilities:

- `security_identity_store_service`: A/B persistence and activation of certificate plus private key
- `security_trust_store_service`: A/B persistence and activation of trust-list payloads
- `security_state_service`: persisted commissioning and pending-rotation state
- `security_certinfo_service`: parse X.509, thumbprints, validity windows, and warning levels
- `security_trust_import_service`: staged chunk upload for trust lists
- `security_identity_import_service`: staged chunk upload for certificate rotation packages
- `security_certificate_group_service`: bridge between flash-backed trust data and open62541 `CertificateGroup` plugin hooks

`security_service` can remain as a compatibility facade while callers are migrated.

## CertificateGroup Integration

The target architecture should stop rebuilding a temporary in-memory trust store during every server startup.

Instead:

- build a flash-backed trust backend exposed through `getTrustList` and `setTrustList`
- plug that backend into `config->secureChannelPKI` and `config->sessionPKI`
- keep `AcceptAll` only for explicit commissioning mode
- make trust-list persistence the source of truth, not an intermediate `UA_TrustListDataType` assembled ad hoc in `opcua_model`

This aligns the application with open62541 certificate lifecycle mechanisms instead of fighting them.

## Commissioning Model

Commissioning should become explicit operational state.

Suggested state machine:

- `strict`
- `commissioning_open`
- `rotation_pending`

Expected semantics:

- `strict`: only trusted certificates, no insecure bootstrap shortcuts
- `commissioning_open`: `AcceptAll` allowed for onboarding or trust recovery, optionally time-bounded
- `rotation_pending`: new identity or trust material is staged and must become active on reboot

Recommended rules:

- entering commissioning mode should be auditable and optionally time-limited
- exiting commissioning mode requires a non-empty valid trust list
- clearing the trust list should automatically force commissioning mode
- factory reset scopes should define whether security identity and trust are preserved or erased

## OPC UA API Direction

Keep lifecycle methods grouped under `Device.Maintenance.Security`.

Recommended additions for trust management:

- `GetTrustState() -> String`
- `ExportTrustList() -> ByteString` or `String` manifest plus chunk methods
- `BeginTrustListImport(size, crc32, schemaVersion) -> Boolean`
- `WriteTrustListChunk(ByteString) -> Boolean`
- `ValidateImportedTrustList() -> String`
- `ApplyImportedTrustList() -> Boolean`
- `AbortTrustListImport() -> Boolean`

Recommended additions for server-identity lifecycle:

- `GetCertificateState() -> String`
- `ExportCertificateMetadata() -> String`
- `BeginIdentityRotation(size, crc32, schemaVersion) -> Boolean`
- `WriteIdentityRotationChunk(ByteString) -> Boolean`
- `ValidateStagedIdentity() -> String`
- `ApplyStagedIdentity() -> Boolean`
- `AbortIdentityRotation() -> Boolean`

Recommended commissioning methods:

- keep `EnterCommissioningMode()`
- keep `ExitCommissioningMode()`
- optionally add `SetCommissioningTimeout(seconds)` later if needed

Compatibility direction:

- keep `SetTrustList(ByteString)` temporarily as a compatibility path
- mark it as a simplified legacy operation once staged import is available

## Payload Formats

### Trust Import Payload

For the first implementation, prefer a simple self-describing package:

- manifest with schema version and content flags
- one or more DER or PEM certificates
- optional issuer chain block
- optional CRL section reserved for later support

The first version may accept a flattened PEM bundle if that keeps implementation size reasonable, but the internal storage format should still be structured enough to map cleanly to `UA_TrustListDataType` later.

### Identity Rotation Payload

Recommended package contents:

- server certificate
- matching private key
- manifest fields for schema version, intended application URI, and expected hostname or IP bindings

Private key handling must stay import-only.

## Validation Rules

Validation must happen before activation.

Trust validation should cover:

- payload format and schema version
- parseability of every provided certificate
- trust list size bounds
- duplicate certificate detection where practical
- optional issuer chain sanity if issuer certificates are included

Identity validation should cover:

- certificate and private key parse correctly
- certificate and private key match
- ApplicationUri matches device configuration expectations
- SAN or CN is compatible with the device endpoint naming policy
- `notAfter` is in the future
- validity window is sensible relative to current device time if time is trusted enough

Operational validation should cover:

- strict mode must not activate with an empty trust list
- identity activation must not leave the device without any valid local certificate
- staged generation activation must be atomic across reboot

## Expiry Warnings And Diagnostics

Promote certificate lifetime into explicit diagnostics.

Add or refine security nodes for:

- `CertThumbprint`
- `CertNotBefore`
- `CertNotAfter`
- `DaysUntilExpiry`
- `ExpiryState`
- `CommissioningState`
- `TrustListVersion`
- `ActiveIdentityGeneration`
- `ActiveTrustGeneration`

Recommended thresholds:

- expired: severity critical
- less than or equal to `7` days: high warning
- less than or equal to `30` days: medium warning

`model_evaluate_security_condition()` should evolve from only `expired` versus `normal` into a richer state model that also reports `expires soon`.

## Access Control Direction

Do not deepen dependency on open62541 RBAC for production authorization.

Recommended approach:

- continue using the access-control plugin for session authentication
- continue mapping users to coarse application roles such as Viewer, Operator, Service, and Admin
- keep explicit application-level role checks on sensitive maintenance methods
- limit certificate, trust, and commissioning operations to Admin or a similarly strong service role

This keeps authorization logic understandable and under application control while open62541 RBAC remains experimental.

## Tooling Direction

Add dedicated client tools similar to the OTA and config staging scripts:

- `static/tools/stage_trust.py`
- `static/tools/rotate_identity.py`

Suggested workflows:

- export trust list to a local package for backup and migration
- stage new trust material in chunks
- validate before apply
- optionally request reboot after apply
- stage identity rotation package and validate it before activation

Do not export the private key in any tooling path.

## Migration Plan

1. Move security flash allocation into `ota/cmake/ota_layout.cmake`.
2. Add new generated layout constants and compile-time sanity checks.
3. Introduce split security storage services while preserving `security_service` compatibility wrappers.
4. Migrate trust-list persistence to the new trust store.
5. Add explicit commissioning state persistence.
6. Implement flash-backed CertificateGroup integration for secure channel and session PKI.
7. Add staged trust import and export OPC UA methods.
8. Add staged identity rotation OPC UA methods.
9. Add richer certificate lifetime diagnostics and alarms.
10. Add client-side tooling and verification flows.

## Verification Plan

- Verify slot selection with empty, valid, corrupt, and mixed-generation security slots.
- Verify that strict mode refuses to start without a valid trust list.
- Verify that commissioning mode still boots and exposes only the intended reduced endpoint set.
- Verify staged trust import with valid and invalid bundles.
- Verify staged identity rotation with matching and mismatched certificate-key pairs.
- Verify that expired certificates raise a critical security condition.
- Verify that near-expiry certificates raise a warning before expiration.
- Verify that rollback remains possible when a newly staged identity fails validation before activation.

## Immediate Implementation Order

The safest implementation order is:

1. flash layout alignment
2. split security stores and explicit commissioning state
3. flash-backed CertificateGroup trust integration
4. staged trust import and export
5. staged server certificate rotation
6. expiry warnings, alarms, and tooling

This order fixes the persistence foundation first and avoids layering new lifecycle APIs on top of the current conflicting storage model.

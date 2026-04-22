# Certificate Expiry Notifications Plan

## Goal

- Make certificate expiry operationally visible before it becomes a connectivity outage.
- Warn on both the device identity certificate and trusted client certificates stored in the flash-backed trust list.
- Expose expiry state through OPC UA variables, OPC UA events, logs, and maintenance tooling.
- Keep the design aligned with the current `commissioning_open` and `strict` lifecycle model.

## Implementation Status

- Done: trust-list expiry analysis service with aggregate counts and earliest-expiry tracking.
- Done: trust expiry summary is exposed through `Device.Maintenance.Security.GetTrustState()` JSON and shown by `static/tools/stage_trust.py`.
- Done: maintenance events and logs are emitted on server-certificate and trust-store expiry state transitions.
- Done: connection-time client-certificate expiry warnings are emitted from the access-control/session activation path.
- Deferred by design: dedicated flat `Device.Security.*` trust-expiry variables. The current product path uses maintenance JSON plus tooling first.

## Why This Is Needed

The current lifecycle work already gives us:

- flash-backed device identity storage
- flash-backed trust list storage
- commissioning and strict modes
- staged trust import and staged identity rotation
- device certificate metadata such as `notBefore`, `notAfter`, `daysUntilExpiry`, and `expiryState`

That is enough to manage certificates, but not yet enough to operate them comfortably over time.

Operationally, the dangerous cases are:

- the server certificate is close to expiry and nobody notices until clients start rejecting the server
- one or more trusted client certificates expire and field tools lose access in `strict`
- trust list contents are technically valid, but already operationally risky because all trusted clients are close to expiry

## Scope

This stage should cover:

- expiry diagnostics for the server certificate
- expiry diagnostics for all certificates inside the trust list
- aggregated trust-list expiry health indicators
- OPC UA events, state variables, and logs for soon-to-expire and expired certificates
- connection-time warnings for near-expiry client certificates
- maintenance tooling to inspect expiry health remotely

This stage should not include:

- external email, SMS, or cloud notification delivery
- full GDS enrollment workflows
- CRL or OCSP implementation unless later required
- automatic certificate renewal against a remote CA

## Invariants

- `strict` mode must continue to enforce trust-list based access.
- `commissioning_open` remains the recovery/bootstrap path and must not be silently enabled by expiry logic.
- Expiry monitoring must not require private key export.
- The source of truth for trusted client certificates remains the flash-backed trust list.
- Warning generation must degrade safely if device time is not synchronized.
- OPC UA `Alarms & Conditions` are currently not a reliable notification path in this product and must not be a dependency for this stage.

## Current State

Already present in the codebase:

- device certificate metadata nodes under `Device.Security`
- server-side `expiryState` evaluation for the device identity certificate
- partial internal security condition evaluation for server identity expiry, but not a production-ready `Alarms & Conditions` notification path
- trust import/export workflows and active trust generation tracking

Remaining follow-up ideas:

- dedicated nodes exposing trust-list expiry health
- explicit operator guidance on what to rotate first
- optional tooling support for a single combined expiry-status command

## Target Diagnostics Model

Keep the existing device-certificate nodes and add trust-list expiry aggregates under `Device.Security`.

Recommended additions:

- `TrustedClientCertCount`
- `TrustedClientCertsWithKnownExpiryCount`
- `TrustedClientCertsExpiringSoonCount`
- `TrustedClientCertsExpiredCount`
- `EarliestTrustedClientCertExpiry`
- `DaysUntilEarliestTrustedClientCertExpiry`
- `TrustExpiryState`
- `SecurityExpirySummary`

Suggested semantics:

- `TrustExpiryState = valid` when all parsed trusted client certificates are comfortably valid
- `TrustExpiryState = warning` when at least one trusted client certificate expires within the warning window
- `TrustExpiryState = critical` when at least one trusted client certificate is expired, or when no healthy admin path remains in strict mode
- `SecurityExpirySummary` returns a compact operator-facing JSON summary suitable for tools and logs

## Warning Thresholds

Use the same default windows already used for server certificate warnings unless configuration later requires otherwise.

Recommended default policy:

- `<= 30 days`: warning
- `<= 7 days`: high warning
- `< 0 days` or expired now: critical

These thresholds should apply independently to:

- the active server identity certificate
- each parsed trusted client certificate in the trust list
- the aggregate trust-list health state

## Service Architecture

Add trust-list expiry analysis as a dedicated service instead of growing `security_service.c` further.

Recommended new module:

- `services/include/services/security_trust_expiry_service.h`
- `services/src/security_trust_expiry_service.c`

Responsibilities:

- parse the flash-backed trust list
- iterate all trusted certificates
- extract `notBefore` and `notAfter`
- compute counts and earliest expiry
- derive aggregate warning level and operator summary

Keep existing device certificate parsing in the current certificate metadata path unless refactoring later makes a shared cert-info service worthwhile.

## OPC UA Model Direction

Expose two layers of information.

### Layer 1: Always-Readable State

Under `Device.Security` add flat status variables for dashboards and polling clients.

Recommended variables:

- `TrustExpiryState`
- `TrustedClientCertsExpiringSoonCount`
- `TrustedClientCertsExpiredCount`
- `DaysUntilEarliestTrustedClientCertExpiry`
- `EarliestTrustedClientCertExpiry`
- `SecurityExpirySummary`

Current implementation note:

- trust-expiry state is currently surfaced through the existing maintenance JSON methods and Python tooling rather than new flat OPC UA variables.

### Layer 2: Events And Logs

Do not depend on OPC UA `Alarms & Conditions` for this stage.

Use a simpler combination instead:

- normal OPC UA events for subscribed clients
- `Device.Security` status variables for polling clients
- device logs and audit entries for offline diagnostics

Recommended event sources:

- `server_identity_expiry_warning`
- `server_identity_expiry_critical`
- `trusted_client_expiry_warning`
- `trusted_client_expiry_critical`
- `trust_store_expiry_aggregate_warning`
- `trust_store_expiry_aggregate_critical`

Recommended event payload fields:

- source type: `server_identity` or `trusted_client`
- certificate thumbprint when available
- `notAfter`
- `daysUntilExpiry`
- current `commissioningState`
- `activeTrustGeneration`
- short message for operators

## Runtime Behavior

### Server Certificate

At startup and during periodic security-health refresh:

- evaluate the active server certificate expiry window
- update `Device.Security` nodes
- emit transition events and logs when the warning state changes
- log state transitions only, not every poll cycle

### Trusted Client Certificates

After trust-list apply, on startup, and on periodic refresh:

- parse all trusted certificates in flash
- compute aggregate expiry state
- update trust expiry nodes
- raise warning or critical events only when the aggregate state changes or a new certificate enters a threshold window

### Connection-Time Checks

When a client connects with a certificate:

- inspect the presented client certificate validity window if available in the access-control path
- if it is near expiry, log and emit a maintenance/security event
- if it is expired, strict mode should continue to reject it through normal certificate validation

Connection-time checks are a supplement, not the main source of truth. The main source remains periodic analysis of the stored trust list.

## Time Validity Strategy

Expiry checks depend on wall-clock time, so the design must behave predictably when time is unknown.

Recommended behavior:

- if time has not been synchronized yet, expose `timeCheckPerformed = false` style metadata where applicable
- avoid raising false expiry warnings or events before time is known
- once time becomes known, recompute expiry states and emit transition events if thresholds are crossed
- log a distinct warning when certificate expiry cannot be evaluated because time is unavailable

## Tooling Direction

Extend the current Python tools rather than creating another maintenance client.

Recommended additions:

- `static/tools/stage_trust.py status` should optionally print trust expiry aggregates in a friendlier summary form
- `static/tools/rotate_identity.py metadata` should keep exposing server certificate expiry state
- optionally add `static/tools/security_expiry_status.py` only if combining trust and identity views into one tool is meaningfully cleaner

CLI output should make it obvious:

- whether the server certificate is healthy
- whether any trusted client certificates are near expiry
- what expires first
- whether the device is in `commissioning_open` or `strict`

## Operator Workflow

Expected operational workflow:

1. Poll `Device.Security` expiry nodes or subscribe to security events.
2. When a warning appears, inspect whether the risk is on the server identity or the trust list.
3. If the server certificate is expiring, stage identity rotation before the critical window.
4. If trusted client certificates are expiring, import an updated trust list before the oldest trusted admin certificate expires.
5. Verify post-change state and ensure warning indicators return to normal.

## Implementation Stages

### Stage 1: Trust Expiry Analysis Service

- add `security_trust_expiry_service`
- compute aggregate trust-list expiry counts and earliest expiry
- define a compact summary struct and helper APIs

### Stage 2: OPC UA Variables

- add new `Device.Security` trust expiry variables
- wire them to the new service
- expose a summarized JSON string for tools

### Stage 3: Events Integration

- extend expiry state evaluation to include aggregate trust-list expiry state
- emit discrete maintenance/security events on threshold transitions
- avoid duplicate event spam by tracking previous state

### Stage 4: Connection-Time Warnings

- inspect presented client certificate validity where the access-control path already has certificate context
- emit warnings for near-expiry client certificates that still authenticate successfully

### Stage 5: Tooling And Verification

- improve CLI output in the existing Python tools
- add operator verification steps and known-failure scenarios to docs

## Verification Plan

Verify the following scenarios on hardware:

- server certificate far from expiry reports `valid`
- server certificate in warning window reports warning severity
- server certificate expired reports critical severity
- trust list with all healthy certificates reports `TrustExpiryState = valid`
- trust list with one near-expiry certificate reports warning and increments `TrustedClientCertsExpiringSoonCount`
- trust list with one expired certificate reports critical and increments `TrustedClientCertsExpiredCount`
- time unavailable suppresses false expiry warnings and marks the check as unknown or not performed
- importing an updated trust list clears prior trust expiry warnings when appropriate
- connecting with a near-expiry client certificate emits a connection-time warning while the session still succeeds
- connecting before time synchronization does not emit false client-certificate expiry failures and reports the state as unknown

## Deliverables

- new trust expiry analysis service
- new `Device.Security` trust expiry nodes
- security events and status-node updates for expiry transitions
- updated Python tooling for readable expiry inspection
- verification notes for field testing

## Success Criteria

This stage is complete when:

- operators can see both server and trusted-client expiry risk remotely
- expiring certificates are visible before they break `strict` mode access
- trust-list updates and identity rotation can be scheduled from warning state instead of outage state
- the device emits actionable security lifecycle signals without requiring private key export or manual certificate parsing

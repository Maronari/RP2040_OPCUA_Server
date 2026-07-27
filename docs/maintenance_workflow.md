# Maintenance workflow

Firmware `0.3.2` exposes a volatile, service-owned AMB maintenance workflow.
Maintenance state is reset when the device restarts; persistence and historical
event access are not part of this release.

## State model

The initial state is `Finished`, allowing the first planned activity to use the
standard `Finished -> Planned` transition. The only supported transitions are:

| Method | Source | Result | Transition event |
| --- | --- | --- | --- |
| `PlanMaintenance` | `Finished` | `Planned` | Exactly one |
| `StartMaintenance` | `Planned` | `Executing` | Exactly one |
| `FinishMaintenance` | `Executing` | `Finished` | Exactly one |

`RescheduleMaintenance` updates an activity in `Planned` without changing the
state and emits no maintenance transition event. Rejected calls do not change
state and do not emit a transition event.

## Methods and authorization

All methods are components of `ns=1;s=Device.Maintenance`. They require a
secure authenticated session with the `service` or `admin` role. `viewer` and
`operator` receive `BadUserAccessDenied`.

| Method NodeId | Inputs | Output |
| --- | --- | --- |
| `Device.Maintenance.PlanMaintenance` | `OperationId: String`, `PlannedDate: DateTime`, `EstimatedDowntime: Duration`, `MaintenanceMethod: Int32`, `MaintenanceSupplier: String`, `QualificationOfPersonnel: String` | `Success: Boolean` |
| `Device.Maintenance.RescheduleMaintenance` | `PlannedDate: DateTime`, `EstimatedDowntime: Duration`, `MaintenanceMethod: Int32`, `MaintenanceSupplier: String`, `QualificationOfPersonnel: String` | `Success: Boolean` |
| `Device.Maintenance.StartMaintenance` | none | `Success: Boolean` |
| `Device.Maintenance.FinishMaintenance` | `PartsOfAssetServiced: String[]`, `PartsOfAssetReplaced: String[]`, `ConfigurationChanged: Boolean`, `Result: String` | `Success: Boolean` |

`MaintenanceMethod` accepts `Local=0` and `Remote=1`. Duration is expressed in
milliseconds and is limited to 30 days.

## Resource limits

Capacities below include the terminating NUL in native storage:

| Field | Limit |
| --- | --- |
| Operation ID, supplier, personnel, each part | 47 UTF-8 bytes |
| Actor | 31 UTF-8 bytes |
| Result | 95 UTF-8 bytes |
| Serviced parts | 4 elements |
| Replaced parts | 4 elements |

The service stores one logical current activity using two fixed snapshot slots
and one bounded pending transaction. It uses no Flash, hardware driver,
persistence journal, or unbounded allocation.

## OPC UA model

The existing condition remains
`ns=1;s=Device.DeviceHealthAlarms.MaintenanceRequired`. Its state text and
`CurrentState.Id` are synchronized from one service transaction using standard
AMB state NodeIds `5006`, `5007`, and `5008`.

The following read-only instance properties are exposed below the condition:

- `PlannedDate`
- `EstimatedDowntime`
- `MaintenanceMethod`
- `MaintenanceSupplier`
- `QualificationOfPersonnel`
- `PartsOfAssetReplaced`
- `PartsOfAssetServiced`
- `ConfigurationChanged`

Supplier, qualification, and part values use AMB `NameNodeIdDataType`
(`ns=<AMB>;i=3003`). Existing NodeIds and BrowseNames remain unchanged; new
properties use the condition NodeId followed by `.<BrowseName>`.

Invalid argument shape or type returns `BadInvalidArgument`; values outside
documented bounds return `BadOutOfRange`; illegal transitions return
`BadStateNotActive`; authorization and secure-channel failures return
`BadUserAccessDenied` and `BadSecurityModeInsufficient`, respectively.

## Atomic publication and event recovery

Prepared values remain in inactive service storage. The only commit point is
the active snapshot-index and revision swap; OPC UA data-source callbacks read
only the committed snapshot. Atomicity applies to one callback value, not to an
entire multi-node Read request.

Transition event fields are prepared before commit and submitted through the
public open62541 event API only after commit. A successful API return is
successful firmware publication. If that API reports failure, the committed
transition is retained, an `event_publication_fault` diagnostic records its
revision and operation, and subsequent state-changing maintenance Methods
return `BadResourceUnavailable` until volatile reinitialization or a
deterministic event health check. Delivery failures hidden inside open62541's
internal subscriber fan-out are platform limitations outside the firmware
correctness boundary.

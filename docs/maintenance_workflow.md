# Maintenance workflow

Firmware `0.3.3` exposes a bounded, service-owned AMB maintenance workflow.
The live state machine is unchanged from `v0.3.2`, but successful workflow
mutations, bounded logical export, and admin reset are now backed by the
maintenance journal described in [maintenance_journal.md](maintenance_journal.md).
Historical Event Access remains out of scope.

## State model

The initial state is `Finished`, allowing the first planned activity to use the
standard `Finished -> Planned` transition. Recovery restores the newest
unambiguous committed snapshot before the OPC UA maintenance model is exposed.
The only supported transitions are:

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
secure authenticated session. `viewer` and `operator` receive
`BadUserAccessDenied`.

| Method NodeId | Inputs | Output |
| --- | --- | --- |
| `Device.Maintenance.PlanMaintenance` | `OperationId: String`, `PlannedDate: DateTime`, `EstimatedDowntime: Duration`, `MaintenanceMethod: Int32`, `MaintenanceSupplier: String`, `QualificationOfPersonnel: String` | `Success: Boolean` |
| `Device.Maintenance.RescheduleMaintenance` | `PlannedDate: DateTime`, `EstimatedDowntime: Duration`, `MaintenanceMethod: Int32`, `MaintenanceSupplier: String`, `QualificationOfPersonnel: String` | `Success: Boolean` |
| `Device.Maintenance.StartMaintenance` | none | `Success: Boolean` |
| `Device.Maintenance.FinishMaintenance` | `PartsOfAssetServiced: String[]`, `PartsOfAssetReplaced: String[]`, `ConfigurationChanged: Boolean`, `Result: String` | `Success: Boolean` |
| `Device.Maintenance.ExportMaintenanceJournal` | `ContinuationToken: String` | `Result: String` |
| `Device.Maintenance.ResetMaintenanceJournal` | none | `Success: Boolean` |

Roles:

- `PlanMaintenance`, `RescheduleMaintenance`, `StartMaintenance`,
  `FinishMaintenance`, and `ExportMaintenanceJournal` require `service` or
  `admin`.
- `ResetMaintenanceJournal` is admin-only.

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
and one bounded pending transaction. `v0.3.3` adds one bounded Flash journal
and one bounded export scratch buffer in PSRAM. There is no unbounded
allocation.

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

The maintenance root also exposes:

- `ns=1;s=Device.Maintenance.ExportMaintenanceJournal`
- `ns=1;s=Device.Maintenance.ResetMaintenanceJournal`

The existing device diagnostics remain available, including:

- `ns=1;s=Device.Diagnostics.PSRAM.Ready`
- `ns=1;s=Device.Diagnostics.PSRAM.TotalBytes`
- `ns=1;s=Device.Diagnostics.PSRAM.UsedBytes`
- `ns=1;s=Device.Diagnostics.PSRAM.FreeBytes`
- `ns=1;s=Device.Diagnostics.PSRAM.LargestFreeBlockBytes`
- `ns=1;s=Device.Diagnostics.PSRAM.AllocatorMode`

Invalid argument shape or type returns `BadInvalidArgument`; values outside
documented bounds return `BadOutOfRange`; illegal transitions return
`BadStateNotActive`; authorization and secure-channel failures return
`BadUserAccessDenied` and `BadSecurityModeInsufficient`, respectively.
Invalid, expired, or reset-invalidated maintenance export tokens return
`BadContinuationPointInvalid`; continuation exhaustion returns
`BadNoContinuationPoints`.

## Persistence, export, and reset

Every successful workflow mutation appends one logical journal record and only
becomes public after the persisted record is classified `COMMITTED`. A reset
creates a new logical epoch, preserves the workflow snapshot and revision, and
invalidates active export continuations.

`ExportMaintenanceJournal` returns validated logical records only. A page
contains at most four records and the returned JSON includes:

- `records`
- `continuationToken`
- `more`

Each record includes epoch, sequence, operation, transition, state, revision,
timestamps, bounded text fields, `configurationChanged`, and bounded part
lists. Raw Flash addresses and slot bytes are never exported.

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
return `BadResourceUnavailable` until reconciliation. Recovery restores state
and condition semantics silently; it does not replay transition events or audit
events. Delivery failures hidden inside open62541's internal subscriber fan-out
are platform limitations outside the firmware correctness boundary.

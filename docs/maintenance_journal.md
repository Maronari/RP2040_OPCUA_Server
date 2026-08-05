# Maintenance journal

Firmware `0.3.3` persists the AMB maintenance workflow in a bounded Flash
journal and restores one coherent snapshot before the OPC UA maintenance model
is exposed. The journal stores logical maintenance records only. Clients never
see raw Flash offsets, sector numbers, or partially written state.

## Physical layout

The authoritative journal region is `0xf78000+0x8000` on the
`waveshare_core2350b_16mb_psram8mb` reference target. It contains eight
4 KiB sectors. Each sector contains:

- one immutable 256-byte header page;
- three 1024-byte record slots;
- one erased 768-byte tail that remains unused in schema 1.

This yields 24 physical record slots. The backend retains at most 15 logical
records after reclamation and requires at least two erased sectors before it
initializes a successor sector or logical reset epoch.

## Record schema

Schema 1 is storage-independent and byte-stable. It never serializes native
struct layouts.

- integer fields are little-endian exact-width unsigned or two's-complement;
- `estimated_downtime_ms` is IEEE-754 binary64;
- booleans are exactly `0x00` or `0x01`;
- strings are `u8 length + UTF-8 bytes` with no stored NUL terminator;
- the encoded body is fixed at 734 bytes maximum;
- each slot is `768` body bytes plus one `256`-byte commit page;
- CRC is CRC-32/ISO-HDLC over the encoded bytes before the stored CRC field.

Schema 1 stores:

- epoch and sequence;
- operation and transition;
- command timestamp;
- snapshot state, revision, committed timestamp, and `has_activity`;
- planned date, downtime, method, `configuration_changed`;
- operation id, supplier, personnel, actor, result;
- serviced and replaced part lists with explicit counts.

## Post-state and commit classification

The first three 256-byte pages of a slot are programmed and then read back
before the commit page is attempted. The commit page is programmed once and is
then classified from persistent post-state:

- `COMMITTED`: the commit page is readable and exact, and the full record body,
  CRC, and lineage decode correctly;
- `NOT_COMMITTED`: the page is erased or readable but not exact;
- `INDETERMINATE`: the page cannot be read or classified.

The exact commit page is the only durability point. A readable non-exact page
never becomes visible. An indeterminate page blocks later mutations until the
next bounded recovery or reconciliation classifies the persisted state.

## Recovery

Recovery completes before the maintenance condition or methods are exposed.
It scans all eight headers and all three slots in each committed header. It
never stops on the first bad slot because interrupted writes can leave holes.

Recovery outcomes are:

- `EMPTY`: the region is erased and the service restores `Finished`,
  revision `1`;
- `OK`: one newest supported snapshot is selected unambiguously;
- `DEGRADED`: an older corruption exists, but a newer supported snapshot is
  still unambiguous;
- `INDETERMINATE`: a commit page cannot be read or classified;
- `FATAL`: non-erased media contains no unambiguous supported snapshot;
- `UNSUPPORTED`: a newer exact-committed record uses an unknown schema.

`INDETERMINATE`, `FATAL`, and `UNSUPPORTED` preserve any last unambiguous read
snapshot but block mutation and logical reset.

## Retention and wear assumptions

The backend retains at most 15 logical records. A sector can be erased only
when it contains neither the authoritative snapshot, nor a retained record,
nor an unknown record. Reclamation is whole-sector only and is separately
read-verified.

Operational assumptions enforced by the firmware:

- two erased sectors must exist before a successor is created;
- record slots are append-only and a commit page is never reprogrammed;
- header pages are written once after erase;
- reset creates a successor epoch with sequence `1` and preserves workflow
  state and revision;
- sequence, epoch, and generation refuse reserved values instead of silently
  wrapping.

The service tracks erase activity internally through backend diagnostics,
including total erase count and per-sector erase counters, for hardware and CI
evidence. These counters are not currently exported as public OPC UA nodes.

## Public diagnostics and resource impact

The journal adds bounded native diagnostics through
`maintenance_service_journal_diagnostics_t`:

- `enabled`;
- `mutation_blocked`;
- `recovery`;
- `last_backend_result`;
- `corrupt_record_count`;
- `unknown_record_count`;
- `retained_record_count`;
- `erased_sector_count`.

The public OPC UA model does not add dedicated journal counter nodes in
`v0.3.3`. The build keeps the existing device diagnostics, including
`Device.Diagnostics.PSRAM.*`, and the journal export path now allocates its
24576-byte JSON scratch buffer from PSRAM so the reference Debug build fits the
required 360 KiB FreeRTOS heap.

## Export and reset contract

Public methods:

- `ns=1;s=Device.Maintenance.ExportMaintenanceJournal`
  - role: `service` or `admin`
  - input: `ContinuationToken: String`
  - output: `Result: String`
  - empty input begins an export; non-empty input resumes an export
  - one page contains at most four logical records
  - at most four continuations can exist simultaneously
  - tokens are 8 hex characters and expire after 30 seconds
  - invalid, expired, reclaimed, or reset-invalidated tokens return
    `BadContinuationPointInvalid`
  - continuation exhaustion returns `BadNoContinuationPoints`

- `ns=1;s=Device.Maintenance.ResetMaintenanceJournal`
  - role: `admin`
  - input: none
  - output: `Success: Boolean`
  - preserves workflow state and revision
  - invalidates all active export continuations
  - emits a live reset audit only when a new logical reset record is committed
    and the maintenance condition is resynchronized successfully

The export JSON schema is stable and logical. Each record contains:

- `epoch`
- `sequence`
- `operation`
- `transition`
- `state`
- `revision`
- `committedAt`
- `plannedDate`
- `estimatedDowntime`
- `method`
- `configurationChanged`
- `operationId`
- `supplier`
- `personnel`
- `actor`
- `result`
- `partsServicedCount`
- `partsServiced`
- `partsReplacedCount`
- `partsReplaced`

The outer payload contains `records`, `continuationToken`, and `more`.

## Automated OPC UA verification

Use the pinned client runtime so Method calls are sent against the maintenance
object with the stable Method NodeId, rather than relying on asyncua's
method-node shortcut:

```powershell
py -3.13 -m pip install --user asyncua==2.0.1
py -3.13 -B static/tools/verify_maintenance_workflow.py opc.tcp://<device>:4840 --viewer-username <viewer> --operator-username <operator> --service-username <service> --admin-username <admin>
```

The verifier reports and fails before connection if it is not running on
Python 3.13 with `asyncua==2.0.1`. Username authentication uses
`Basic256Sha256/SignAndEncrypt` by default. Pass `--security-mode Sign` only
when that secure endpoint is deliberately configured. `--no-auto-client-cert`
requires both `--client-cert` and `--client-key`; otherwise the common client
tooling creates a temporary client certificate.

Method-call arity follows the canonical OPC UA Call contract:
`BadArgumentsMissing` for omitted required arguments and
`BadTooManyArguments` for extras. Supplied arguments that violate project
constraints use `BadInvalidArgument` (or a per-argument result when the stack
reports one). A no-input Method may omit `InputArguments` or expose an empty
`Argument[]`.

For v0.3.2 compatibility, Viewer and Operator sessions observe
`UserExecutable=false` on maintenance workflow Methods. Calls from those roles
therefore return `BadNotExecutable`; Service and Admin authorization remains
enforced by the adapter and access-control configuration.

## Power-loss assumptions

`v0.3.3` does not assume page-program atomicity. Its guarantee is narrower and
explicit:

- if power is lost before the exact commit page is physically programmed, the
  predecessor snapshot remains authoritative;
- if power is lost after exact commit-page programming, recovery can restore
  the newer snapshot even if the interrupted method did not return `Good`;
- a readable non-exact commit page is ignored;
- an unreadable or unclassifiable commit page leaves the journal in
  `INDETERMINATE` recovery and blocks mutation/reset until reconciliation.

Normal live execution switches the service snapshot only after persistent
`COMMITTED` classification, so a successful method result is reboot
recoverable.

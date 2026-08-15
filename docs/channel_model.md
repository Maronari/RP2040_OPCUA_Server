# Generic OPC UA channel model

Firmware v0.4.2 creates the public channel topology once during OPC UA server
startup from caller-owned snapshots of the service channel registry. Services
remain the sole owners of metadata, runtime values, quality, source timestamps,
driver lifecycle, and writes. Structural configuration changes therefore use
the existing controlled reboot path; live topology regeneration is not part of
this release.

## Hierarchy and deterministic NodeIds

Every configured runtime channel is a component of the existing
`ns=1;s=Device.Channels` object. Generic objects and children use the namespace
URI `urn:maronari:rp2-industrial-opcua:channel-model`; clients resolve that URI
instead of assuming its runtime namespace index:

```text
nsu=urn:maronari:rp2-industrial-opcua:channel-model;s=Device.Channels.c_<encoded-id>
nsu=urn:maronari:rp2-industrial-opcua:channel-model;s=Device.Channels.c_<encoded-id>.<field>
```

`<encoded-id>` is the uppercase hexadecimal encoding of every byte in the
validated, nonempty channel ID. It is reversible and prevents collisions with
the `.` field separator or reserved suffixes. NodeIds never use registry slot,
registration order, generation, address, boot state, driver pointer, or display
name. The object BrowseName is the exact channel ID and its DisplayName is the
configured channel name.

| BrowseName | OPC UA type | Source |
| --- | --- | --- |
| `Value` | Exact channel built-in type | One coherent runtime snapshot |
| `Description` | `String` | Configured description |
| `ChannelKind` | `UInt32` | Channel kind enum |
| `DriverId` | `UInt32` | Stable configured driver ID |
| `SourceIndex` | `UInt32` | Stable configured source index |
| `SourceFlags` | `UInt32` | Stable configured source flags |
| `SamplePeriodMs` | `UInt32` | Runtime settings |
| `PublishPeriodMs` | `UInt32` | Runtime settings |
| `Historizing` | `Boolean` | Validated channel capability |
| `Writable` | `Boolean` | Validated channel capability |

When a unit is configured, `Value.EngineeringUnits` is an `EUInformation`
property. When both finite range bounds are configured, `Value.EURange` is a
`Range` property. The exact `Value` built-in type is `Float`, `Double`, `Int32`,
`UInt32`, `Int64`, `UInt64`, `Boolean`, or bounded `String`.

## Quality, timestamp, and writes

Each Value read uses one copied registry snapshot for value, StatusCode, and
source timestamp. Quality maps as follows:

| Service quality | OPC UA StatusCode |
| --- | --- |
| `GOOD` | `Good` |
| `UNCERTAIN` | `Uncertain` |
| `BAD` or unknown | `BadNotConnected` |

The service timestamp is UTC Unix time in microseconds and is converted exactly
to `UA_DateTime`. `UINT64_MAX` denotes an unavailable timestamp; the source
timestamp is then omitted and model/server time is not substituted. Unix epoch
zero remains valid.

A `Value` node is writable only when the validated channel capability is
writable and the session is authenticated, encrypted, and has Operator,
Service, or Admin role. The adapter accepts only the exact scalar built-in type
and a string of at most 63 data bytes. It calls the channel-manager write API;
the OPC UA layer never calls a driver vtable or inspects driver-private state.
Driver I/O occurs outside the registry critical section, and publication is
discarded if the channel generation changes while the operation is active.

## Compatibility and release boundary

The established `ns=1;s=Device.Channels.Temperature1` nodes remain available
for their documented deprecation period. Their value, quality, and source
timestamp are a stateless view of the same registry-owned sample as the generic
channel Value. v0.4.2 does not generalize the legacy temperature history or
alarm services; that work remains scheduled for v0.4.3.

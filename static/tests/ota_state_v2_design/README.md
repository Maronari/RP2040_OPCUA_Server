# OTA state v2 byte-codec prototype

This is a host-only, pure-C T0 prototype for the byte-level persistent-state
format frozen in [`docs/releases/v0.6.0-ota-design.md`](../../../docs/releases/v0.6.0-ota-design.md).
It is not linked into an application or bootloader target, has no Flash I/O,
and does not replace the existing v1 OTA state format.

`ota_state_v2_design.c` encodes and validates one complete 4 KiB sector using
explicit little-endian byte operations. A valid sector has:

- a 1,024-byte `RP2OST06` format-2 record at offset zero;
- erased (`0xff`) bytes from offset 1,024 through 3,839; and
- one complete 256-byte `RP2COM06` marker page at offset 3,840.

The record CRC covers exactly the 1,024 bytes with its four CRC bytes zeroed.
The marker duplicates the nonzero generation and record CRC, and its remaining
236 bytes must be zero. The decoder requires the exact 4,096-byte input length;
truncation and appended bytes are rejected.

The dual-sector selector chooses the highest committed generation. Generation
zero is invalid. `UINT64_MAX` remains a valid committed final state, but
`ota_v2_encode_successor()` refuses a successor from it, so generation never
rolls over. Equal generations are accepted only when all 4,096 bytes are
identical; different valid records are a corruption conflict. A successor is
only generation one without a predecessor or exactly predecessor-plus-one.
It intentionally has no erase/program operation: production code must erase
only the inactive sector and program the record before the commit page.

The frozen descriptor presence rules are exact:

| Phase | Presence bits |
|---|---:|
| `CONFIRMED` | confirmed (`1`) |
| `VERIFIED`, `BACKUP` | confirmed + candidate (`3`) |
| `INSTALL`, `LAUNCH_CONSUMED`, `RESTORE` | confirmed + candidate + backup (`7`) |

The confirmed manifest is mandatory in every phase. The confirmed version floor
must equal its manifest version. A candidate version must be at least that
floor; equality requires byte-identical signed prefixes (bytes `[0,256)`). A
backup is allowed only in the last three phases and must have that same signed
prefix as confirmed, while its signature bytes may differ.

This prototype structurally checks the fixed 320-byte manifest: magic,
lengths, layout and execution origin, ranges, canonical three 32-byte IDs,
zero reserved bytes, and P-256 scalar encodings (`1 <= r < n`,
`1 <= s <= n/2`). It does **not** authenticate keys, ECDSA, image bytes or
compatibility policy. Those are T1 native verifier responsibilities.
The reference SDK board name is represented by the separately frozen signed
alias `waveshare_core2350b_16m8m`; no automatic truncation is permitted.

Failure reasons are frozen as numeric values 0 through 15:
`NONE`, `MANIFEST_REQUIRED`, `MANIFEST_INVALID`, `UNKNOWN_KEY`,
`SIGNATURE_INVALID`, `IMAGE_HASH_MISMATCH`, `INCOMPATIBLE`,
`VERSION_REJECTED`, `STORAGE_ERROR`, `STATE_INVALID`, `BACKUP_INVALID`,
`CANDIDATE_INVALID`, `HEALTH_TIMEOUT`, `HEALTH_FAILED`, `ALLOCATION_FAILED`,
and `RECOVERY_REQUIRED`. Policy ID `1` is only a syntactic fixture for this
prototype; it does not claim that the production confirmation timing policy is
selected or measured.

Run the portable compiled suite with:

```powershell
py -3.12 static/tools/test_ota_state_v2_design.py
```

The suite compiles with `-std=c11 -Wall -Wextra -Werror` and uses explicit
runtime checks, so it remains meaningful under `NDEBUG`. It exercises every
phase, malformed/truncated/extended sectors, unknown fields, canonical scalar
and ID boundaries, successor and equal-generation conflict rules, and every byte
prefix of interrupted erase, record programming, marker-page programming and
NOR `old & new` programming. In each interrupted case it proves the untouched
old sector remains the selected committed state.

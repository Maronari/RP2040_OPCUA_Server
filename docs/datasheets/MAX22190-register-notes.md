# MAX22190 register/command reference (for v0.5.1 Phase 1, DI0-DI3)

Extracted from the official Analog Devices/Maxim Integrated datasheet
(`MAX22190.pdf`) for the single `MAX22190ATJ+` on
`RP2350_INDUSTRIAL_MVP_V1` (`HARDWARE_MVP.md` section covering `DI0..DI3`).
This is a working reference, not a copy of the datasheet.

## Board-fixed SPI mode: Mode 0 (24-bit, CRC, no daisy chain)

The SPI mode is a **hardware pin-strapping choice** (`M0`/`M1`), not
software-selectable. `HARDWARE_MVP.md:750-755` ties both `M0` and `M1` to
`GND_SYS`, which per Table 1 of the datasheet selects:

| M1 | M0 | Frame length | CRC | Daisy chain |
|---|---|---|---|---|
| 0 | 0 | 24-bit | Yes | No |

So this driver only ever needs to implement **Mode 0** — the CRC field is
mandatory on this board, not optional. `/LATCH` is tied permanently high
(`HARDWARE_MVP.md:776`, only one device on this board); falling `/CS` alone
latches a fresh input snapshot.

Only 4 of the chip's 8 physical inputs are wired: `DI0..DI3` map to the
chip's `IN1..IN4` pins, which the datasheet's own register map calls
`DI[3:0]` bits 0-3 (`IN1`→`DI0` ... `IN4`→`DI3`). `IN5..IN8`/`DI[7:4]` are
unconnected on this board revision.

`VDD24` is intentionally left unconnected on this board (MAX22190 is
powered from the board logic rail instead) — `HARDWARE_MVP.md:744`
explicitly requires the `24V`-supply-related fault-pin enables to stay
masked. This is **already the POR default**: `FAULT1EN` (address `0x24`)
resets to `0xC0` (only `CRCE`/`PORE` enabled; `24VLE`/`24VME` = 0) — no
extra firmware write is needed to satisfy this requirement as long as
`FAULT1EN` is never written with those bits set.

## 24-bit frame layout (Mode 0)

| Bits (MSB→LSB) | Field |
|---|---|
| 1 | R/W (`1`=write, `0`=read) |
| 7 | `A[6:0]` register address |
| 8 | `D[7:0]` write data (write) or don't-care (read) |
| 3 | reserved, `0 0 0` |
| 5 | `CRC[4:0]` |

**SDO response, every frame** (regardless of read/write): byte 1 (bits
1-8) is *always* the live `DI[7:0]` input status — "the first 8-bits
clocked out of SDO after CS is asserted are data bits showing the status
of inputs IN8-IN1", true for every transaction, not just an explicit `DI`
register read. Byte 2 (bits 9-16) is the addressed register's *current*
value on a read (Mode 0/1 decode and execute within the same frame — not
daisy-chain compatible, but this board doesn't daisy-chain); on a write, byte
2 is instead the `WB` (wire-break) status byte, not an echo of what was
written. Bits 17-24 are the response's own CRC (chip-generated).

**Consequence for reading channel state**: any well-formed frame's first
response byte already carries fresh `DI[7:0]` — a driver does not need a
dedicated "read DI register" round trip merely to sample inputs; issuing a
`DI`-register read command still works and is the simplest choice (keeps
the driver's request semantics uniform), but the *response's first byte* is
what actually matters for the live sample, not necessarily byte 2.

## CRC (5-bit, mandatory in Mode 0)

Polynomial: `P(x) = x^5 + x^4 + x^2 + x^0`. Computed over the **first 19
bits** of the 24-bit frame (R/W + address\[6:0\] + data\[7:0\] + 3 reserved
zero bits, MSB-first), then the 5-bit result is appended as bits 20-24.
The chip checks the CRC on every incoming frame (setting `FAULT1.CRC` on
mismatch and discarding the frame's commands) and generates its own CRC
for the outgoing response.

**Not independently verified against a datasheet worked example or
reference implementation — none is provided in this datasheet.** This
driver implements a standard bit-by-bit CRC-5 consistent with the stated
polynomial (XOR mask `0x15`, the polynomial's low 5 bits with the implicit
leading `x^5` term dropped, MSB-first over the 19 covered bits, zero
initial state) — the common construction for this class of CRC, but
**this exact bit-for-bit behavior has not been cross-checked against real
MAX22190 silicon or a reference test vector**, since none exists in this
project and no chip is physically available. This is the single largest
correctness risk in this driver and must be the first thing verified
during hardware bring-up (a mismatched CRC implementation would make every
real SPI transaction with the chip fail with a `FAULT1.CRC` error,
producing a very obvious, easy-to-diagnose failure mode rather than a
silent one).

## Register map (Table 4; only registers this driver touches)

| Register | Address | Type | POR default |
|---|---|---|---|
| `WB` (wire break status) | `0x00` | Clear-on-read | `0x00` |
| `DI` (digital input state) | `0x02` | Read-only | `0x00` |
| `FAULT1` | `0x04` | Mixed | `0x46` |
| `FLT1..FLT8` (per-channel filter) | `0x06`, `0x08`, `0x0A`, `0x0C`, ... (increments of 2) | R/W | `0x08` |
| `CFG` | `0x18` | R/W | `0x00` |
| `INEN` (input enable) | `0x1A` | R/W | `0xFF` |
| `FAULT2` | `0x1C` | Clear-on-read | `0x00` |
| `FAULT2EN` | `0x1E` | R/W | `0x00` |
| `GPO` | `0x22` | R/W | `0x00` |
| `FAULT1EN` | `0x24` | R/W | `0xC0` (`CRCE`+`PORE` only) |
| `NOP` | `0x26` | — | dummy register, no effect |

`DI[7:0]`: `DIx` = 1 means input `x` is driven high; `IN1` maps to `DI0`
… `IN4` maps to `DI3` (this board's 4 wired channels).

`FLTn` (`n`=1..4 for this board's `DI0..DI3`, address `0x06 + 2*n_zero_based`):
bit 4 `WBE` (wire-break detect enable, POR 0 — leave disabled, this board
has no `REFWB` wire-break sensing requirement documented), bit 3 `FBP`
(filter bypass), bits 2-0 `DELAY[2:0]` (glitch-filter time: `000`=50 µs …
`111`=20 ms). POR default `0x08` = `FBP=1` (filter bypassed), `DELAY=000`.
This driver leaves POR defaults unless a channel's `source_flags` requests
otherwise (see below).

`FAULT1` bits relevant to a basic driver: bit 7 `CRC` (last received frame
was corrupted — check this on every response to detect a communication
problem distinct from a `false` transport-level failure), bit 6 `POR`
(register map was reset — worth surfacing once at `start()`).

## Notes for the driver implementation

- `source_index` = 0-3 (`DI0..DI3`); `source_flags` = the target `FLTn`
  register's `FBP`/`DELAY[2:0]` bits (bits 3-0 of the byte, matching the
  register's own bit layout) — an optional per-channel filter override; POR
  default (`0x08`, filter bypassed) applies if a channel doesn't explicitly
  configure this via `source_flags`. Kept in `source_flags` rather than a
  runtime `configure()` payload, consistent with the v0.5.1 T1 finding.
- `start()`: read the `FAULT1` register once (a real, CRC-checked
  transaction) to confirm the chip communicates correctly; if `source_flags`
  requests a non-default filter setting, write the channel's `FLTn`
  register. Do **not** write `FAULT1EN`/`INEN` — the POR defaults already
  satisfy this board's masking requirement (see above) and enable all 8
  physical inputs (harmless for the 4 unwired ones).
- `stop()`: no-op — `MAX22190` is a read-only input device with nothing
  meaningful to disable per channel (matches `internal_temperature`/
  `adc_voltage`/`gpio_input`'s convention).
- `read()`: issue a `DI`-register read frame; the response's first byte is
  the live `DI[7:0]` status regardless. Extract bit `source_index`. Check
  the response's own CRC; a mismatch is treated as a driver-level
  communication failure (not a silent stale read).
- `self_test()`: write two distinct values to the channel's `FLTn`
  register (a real R/W register, unlike the read-only `DI`/clear-on-read
  `WB`), read each back, and apply the same three-way classification used
  by `ADS8688`/`AD5422` (matches echoed value / constant-wrong / varies-
  but-wrong).

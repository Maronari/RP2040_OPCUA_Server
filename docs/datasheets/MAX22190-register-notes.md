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
mandatory on this board, not optional.

"Mode 0" here is the chip's *interface* mode (Table 1), not the SPI clock
mode — but the clock mode happens to be 0 as well: the datasheet states the
serial interface runs "with CPHA = 0 and CPOL = 0" (SDI sampled on the
rising SCLK edge, SDO updated on the falling edge) and "SCLK must idle low
when CS is asserted". `BOARD_SPI` also carries the mode-1 `ADS8688`, so the
RP2350 SPI backend applies this device's mode on every transaction
(`drivers/src/rp2350_spi_bus.c`), not once at bus initialization. `/LATCH` is tied permanently high
(`HARDWARE_MVP.md:776`, only one device on this board); falling `/CS` alone
latches a fresh input snapshot.

Only 4 of the chip's 8 physical inputs are wired: `DI0..DI3` map to the
chip's `IN1..IN4` pins, which the datasheet's own register map calls
`DI[3:0]` bits 0-3 (`IN1`→`DI0` ... `IN4`→`DI3`). `IN5..IN8` are tied to
`GND_SYS` on this board revision, and `HARDWARE_MVP.md` section 3.3.1
requires firmware to change the POR default `INEN = 0xFF` to `INEN = 0x0F`
so only `IN1..IN4` stay enabled.

`VDD24` is intentionally left unconnected on this board (MAX22190 is
powered from the board logic rail instead), so the `24VL`/`24VM` monitors
are meaningless and `HARDWARE_MVP.md` section 3.3.2 requires their
`/FAULT` enables to stay masked. The frozen board value `FAULT1EN = 0xF8`
does that (`24VLE`/`24VME`/`WBGE` = 0) while enabling `CRCE`, `PORE`,
`FAULT2E`, `ALRMT2E` and `ALRMT1E` — see "Board initialization sequence"
below. (An earlier revision of these notes left `FAULT1EN`/`INEN` at their
POR defaults; that contradicted `HARDWARE_MVP.md` and is superseded.)

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

Bit layouts used by the board configuration (datasheet register map):

| Register | b7 | b6 | b5 | b4 | b3 | b2 | b1 | b0 |
|---|---|---|---|---|---|---|---|---|
| `CFG` | 0 | 0 | 0 | `24VF` | `CLRF` | 0 | 0 | `REFDI_SH_ENA` |
| `FAULT2EN` | 0 | 0 | `FAULT8CKE` | `OTSHDNE` | `RFDIOE` | `RFDISE` | `RFWBOE` | `RFWBSE` |
| `GPO` | `STK` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `FAULT1EN` | `CRCE` | `PORE` | `FAULT2E` | `ALRMT2E` | `ALRMT1E` | `24VLE` | `24VME` | `WBGE` |

`DI[7:0]`: `DIx` = 1 means input `x` is driven high; `IN1` maps to `DI0`
… `IN4` maps to `DI3` (this board's 4 wired channels).

`FLTn` (`n`=1..4 for this board's `DI0..DI3`, address `0x06 + 2*n_zero_based`):
bit 4 `WBE` (wire-break detect enable, POR 0 — `HARDWARE_MVP.md` section
3.3.5 defaults wire-break OFF on every channel), bit 3 `FBP` (filter
bypass), bits 2-0 `DELAY[2:0]` (glitch-filter time: `000`=50 µs …
`111`=20 ms). POR default `0x08` = `FBP=1` (filter bypassed), `DELAY=000`;
the frozen board default is `0x04` (`FBP=0`, `DELAY=100b` = 1.6 ms), which
chip startup writes to `FLT1..FLT4` before any channel's own
`source_flags` override its register.

`FAULT1` bits relevant to a basic driver: bit 7 `CRC` (last received frame
was corrupted; cleared by the next uncorrupted frame, not by a read), bit 6
`POR` (register map was reset). `POR` "is cleared only if the user writes
'0' to it. The other bits in this register are unaffected by the write
access" — so writing `FAULT1 = 0x00` clears `POR` and nothing else.

## Board initialization sequence (`HARDWARE_MVP.md` section 3.3.6)

Run once per chip, before any channel is trusted:

1. configure `GPIO24` as the `DI_READY_N` input (internal pulls off — the
   board has the `10k -> +3V3_DIG` pull-up);
2. wait until `DI_READY_N` reads low (bounded poll count);
3. SPI clock mode 0, SCLK idle low (the board binding);
4. CRC-protected `FAULT1` read — proves the chip answers;
5. `INEN = 0x0F`;
6. `FLT1..FLT4 = 0x04`;
7. `CFG = 0x01` (`REFDI_SH_ENA`);
8. read `WB` and `FAULT2` (both clear-on-read) to drop stale/POR state;
9. `FAULT2EN = 0x3C` (`FAULT8CKE`, `OTSHDNE`, `RFDIOE`, `RFDISE`; `REFWB`
   open/short stay masked while no channel has `WBE=1`);
10. `GPO = 0x80` (`STK`, sticky `/FAULT`);
11. `FAULT1EN = 0xF8`;
12. `FAULT1 = 0x00` to clear `POR`;
13. read back `INEN`, `FLT1..FLT4`, `CFG`, `FAULT2EN`, `GPO`, `FAULT1EN`,
    and `FAULT1` to confirm `POR` is clear.

Any failed transaction, mismatched readback or `POR` still set fails the
start with `HARDWARE_FAILURE` and is not latched — the next `start()` runs
the whole sequence again.

## Notes for the driver implementation

- `source_index` = 0-3 (`DI0..DI3`); `source_flags` = the target `FLTn`
  register's `FBP`/`DELAY[2:0]` bits (bits 3-0 of the byte, matching the
  register's own bit layout). Every channel start writes it, so
  `source_flags = 0x04` is the board default. Kept in `source_flags` rather
  than a runtime `configure()` payload, consistent with the v0.5.1 T1
  finding.
- `start()`: on the first channel since the bus was bound, run the board
  initialization sequence above; then write this channel's `FLTn` register
  from `source_flags` (a CRC-checked transaction, so every channel proves
  it reaches the chip).
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

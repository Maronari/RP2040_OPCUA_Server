# AD5422 register/command reference (for v0.5.1 Phase 1, AO0/AO1)

Extracted from the official Analog Devices datasheet (`AD5422.pdf`,
AD5412/AD5422, Rev. M) for the two `AD5422BREZ` analog-output channels on
`RP2350_INDUSTRIAL_MVP_V1` (`HARDWARE_MVP.md` section 3.2). This is a
working reference, not a copy of the datasheet.

This project only uses the AD5422 (16-bit) variant, not the AD5412
(12-bit); the two share one datasheet and interface, differing only in the
data-register bit width (Table 13/14).

## Board wiring is a 2-device daisy chain, not two independent chip-selects

Per `HARDWARE_MVP.md:343-365`: `AO0` and `AO1` share `SCLK` and `LATCH`
(there is only one shared `AO_LATCH`, no per-channel CS). `SDIN`/`SDO` are
chained: the controller's MOSI feeds `AO0`'s `SDIN`; `AO0`'s `SDO` feeds
`AO1`'s `SDIN`; **`AO1`'s `SDO` ends at a test point and is explicitly NOT
wired back to the shared `BOARD_SPI_MISO`** ("would create a push-pull
contention risk"). Neither DAC's `SDO` therefore reaches the MCU: `AO0`'s
readback would be clocked into `AO1`, and `AO1`'s goes nowhere. The board
has **no SPI readback path at all**, for either device, so the driver is
write-only and has no SPI self-test (an earlier revision of these notes
claimed `AO0` readback was observable; it is not). Runtime fault
diagnosis on this board is the shared open-drain `BOARD_FAULT_N` line
(`GPIO19`), which both DAC `FAULT` outputs and `MAX22190 /FAULT` pull
low.

Datasheet confirmation (`ad5412_5422.pdf` p.30-31, "Daisy-Chain
Operation"): daisy-chain mode is enabled per-device via the `DCEN` bit of
that device's own control register. Each device needs exactly 24 clock
pulses; a chain of *n* devices needs `24 × n` total clocks before `LATCH`
is taken high once. **The word transmitted *first* propagates furthest down
the chain** (to `AO1`); the word transmitted *second* stays in the nearest
device (`AO0`) — matches `HARDWARE_MVP.md`'s own framing note exactly.

## Per-device input shift register: 24 bits, address byte + 16-bit data

| Bits | Field |
|---|---|
| D23-D16 | 8-bit address byte |
| D15-D0 | 16-bit data word (full 16 bits for AD5422; AD5412 only uses D15-D4) |

Data is clocked MSB-first on `SCLK` rising edges ("Data is clocked in on
the rising edge of SCLK", p.30); in daisy-chain mode `SDO` is clocked out
on the previous falling edge. That is SPI clock mode 0. `BOARD_SPI` also
carries the mode-1 `ADS8688`, so the RP2350 SPI backend applies each
device's mode per transaction. The 24-bit word is unconditionally latched
into that device's *own* register on the `LATCH` rising edge — whatever 24
bits were most recently clocked into that device's position at that
instant. "Data continues to be clocked in irrespective of the state of
LATCH", so SCLK activity for the other devices on the bus shifts garbage
into both DACs; it is harmless because nothing latches until this driver's
own complete 48-bit frame ends with the `LATCH` rising edge.

## Address byte values (Table 9)

| Address (binary) | Function |
|---|---|
| `00000000` | NOP (no operation — this device's registers are untouched) |
| `00000001` | Data register (DAC output code) |
| `00000010` | Readback request (selects which register to clock out next; read-address in D1-D0, see below) |
| `01010101` | Control register |
| `01010110` | Reset register |

**Because each device in the chain applies its own 24-bit word
independently, updating only one channel (say `AO0`) while leaving the
other (`AO1`) completely untouched is just: send `NOP` (`0x000000`) for the
untouched device's position and the real command word for the target
device's position, in the same 48-bit / `LATCH` cycle.** No caching of the
other channel's last-commanded value is needed — this is simpler than a
first read of the daisy-chain wiring might suggest.

## Data register (address `0x01`)

16-bit DAC code, full `D15-D0` (AD5422). Power-on state is all registers
zeroed (both outputs tristated) until `OUTEN` is set in the control
register.

## Control register (address `0x55`)

| Bits | Field | Notes |
|---|---|---|
| D15 | `CLRSEL` | Clear-to value select (0 = zero-scale/0V, 1 = midscale unipolar / zero-scale bipolar) |
| D14 | `OVRRNG` | +10% voltage overrange enable — leave 0 |
| D13 | `REXT` | 1 = external current-set resistor. **The schematic requires it:** `RSET` -> 15.0 kΩ 0.1% -> `AGND`, and `HARDWARE_MVP.md` section 3.2.1 says "Firmware must select the external resistor (`REXT = 1`)". Datasheet Table 16: "it is recommended to only set REXT when also setting the OUTEN bit" (the alternative, `REXT` ahead of `OUTEN`, requires the range to change on the enabling write) |
| D12 | `OUTEN` | Output enable — must be 1 for the channel to drive anything |
| D11-D7 | `SR clock` (2 bits) + `SR step` (3 bits) | Digital slew-rate control — not used by this driver (left 0, slew disabled); *exact split within D11-D7 not independently re-derived here since both fields are always written 0* |
| D6 | `SREN` | Slew-rate enable — leave 0 |
| D5 | `DCEN` | Daisy-chain enable — **must be 1** on this board (shared bus wiring requires it) |
| D4-D3 | reserved | write 0 |
| D2-D0 | `R[2:0]` | Output range select, see table below |

## Output range select (`R[2:0]`, Table 17)

| R2 R1 R0 | Range | Gain (for voltage formula) |
|---|---|---|
| `000` | 0 V to 5 V | 1 |
| `001` | 0 V to 10 V | 2 |
| `010` | ±5 V | 2 |
| `011` | ±10 V | 4 |
| `100` | *reserved — not a valid range* | — |
| `101` | 4 mA to 20 mA | — |
| `110` | 0 mA to 20 mA | — |
| `111` | 0 mA to 24 mA | — |

**`RP2350_INDUSTRIAL_MVP_V1` frozen MVP scope** (`HARDWARE_MVP.md` section
3.2): only **0 V to 10 V** (`001`) and **4 mA to 20 mA** (`101`) are
targets for this board; bipolar ranges are physically impossible here
(`AVSS` -> `AGND`, no negative analog rail). The driver accepts exactly
these two codes and rejects every other one, reserved or not, so neither a
configuration nor the host-side descriptor can offer a range the board
does not have.

## Voltage/current scaling (internal reference `REFIN` = 5.0 V typical, `REFOUT` = 5 V ± 5 mV)

- Unipolar voltage: `VOUT = (D / 65536) * Gain * 5.0`
- Bipolar voltage: `VOUT = (D / 65536) * Gain * 5.0 - (Gain * 5.0 / 2)` —
  **straight/unsigned 16-bit code** (0-65535), *not* 2's-complement.
  ADS8688 bipolar ADC coding is also offset binary; the older notes claiming
  two's-complement ADS8688 data were incorrect.
- 0-20 mA: `IOUT = (D / 65536) * 20 mA`
- 0-24 mA: `IOUT = (D / 65536) * 24 mA`
- 4-20 mA: `IOUT = 4 mA + (D / 65536) * 16 mA`

`D` is the raw 16-bit DAC code (0-65535); `N` = 16.

## Reset register (address `0x56`)

Only `D0` is meaningful ("Reset"); write `0x0001` to force a power-on-reset
equivalent (all registers zeroed, both outputs tristated). The datasheet
asks for it after power-on when `DVCC`/`AVDD` ramp rates cannot be
guaranteed, followed by 40 µs "to ensure correct calibration and to allow
the internal reference to settle" — the case on this board, where `DVCC`
(`+3V3_DIG`) and `AVDD` (`+15V_AO`) come from independent rails.

## Chain startup on this board

Two datasheet rules collide on a daisy chain that starts from reset:
a standalone device needs *exactly* 24 clocks per `LATCH` cycle, while a
chain of two needs 48. After a reset both devices are standalone
(`DCEN = 0`), and `AO0`'s `SDO` does not forward anything to `AO1` until
`AO0.DCEN = 1`. The driver therefore brings the chain up in this order,
with `AO_ENABLE_REQ` held low (so `CLEAR` holds both outputs) throughout:

1. 24-bit `RESET` → `AO0`. Valid both after power-up (`AO0` standalone)
   and after an MCU-only restart (`AO0` still chained — its own shift
   register holds exactly this word either way).
2. 24-bit `CONTROL = DCEN` → `AO0`.
3. 48-bit `[RESET][NOP]` → `AO1` reset.
4. 48-bit `[CONTROL = DCEN][NOP]` → `AO1` chained.
5. `AO_ENABLE_REQ` high.

At least 40 µs (the driver uses 100 µs) follows every frame, covering the
reset settle and the 5 µs standalone `LATCH` high time (t3). Whatever
`AO1` latches before step 3 — its `SDIN` floats while `AO0` is standalone —
is wiped by its reset. Steps 3-4 rely on the datasheet's "the last 24
bits to be clocked in before the rising edge of LATCH is the data that is
latched" for a still-standalone `AO1` that sees 48 clocks; no sequence can
avoid that for the far device of a chain, so bring-up must confirm it.

## Status register (read-only, via readback request `0x02` + read-address `00`)

| Bit | Field |
|---|---|
| D2 | `IOUT fault` — fault detected on `IOUT` (open-loop / compliance issue) |
| D1 | `Slew active` — output currently slewing (not used by this driver) |
| D0 | `Over temp` — core temperature exceeded ~150°C |

The `FAULT` pin (open-drain, shared with `MAX22190`'s `/FAULT` on
`BOARD_FAULT_N` per `HARDWARE_MVP.md:384-392`) asserts for either fault
condition; the status register above is how a driver distinguishes which.

## Readback operation (Table 10, Table 12) — not usable on this board

1. Write `0x020001` (address `0x02` = readback request; data field's D1-D0
   = read-address `01` = data register; `00`=status, `10`=control) to the
   target device's position (`NOP` for the other device in the chain).
2. Follow with a `NOP` (`0x000000`) to the target device's position; the
   previously-addressed register's 16-bit value is clocked out on `SDO`
   during this second frame.

Documented for completeness only. With no `SDO` routed to
`BOARD_SPI_MISO` (see the wiring section), the MCU would sample a line no
DAC drives, so a readback-based self-test could never pass — and the
activation path treats a non-answering self-test as an absent device,
which would leave both outputs unusable. The driver therefore implements
no `self_test` and does not advertise the capability. A future board
revision that routes a readback path can reintroduce a non-invasive
CONTROL/STATUS readback (never diagnostic writes to DATA, which would
actuate a live output).

## Notes for the driver implementation

- Chip-select/board binding: **shared** `LATCH`+`SCLK`+`MOSI` across both
  channels (one shared `spi_bus_t`, matching `ads8688_driver.c`'s
  single-bus pattern), SPI clock mode 0. No `MISO` path from either DAC
  (see wiring note above): the driver never requests receive data.
- `source_index` = 0 (`AO0`, near/first-in-chain-from-SDO-perspective —
  transmitted *second*) or 1 (`AO1`, far — transmitted *first*).
  `source_flags` = `R[2:0]`, board-restricted to `0x1` (0-10 V) and `0x5`
  (4-20 mA).
- Every 48-bit transaction targets exactly one device with a real command
  and sends `NOP` (`0x000000`) for the other — no cross-channel state
  caching needed.
- `start()`: CONTROL = `DCEN | R` (output disabled, range selected), DATA =
  `0x0000` (zero scale: 0 V or 4 mA), then CONTROL =
  `OUTEN | REXT | DCEN | R` — `REXT` set together with `OUTEN` as Table 16
  recommends; `CLRSEL`, `OVRRNG`, slew all 0 (`HARDWARE_MVP.md` section
  3.2.3's safe-state invariants). A failed enabling write is followed by
  the disabled word again.
- `stop()`: CONTROL = `DCEN | R` — `OUTEN` and `REXT` cleared together,
  `DCEN` kept so `AO1` remains reachable through `AO0`.
- `write()`: write the data register with the commanded 16-bit code
  (computed from the OPC UA channel's engineering-unit value via the
  inverse of the scaling formulas above) to the target device, `NOP` to
  the other.
- No `self_test()` — see the readback section.

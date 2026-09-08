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
chained: the controller's `SDIN` feeds `AO0`; `AO0`'s `SDO` feeds `AO1`'s
`SDIN`; **`AO1`'s `SDO` is explicitly NOT wired back to the shared
`BOARD_SPI_MISO`** ("would create a push-pull contention risk"). This means
on the real board, only `AO0`'s readback is ever observable on the host
SPI's MISO line — `AO1` has no working hardware readback path at all. A
driver does not need to special-case this: the datasheet-documented
echo/readback self-test naturally reports `AO1` as producing no valid echo
once real hardware exists, for a genuine wiring reason rather than a driver
defect.

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

Data is clocked MSB-first on `SCLK` rising edges; the 24-bit word is
unconditionally latched into that device's *own* register on the `LATCH`
rising edge — whatever 24 bits were most recently clocked into that
device's position at that instant.

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
| D13 | `REXT` | 1 = external current-set resistor; leave 0 (use internal) unless the schematic requires it |
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

**`RP2350_INDUSTRIAL_MVP_V1` frozen MVP scope** (`HARDWARE_MVP.md:255-260`):
only **0 V to 10 V** and **4 mA to 20 mA** are electrically validated targets
for this board; bipolar ranges are out of scope (no negative analog rail).
A driver may still accept all documented non-reserved codes generically
(matching this project's existing pattern of driver-level generality plus a
separate, documented board-level electrical restriction), but board
integration/qualification must not claim the other ranges.

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
equivalent (all registers zeroed, both outputs tristated). Not used by
normal driver operation; potentially useful for a future safe-state
recovery path.

## Status register (read-only, via readback request `0x02` + read-address `00`)

| Bit | Field |
|---|---|
| D2 | `IOUT fault` — fault detected on `IOUT` (open-loop / compliance issue) |
| D1 | `Slew active` — output currently slewing (not used by this driver) |
| D0 | `Over temp` — core temperature exceeded ~150°C |

The `FAULT` pin (open-drain, shared with `MAX22190`'s `/FAULT` on
`BOARD_FAULT_N` per `HARDWARE_MVP.md:384-392`) asserts for either fault
condition; the status register above is how a driver distinguishes which.

## Readback operation (Table 10, Table 12)

1. Write `0x020001` (address `0x02` = readback request; data field's D1-D0
   = read-address `01` = data register; `00`=status, `10`=control) to the
   target device's position (`NOP` for the other device in the chain).
2. Follow with a `NOP` (`0x000000`) to the target device's position; the
   previously-addressed register's 16-bit value is clocked out on `SDO`
   during this second frame.

Self-test must not write diagnostic values to DATA: OUTEN may already be
enabled, and such probes would actuate the output. Read CONTROL and STATUS
instead, checking the configured range/DCEN/OUTEN and fault bits. Healthy
STATUS alone is zero and cannot prove presence; a valid nonzero CONTROL
readback is required. Transport/absent-device results remain distinct from
control/status mismatches. No restoration writes are necessary.

## Notes for the driver implementation

- Chip-select/board binding: **shared** `LATCH`+`SCLK`+`MOSI` across both
  channels (one shared `spi_bus_t`, matching `ads8688_driver.c`'s
  single-bus pattern); `MISO` is only meaningfully connected for `AO0` on
  this specific board revision (see wiring note above) — the driver itself
  does not need to encode this; it falls out of real hardware behavior.
- `source_index` = 0 (`AO0`, near/first-in-chain-from-SDO-perspective —
  transmitted *second*) or 1 (`AO1`, far — transmitted *first*).
  `source_flags` = `R[2:0]` (0,1,2,3,5,6,7 valid; 4 reserved/invalid).
- Every 48-bit transaction targets exactly one device with a real command
  and sends `NOP` (`0x000000`) for the other — no cross-channel state
  caching needed.
- `start()`: write the control register (`OUTEN=1`, `DCEN=1`, `R[2:0]`
  from `source_flags`, everything else 0) to the target device, `NOP` to
  the other.
- `write()`: write the data register with the commanded 16-bit code
  (computed from the OPC UA channel's engineering-unit value via the
  inverse of the scaling formulas above) to the target device, `NOP` to
  the other.
- `self_test()`: non-invasive CONTROL/STATUS readback described above.

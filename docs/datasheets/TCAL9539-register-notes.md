# TCAL9539 register/command reference (for v0.5.1 Phase 1, board-power-service)

Extracted from the official Texas Instruments datasheet (`TCAL9539.pdf`,
literature number SCPS241, July 2022) for the single `TCAL9539` internal
service I/O expander on `RP2350_INDUSTRIAL_MVP_V1`
(`HARDWARE_MVP.md` section covering `TCAL9539`/`TCA4307`/`TPS2553`). This
is a working reference, not a copy of the datasheet.

## I2C device address -- NOT confirmed against this board's schematic

The device address is `1110 A1 A0` (7 bits), where `A1`/`A0` are hardware
address-strap pins:

| A1 | A0 | Address (hex) |
|---|---|---|
| L | L | `0x74` |
| L | H | `0x75` |
| H | L | `0x76` |
| H | H | `0x77` |

`HARDWARE_MVP.md` documents the `TCAL9539`'s pin *functions* (`P00..P17`,
`/INT`, `/RESET`) in detail but does **not** document how `A1`/`A0` are
strapped on this board revision (consistent with that file's own note at
the component-table entry: "exact package/orderable suffix TBD"). This
driver defaults to `0x74` (`A1=A0=L`, the most common default strap) as a
**placeholder, not a confirmed hardware fact** -- the real address must be
read from the actual schematic/PCB before hardware bring-up; a wrong
address here fails loudly (every transaction NACKs) rather than silently.

## Register map (Table 8-3, only the base 8 registers this driver touches)

| Register | Address | Type | POR default |
|---|---|---|---|
| Input Port 0 | `0x00` | Read-only | reflects pins (`xxxx xxxx`) |
| Input Port 1 | `0x01` | Read-only | reflects pins (`xxxx xxxx`) |
| Output Port 0 | `0x02` | R/W | `0xFF` |
| Output Port 1 | `0x03` | R/W | `0xFF` |
| Polarity Inversion 0 | `0x04` | R/W | `0x00` |
| Polarity Inversion 1 | `0x05` | R/W | `0x00` |
| Configuration 0 | `0x06` | R/W | `0xFF` (all inputs) |
| Configuration 1 | `0x07` | R/W | `0xFF` (all inputs) |

Extended registers (`0x40`+: output drive strength, input latch,
pull-up/pull-down enable/selection) exist but are left at their power-on
defaults -- this board's design doesn't call for latching, drive-strength,
or pull configuration on any `TCAL9539` pin (`HARDWARE_MVP.md` section
covering pin assignment has no such requirement).

**Auto-increment across the register pair**: writing/reading Port-1
register N automatically continues into the paired Port-0 register (N-1)
on the next byte if the transaction continues without a new command byte
(e.g. reading Output Port 1 then continuing the same read yields Output
Port 0 next). This driver never relies on that -- every access explicitly
sets the command byte (register pointer) for the exact register it wants,
via `i2c_bus_read_register()`/`i2c_bus_write_register()`
(`drivers/i2c_transport.h`), for clarity and to avoid an off-by-one bug
class entirely.

## This board's `P00..P17` pin mapping (`HARDWARE_MVP.md`)

| Pin | Direction | Net | Meaning |
|---|---|---|---|
| P00 | OUT | `SENSOR_PWR_EN0` | Enable `TPS2553` sensor-power switch, port 0 (`I2C0`) |
| P01 | OUT | `SENSOR_PWR_EN1` | ... port 1 (`I2C1`) |
| P02 | OUT | `SENSOR_PWR_EN2` | ... port 2 (`SPI`) |
| P03 | OUT | `SENSOR_PWR_EN3` | ... port 3 (`UART`) |
| P04 | IN | `SENSOR_PWR_FAULT0_N` | `TPS2553` fault, port 0, active-low |
| P05 | IN | `SENSOR_PWR_FAULT1_N` | ... port 1 |
| P06 | IN | `SENSOR_PWR_FAULT2_N` | ... port 2 |
| P07 | IN | `SENSOR_PWR_FAULT3_N` | ... port 3 |
| P10 | OUT | `I2C0_ISOL_EN` | `TCA4307` #0 `EN` |
| P11 | IN | `I2C0_READY` | `TCA4307` #0 `READY` |
| P12 | OUT | `I2C1_ISOL_EN` | `TCA4307` #1 `EN` |
| P13 | IN | `I2C1_READY` | `TCA4307` #1 `READY` |
| P14 | IN | `DI_READY_N` | `MAX22190` `/READY`, active-low |
| P15-P17 | -- | RESERVED | leave as POR default (input, unread) |

`P0x` pins live in Input/Output/Configuration/Polarity register bit 0-7 of
the "0" register; `P1x` pins live in bit 0-7 of the "1" register (e.g.
`P10` = bit 0 of the "1" registers, `P14` = bit 4).

**Neither `TCA4307` nor `TPS2553` is a separately I2C-addressable device on
this board** -- both are controlled/read purely through these `TCAL9539`
GPIO pins (`HARDWARE_MVP.md`: "The four `TPS2553 EN` controls... moved to
`TCAL9539` outputs"; `TCA4307`'s `EN`/`READY` likewise routed through
`P10..P13`). This release therefore implements only a `TCAL9539` register
driver plus a `board_power_service.c` that interprets specific pins by
their board-assigned meaning -- no dedicated `TCA4307`/`TPS2553` driver
files exist or are needed.

`/INT` (`GPIO39`/`BOARD_IO_INT_N`) and `/RESET` (tied to `SYS_RESET_N`) are
both hardware-only signals on this board revision; `/RESET` needs no
firmware action at all (the supervisor resets the expander to its POR
state automatically), and `/INT` is not serviced by this release (no ISR
wiring) -- `board_power_service.c` polls `P04..P07`/`P11`/`P13`/`P14`
directly instead, deferring interrupt-driven servicing to a future pass if
poll latency proves insufficient at hardware bring-up.

## Notes for the driver implementation

- Configuration register bits: `0` = output, `1` = input (POR default
  `0xFF` = all inputs, matching the hardware-safe expectation that no pin
  drives anything until firmware explicitly configures it).
- `board_power_service_init()` must set Configuration 0 bits 0-3 to `0`
  (`SENSOR_PWR_EN0..3` as outputs) and bits 4-7 to `1` (`FAULT0..3_N` as
  inputs, matching POR already); Configuration 1 bit 0 and bit 2 to `0`
  (`I2C0_ISOL_EN`/`I2C1_ISOL_EN` as outputs), bits 1/3/4 to `1`
  (`I2C0_READY`/`I2C1_READY`/`DI_READY_N` as inputs, matching POR
  already); bits 5-7 (`P15..P17`, reserved) left at POR (`1`, input).
- Output Port 0/1 POR default is `0xFF` (all pins driven high before
  Configuration even runs) -- for `SENSOR_PWR_EN0..3` and
  `I2C0/1_ISOL_EN`, this driver must write the Output Port registers to
  the safe/disabled level (`0`, per `HARDWARE_MVP.md`'s "default `TCA4307
  EN` to external-bus-disconnected during reset" and the general
  hardware-safe-state convention already established for `AD5422`/
  `IPS2050`) **before** flipping the corresponding Configuration bits to
  output, so no pin is ever briefly driven high once it becomes an output
  (mirrors `gpio_output_channel_driver.c`'s "set level before enabling
  direction" ordering).
- Fault/ready reads are a single Input Port register read each (`P04..P07`
  = Input Port 0 bits 4-7; `P11`/`P13`/`P14` = Input Port 1 bits 1/3/4).

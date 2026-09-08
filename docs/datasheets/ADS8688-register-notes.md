# ADS8688 register/command reference (for v0.5.1 T2)

Extracted from the official TI datasheet (`ADS8688.pdf`, SBAS582C, JULY
2014–REVISED APRIL 2015) for `drivers/src/ads8688_driver.c`. This is a
working reference, not a copy of the datasheet — see the PDF for full
detail, timing diagrams, and electrical specifications.

Covers ADS8688 (8-channel) and ADS8684 (4-channel, shaded/unavailable bits
noted below); this project only targets the 8-channel ADS8688 per
`ROADMAP.md`.

## SPI frame basics

- Full-duplex, MSB-first, CPOL/CPHA per datasheet timing diagrams (mode 1:
  CPOL=0, CPHA=1 — verify against `Figure 1` before implementation).
- Every data frame starts on the falling edge of `CS`. Program-register
  read/write frames require a minimum of 24 SCLK cycles. A command-only
  frame may end after its first 16 clocks, but `MAN_Ch_n` must use a complete
  32-SCLK data frame to acquire and convert the selected channel; its result
  is clocked out during clocks 17-32. `CS` must stay low for the complete
  selected frame; an early rising edge enters `FRAME_ABORT` (`INVALID`) and
  subsequent reads return invalid data until a valid command is written.
- The device executes the command written in frame *N* starting at the
  `CS` rising edge that ends frame *N*; the result is read back during
  frame *N+1* (pipeline delay of one frame).

## Command register (16-bit, write-only, address-less)

| Command | Hex | Bits (B15..B8) | Effect |
|---|---|---|---|
| `NO_OP` (continued operation) | `0000h` | `0000 0000` | continue previous mode |
| `STDBY` | `8200h` | `1000 0010` | standby mode |
| `PWR_DN` | `8300h` | `1000 0011` | power-down mode (needs 15 ms to recover) |
| `RST` | `8500h` | `1000 0101` | reset program registers to default |
| `AUTO_RST` | `A000h` | `1010 0000` | auto-scan mode, sequence restarts |
| `MAN_Ch_0`..`MAN_Ch_7` | `C000h` + (`channel << 10`) i.e. `C000h, C400h, C800h, CC00h, D000h, D400h, D800h, DC00h` | `11cc c000` | manual single-channel select (channel 4-7 ADS8688-only) |
| `MAN_AUX` | `E000h` | `1110 0000` | select AUX input |

Lower byte (B7-B0) is always `00h` for command-register writes.

## Program register read/write framing (min 24 SCLK)

- `SDI` bits 15-9 = 7-bit register address (`ADDR[6:0]`); bit 8 = R/W
  (`1` = write, `0` = read); bits 7-0 = write data (write) or don't-care
  (read).
- Write: device echoes the written byte back on `SDO` during the next 8
  clocks (read-back verification opportunity).
- Read: `SDO` stays low through bit 8, then outputs the addressed
  register's 8-bit value MSB-first over the next 8 clocks.

## Program register map

| Register | Addr | Default | Bits |
|---|---|---|---|
| `AUTO_SEQ_EN` | `01h` | `FFh` | `CH7_EN..CH0_EN` — 1 = channel included in `AUTO_RST` sequence (default: all on) |
| Channel Power Down | `02h` | `00h` | `CH7_PD..CH0_PD` — 1 = channel analog front-end powered down (excluded from `AUTO_RST`; manual read of a powered-down channel returns invalid data) |
| Feature Select | `03h` | `00h` | `DEV[1:0]` (bits 7-6, daisy-chain device ID), bits 5-3 reserved (must be 0), `SDO[2:0]` (bits 2-0, output format, see below) |
| Channel *n* Input Range | `05h`+*n* (`05h`..`0Ch` for *n*=0..7) | `00h` | bits 3-0 = `Range_CHn[3:0]`; bits 7-4 must be 0 |
| Command Read-Back | `3Fh` (read-only) | `00h` | echoes the command-register word (B15-B8) executed in the *previous* data frame |

## Range select bits (`Range_CHn[3:0]`, addresses `05h`-`0Ch`)

| Bits | Analog input range |
|---|---|
| `0000` | ±2.5 × VREF (default) |
| `0001` | ±1.25 × VREF |
| `0010` | ±0.625 × VREF |
| `0101` | 0 to 2.5 × VREF |
| `0110` | 0 to 1.25 × VREF |

`VREF` = internal 4.096 V reference (default) or external via `REFIO`. So
concretely: ±10.24 V, ±5.12 V, ±2.56 V, 0–10.24 V, 0–5.12 V.

## SDO output format (`SDO[2:0]` in Feature Select register, `03h`)

Selects how much metadata rides alongside the 16-bit conversion result in
the final 16 clocks of a 32-clock `AUTO_RST`/`MAN_Ch_n` data frame. With the
default `000` format, all 16 result bits are conversion data; enabled
metadata occupies low-order result-frame bits as below:

| `SDO[2:0]` | Bits 8-5 | Bits 4-3 | Bits 2-0 |
|---|---|---|---|
| `000` | — | — | — |
| `001` | channel address | — | — |
| `010` | channel address | device address | — |
| `011` | channel address | device address | input range |

Channel address encoding (bits 8-5): `0000`=CH0 .. `0111`=CH7.

## Conversion coding

Unipolar ranges use straight binary (`0000h` = 0 V, `FFFFh` approaches full
scale). Bipolar ranges use **offset binary**, not two's complement:
`0000h` = negative full scale, `8000h` = 0 V, and `FFFFh` approaches positive
full scale. The driver therefore converts bipolar samples as
`(code - 32768) / 32768 * full_scale_volts` in a signed type wide enough to
represent the subtraction.

## Self-test / diagnostics — no dedicated status register

The ADS8688 has **no dedicated alarm/fault/diagnostic register**. The only
device-reported health signals are:
- `FRAME_ABORT`/`INVALID` state (malformed `CS` framing) — recoverable by
  writing a valid command.
- A powered-down or fully-powered-down-channel-set read returns "invalid
  data" (not flagged out-of-band — the datasheet does not define a specific
  invalid-data bit pattern to detect this).
- Command Read-Back (`3Fh`) lets a driver verify the *last executed*
  command word matches what was written — this is the only real
  "is the chip actually there and listening" signal without external
  wiring, and is the natural basis for `native_ads8688_self_test()`: write
  a command, then read back `3Fh` and confirm it matches (SPI transaction
  failure or an unresponsive/absent chip will not produce a matching
  echo). This mirrors `ROADMAP.md`'s required "supported but hardware
  unavailable" self-test classification for a chip that is not physically
  present on the reference board.

## Notes for `drivers/src/ads8688_driver.c` (T2)

- Chip select, SPI peripheral instance, and SCLK/MOSI/MISO pins are board
  binding concerns (see `docs/releases/v0.5.1.md`'s "Architecture and
  public-contract rules" — isolate these from the register/command logic
  above so a schematic correction only touches the binding layer).
- Driver-level resource claims: one `DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT`
  (exclusive, per channel's configured CS index) + one shared
  `DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL` (matches the existing
  `ADC_INPUT`/`ADC_PERIPHERAL` pairing pattern in `driver_catalog.c`).
- `Range_CHn[3:0]` is a natural fit for the channel's persisted
  `source_flags` (mirrors this project's existing native drivers' use of
  `source_flags` for per-channel mode bits) rather than a runtime
  `configure()` payload — consistent with the `native_gpio_output`
  T1 finding that every real `configure()` call site passes `NULL` and
  mode-like properties belong in `source_flags`, validated by the driver's
  `*_source_is_valid()`.

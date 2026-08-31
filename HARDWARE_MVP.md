# RP2350 Industrial OPC UA Node — Hardware MVP

> **Status:** living source of truth for Hardware Prototype v1 / firmware through `v1.0.0` unless deliberately revised.
>
> **Board profile:** `RP2350_INDUSTRIAL_MVP_V1`
>
> **Last hardware consolidation:** 2026-08-31

This document describes the current hardware architecture, frozen component choices, power tree, MCU pin allocation, completed schematic sheets, PCB/mechanical constraints, status indication, firmware-facing hardware contract, and the remaining work before production PCB release.

The prototype is a single monolithic RP2350B industrial controller intended to demonstrate the complete path from real industrial/sensor signals through hardware drivers and firmware channel runtime to the OPC UA information model.

The MVP is not intended to be a certified commercial PLC. Product-level features such as modular I/O, redundant Ethernet, two-port switching, full per-channel isolation, and final certification remain outside this revision.

---

## 1. Fixed architecture

Hardware Prototype v1 uses one main PCB and has no user-replaceable I/O modules, internal expansion slots, module EEPROMs, or board-to-board I/O expansion connectors.

All native hardware drivers are statically compiled into firmware. The configurator selects and parameterizes installed drivers by stable `driver_id`; it does not upload executable driver code.

The hardware profile identifies a fixed board revision, not a set of installed modules.

```text
RP2350_INDUSTRIAL_MVP_V1
```

Main functional blocks:

- MCU: `RP2350B`, stepping A4;
- external QSPI Flash: 16 MB;
- external PSRAM: 8 MB;
- one W5500 10/100 Ethernet interface;
- USB-C service/recovery;
- one isolated half-duplex RS-485 interface;
- 4 × analog input;
- 2 × analog output;
- 4 × industrial digital input;
- 2 × protected high-side digital output;
- 4 × universal/counter/frequency/encoder input, with RP2350 ADC capability reserved on the same GPIOs;
- 2 × I²C sensor ports with stuck-bus isolation/recovery;
- 1 × SPI sensor port;
- 1 × UART sensor port;
- 4 × independently switched/current-limited sensor power channels;
- one 16-bit internal I²C GPIO expander for slow control/diagnostic signals;
- RP2350 internal ADC monitoring of selected board supply rails;
- external supervisor/watchdog;
- hardware safe-state gating for AO and DO;
- status LEDs;
- 9–36 V DC input power stage.

---

## 2. Frozen key BOM

| Function | Component | Notes |
|---|---|---|
| MCU | `RP2350B` | QFN-80, stepping A4 |
| QSPI Flash | `W25Q128JVBIQ` | 16 MB |
| PSRAM | `APS6404L-3SQR-SN` | 8 MB |
| Ethernet controller | `W5500` | 10/100 Ethernet, SPI |
| Ethernet crystal | `445I23D25M00000` | 25 MHz, CL=18 pF, SMD 5032, 2-pin |
| RJ45 MagJack | `TMJD4012BENL` | vertical/top-entry, 10/100, integrated magnetics + LEDs |
| USB-C | `USB4145-03-0230-C` | vertical/top-entry, USB 2.0 |
| USB ESD | `TPD2EUSB30ADRTR` | shunt ESD protection |
| Isolated RS-485 | `ISOW1432DFMR` | integrated isolated power, half-duplex use |
| RS-485 TVS | `SM712` | field-side protection |
| Input eFuse | `TPS26600RHFT` | protected 9–36 V input path |
| Main buck | `LM76003RNPR` | generates `+5V_PRE` |
| Digital buck | `TPS62130RGTR` | generates `+3V3_DIG` |
| Analog 3.3 V LDO | `TPS7A2033PDBVR` | generates `+3V3_ANALOG` |
| ADC 5 V LDO | `TPS7A2050PDBVR` | generates `+5V_ANALOG` |
| AO supply | `LT8362EMSE#PBF` | SEPIC to `+15V_AO` |
| ADC | `ADS8688IDBTR` | external industrial ADC |
| Analog outputs | 2 × `AD5422BREZ` | unified 0–10 V / 4–20 mA output |
| AO sense buffer | `OPA2991IDR` | dual RRIO high-voltage buffer, one channel per AO |
| AO CLEAR inverter | `SN74LVC2G04DBVR` | `AOx_CLEAR = !AO_SAFE_EN` |
| AO rail clamp | 2 × `BAS70-04LT1G` | one dual Schottky SOT-23 per AO channel |
| AO field TVS | 2 × `SMBJ15A` | one unidirectional TVS per AO channel |
| AO terminal blocks | 2 × `DG350-3.5-02P-14` | `OUT/RET`, 3.5 mm pitch |
| Digital inputs | `MAX22190ATJ+` | 4 × IEC 61131-2 Type 1/Type 3 DI; VDD/VL from +3V3_DIG, VDD24 NC |
| DI series resistors | 4 × `CRCW25121K50FKEGHP` | 1.5 kΩ, 1%, 2512 pulse-withstanding, one per physical DI |
| DI terminal block | `DG350-3.5-06P-14` | DI0..DI3 + 2 × DI_COM, 3.5 mm pitch |
| Digital outputs | `IPS2050HTR` | protected high-side outputs |
| Universal inputs | `TLV3214PWR` | current comparator-based UIC frontend; analog-path revision still required if analog UIC mode is retained |
| Supervisor/watchdog | `TPS3851H33EDRBR` | reset + watchdog |
| Safe-state logic | `SN74LVC2G08DCTR` | AO/DO enable gating |
| Sensor power switches | 4 × `TPS2553DBVR` | current-limited switched sensor power |
| Internal I/O expander | `TCAL9539` | 16-bit I²C GPIO expander, `/INT` + `/RESET`; exact package/orderable suffix TBD |
| External I²C recovery | 2 × `TCA4307` | one per external I²C port; hot-swap buffer and stuck-bus recovery; exact package/orderable suffix TBD |

### 2.1 LED support components added to the PCB concept

The current PCB concept includes discrete status LEDs and small transistor/buffer stages so diagnostic nets are not directly burdened by LED current.

Current implementation concept:

- `2N7002`-class N-MOSFET for active-high driven board-status LEDs;
- `BSS84`-class P-MOSFET / suitable active-low buffer stage where needed;
- 0603 LEDs;
- dedicated current-limiting resistors for ordinary board-status LEDs.

The four `DI0..DI3` state LEDs are a deliberate exception: they are driven from the MAX22190 `LED1..LED4` energyless current outputs and therefore do **not** use ordinary series current-limiting resistors. These LEDs are hardware input-state indicators, not firmware-controlled status LEDs.

The remaining discrete board-status LED driver details must be captured and audited in the final schematic before PCB release.

---

## 3. Required I/O set

### 3.1 Analog inputs

Provide **4 analog input channels**: `AI0..AI3`.

Target ranges:

- `0–10 V`;
- `0–5 V` where supported by the frontend;
- `0–20 mA`;
- `4–20 mA`.

The channels use the external `ADS8688IDBTR`; direct industrial signals must not be connected to the RP2350 internal ADC.

Minimum firmware-visible properties:

- configured electrical range;
- raw value;
- scaled engineering value;
- engineering unit;
- quality/status;
- calibration coefficients;
- source timestamp;
- overrange/underrange diagnostics.

Current schematic status: the four-channel topology is practically closed and uses `ADS8688IDBTR` channels 0..3.

ADS8688 channel mapping:

```text
AI0 -> AIN_0P pin16 / AIN_0GND pin17
AI1 -> AIN_1P pin18 / AIN_1GND pin19
AI2 -> AIN_2P pin21 / AIN_2GND pin20
AI3 -> AIN_3P pin23 / AIN_3GND pin22
```

Unused CH4..CH7 and AUX are tied to `AGND`.

ADC supplies/reference:

```text
AVDD9  -> +5V_ANALOG
AVDD30 -> +5V_ANALOG
DVDD34 -> +3V3_DIG

REFSEL4 -> 1k -> AGND
REFIO5  -> 10u -> AGND
REFCAP7 -> 1u || 22u -> AGND
```

The internal 4.096 V reference is used. All ADS8688 ground pins, including `DGND33`, belong to the common analog ground domain `AGND`; no separate digital-ground island is created at the ADC.

Reference AI channel topology, for `x = 0..3`:

```text
external connector
     |
   AIx_IN
     |
   PTC
     |
NODE_AIx_CLAMP
     |
     +---- SMBJ12CA -------- AIx_RET
     |
    100R
     |
NODE_AIx_PROT
     |
     +---- CDSOD323-T12C --- AIx_RET
     |
     +---- 27nF ------------ AIx_RET
     |
     +---- 200R + jumper --- AIx_RET   (current-mode burden)
     |
    1k
     |
  AIN_xP
     |
    1nF
     |
 AIN_xGND
     |
    1k
     |
  AIx_RET
     |
    0R
     |
   AGND
```

Current-mode burden:

```text
4 mA  -> ~0.8 V across 200R
20 mA -> ~4.0 V across 200R
```

AI protection parts currently selected:

```text
primary TVS   : Littelfuse SMBJ12CA
secondary TVS : CDSOD323-T12C
PTC intended  : PTS120660V005
```

Important: older Altium screenshots temporarily used `PTS120630V012`; all four channels must use `PTS120660V005` before final BOM/PCB release.

AI terminal block:

```text
DG350-3.5-08P-14
1 AI0_IN
2 AI0_RET
3 AI1_IN
4 AI1_RET
5 AI2_IN
6 AI2_RET
7 AI3_IN
8 AI3_RET
```

PCB priorities:

- PTC + `SMBJ12CA` at the connector/protection boundary;
- compact surge loop `AIx_IN -> PTC -> SMBJ12CA -> AIx_RET`;
- the `100R` element separates the primary energy clamp from the ADC-side network;
- secondary TVS/filter closer to the ADC region;
- `1k/1k + 1nF` immediately at the corresponding ADC input pair;
- reference capacitors kept away from SPI/protection currents;
- continuous `AGND` plane under the ADC;
- `AGND <-> GND_SYS` remains the single controlled tie on `02_POWER_RAILS`.

### 3.2 Analog outputs

Provide **2 analog output channels**: `AO0..AO1`.

Frozen MVP target ranges:

- `0–10 V`;
- `4–20 mA`.

The AD5422 silicon also supports additional ranges, but firmware/hardware qualification for this board shall not claim them unless explicitly validated. Bipolar `±10 V` output is out of scope because the MVP has no negative analog rail.

Implementation:

```text
2 × AD5422BREZ
1 × OPA2991IDR        (dual sense buffer)
1 × SN74LVC2G04DBVR   (dual CLEAR inverter)
+15V_AO
+3V3_DIG
AGND
```

#### 3.2.1 AD5422 supply, ground and reference

For each AD5422:

```text
pin 1  AVSS         -> AGND
pin 2  DVCC         -> +3V3_DIG
pin 4  GND          -> AGND
pin 11 GND          -> AGND
pin 12 GND          -> AGND
pin 16 DVCC SELECT  -> AGND
pin 24 AVDD         -> +15V_AO
pin 25 EPAD         -> AGND
```

`DVCC SELECT = 0` selects the external `+3V3_DIG` supply instead of the internal ~4.5 V digital regulator, keeping the serial interface at the RP2350-compatible 3.3 V domain.

Local decoupling per AD5422:

```text
AVDD  -> 100n || 10u -> AGND
DVCC  -> 100n || 10u -> AGND
```

The 100 nF capacitors are high-frequency bypass parts and must be physically adjacent to the associated supply/ground pins. The 10 µF parts are local reservoirs and may be slightly farther away.

The internal 5 V reference is used independently on each DAC:

```text
pin14 REFOUT ----+
                 +---- AOx_REF_5V
pin15 REFIN  -----+
                 |
                100n
                 |
                AGND
```

External current-setting resistor:

```text
pin13 RSET -> 15.0k precision -> AGND
```

Requirements for RSET:

- `15.0 kΩ`;
- `0.1%` max tolerance;
- low TCR, preferably `<= 5..10 ppm/°C`;
- thin-film preferred.

Firmware must select the external resistor (`REXT = 1`).

Unused/special pins:

```text
pin18 NC    -> NC
pin20 BOOST -> NC
```

No external boost transistor is required with the present `+15V_AO` power architecture.

#### 3.2.2 SPI and daisy chain

Both DACs share `BOARD_SPI_SCK` and one common `AO_LATCH`.

Daisy-chain wiring:

```text
RP2350 BOARD_SPI_MOSI
        |
        v
AO0 AD5422 pin9 SDIN
AO0 AD5422 pin10 SDO
        |
        +---- AO0_SDO_TO_AO1
                         |
                         v
AO1 AD5422 pin9 SDIN
AO1 AD5422 pin10 SDO -> NC / dedicated test point
```

Common signals:

```text
AO0 pin8 SCLK -> BOARD_SPI_SCK
AO1 pin8 SCLK -> BOARD_SPI_SCK

AO0 pin7 LATCH -> AO_LATCH
AO1 pin7 LATCH -> AO_LATCH
```

Do not connect the final AO1 `SDO` directly to the shared `BOARD_SPI_MISO`; that bus is also used by the ADC and direct connection would create a push-pull contention risk during unrelated SPI clocks.

After daisy-chain mode is enabled, a normal two-DAC update uses 48 SCLK edges before the common LATCH rising edge. The first transmitted 24-bit word propagates to the far AO1 device; the second remains in AO0.

Conceptual update framing:

```text
AO_LATCH = LOW
send WORD_AO1
send WORD_AO0
AO_LATCH = HIGH
```

`AO_LATCH` has one hardware pull-up:

```text
AO_LATCH -> 10k -> +3V3_DIG
```

#### 3.2.3 FAULT and hardware safe-state

Both AD5422 `FAULT` outputs are open-drain and share the board fault net. The MAX22190 `/FAULT` output on `09_DIGITAL_INPUTS` is also open-drain and joins the same net:

```text
AO0 AD5422 pin3 FAULT -----+
AO1 AD5422 pin3 FAULT -----+---- BOARD_FAULT_N -> RP2350 GPIO19
MAX22190 pin14 /FAULT -----+
```

There is one common pull-up on the MCU sheet:

```text
BOARD_FAULT_N -> 10k -> +3V3_DIG
```

Do not add separate pull-ups for the individual AO or DI fault sources. Firmware must identify the asserting source by reading the relevant device status registers.

The existing supervisor logic provides:

```text
AO_SAFE_EN = SYS_RESET_N AND AO_ENABLE_REQ
```

`AO_ENABLE_REQ` requires a hardware default LOW:

```text
AO_ENABLE_REQ -> 100k -> GND_SYS
```

The dual inverter generates active-high asynchronous CLEAR:

```text
SN74LVC2G04DBVR

1A <- AO_SAFE_EN
1Y -> AO0_CLEAR

2A <- AO_SAFE_EN
2Y -> AO1_CLEAR

VCC -> +3V3_DIG
GND -> GND_SYS
100n from VCC to GND_SYS
```

Final intended ground domain for this inverter is `GND_SYS`, not `AGND`, because it is purely digital safe-state logic. If an older schematic revision still shows its GND/bypass return on `AGND`, move them to `GND_SYS` before PCB release.

Each AD5422:

```text
pin5 CLEAR SELECT -> AGND
pin6 CLEAR        -> AOx_CLEAR
```

Therefore, during reset/watchdog or while firmware has not requested output enable:

```text
AO_SAFE_EN = 0
AOx_CLEAR  = 1
```

Important safe-state semantics:

- in the qualified `0–10 V` mode, CLEAR is intended to command the bottom of range (`0 V`) when software `CLRSEL` remains 0;
- in the qualified `4–20 mA` mode, CLEAR commands the bottom of range (`4 mA`), not `0 mA` and not Hi-Z;
- therefore the present hardware safe-state is **4 mA in current mode**;
- if a future product requirement demands hard `0 mA`/Hi-Z independent of register state, a separate output-disconnect element is required.

Firmware invariants for safety-critical operation:

```text
CLRSEL = 0
SREN   = 0 unless a deliberate safe-state slew is required
```

The hardware CLEAR signal is supervisor-driven, but the exact analog clear level still depends on supported AD5422 register configuration; firmware must not enable a configuration that defeats the intended safe level.

#### 3.2.4 Combined VOUT/IOUT topology

Each channel uses one physical field output for both voltage and current modes.

Internal DAC-side node:

```text
AD5422 pin19 IOUT ----+
                      +---- AOx_RAW
AD5422 pin21 VOUT ----+
                      |
pin17 CCOMP -- 4nF ---+
```

The `4 nF` compensation capacitor is connected between `CCOMP pin17` and the `VOUT/AOx_RAW` node. C0G/NP0 dielectric is preferred.

`+VSENSE` is buffered because directly tying the combined output to the internal sense input would introduce unacceptable leakage/error in current mode.

For AO0, OPA2991 channel A:

```text
AO0_OUT -> 10k -> pin3 +INA

pin2 -INA ----+
              +---- pin1 OUTA -> AO0_VSENSE -> AO0 AD5422 pin22 +VSENSE
```

For AO1, OPA2991 channel B:

```text
AO1_OUT -> 10k -> pin5 +INB

pin6 -INB ----+
              +---- pin7 OUTB -> AO1_VSENSE -> AO1 AD5422 pin22 +VSENSE
```

OPA2991 supply:

```text
pin8 V+ -> +15V_AO
pin4 V- -> AGND
100n from +15V_AO to AGND adjacent to the IC
```

Both AD5422 negative-sense pins are local analog-ground references:

```text
pin23 -VSENSE -> AGND
```

The OPA2991 inputs must sense `AOx_OUT` **after** the 33 Ω protection resistor. They must not sense `AOx_RAW`; this is what allows voltage-mode feedback to compensate the normal load-dependent drop across the series protection resistor.

#### 3.2.5 Output protection and connector

Each channel uses:

```text
AOx_RAW
   |
  33R
   |
AOx_OUT ------------------------- connector pin1
   |
 SMBJ15A
   |
AOx_RET ------------------------- connector pin2
   |
  0R
   |
 AGND
```

`SMBJ15A` is unidirectional:

```text
cathode -> AOx_OUT
anode   -> AOx_RET
```

The output rail clamp is one `BAS70-04LT1G` per channel. The required symbol/pad mapping is:

```text
pin1 -> AGND
pin3 -> AOx_RAW
pin2 -> +15V_AO
```

Equivalent internal clamp direction:

```text
AGND --|>|-- AOx_RAW --|>|-- +15V_AO
```

The BAS70 is the low-leakage secondary rail clamp; it does not replace the field TVS. The `SMBJ15A` at the connector takes the main transient energy, the 33 Ω element limits residual current into the internal node, and the BAS70 limits the remaining excursion around the local rails.

Series resistor requirements:

```text
33 Ω
1%
pulse-rated
2512 preferred
```

At nominal 20 mA:

```text
V33 = 0.020 * 33 = 0.66 V
P33 = 0.020^2 * 33 = 13.2 mW
```

With `AVDD = 15 V`, AD5422 current-mode compliance is approximately:

```text
15 V - 2.5 V = 12.5 V
```

For a 500 Ω loop load at 20 mA:

```text
VLOAD = 10.0 V
VDAC  = 10.0 + 0.66 = 10.66 V
nominal compliance margin ~= 1.84 V
```

This is acceptable for the frozen `4–20 mA` target. Do not automatically claim `24 mA @ 500 Ω` or other extended ranges without rechecking compliance.

Voltage-mode remote sense compensates the normal drop across 33 Ω. For example, at roughly 10 mA load current the 33 Ω element drops about 0.33 V and the DAC raises its internal VOUT accordingly to keep the terminal near the commanded value.

The 10 kΩ sense resistor is a protection/current-limiting element for the OPA2991 input; it is not in the power-output path.

Connectors:

```text
J_AO0 = DG350-3.5-02P-14
1 AO0_OUT
2 AO0_RET

J_AO1 = DG350-3.5-02P-14
1 AO1_OUT
2 AO1_RET
```

`AO0_RET` and `AO1_RET` are not isolated/floating returns. Each is tied through its own local 0 Ω element to the common `AGND` domain.

The present network is a transient/ESD/EFT/surge-oriented protection architecture. It is **not yet a validated sustained 24/36 V field-miswire survival circuit**. If sustained DC miswire survival becomes a requirement, add/qualify a disconnect/current-limiting stage rather than assuming the TVS can dissipate indefinite DC energy.

#### 3.2.6 AO PCB current loops and placement

Normal current-mode DC loop:

```text
+15V_AO
 -> AD5422
 -> IOUT / AOx_RAW
 -> 33R
 -> AOx_OUT
 -> external load
 -> AOx_RET
 -> 0R
 -> AGND
 -> AD5422
```

Voltage-mode power loop is analogous through VOUT. The sense loop is separate:

```text
AOx_OUT
 -> 10k
 -> OPA2991 +IN
 -> OPA2991 OUT
 -> AD5422 +VSENSE
```

Route the sense pickup as a Kelvin branch from `AOx_OUT` on the field side of 33 Ω. Do not share it with the surge-current path.

Protection placement from field toward the DAC:

```text
connector
 -> SMBJ15A shunt to AOx_RET
 -> AOx_OUT
 -> 33R
 -> AOx_RAW
 -> BAS70-04 rail clamp
 -> AD5422
```

TVS/surge loop:

```text
AOx_OUT terminal -> SMBJ15A -> AOx_RET terminal
```

This loop must be compact and kept out of the reference/sense area.

HF supply loops must also be compact:

```text
AVDD24 -> AD5422 -> GND/AVSS -> 100n -> AVDD24
DVCC2  -> AD5422 -> GND4     -> 100n -> DVCC2
```

Reference loop:

```text
REFOUT14 / REFIN15 / 100n / nearby GND12
```

Keep the reference cluster away from SCLK, LT8362 switching nodes, rail-clamp current paths, and the external protection area.

Positive transient current conducted by the upper BAS70 diode can inject into `+15V_AO`; the LT8362 output is not assumed to be an ideal energy sink. During EMC qualification, observe the `+15V_AO` rail for uplift. AO bulk capacitors should have adequate voltage rating and effective capacitance at 15 V; `>=35 V`, preferably 50 V where practical, is recommended for the local 10 µF parts.

---

### 3.3 Digital inputs

Provide **4 physical industrial digital input channels**: `DI0..DI3`.

The frozen MVP implementation uses one `MAX22190ATJ+` in a **4 × IEC 61131-2 Type 1/Type 3** configuration. The silicon has eight input channels, but Hardware Prototype v1 intentionally uses only four of them. A quad Type 2 implementation using paired MAX22190 channels is deferred to a future board revision and shall not be claimed for this MVP.

Target electrical behavior:

- nominal `24 V DC` field inputs;
- IEC 61131-2 Type 1/Type 3 input-current characteristic;
- hardware input-state LEDs;
- programmable internal debounce/filtering;
- optional wire-break/open-wire diagnostics where the field wiring actually provides diagnostic current;
- shared SPI interface with CRC;
- board-level fault interrupt plus deterministic polling for normal input-state changes.

#### 3.3.1 Channel allocation and unused channels

Physical channel mapping:

```text
DI0 -> MAX22190 IN1 pin1  -> internal DI bit0
DI1 -> MAX22190 IN2 pin3  -> internal DI bit1
DI2 -> MAX22190 IN3 pin5  -> internal DI bit2
DI3 -> MAX22190 IN4 pin7  -> internal DI bit3
```

Unused internal channels:

```text
IN5 pin18 -> GND_SYS
IN6 pin20 -> GND_SYS
IN7 pin22 -> GND_SYS
IN8 pin24 -> GND_SYS

LED5 pin17 -> GND_SYS
LED6 pin19 -> GND_SYS
LED7 pin21 -> GND_SYS
LED8 pin23 -> GND_SYS
```

Firmware must change the power-on default `INEN = 0xFF` to:

```text
INEN = 0x0F
```

so only IN1..IN4 remain enabled. The grounded unused channels are therefore deterministic during POR and are subsequently disabled in firmware.

#### 3.3.2 Supply, grounds and serial interface

The MAX22190 is powered from the board logic rail instead of from the field-supply pin:

```text
pin31 VDD   -> +3V3_DIG
               100n || 1u -> GND_SYS

pin16 VL    -> +3V3_DIG
               100n || 1u -> GND_SYS

pin30 VDD24 -> NC

pin25 GND   -> GND_SYS
pin32 GND   -> GND_SYS
pin33 EPAD  -> GND_SYS
```

`VDD24` remains physically unconnected. Therefore the MAX22190 VDD24 undervoltage/missing monitors are not meaningful for this board and their FAULT-pin enables must remain masked in firmware.

The exposed pad is represented in the Altium symbol/footprint as pad `33` and must be electrically and thermally connected to `GND_SYS`.

SPI mode straps:

```text
pin29 M1 -> GND_SYS
pin28 M0 -> GND_SYS
```

This selects the MAX22190 24-bit non-daisy-chain SPI mode with CRC. The RP2350 SPI peripheral shall use SCLK idle LOW and the edge relationship required by the MAX22190 datasheet; this corresponds to conventional SPI mode 0 semantics.

Serial/control wiring:

```text
pin9  /CS    -> DI_CS_N         -> RP2350 GPIO16
DI_CS_N      -> 10k -> +3V3_DIG

pin10 SCLK   -> BOARD_SPI_SCK   -> RP2350 GPIO14
pin11 SDI    -> BOARD_SPI_MOSI  -> RP2350 GPIO15
pin13 SDO    -> BOARD_SPI_MISO  -> RP2350 GPIO12

pin12 /LATCH -> +3V3_DIG

pin14 /FAULT -> BOARD_FAULT_N
pin15 /READY -> DI_READY_N
DI_READY_N   -> 10k -> +3V3_DIG
```

`SDO` is tri-stated while `/CS` is inactive, so it can share `BOARD_SPI_MISO` with other properly chip-selected devices.

`/LATCH` is held HIGH because only one MAX22190 is used. Falling `/CS` provides the required input snapshot for normal transactions.

`/FAULT` is open-drain and joins the existing `BOARD_FAULT_N` wired-OR net together with both AD5422 fault outputs. The only board-level pull-up remains the `10k -> +3V3_DIG` pull-up on the MCU sheet.

`/READY` is open-drain and is reserved as:

```text
DI_READY_N -> TCAL9539 P14
```

The service-I/O sheet must complete that cross-sheet connection. Firmware initializes the service expander early enough to observe `DI_READY_N` before relying on MAX22190 register accesses.

#### 3.3.3 Type 1/Type 3 field frontend

The shared Type 1/Type 3 current-setting resistor is:

```text
pin27 REFDI -> 7.50k 1% -> GND_SYS
```

This sets the MAX22190 input current limit to approximately `2.35 mA typ`, with the datasheet Type 1/Type 3 operating characteristic used for this board.

Each physical input has one pulse-withstanding series resistor:

```text
DI0_IN -> 1.5k -> IN1
DI1_IN -> 1.5k -> IN2
DI2_IN -> 1.5k -> IN3
DI3_IN -> 1.5k -> IN4
```

Frozen orderable part for all four:

```text
CRCW25121K50FKEGHP
1.5 kΩ
1%
2512
pulse-withstanding
```

No external capacitor is placed on `IN1..IN4`; debounce/filtering is performed inside the MAX22190. No per-channel TVS is currently used. The selected topology intentionally follows the MAX22190 resistor-only robust-input approach rather than adding a clamp that could disturb the IEC input characteristic.

Terminal block:

```text
J_DI = DG350-3.5-06P-14

1 DI0_IN
2 DI1_IN
3 DI2_IN
4 DI3_IN
5 DI_COM
6 DI_COM
```

Both COM contacts are paralleled:

```text
DI_COM -> GND_SYS
```

The duplicate COM terminal is intentional and allows convenient field return/daisy wiring. The DI block is **not galvanically isolated** from the MCU/system ground domain.

Normal field connection is therefore conceptually:

```text
+24V ---- sensor/contact ---- DIx_IN
0V  ------------------------ DI_COM
```

The MAX22190 `INx` pins have ±40 V input protection/absolute capability, but Hardware Prototype v1 only freezes **24 V nominal** operation. Sustained 36 V field operation/miswire shall not be claimed as qualified until it is explicitly verified over device, resistor, threshold and temperature tolerances. The 9–36 V main-board power-input range does not automatically imply a qualified 36 V DI field-input rating.

ADI characterizes the MAX22190 robust-input approach with a minimum 1 kΩ series resistor for high ESD/EFT/surge immunity. Those manufacturer/reference-design results are evidence for the topology, not certification of this PCB. The final board must be tested with its actual connector, footprint clearances, grounding and enclosure.

#### 3.3.4 Hardware DI LEDs

The four used MAX22190 energyless LED outputs drive four physical channel LEDs:

```text
pin2 LED1 -> DI0_LED -> LED anode
                         LED cathode -> GND_SYS

pin4 LED2 -> DI1_LED -> LED anode
                         LED cathode -> GND_SYS

pin6 LED3 -> DI2_LED -> LED anode
                         LED cathode -> GND_SYS

pin8 LED4 -> DI3_LED -> LED anode
                         LED cathode -> GND_SYS
```

No ordinary series resistor is used; the LED current comes from the MAX22190 field-input current path. Green high-efficiency 0603 LEDs are preferred, but the exact orderable LED remains to be frozen.

These are **hardware input-state indicators**. They are not software-addressable and continue to represent field input activity independently of RP2350 firmware execution.

Do not DNP one of these LEDs while leaving the corresponding MAX22190 `LEDx` pin floating. If a DI LED is deliberately omitted in a future assembly variant, its `LEDx` pin must instead be tied to `GND_SYS` as required for an unused LED output.

#### 3.3.5 Wire-break/open-wire diagnostics

The common wire-break reference is:

```text
pin26 REFWB -> 24.0k 1% -> GND_SYS
```

This gives a wire-break threshold of approximately:

```text
IWB ~= 2.44 V / 24 kΩ ~= 102 uA typ
```

Wire-break detection has its own approximately 20 ms filtering and is enabled independently per channel through the corresponding `FLTx.WBE` bit.

Hardware Prototype v1 defaults wire-break **OFF** for every channel. It is only meaningful when the field circuit maintains a small diagnostic current in the normal OFF state, for example:

- a compatible two-wire proximity sensor whose OFF-state current remains safely above the wire-break threshold; or
- a dry contact with a diagnostic/end-of-line resistor installed remotely across the field contact.

A plain open dry contact and a broken conductor both produce approximately zero current and therefore cannot be distinguished. Likewise, generic three-wire PNP sensors shall not be assumed to support wire-break unless their guaranteed OFF-state current makes the diagnostic valid.

No end-of-line resistor is installed on the controller PCB. An EOL resistor intended to diagnose the field cable must be physically located at the remote sensor/contact end.

While no channel has `WBE=1`, the REFWB open/short FAULT2 sources remain masked because `RFWBO` is expected after POR when all wire-break functions are disabled.

#### 3.3.6 Filtering, diagnostics and firmware defaults

The normal DI filter is internal and programmable per channel. Supported MAX22190 delays are:

```text
50 us
100 us
400 us
800 us
1.6 ms
3.2 ms
12.8 ms
20 ms
```

with an additional filter-bypass mode.

Frozen board/firmware default for `DI0..DI3`:

```text
WBE   = 0
FBP   = 0
DELAY = 100b = 1.6 ms

FLT1 = 0x04
FLT2 = 0x04
FLT3 = 0x04
FLT4 = 0x04
```

Longer filtering can be selected for switches/relays; shorter filtering can be selected for faster sensors. High-rate counter/frequency/encoder work should normally use the separate `UIC0..UIC3` subsystem rather than these general-purpose DI channels.

Relevant register contract:

| Register | Address | POR/default | MVP target/use |
|---|---:|---:|---|
| `WB` | `0x00` | `0x00` | read / clear-on-read wire-break flags |
| `DI` | `0x02` | `0x00` | read input state |
| `FAULT1` | `0x04` | status | primary diagnostics |
| `FLT1..FLT4` | `0x06..0x0C` | `0x08` | write `0x04` default |
| `FLT5..FLT8` | `0x0E..0x14` | `0x08` | unused channels; leave bypassed/disabled by INEN |
| `CFG` | `0x18` | `0x00` | target `0x01`, enable REFDI-short detection |
| `INEN` | `0x1A` | `0xFF` | target `0x0F` |
| `FAULT2` | `0x1C` | status | read / clear-on-read secondary diagnostics |
| `FAULT2EN` | `0x1E` | `0x00` | target `0x3C` while wire-break disabled |
| `GPO` | `0x22` | `0x00` | target `0x80`, sticky FAULT behavior |
| `FAULT1EN` | `0x24` | `0xC0` | target `0xF8` while wire-break disabled |

`FAULT2EN = 0x3C` enables:

- malformed SPI clock-count diagnostic;
- thermal shutdown diagnostic;
- REFDI open diagnostic;
- REFDI short diagnostic;

while leaving REFWB open/short masked until wire-break is actually used.

`FAULT1EN = 0xF8` enables CRC, POR, FAULT2, and both overtemperature alarms while deliberately masking:

- VDD24-low (`24VL`), because `VDD24` is NC;
- VDD24-missing (`24VM`), because `VDD24` is NC;
- group wire-break fault (`WBG`) while wire-break is globally unused.

Recommended initialization sequence:

```text
1. initialize TCAL9539 sufficiently to observe DI_READY_N / P14
2. wait until DI_READY_N == LOW
3. configure SPI for 24-bit MAX22190 CRC operation, SCLK idle LOW
4. perform a valid CRC-protected transaction and inspect FAULT1/POR
5. write INEN = 0x0F
6. write FLT1..FLT4 = 0x04
7. write CFG = 0x01
8. read WB and FAULT2 to clear stale/POR diagnostic state
9. write FAULT2EN = 0x3C
10. write GPO = 0x80
11. write FAULT1EN = 0xF8
12. explicitly clear the POR indication according to the datasheet write semantics
13. read back configuration registers and enter normal operation
```

The CRC polynomial and frame-generation algorithm shall follow the official MAX22190 CRC documentation rather than a hand-invented variant.

Normal DI `0/1` changes do **not** inherently assert `/FAULT`; firmware must use deterministic SPI polling for state/edge/event processing. `/FAULT` is reserved for diagnostics and is shared with the analog-output fault sources on `BOARD_FAULT_N`.

When wire-break is enabled on at least one channel, firmware may additionally unmask the REFWB diagnostics and `WBG`; it must clear stale `WB/FAULT2` state during that transition. When the last wire-break-enabled channel is disabled, mask `WBG` and the REFWB diagnostics before clearing the final `WBE` bit.

### 3.4 Digital outputs

Provide **2 digital output channels**: `DO0..DO1`.

Target implementation:

- protected 24 V high-side outputs;
- current limiting and thermal protection;
- short-circuit/fault diagnostics where supported;
- deterministic OFF state during reset and boot.

Target output IC: `IPS2050HTR`.

Firmware shall support commanded state, fault/actual state, startup state, and watchdog state.

### 3.5 Universal inputs / counters / frequency / encoder

Provide **4 universal protected channels**: `UIC0..UIC3`.

Primary modes:

- digital input;
- pulse counter;
- frequency input;
- period measurement;
- quadrature encoder using channel pairs;
- optional low-voltage GPIO-style mode where electrically safe;
- target analog-voltage mode through RP2350 `ADC0..ADC3`, if the final UIC frontend preserves an analog path.

RP2350 assignment is intentionally:

```text
UIC0 -> GPIO40 / ADC0
UIC1 -> GPIO41 / ADC1
UIC2 -> GPIO42 / ADC2
UIC3 -> GPIO43 / ADC3
```

This preserves the option to reuse the generic RP2350 ADC-voltage driver for UIC analog mode.

**Important:** the currently selected `TLV3214PWR` comparator path produces a digital output and therefore does not preserve analog amplitude. Merely changing GPIO40..43 to ADC mode is not enough with the current comparator-only topology. Before analog UIC mode is claimed, the UIC schematic must provide a valid protected/scaled analog path to `ADC0..ADC3`, while retaining a clean digital/comparator path for counter/frequency/encoder operation.

Counter and encoder processing should use RP2350 PIO and DMA where appropriate rather than nondeterministic task-level GPIO polling.

Typical digital configurations:

```text
4 independent counters
2 quadrature encoders
1 quadrature encoder + 2 counters
4 protected digital inputs
```

---

## 4. External sensor interfaces

### 4.1 I²C

Provide **2 I²C sensor ports**:

```text
I2C0: 3V3, GND, SDA, SCL
I2C1: 3V3, GND, SDA, SCL
```

Requirements:

- 3.3 V logic only;
- ESD protection and series damping where appropriate;
- current-limited switched sensor power;
- intended for short cables inside an enclosure or laboratory setup;
- external stuck-bus isolation/recovery through `TCA4307`.

I²C is not treated as a long-distance industrial field bus.

Frozen internal/external topology:

```text
RP2350 GPIO24/25 / I2C0
    |
    +---- TCAL9539              internal service device
    |
    +---- TCA4307 #0 ---------- external I2C0 connector

RP2350 GPIO26/27 / I2C1
    |
    +---- TCA4307 #1 ---------- external I2C1 connector
```

The `TCAL9539` remains on the **internal side** of the I2C0 recovery buffer. A faulted external I2C0 device therefore must not prevent firmware from reaching the expander and power-cycling the external sensor port.

Each `TCA4307` provides:

- physical separation of internal and external bus capacitance;
- `EN` control;
- open-drain `READY` status;
- automatic stuck-bus recovery. The device disconnects the external side after a prolonged low state (about 40 ms per TI documentation) and can generate up to 16 SCL pulses on the external side while attempting recovery.

Each `TCA4307 EN` shall have a hardware default that keeps the external branch disconnected during reset until firmware intentionally enables it. Pull-up sizing, bus pull-ups on both sides, and exact startup timing remain schematic-level items to finalize.

### 4.2 SPI

Provide **1 SPI sensor port**:

```text
3V3, GND, SCK, MOSI, MISO, CS
```

The external branch must be protected and kept from disturbing internal SPI devices. Firmware must use a dedicated chip-select and bounded transaction timeouts.

### 4.3 UART

Provide **1 UART sensor port**:

```text
3V3, GND, TX, RX
```

This is a 3.3 V logic interface. Direct RS-232 voltage levels are not allowed without an external transceiver.

### 4.4 Sensor power and internal service I/O

Four current-limited sensor power channels are implemented using 4 × `TPS2553DBVR`, one for each external low-voltage sensor port:

```text
SENSOR_PWR0 -> I2C0 port
SENSOR_PWR1 -> I2C1 port
SENSOR_PWR2 -> SPI port
SENSOR_PWR3 -> UART port
```

The four `TPS2553 EN` controls no longer consume RP2350 ADC-capable GPIO44..47. They are moved to `TCAL9539` outputs.

Frozen expander allocation:

```text
TCAL9539 P00 -> SENSOR_PWR_EN0        OUT
TCAL9539 P01 -> SENSOR_PWR_EN1        OUT
TCAL9539 P02 -> SENSOR_PWR_EN2        OUT
TCAL9539 P03 -> SENSOR_PWR_EN3        OUT

TCAL9539 P04 <- SENSOR_PWR_FAULT0_N   IN
TCAL9539 P05 <- SENSOR_PWR_FAULT1_N   IN
TCAL9539 P06 <- SENSOR_PWR_FAULT2_N   IN
TCAL9539 P07 <- SENSOR_PWR_FAULT3_N   IN

TCAL9539 P10 -> I2C0_ISOL_EN          OUT -> TCA4307 #0 EN
TCAL9539 P11 <- I2C0_READY            IN  <- TCA4307 #0 READY
TCAL9539 P12 -> I2C1_ISOL_EN          OUT -> TCA4307 #1 EN
TCAL9539 P13 <- I2C1_READY            IN  <- TCA4307 #1 READY

TCAL9539 P14 <- DI_READY_N             IN  <- MAX22190 /READY
TCAL9539 P15..P17                     RESERVED
```

`TCAL9539 /INT` is connected to RP2350 GPIO39 and becomes the shared slow-service interrupt:

```text
TCAL9539 /INT -> BOARD_IO_INT_N -> GPIO39
```

Firmware must not perform I²C transactions directly inside the GPIO ISR. The ISR should only notify/defer work; task context then reads the expander interrupt/status/input registers and identifies the changed source.

`TCAL9539 /RESET` shall be tied into `SYS_RESET_N` so supervisor reset returns the service I/O expander to its hardware-reset state. Sensor-power enable nets must also have safe external biasing so reset/Hi-Z cannot accidentally power a port.

This replaces the old architecture where GPIO44..47 directly drove `SENSOR_PWR_EN0..3` and GPIO39 only reported one combined sensor-power fault.

Benefits of the current service-I/O architecture:

- individual fault identity for all four `TPS2553` channels;
- no RP2350 GPIO cost for the four slow sensor-power enables;
- both external I²C branches can be disconnected/recovered independently;
- a stuck external I²C0 device cannot block access to the internal expander;
- MAX22190 `/READY` is observable without consuming a direct RP2350 GPIO;
- three expander GPIOs remain reserved for later slow control/diagnostics.

### 4.5 RP2350 internal ADC board monitoring

GPIO44..47 are released from sensor-power control and reserved for board-voltage diagnostics.

Frozen allocation:

```text
GPIO44 / ADC4 -> VIN_PROTECTED_MON
GPIO45 / ADC5 -> AO_15V_MON
GPIO46 / ADC6 -> ANA_5V_MON
GPIO47 / ADC7 -> ADC7_MON_RESERVED
```

Current intent:

- `VIN_PROTECTED_MON`: actual protected input voltage after the eFuse;
- `AO_15V_MON`: actual `+15V_AO` rail feeding the analog-output domain;
- `ANA_5V_MON`: actual `+5V_ANALOG` rail used by the analog/ADC domain;
- `ADC7_MON_RESERVED`: intentionally uncommitted until a more valuable internal diagnostic node is identified.

`+3V3_ANALOG` is **not** currently assigned a dedicated ADC monitor. Its diagnostic value does not justify consuming the last spare ADC channel at this stage.

Each monitored rail requires an appropriate divider/protection/RC network such that the ADC pin remains inside the RP2350 absolute and operating limits over worst-case rail/input tolerance. Exact divider and RC values are **not frozen yet** and must be verified against RP2350 ADC acquisition/source-impedance requirements before schematic release.

The existing firmware `adc_voltage` driver is architecturally reusable for these board monitors, but the current implementation only validates source indices `0..3`. RP2350 board support must extend/validate that driver for `ADC4..ADC7` before these monitor channels are enabled in the industrial firmware profile.

---

## 5. Power architecture

### 5.1 Power tree

```text
J1 9–36 V
 -> TPS26600
 -> VIN_PROTECTED
    +-> LM76003 -> +5V_PRE ~5.3 V
    |   +-> TPS62130 -> +3V3_DIG
    |   +-> TPS7A2033 -> +3V3_ANALOG
    |   +-> TPS7A2050 -> +5V_ANALOG
    |
    +-> LT8362 SEPIC -> +15V_AO
```

Main rails/nets:

```text
VIN_RAW
VIN_FUSED
VIN_FILTERED
VIN_PROTECTED
VFIELD
VFIELD_OUT

+5V_PRE
+3V3_DIG
+3V3_ANALOG
+5V_ANALOG
+1V1_CORE
+15V_AO

GND_SYS
AGND
GND_RS485_ISO
+3V3_RS485_ISO
```

### 5.2 Input protection — sheet `01_POWER_INPUT`

Status: **practically closed**.

```text
J1
 -> F1
 -> VIN_FUSED
 -> TVS
 -> optional EMI
 -> VIN_FILTERED
 -> TPS26600
 -> VIN_PROTECTED
```

Critical rule:

```text
EFUSE_RTN != GND_SYS
```

`PWR_IN_FAULT_N` has:

```text
10k -> +3V3_DIG
```

### 5.3 Main and digital rails — sheet `02_POWER_RAILS`

Status: **practically closed**.

#### LM76003

```text
VIN_PROTECTED -> LM76003 -> +5V_PRE
```

Current design values:

- `SW1..SW5` joined;
- `L1 = 6.8 µH` PRELIM;
- FB divider = `100k / 23.4k`;
- `PGOOD16 -> PWR_5V_PGOOD_N`;
- PGOOD pull-up = `10k -> +3V3_DIG`.

#### TPS62130

```text
+5V_PRE -> TPS62130 -> +3V3_DIG
```

Current design values:

- `SW1/SW2/SW3` joined;
- `L2 = 2.2 µH` PRELIM;
- FB divider = `470k / 150k`;
- `PG4 -> PWR_3V3_PGOOD_N`;
- pull-up = `10k -> +3V3_DIG`.

#### TPS7A2033

```text
+5V_PRE -> +3V3_ANALOG
```

- CIN/COUT = `2.2 µF`;
- GND -> `AGND`;
- EN -> `+5V_PRE` through 0 Ω.

#### TPS7A2050

```text
+5V_PRE -> +5V_ANALOG
```

Topology analogous to TPS7A2033.

#### AGND / GND_SYS controlled tie

There is exactly one intentional DC tie between the digital/system and analog ground domains:

```text
GND_SYS -> 0R (R_AGND_TIE) -> AGND
```

Place this physical tie near the analog LDO region. Do not add a second `AGND <-> GND_SYS` tie on AI/AO sheets.

### 5.4 AO power — sheet `02B_AO_POWER`

Status: **practically closed**.

```text
VIN_PROTECTED -> LT8362 SEPIC -> +15V_AO
```

Current values:

- inductors: `4.7 µH + 4.7 µH`;
- `C_SEPIC = 1 µF / 100 V`;
- `D_SEPIC >= 80 V` PRELIM;
- `COUT = 10 µF`;
- `RT = 20k`;
- `SS = 10 nF`;
- `SYNC/MODE = GND`;
- FBX divider = `1M / 120k`;
- `Cff = 4.7 pF`;
- VC compensation = `36.5k + 680 pF` series.

### 5.5 Supervisor/watchdog and safe-state

Supervisor: `TPS3851H33EDRBR`.

```text
RESET_N + WDO_N -> wired-OR -> SYS_RESET_N
```

Associated defaults:

```text
SYS_RESET_N       -> 10k pull-up
MANUAL_RESET_N    -> 10k pull-up
WD_ENABLE         -> 100k pull-down
WD_KICK           -> 100k pull-down
AO_ENABLE_REQ     -> 100k pull-down to GND_SYS
```

Hardware output gating uses `SN74LVC2G08DCTR`:

```text
AO_SAFE_EN = SYS_RESET_N AND AO_ENABLE_REQ
DO_SAFE_EN = SYS_RESET_N AND DO_ENABLE_REQ
```

For AO, `AO_SAFE_EN` is then inverted by `SN74LVC2G04DBVR` so reset/watchdog produces `AOx_CLEAR = 1`. The analog safe-state semantics are documented in section 3.2.3.

The physical output stages must therefore remain at their defined safe state while the MCU is in reset or the external supervisor is asserting reset.

---

## 6. RP2350B core — sheet `03_MCU_RP2350`

Status: **practically closed; GPIO39 and GPIO44..47 net update required before final audit**.

### 6.1 Internal core regulator

```text
+3V3_DIG -> VREG_VIN64

+3V3_DIG
 -> 33R
 -> VREG_AVDD61

VREG_LX63
 -> 3.3uH
 -> +1V1_CORE

VREG_FB65   -> +1V1_CORE
VREG_PGND62 -> GND_SYS
```

### 6.2 SWD / reset

```text
RUN35   -> SYS_RESET_N
SWCLK33 -> SWD_CLK
SWDIO34 -> SWD_IO
```

SWD, RUN/reset, BOOTSEL, and production/test points must remain accessible in the enclosure/service workflow.

### 6.3 Frozen GPIO map

```text
GPIO0  PSRAM_CS_N
GPIO1  USB_VBUS_DET
GPIO2  WD_KICK
GPIO3  WD_ENABLE

GPIO4  ETH_MISO
GPIO5  ETH_CS_N
GPIO6  ETH_SCK
GPIO7  ETH_MOSI
GPIO8  ETH_INT_N

GPIO9  AO_ENABLE_REQ
GPIO10 DO_ENABLE_REQ
GPIO11 PWR_IN_FAULT_N

GPIO12 BOARD_SPI_MISO
GPIO13 ADC_CS_N
GPIO14 BOARD_SPI_SCK
GPIO15 BOARD_SPI_MOSI
GPIO16 DI_CS_N
GPIO17 AO_LATCH
GPIO18 ADC_RST_PD_N
GPIO19 BOARD_FAULT_N

GPIO20 DO1_CMD
GPIO21 DO2_CMD
GPIO22 DO1_FAULT
GPIO23 DO2_FAULT

GPIO24 I2C0_SDA
GPIO25 I2C0_SCL
GPIO26 I2C1_SDA
GPIO27 I2C1_SCL

GPIO28 SENSOR_UART_TX
GPIO29 SENSOR_UART_RX

GPIO30 SENSOR_SPI_MISO
GPIO31 SENSOR_SPI_CS_N
GPIO32 SENSOR_SPI_SCK
GPIO33 SENSOR_SPI_MOSI

GPIO34 PWR_3V3_PGOOD_N
GPIO35 PWR_5V_PGOOD_N

GPIO36 RS485_TX
GPIO37 RS485_RX
GPIO38 RS485_DE_RE

GPIO39 BOARD_IO_INT_N

GPIO40 UIC0_IN / ADC0
GPIO41 UIC1_IN / ADC1
GPIO42 UIC2_IN / ADC2
GPIO43 UIC3_IN / ADC3

GPIO44 VIN_PROTECTED_MON / ADC4
GPIO45 AO_15V_MON / ADC5
GPIO46 ANA_5V_MON / ADC6
GPIO47 ADC7_MON_RESERVED / ADC7
```

All RP2350 GPIOs remain allocated/reserved. `SENSOR_PWR_EN0..3` are no longer direct MCU pins; they move to the `TCAL9539`. GPIO39 is no longer a single combined sensor-power fault input; it is the expander interrupt `BOARD_IO_INT_N`.

AO/DI-related board-level defaults on the MCU sheet:

```text
AO_LATCH      -> 10k -> +3V3_DIG
BOARD_FAULT_N -> 10k -> +3V3_DIG
AO_ENABLE_REQ -> 100k -> GND_SYS
```

Do not duplicate the `BOARD_FAULT_N` pull-up on either the AO or DI sheet.

---

## 7. External memory — sheet `04_MEMORY`

Status: **practically closed**.

### 7.1 Flash

`W25Q128JVBIQ`, 16 MB.

```text
A4 /RESET -> 10k -> +3V3_DIG
B2 CLK    -> QSPI_SCLK
B3 GND    -> GND_SYS
B4 VCC    -> +3V3_DIG
C2 /CS    -> QSPI_SS
C4 IO2    -> QSPI_SD2
D2 IO1    -> QSPI_SD1
D3 IO0    -> QSPI_SD0
D4 IO3    -> QSPI_SD3
```

### 7.2 BOOTSEL

```text
+3V3_DIG
   |
  10k
   |
QSPI_SS
   |
button
   |
  1k
   |
GND_SYS
```

### 7.3 PSRAM

`APS6404L-3SQR-SN`, 8 MB.

```text
1 /CE  -> PSRAM_CS_N
2 SIO1 -> QSPI_SD1
3 SIO2 -> QSPI_SD2
4 VSS  -> GND_SYS
5 SIO0 -> QSPI_SD0
6 SCLK -> QSPI_SCLK
7 SIO3 -> QSPI_SD3
8 VDD  -> +3V3_DIG
```

---

## 8. Ethernet + USB — sheet `05_ETHERNET_USB`

Status: **practically closed; final audit required before PCB release**.

### 8.1 W5500 power domains

Analog Ethernet rail:

```text
+3V3_DIG -> FB_ETH -> +3V3_ETH_A
```

Digital supply:

```text
VDD28 -> +3V3_DIG
GND29 -> GND_SYS
```

Local digital decoupling:

```text
100n + 10u
```

Analog supply pins:

```text
AVDD4
AVDD8
AVDD11
AVDD15
AVDD17
AVDD21
 -> +3V3_ETH_A
```

Analog grounds:

```text
AGND3
AGND9
AGND14
AGND16
AGND19
AGND48
 -> GND_SYS
```

Special pins:

```text
EXRES1 pin10 -> 12.4k 1% -> GND_SYS
VBG18        -> NC
TOCAP20      -> 4.7u -> GND_SYS
1V2O22       -> 10n -> GND_SYS
RSVD23       -> GND_SYS
```

PMODE:

```text
PMODE2 pin43 -> 10k -> +3V3_DIG
PMODE1 pin44 -> 10k -> +3V3_DIG
PMODE0 pin45 -> 10k -> +3V3_DIG

PMODE[2:0] = 111
```

SPI:

```text
SCSn32 -> ETH_CS_N
SCLK33 -> ETH_SCK
MISO34 -> ETH_MISO
MOSI35 -> ETH_MOSI
INTn36 -> ETH_INT_N

ETH_CS_N -> 10k -> +3V3_DIG
```

Reset:

```text
RSTn37 -> 0R -> SYS_RESET_N
```

### 8.2 Ethernet crystal

Current crystal:

```text
445I23D25M00000
25 MHz
CL = 18 pF
SMD 5032, 2-pin
```

Connection:

```text
XI -- crystal -- 0R -- XO
```

Supporting components:

- 18 pF from XI to GND;
- 18 pF from XO-side to GND;
- 1 MΩ feedback between XI and XO;
- 0 Ω series element in XO branch.

### 8.3 RJ45 MagJack

Current part:

```text
TMJD4012BENL
vertical / top-entry
10/100 Base-T
integrated magnetics
```

Pin mapping:

```text
1  TD+
2  CT_TX
3  TD-

4  RD+
5  CT_RX
6  RD-

7  NC
8  TERM_RET

9  A_YELLOW
10 K_YELLOW

11 A_GREEN
12 K_GREEN
```

PHY polarity is straight; no crossover:

```text
W5500 TXP pin2 -> TD+ pin1
W5500 TXN pin1 -> TD- pin3

W5500 RXP pin6 <- RD+ pin4
W5500 RXN pin5 <- RD- pin6
```

TX matching:

```text
W5500 TXP
 -> 3.3R
 -> node
 -> J2 TD+

node
 -> 49.9R 1%
 -> +3V3_ETH_A
```

Same topology for TXN.

TX center tap:

```text
+3V3_ETH_A
 -> 10R
 -> CT_TX
 -> 22n
 -> GND_SYS
```

RX matching:

```text
W5500 RXP
 -> 3.3R
 -> node
 -> 6.8n
 -> RD+

node
 -> 49.9R
 -> CT_RX
```

```text
W5500 RXN
 -> 3.3R
 -> node
 -> 6.8n
 -> RD-

node
 -> 49.9R
 -> CT_RX
```

```text
CT_RX -> 10n -> GND_SYS
```

`TERM_RET pin8` belongs to the integrated Bob-Smith/common-mode termination and must be routed to the intended chassis/IO shield network; it is not a normal NC.

### 8.4 Ethernet LEDs

The MagJack contains the two user-visible Ethernet LEDs.

```text
A_YELLOW pin9 -> +3V3_DIG
K_YELLOW pin10 -> 330R -> W5500 ACTLED pin27

A_GREEN pin11 -> +3V3_DIG
K_GREEN pin12 -> 330R -> W5500 LINKLED pin25
```

Unused W5500 LED outputs:

```text
SPDLED24 -> NC
DUPLED26 -> NC
```

No separate PCB LINK/ACT LEDs are required because they are already integrated in the RJ45.

### 8.5 USB-C

Current connector:

```text
USB4145-03-0230-C
vertical / top-entry
USB 2.0 Type-C
```

Connector:

```text
A1/A12/B1/B12 -> GND_SYS

A4/A9/B4/B9 -> USB_VBUS

A6/B6 -> USB_DP
A7/B7 -> USB_DM

A5 CC1 -> 5.1k -> GND_SYS
B5 CC2 -> 5.1k -> GND_SYS

A8 SBU1 -> NC
B8 SBU2 -> NC
```

USB does not power the main board:

```text
USB_VBUS != +5V_PRE
USB_VBUS != +3V3_DIG
```

USB ESD:

```text
TPD2EUSB30ADRTR
pin1 -> USB_DP
pin2 -> USB_DM
pin3 -> GND_SYS
```

The ESD device is a shunt protector and must be placed immediately adjacent to the USB-C connector.

VBUS detect:

```text
USB_VBUS
   |
  68k
   |
   +---- USB_VBUS_DET -> RP2350 GPIO1
   |
 100k
   |
GND_SYS
```

USB series resistors already exist on the MCU sheet:

```text
USB_DP67 -> 27R -> USB_DP
USB_DM66 -> 27R -> USB_DM
```

Do **not** add a second 27 Ω pair on the USB sheet.

---

## 9. Isolated RS-485 — sheet `06_RS485`

Status: **practically closed**.

Transceiver:

```text
ISOW1432DFMR
```

Configured for isolated half-duplex RS-485.

### 9.1 Digital side

```text
pin1 VIO -> +3V3_DIG
           100n -> GND_SYS

pin2 D -> RS485_TX
pin3 DE -> RS485_DE_RE
pin4 R -> RS485_RX
pin5 /RE -> RS485_DE_RE
pin6 GNDIO -> GND_SYS
pin7 OUT -> NC
pin8 EN/FLT -> 10k -> +3V3_DIG
pin9 VDD -> +3V3_DIG
           10n + 10u -> GND_SYS
pin10 GND1 -> GND_SYS
```

Control default:

```text
RS485_DE_RE -> 10k -> GND_SYS
```

Therefore after reset:

```text
DE = 0   -> driver disabled
/RE = 0  -> receiver enabled
```

### 9.2 Isolated side

```text
pin11 GND2 -> GND_RS485_ISO
pin12 VISOOUT -> +3V3_RS485_ISO
pin13 MODE -> GND_RS485_ISO
pin14 IN -> GND_RS485_ISO
pin15 GISOIN -> GND_RS485_ISO
pin16 VISOIN -> +3V3_RS485_ISO
```

Critical net equivalence:

```text
VISOOUT12 and VISOIN16 = +3V3_RS485_ISO
GND2_11 and GISOIN15   = GND_RS485_ISO
```

Decoupling:

```text
VISOIN  -> 100n -> GND_RS485_ISO
VISOOUT -> 10n + 10u -> GND_RS485_ISO
```

### 9.3 Half-duplex bus combining

```text
Y17 -> 0R -> RS485_A_ISO
A20 -> 0R -> RS485_A_ISO

Z18 -> 0R -> RS485_B_ISO
B19 -> 0R -> RS485_B_ISO
```

Therefore:

```text
Y + A = A-line
Z + B = B-line
```

### 9.4 Termination

Selectable termination:

```text
RS485_A_ISO
   |
 120R
   |
 JP_TERM
   |
RS485_B_ISO
```

```text
R_RS485_TERM = 120R 1%
```

Termination is enabled only when this node is physically at the end of the RS-485 bus.

No external bias resistors are currently used.

### 9.5 Protection and connector

TVS:

```text
SM712
pin1 -> RS485_A_ISO
pin2 -> RS485_B_ISO
pin3 -> GND_RS485_ISO
```

TVS must be physically close to the external connector.

Minimum connector:

```text
1 A
2 B
3 COM
```

where:

```text
COM = GND_RS485_ISO
```

Critical isolation rule:

```text
GND_RS485_ISO != GND_SYS
```

They are not connected by normal PCB copper.

Under the ISOW1432 isolation barrier on **all copper layers**:

- no polygon pours;
- no tracks;
- no vias;
- no copper crossing the barrier.

Creepage/clearance must be validated against the final stackup, board contamination class, enclosure, and target regulatory environment.

---

## 10. Status indication

The enclosure-facing diagnostic concept uses one compact LED row suitable for individual apertures or a multi-channel light pipe.

Main indicators:

| LED | Target color | Meaning | Source |
|---|---|---|---|
| `PWR` | green | `+3V3_DIG` is present | power rail |
| `RUN` | green | firmware heartbeat / watchdog servicing | `WD_KICK` via high-impedance driver |
| `FAULT` | red | board-level hardware fault | `BOARD_FAULT_N` via driver |
| `AO` | amber | analog outputs are actually hardware-enabled | `AO_SAFE_EN` |
| `DO` | amber | digital outputs are actually hardware-enabled | `DO_SAFE_EN` |
| `SENS` | red | any sensor-power fault | derived from individual `SENSOR_PWR_FAULTx_N` states read through `TCAL9539`; final LED electrical drive TBD |
| `RS485` | amber | RS-485 transmit activity | `RS485_DE_RE` |

Ethernet indicators are provided by the MagJack:

- green LINK;
- yellow ACT.

Two additional service/debug LED footprints are reserved as DNP:

- `5V_PGOOD`;
- `3V3_PGOOD`.

These are intended for board bring-up and are not required on production assemblies.

### 10.1 RUN behavior

`RUN` is intentionally not assigned a new GPIO because the current GPIO map is fully allocated.

Target signal path:

```text
RP2350 GPIO2 / WD_KICK
   +-> TPS3851 watchdog input
   +-> high-impedance MOSFET/buffer input
       -> RUN LED
```

The LED must not directly load the watchdog input with LED current.

Normal behavior is **blinking heartbeat**, not permanently ON.

Firmware may toggle the watchdog service level, but watchdog timing has priority over visual blink rate. If the selected TPS3851 watchdog window requires a service rate that is too fast to produce a useful visible blink, a pulse-stretcher/divider should be inserted in the LED-only path rather than altering watchdog timing.

### 10.2 Diagnostic semantics

The intended quick diagnosis is:

```text
PWR off          -> board power absent / major power fault
PWR on, RUN off  -> MCU not executing normal firmware / reset / crash
RUN blinking     -> firmware execution + watchdog servicing alive
FAULT on         -> board hardware fault condition
AO on            -> AO_SAFE_EN is actually asserted
DO on            -> DO_SAFE_EN is actually asserted
SENS on          -> one or more sensor power channels report fault
RS485 blinking   -> transmit activity
```

AO and DO LEDs deliberately represent the **safe-gated hardware enable**, not only the MCU request, so they remain truthful during reset/watchdog fault conditions.

### 10.3 Digital-input state LEDs

The four DI state LEDs are separate from the board-status row:

```text
DI0_LED <- MAX22190 LED1
DI1_LED <- MAX22190 LED2
DI2_LED <- MAX22190 LED3
DI3_LED <- MAX22190 LED4
```

They are driven directly by the MAX22190 energyless LED outputs and indicate actual physical input activity. They require no firmware GPIO and no ordinary LED series resistor. They should be positioned where the enclosure can associate each indicator with the corresponding DI terminal.

---

## 11. Current PCB / mechanical baseline

The current floorplan baseline is a **120 × 90 mm** PCB.

Mechanical concept:

- 4 × M3 mounting holes;
- nominal drill: 3.2 mm;
- hole centers approximately `(5,5)`, `(115,5)`, `(5,85)`, `(115,85)` mm in the current placement coordinate system;
- component keepout around each mounting hole: 5 mm radius on both PCB sides;
- connector layout is intentionally compatible with an enclosure and top access.

### 11.1 Connector placement concept

Top region:

```text
AO terminal blocks
AI terminal block
RS-485 terminal block at/near the top edge

USB-C + RJ45 below the edge, next to each other
```

USB-C and RJ45 are both **top-entry**, so they are deliberately placed inboard rather than constrained to the PCB edge. This gives the enclosure designer freedom to place openings/light wells directly above them.

Bottom region:

```text
VIN | DI | DO | UIC
```

The lower terminal blocks are distributed with symmetric mechanical margins relative to the mounting geometry.

Sensor connectors are grouped on the right-hand side.

### 11.2 Component placement policy

The current placement concept contains the complete component population, including resistors, capacitors, ferrites, TVS parts, inductors, crystals, and optional/DNP parts.

Placement follows signal/current flow instead of placing passives in arbitrary rows around ICs.

Examples:

```text
VIN -> fuse/TVS -> eFuse -> regulators
switch node -> inductor -> output capacitor
terminal -> protection -> filter -> ADC/frontend
USB-C -> ESD -> series network -> RP2350
W5500 -> MDI matching -> MagJack
RS-485 connector -> TVS/termination -> isolated transceiver
DI connector -> pulse-rated 2512 resistors -> MAX22190
```

Critical/high-current/high-speed/protection components remain on TOP where practical. Small noncritical configuration/bias/DNP components may be placed on BOTTOM.

The latest placement concept also reserves a single LED row for enclosure light pipes plus the four DI channel LEDs close to their field terminals.

The `TCAL9539` must remain on the internal side of the I2C0 `TCA4307`; the two `TCA4307` devices should be placed near the transition from internal logic to the corresponding external sensor connector so that external bus faults/capacitance are isolated from the internal bus segment.

### 11.3 Current validation state of the scripted floorplan

The latest placement concept has been mechanically checked for:

- component body overlap;
- board-edge violations;
- mounting-hole keepout violations.

The scripted concept predates the newly frozen `TCAL9539` + 2 × `TCA4307` addition, completed AI protection population, the AO output/sense/protection circuitry, and the finalized DI MAX22190 frontend. It must be regenerated after those parts and the ADC monitor networks are placed.

The scripted concept is **not** a substitute for production Altium DRC, courtyard checks, exact footprint clearances, assembly access, or pin-level routing verification.

### 11.4 Layout-critical rules before routing

#### Switch-mode power

Placement must prioritize the real high-frequency current loops:

- input capacitor -> switcher -> PGND;
- SW -> inductor -> output capacitor -> PGND;
- bootstrap and compensation components immediately adjacent to their pins;
- no long SW copper;
- keep analog FB/VC nodes away from SW nodes.

For `LM76003`, `TPS62130`, and `LT8362`, final placement should follow the manufacturer layout examples closely.

#### USB

- ESD device immediately at USB-C;
- D+/D− routed as a controlled differential pair according to final stackup;
- no duplicate 27 Ω resistors;
- minimize stubs and layer changes;
- maintain a continuous reference plane.

#### Ethernet

- W5500 MDI network kept compact;
- TX/RX polarity as documented above, no crossover;
- symmetric pair routing;
- minimal stubs;
- magnetics close to MDI network;
- keep noisy switch nodes away from the MDI corridor.

#### RS-485 isolation

- explicit copper keepout under the isolation barrier on every layer;
- TVS adjacent to the external connector;
- isolated COM must remain in the isolated ground domain;
- creepage/clearance audit after real footprints and board stackup are fixed.

#### Analog

Prefer physically linear frontends:

```text
AI: field terminal -> primary protection -> secondary protection/filter -> ADC
AO: AD5422 -> internal clamp/protection -> series resistor -> field TVS -> terminal
```

Keep analog signal returns and reference-sensitive paths away from Ethernet, USB, switch nodes, and digital clock routing.

For AO specifically:

- `SMBJ15A` directly adjacent to the `AOx_OUT/AOx_RET` connector region;
- 33 Ω pulse-rated resistor between field-side `AOx_OUT` and internal `AOx_RAW`;
- `BAS70-04LT1G` close to the `AOx_RAW`/AD5422 region;
- sense pickup is Kelvin from `AOx_OUT` after the 33 Ω resistor;
- OPA2991 sense traces do not share the surge-current return;
- CCOMP/reference/bypass parts stay compact around the AD5422;
- positive clamp current can enter `+15V_AO`, so keep rail decoupling local and verify rail uplift in EMC testing.

Internal RP2350 rail-monitor divider/RC networks should be treated as diagnostic analog nodes and kept away from switch nodes. `VIN_PROTECTED_MON` must be designed for the full protected input range plus engineering margin, not only nominal 24 V operation.

#### Digital inputs

The DI block must be physically ordered from field side to logic side:

```text
board edge
 -> DG350-3.5-06P-14
 -> four CRCW2512 1.5k pulse resistors
 -> MAX22190
 -> SPI / logic region
```

Rules:

- keep the raw `DIx_IN` trace from the connector to its 2512 resistor short;
- keep the protected resistor-to-`INx` trace short and avoid vias where practical;
- do not place external filter capacitors on `INx`;
- route the two `DI_COM` connector pins into a wide local `GND_SYS` return adjacent to the MAX22190 ground/EP area;
- do not force DI transient return current to travel through the MCU, Flash, PSRAM or other logic regions;
- use solid `GND_SYS` under the MAX22190 EPAD and multiple thermal/ground vias consistent with the final fabrication process;
- place the VDD31 and VL16 100 nF bypass capacitors immediately adjacent to their corresponding pins; the 1 µF parts may be slightly farther away;
- place the `7.5k REFDI` and `24k REFWB` resistors close to pins 27/26 and keep their reference nodes away from SCLK and raw field traces;
- enter SPI/control routing from the logic side of the IC rather than through the field-input corridor;
- place `DI0_LED..DI3_LED` where each LED can be visually associated with its connector channel.

The actual footprint of `DG350-3.5-06P-14` must be checked for copper-to-copper clearance, not only nominal 3.5 mm pitch. Particular attention is required at the `DI3_IN` pin4 to `DI_COM` pin5 transition and around the 2512 resistor pads. ADI transient-testing experience showed that inadequate copper spacing can arc during high-voltage ESD even when the schematic topology is correct. Where footprint geometry permits, target at least approximately 2.5 mm local copper clearance in the field-exposed DI area rather than squeezing ground pours against field pads.

Reference-design ESD/EFT/surge levels do not become board claims until this exact layout, connector and enclosure are tested.

#### External I²C

- keep `TCAL9539` electrically inside the protected/internal I2C0 segment;
- one `TCA4307` per external I²C connector;
- provide pull-ups appropriate to each side of each buffer;
- avoid creating an external fault path that can hold the internal service bus low;
- default `TCA4307 EN` to external-bus-disconnected during reset;
- route `READY` back to the `TCAL9539` as documented above;
- route `DI_READY_N` to `TCAL9539 P14` as an internal slow diagnostic input.

---

## 12. Enclosure requirements implied by the PCB

The PCB is intended to be installed in an enclosure; mechanical design must therefore be developed together with the PCB rather than after routing.

Current assumptions:

- PCB mounted horizontally on four M3 standoffs;
- practical clearance below the PCB approximately 3–4 mm minimum, subject to bottom-side components and solder joints;
- additional top clearance above the tallest components;
- RJ45, USB-C, terminal blocks, inductors, and light pipes are the primary Z-height/mechanical drivers;
- top-entry USB/RJ45 require access from above;
- terminal blocks require screwdriver and wire-bend volume;
- the status LED row and the four DI state LEDs should align with enclosure apertures or a light-pipe assembly;
- no metal standoff/screw/insert may compromise the RS-485 isolation barrier;
- connector mating volumes must be treated as 3D keepouts, not only PCB body keepouts.

The PCB should remain at 120 × 90 mm until enclosure and routing checks demonstrate a real need to resize it.

---

## 13. Firmware-facing hardware contract

Firmware shall expose a machine-readable hardware capability descriptor containing at least:

- hardware profile and revision;
- number and type of AI/AO/DI/DO/UIC channels;
- supported electrical ranges and modes;
- available I²C/SPI/UART/RS-485 interfaces;
- Ethernet controller type;
- supported native drivers;
- calibration schema version;
- unavailable/unpopulated/DNP hardware features;
- board diagnostics and power-good/fault capabilities;
- internal ADC monitor availability;
- sensor-power switch count and per-port fault state;
- external I²C isolation/recovery status.

The configurator must read these capabilities before creating or applying a configuration.

Recommended identity:

```text
profile: RP2350_INDUSTRIAL_MVP_V1
```

Firmware should treat the frozen GPIO map in this document as the hardware abstraction source for board-level pin definitions.

Required firmware consequences of the 2026-08-28 service-I/O hardware decision:

- replace direct `GPIO44..47 -> SENSOR_PWR_EN0..3` control with a `TCAL9539` driver/service path;
- change GPIO39 from `SENSOR_PWR_FAULT_N` to `BOARD_IO_INT_N`;
- expose four independent TPS2553 fault states instead of only one combined fault;
- control and diagnose both `TCA4307` instances (`EN`/`READY`);
- use `TCAL9539 P14` as `DI_READY_N` from MAX22190 `/READY`;
- keep `TCAL9539 P15..P17` reserved;
- extend/validate the RP2350 generic ADC-voltage driver for ADC source indices 4..7 before internal rail monitors are enabled;
- preserve GPIO40..43 as `UIC0..3` with ADC0..3 capability reserved for a future validated analog UIC path.

Required AO firmware contract from the 2026-08-30 schematic:

- qualified board ranges are `0–10 V` and `4–20 mA` unless additional modes are explicitly validated;
- `AO_ENABLE_REQ` remains LOW until both DACs have been reset/configured and safe output codes are loaded;
- after power-up, perform an AD5422 software RESET and wait at least 40 µs before relying on configuration/calibration state, because `DVCC` and `AVDD` come from independent power rails and their sequencing is not guaranteed;
- configure/use external current-setting resistors (`REXT = 1`);
- keep `CLRSEL = 0` for the intended voltage-mode safe state;
- do not enable digital slew (`SREN`) for a safety-critical immediate clear unless a deliberate safe-state slew is specified;
- configure daisy-chain operation before normal 48-clock dual-DAC updates;
- transmit AO1 word first and AO0 word second before the common `AO_LATCH` rising edge;
- final AO1 SDO is not a shared `BOARD_SPI_MISO` source;
- interpret hardware CLEAR in `4–20 mA` mode as a `4 mA` safe-state, not a hard-off state;
- `BOARD_FAULT_N` combines both AD5422 open-drain FAULT outputs and MAX22190 `/FAULT`; firmware must query all relevant devices when this shared line is asserted;
- do not claim sustained 24/36 V AO miswire survival until that condition is separately designed and qualified.

Required DI firmware contract from the 2026-08-31 MAX22190 schematic:

- expose exactly **4 physical DI channels** and advertise them as IEC 61131-2 Type 1/Type 3, nominal 24 V;
- do not advertise Type 2 support for Hardware Prototype v1;
- use `DI_CS_N = GPIO16`, `BOARD_SPI_SCK = GPIO14`, `BOARD_SPI_MOSI = GPIO15`, and `BOARD_SPI_MISO = GPIO12`;
- use 24-bit non-daisy-chain MAX22190 SPI operation with CRC and SCLK idle LOW;
- observe `DI_READY_N` through `TCAL9539 P14` before relying on MAX22190 accesses;
- configure `INEN = 0x0F` after startup so IN5..IN8 are disabled;
- default `FLT1..FLT4 = 0x04` for 1.6 ms internal filtering with wire-break disabled;
- configure `CFG = 0x01` to enable REFDI-short diagnostics;
- default `FAULT2EN = 0x3C`, masking REFWB faults while no wire-break channel is active;
- default `GPO = 0x80` for sticky `/FAULT` handling;
- default `FAULT1EN = 0xF8`, keeping VDD24-low/missing and WBG masked while unused;
- clear and verify POR/diagnostic state using the official datasheet sequence before declaring DI healthy;
- use deterministic SPI polling for normal input changes; `/FAULT` is diagnostic, not a normal change-of-state interrupt;
- implement per-channel configurable debounce/filter time, inversion and edge/event processing above the hardware DI state;
- keep wire-break disabled by default and only allow it for a channel whose field-device/EOL configuration guarantees a valid OFF-state diagnostic current;
- do not imply that a plain dry-contact OFF state can be distinguished from a broken wire without a remote EOL/diagnostic path;
- when wire-break is enabled on at least one channel, explicitly manage `WBE`, `WBG`, `RFWBO/RFWBS`, stale clear-on-read flags and the associated FAULT enable masks;
- when `BOARD_FAULT_N` asserts, query MAX22190 as well as the AD5422 devices because all three open-drain fault sources share the line;
- do not claim sustained 36 V DI operation or final IEC/EMC certification until the assembled PCB is qualified.

---

## 14. Minimum diploma demonstration

The final prototype should demonstrate at least:

1. one analog input channel with scaling and alarm;
2. one analog output channel commanded through OPC UA with hardware-safe-state behavior;
3. one 24 V digital input with edge/event handling;
4. one protected high-side digital output;
5. one counter or quadrature encoder mode through UIC;
6. one digital sensor connected through I²C, SPI, or UART;
7. one RS-485/Modbus device or simulator;
8. OPC UA browsing, subscriptions, timestamps, quality, alarms, and history;
9. configuration persistence and restart recovery;
10. firmware and hardware diagnostics exposed through OPC UA;
11. visible power/run/fault status indication;
12. per-port sensor-power control/fault reporting and at least one demonstrated external-I²C recovery/power-cycle path;
13. internal voltage diagnostics for the populated RP2350 ADC monitor channels.

Not every physical channel must be simultaneously loaded during the defense, but every channel class must be implemented and verified.

---

## 15. Schematic status

Current status:

```text
01_POWER_INPUT      practically closed
02_POWER_RAILS      practically closed
02B_AO_POWER        practically closed
03_MCU_RP2350       practically closed; GPIO39/44..47 service-I/O update still needs final cross-sheet audit
04_MEMORY           practically closed
05_ETHERNET_USB     practically closed; final audit required
06_RS485            practically closed
07_ANALOG_INPUTS    practically closed; final PTC/footprint/pulse-rating audit required
08_ANALOG_OUTPUTS   practically closed; final annotate/ERC/footprint/ground-domain audit required
09_DIGITAL_INPUTS   practically closed; final footprint/layout/clearance/firmware-driver audit required
```

Before PCB release, `07_ANALOG_INPUTS` must still:

```text
- replace any remaining PTS120630V012 placeholders with PTS120660V005;
- freeze pulse-rated 100R parts;
- freeze 200R burden resistor power/temperature grade;
- perform ADS8688 pin-to-pad audit;
- verify DG350-3.5-08P-14 footprint/3D;
- validate the final sustained-overvoltage/IEC protection requirement.
```

Before PCB release, `08_ANALOG_OUTPUTS` must still:

```text
- ensure SN74LVC2G04 GND/bypass return is GND_SYS, not AGND;
- annotate all parts and remove placeholders;
- freeze exact RSET / 33R pulse-rated / CCOMP / bypass orderable parts;
- verify BAS70-04LT1G symbol pins 1/2/3 against PCB pads;
- verify SMBJ15A polarity and footprint;
- verify OPA2991IDR multi-part symbol-to-SOIC8 pad mapping;
- verify AD5422BREZ EPAD is schematic pin 25 -> footprint pad 25;
- verify one and only one AO_LATCH pull-up and BOARD_FAULT_N pull-up on the MCU sheet;
- confirm AO_ENABLE_REQ 100k default LOW;
- run a full AO0/AO1 net-label and daisy-chain audit;
- qualify the selected transient/EMC level; current protection is not a guaranteed sustained 24/36 V miswire solution.
```

Before PCB release, `09_DIGITAL_INPUTS` must still:

```text
- annotate all parts and remove placeholders;
- verify MAX22190ATJ+ symbol pins 1..32 and EPAD33 against the final TQFN footprint pads;
- verify EPAD33 is solidly connected to GND_SYS with the intended thermal/ground via pattern;
- confirm VDD31/VL16 local 100n || 1u placement and VDD24 pin30 NC;
- confirm one and only one DI_CS_N 10k pull-up and BOARD_FAULT_N pull-up;
- complete DI_READY_N -> TCAL9539 P14 on the service-I/O sheet;
- freeze exact orderable 7.50k REFDI, 24.0k REFWB and four DI LED parts;
- verify all four field resistors are CRCW25121K50FKEGHP / 1.5k / 2512 pulse-withstanding parts;
- verify DG350-3.5-06P-14 footprint, 3D body and real copper clearances, especially DI3_IN pin4 to DI_COM pin5;
- ensure no external capacitors are present on IN1..IN4;
- confirm LED1..LED4 drive fitted LEDs to GND_SYS with no ordinary series resistor and LED5..LED8 are tied to GND_SYS;
- confirm IN5..IN8 are tied to GND_SYS and firmware disables them with INEN=0x0F;
- run a complete BOARD_SPI/DI_CS_N/BOARD_FAULT_N/DI_READY_N cross-sheet net audit;
- implement and verify the CRC-protected MAX22190 driver/startup sequence;
- qualify 24 V operation, sustained 36 V behavior if it is to be claimed, and the final ESD/EFT/surge levels on the assembled PCB.
```

The 2026-08-28 service-I/O architecture decision still requires schematic completion/audit for:

```text
- TCAL9539 internal service I/O expander;
- 2 × TCA4307, one per external I²C port;
- SENSOR_PWR_EN0..3 moved from RP2350 GPIO44..47 to TCAL9539 P00..P03;
- four TPS2553 fault outputs routed individually to TCAL9539 P04..P07;
- TCA4307 EN/READY routed through TCAL9539 P10..P13;
- DI_READY_N routed from MAX22190 /READY to TCAL9539 P14;
- TCAL9539 /INT connected to GPIO39 / BOARD_IO_INT_N;
- TCAL9539 /RESET connected to SYS_RESET_N;
- VIN_PROTECTED, +15V_AO and +5V_ANALOG monitor networks added to ADC4..ADC6;
- ADC7 monitor input reserved;
- UIC frontend revisited if analog UIC mode is retained, because TLV3214 comparator output is not an analog path.
```

Before the PCB is considered release-ready, all sheets require one full-system pass:

```text
Annotate
Compile / Validate
ERC
pin-to-pad audit
footprint audit
3D/mechanical audit
cross-sheet net audit
final BOM audit
PCB DRC
```

The discrete board-status LED circuitry, DI state LEDs, and service-I/O/I²C-recovery circuitry must also be represented in the schematic and included in this audit.

---

## 16. Remaining schematic work after analog I/O and DI

The analog input/output and digital-input sheets are now topology-complete enough to move on after the final cleanup listed in section 15.

Remaining major hardware blocks still requiring schematic completion/finalization include:

- protected high-side outputs around `IPS2050HTR`;
- UIC/counter/frequency/encoder frontend and the decision on an actual analog path to ADC0..3;
- external sensor connectors, their TPS2553 power switches, `TCAL9539`, and both `TCA4307` devices;
- internal ADC4..ADC6 rail-monitor dividers/filters;
- final board-status LED driver circuitry.

Exact sheet numbering/order after `09_DIGITAL_INPUTS` should follow the Altium project organization; do not renumber already-completed sheets merely to match this document.

---

## 17. Deferred product features

Explicitly outside the diploma MVP:

- user-replaceable I/O modules;
- internal expansion slots;
- second Ethernet port / embedded Ethernet switch;
- network redundancy protocols;
- analog `±10 V` outputs;
- MAX22190 quad Type 2 DI topology using paired internal channels;
- full per-channel galvanic isolation;
- relay-output variants;
- CAN/CAN-FD and IO-Link;
- production enclosure tooling;
- CE/UL certification and complete IEC 61000 validation;
- field-upgradeable native driver binaries.

These belong to the post-diploma product roadmap and must not block completion of the MVP.

---

## 18. Frozen MVP summary

```text
RP2350 Industrial OPC UA Node — Hardware MVP

MCU      : RP2350B A4
Flash    : 16 MB W25Q128JVBIQ
PSRAM    : 8 MB APS6404L-3SQR-SN
PCB      : 120 × 90 mm baseline, monolithic
Power    : 9–36 V DC input

4 × AI   : 0–10 V / 0–20 mA / 4–20 mA target via ADS8688
           protected PTC + primary/secondary TVS topology

2 × AO   : 0–10 V / 4–20 mA via 2 × AD5422BREZ
           +15V_AO, OPA2991 buffered remote sense
           33R + SMBJ15A + BAS70-04 protection
           DG350-3.5-02P-14 OUT/RET per channel
           current-mode hardware CLEAR safe state = 4 mA

4 × DI   : MAX22190ATJ+
           IEC 61131-2 Type 1 / Type 3, nominal 24 V
           DI0..DI3 -> IN1..IN4; IN5..IN8 unused/grounded
           7.50k REFDI, 24.0k REFWB
           1.5k CRCW2512 pulse-rated series resistor per channel
           DG350-3.5-06P-14: DI0..DI3 + 2 × DI_COM
           VDD/VL = +3V3_DIG, VDD24 = NC
           24-bit CRC SPI, DI_CS_N = GPIO16
           /FAULT -> shared BOARD_FAULT_N
           /READY -> DI_READY_N -> TCAL9539 P14
           4 × hardware energyless DI state LED
           internal programmable filtering, default 1.6 ms
           optional per-channel wire-break with valid field diagnostic current
           Type 2 deferred to a future board revision

2 × DO   : protected 24 V high-side
4 × UIC  : counter / frequency / encoder / digital input
           ADC0..3 capability reserved; analog frontend path still to be finalized

2 × I2C  : 3.3 V external sensor ports, each through TCA4307 recovery buffer
1 × SPI  : 3.3 V sensor port
1 × UART : 3.3 V sensor port
1 × RS-485 isolated

Sensor I/O control:
1 × TCAL9539 16-bit internal service GPIO expander
4 × TPS2553 independently controlled power switches with individual faults
2 × TCA4307 external-I2C isolation/recovery channels
GPIO39 = BOARD_IO_INT_N
P14 = DI_READY_N
P15..P17 reserved

RP2350 ADC board diagnostics:
ADC4 = VIN_PROTECTED_MON
ADC5 = AO_15V_MON
ADC6 = ANA_5V_MON
ADC7 = reserved monitor

1 × W5500 10/100 Ethernet
1 × top-entry RJ45 MagJack
1 × top-entry USB-C service/recovery

Watchdog : TPS3851H33
Safe AO  : AO_SAFE_EN = SYS_RESET_N AND AO_ENABLE_REQ
           AOx_CLEAR = !AO_SAFE_EN
Safe DO  : SYS_RESET_N AND DO_ENABLE_REQ

LEDs     : PWR / RUN / FAULT / AO / DO / SENS / RS485
DI LEDs  : DI0 / DI1 / DI2 / DI3, hardware-driven by MAX22190
RJ45 LED : LINK / ACT
Debug DNP: 5V_PGOOD / 3V3_PGOOD
```

This document is the hardware-side baseline for firmware integration. Any deliberate change to pin allocation, component selection, electrical interface, or safety behavior must update this file together with the affected schematic and firmware board profile.

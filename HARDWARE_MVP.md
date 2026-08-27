# RP2350 Industrial OPC UA Node — Hardware MVP

> **Status:** living source of truth for Hardware Prototype v1 / firmware through `v1.0.0` unless deliberately revised.
>
> **Board profile:** `RP2350_INDUSTRIAL_MVP_V1`
>
> **Last hardware consolidation:** 2026-08-27

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
- 4 × universal/counter/frequency/encoder input;
- 2 × I²C sensor ports;
- 1 × SPI sensor port;
- 1 × UART sensor port;
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
| Analog outputs | 2 × `AD5422BREZ` | voltage/current output |
| Digital inputs | `MAX22190ATJ+` | industrial DI frontend |
| Digital outputs | `IPS2050HTR` | protected high-side outputs |
| Universal inputs | `TLV3214PWR` | UIC frontend |
| Supervisor/watchdog | `TPS3851H33EDRBR` | reset + watchdog |
| Safe-state logic | `SN74LVC2G08DCTR` | AO/DO enable gating |
| AO optional logic | `SN74LVC2G04DBVR` | optional |
| Sensor power switches | 4 × `TPS2553DBVR` | current-limited switched sensor power |

### 2.1 LED support components added to the PCB concept

The current PCB concept includes discrete status LEDs and small transistor/buffer stages so diagnostic nets are not directly burdened by LED current.

Current implementation concept:

- `2N7002`-class N-MOSFET for active-high driven LEDs;
- `BSS84`-class P-MOSFET / suitable active-low buffer stage where needed;
- 0603 LEDs;
- dedicated current-limiting resistors.

These LED driver details must be captured and audited in the final schematic before PCB release.

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

Detailed AI channel circuitry is not yet frozen; see the open work section.

### 3.2 Analog outputs

Provide **2 analog output channels**: `AO0..AO1`.

Target ranges:

- `0–10 V`;
- `0–20 mA`;
- `4–20 mA`.

Bipolar `±10 V` output is out of scope for the MVP.

Implementation uses 2 × `AD5422BREZ` and the dedicated `+15V_AO` rail.

Each channel shall define:

- commanded value;
- electrical mode;
- startup value;
- communication-loss/watchdog value;
- slew-rate limit where supported;
- readback/diagnostic status where practical;
- deterministic hardware-safe state during MCU reset.

### 3.3 Digital inputs

Provide **4 digital input channels**: `DI0..DI3`.

Target electrical level:

- nominal `24 V DC`;
- protected and filtered frontend;
- interrupt or deterministic polling support.

Target frontend IC: `MAX22190ATJ+`.

Firmware shall support debounce/filter time, inversion, edge detection, event counting, and quality diagnostics.

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
- optional low-voltage GPIO-style mode where electrically safe.

The frontend uses `TLV3214PWR`.

Counter and encoder processing should use RP2350 PIO and DMA where appropriate rather than nondeterministic task-level GPIO polling.

Typical configurations:

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
- intended for short cables inside an enclosure or laboratory setup.

I²C is not treated as a long-distance industrial field bus.

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

### 4.4 Sensor power

Four current-limited sensor power channels are implemented using 4 × `TPS2553DBVR`.

The MCU has four independent enables and one combined sensor-power fault input.

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
```

Hardware output gating uses `SN74LVC2G08DCTR`:

```text
AO_SAFE_EN = SYS_RESET_N AND AO_ENABLE_REQ
DO_SAFE_EN = SYS_RESET_N AND DO_ENABLE_REQ
```

The physical output stages must therefore remain disabled while the MCU is in reset or the external supervisor is asserting reset.

---

## 6. RP2350B core — sheet `03_MCU_RP2350`

Status: **practically closed**.

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

GPIO39 SENSOR_PWR_FAULT_N

GPIO40 UIC0_IN
GPIO41 UIC1_IN
GPIO42 UIC2_IN
GPIO43 UIC3_IN

GPIO44 SENSOR_PWR_EN0
GPIO45 SENSOR_PWR_EN1
GPIO46 SENSOR_PWR_EN2
GPIO47 SENSOR_PWR_EN3
```

All GPIOs are currently allocated. New firmware-controlled features should not assume a spare GPIO exists.

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
| `SENS` | red | sensor-power fault | `SENSOR_PWR_FAULT_N` via driver |
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
SENS on          -> sensor power fault
RS485 blinking   -> transmit activity
```

AO and DO LEDs deliberately represent the **safe-gated hardware enable**, not only the MCU request, so they remain truthful during reset/watchdog fault conditions.

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
AO terminal block
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
```

Critical/high-current/high-speed/protection components remain on TOP where practical. Small noncritical configuration/bias/DNP components may be placed on BOTTOM.

The latest placement concept also reserves a single LED row for enclosure light pipes.

### 11.3 Current validation state of the scripted floorplan

The latest placement concept has been mechanically checked for:

- component body overlap;
- board-edge violations;
- mounting-hole keepout violations.

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

Prefer a physically linear frontend:

```text
field terminal -> protection -> input/output conditioning -> ADC/DAC
```

Keep analog signal returns and reference-sensitive paths away from Ethernet, USB, switch nodes, and digital clock routing.

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
- the status LED row should align with enclosure apertures or a light-pipe assembly;
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
- board diagnostics and power-good/fault capabilities.

The configurator must read these capabilities before creating or applying a configuration.

Recommended identity:

```text
profile: RP2350_INDUSTRIAL_MVP_V1
```

Firmware should treat the frozen GPIO map in this document as the hardware abstraction source for board-level pin definitions.

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
11. visible power/run/fault status indication.

Not every physical channel must be simultaneously loaded during the defense, but every channel class must be implemented and verified.

---

## 15. Schematic status

Current status:

```text
01_POWER_INPUT      practically closed
02_POWER_RAILS      practically closed
02B_AO_POWER        practically closed
03_MCU_RP2350       practically closed
04_MEMORY           practically closed
05_ETHERNET_USB     practically closed; final audit required
06_RS485            practically closed
07_ANALOG_INPUTS    next sheet / not yet frozen
```

Before the PCB is considered release-ready, the closed sheets still require one full-system pass:

```text
Annotate
Compile / Validate
ERC
pin-to-pad audit
footprint audit
3D/mechanical audit
final BOM audit
PCB DRC
```

The newly added discrete LED circuitry must also be represented in the schematic and included in this audit.

---

## 16. Next schematic work — `07_ANALOG_INPUTS`

Main IC:

```text
ADS8688IDBTR
```

Architectural requirement:

```text
4 × Analog Input
```

Before the external AI channel circuit is replicated, the following must be frozen:

- which four ADS8688 channels are used;
- treatment of the remaining four ADC channels;
- final supported ranges;
- exact 0–10 V / current-loop frontend topology;
- external protection;
- shunt selection for current measurement;
- RC anti-alias filtering;
- analog-ground topology;
- ADC reference/power implementation;
- SPI connection;
- `ADC_RST_PD_N` behavior;
- `ADC_CS_N` behavior.

Workflow:

1. verify the `ADS8688IDBTR` symbol against the official datasheet;
2. verify supply/reference requirements;
3. design one complete AI channel;
4. validate electrical ranges/protection/filtering;
5. replicate it to four channels;
6. define behavior of unused ADC channels.

---

## 17. Deferred product features

Explicitly outside the diploma MVP:

- user-replaceable I/O modules;
- internal expansion slots;
- second Ethernet port / embedded Ethernet switch;
- network redundancy protocols;
- analog `±10 V` outputs;
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

4 × AI   : 0–10 V / 0–20 mA / 4–20 mA target
2 × AO   : 0–10 V / 0–20 mA / 4–20 mA
4 × DI   : protected 24 V
2 × DO   : protected 24 V high-side
4 × UIC  : counter / frequency / encoder / digital input

2 × I2C  : 3.3 V sensor ports
1 × SPI  : 3.3 V sensor port
1 × UART : 3.3 V sensor port
1 × RS-485 isolated

1 × W5500 10/100 Ethernet
1 × top-entry RJ45 MagJack
1 × top-entry USB-C service/recovery

Watchdog : TPS3851H33
Safe AO  : SYS_RESET_N AND AO_ENABLE_REQ
Safe DO  : SYS_RESET_N AND DO_ENABLE_REQ

LEDs     : PWR / RUN / FAULT / AO / DO / SENS / RS485
RJ45 LED : LINK / ACT
Debug DNP: 5V_PGOOD / 3V3_PGOOD
```

This document is the hardware-side baseline for firmware integration. Any deliberate change to pin allocation, component selection, electrical interface, or safety behavior must update this file together with the affected schematic and firmware board profile.

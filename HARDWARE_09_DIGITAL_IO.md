# RP2350 Industrial OPC UA Node — Sheet 09 DIGITAL IO

> **Status:** schematic topology practically closed for Hardware Prototype v1.
>
> **Sheet:** keep schematic sheet number `09`; conceptually the page is `09_DIGITAL_IO` because it contains the existing MAX22190 digital-input block plus the new IPS2050H digital-output block.
>
> **Role:** detailed implementation companion to `HARDWARE_MVP.md`. `HARDWARE_MVP.md` remains the architecture source of truth.

This document records the final schematic decisions, electrical contracts, protection strategy, safe-state behavior, firmware-facing semantics, and PCB constraints for sheet `09_DIGITAL_IO`.

---

## 1. Sheet scope

Sheet `09_DIGITAL_IO` contains:

- 4 × industrial digital input channels `DI0..DI3` based on `MAX22190ATJ+`;
- 2 × protected industrial high-side digital output channels based on `IPS2050HTR`;
- hardware safe-state gating for both DO channels;
- per-channel DO fault diagnostics;
- field-side DO supply decoupling and output filtering;
- DO field connectors and DI connector.

The digital-output design target is:

```text
2 × protected industrial high-side DO
nominal field level: 24 VDC
qualified continuous current target: 1.0 A/channel
both channels may be ON simultaneously
short-circuit/current-limit/thermal diagnostics
hardware deterministic OFF during reset/watchdog/boot
```

The DO stage is not galvanically isolated from `GND_SYS`. The TLP383 devices are used as power-domain/level-interface elements and for deterministic fail-safe behavior; they are not being used to claim field isolation of the board.

---

## 2. Frozen MCU allocation

```text
GPIO10 -> DO_ENABLE_REQ

GPIO20 -> DO1_CMD
GPIO21 -> DO2_CMD
GPIO22 <- DO1_FAULT
GPIO23 <- DO2_FAULT
```

The existing supervisor/safe-state logic provides:

```text
DO_SAFE_EN = SYS_RESET_N AND DO_ENABLE_REQ
```

Required hardware default biases:

```text
DO_ENABLE_REQ -> 100k -> GND_SYS
DO1_CMD       -> 100k -> GND_SYS
DO2_CMD       -> 100k -> GND_SYS
```

These defaults ensure that MCU reset, Hi-Z GPIO state, boot delay, or watchdog reset cannot accidentally assert an output.

---

## 3. Existing digital-input block

The DI portion of sheet 09 remains the frozen `MAX22190ATJ+` implementation already documented in `HARDWARE_MVP.md`.

Physical channel mapping:

```text
DI0 -> MAX22190 IN1 pin1
DI1 -> MAX22190 IN2 pin3
DI2 -> MAX22190 IN3 pin5
DI3 -> MAX22190 IN4 pin7
```

The four used field inputs each use:

```text
DIx_IN -> CRCW25121K50FKEGHP 1.5k -> MAX22190 INx
```

Shared references:

```text
REFDI pin27 -> 7.50k 1% -> GND_SYS
REFWB pin26 -> 24.0k 1% -> GND_SYS
```

Unused MAX22190 input/LED channels remain grounded as documented in `HARDWARE_MVP.md`. `VDD24` remains NC; `VDD` and `VL` use `+3V3_DIG`.

DI terminal block:

```text
DG350-3.5-06P-14
1 DI0_IN
2 DI1_IN
3 DI2_IN
4 DI3_IN
5 DI_COM
6 DI_COM

DI_COM -> GND_SYS
```

MAX22190 `/FAULT` remains part of the shared open-drain `BOARD_FAULT_N` network. `/READY` remains `DI_READY_N` for the internal service expander.

The four hardware DI LEDs remain driven directly from MAX22190 `LED1..LED4` energyless current outputs and therefore use no ordinary series current-limiting resistors.

---

## 4. DO field-power architecture

The IPS2050H must **not** be powered from `VIN_PROTECTED` behind the `TPS26600` system eFuse. The TPS26600 current-limit capability is lower than the IPS2050H short-circuit/current-limit region and would cause a DO short to disturb the system rail before the IPS2050H protection could operate normally.

The DO field-power branch therefore splits directly from `VIN_RAW` and has its own fuse, surge protection and reverse-polarity diode:

```text
J1 +9...36 V
    |
  VIN_RAW
    |
    +---------------------- system branch -> F1 -> VIN_FUSED -> TPS26600 -> VIN_PROTECTED
    |
    +-> F_DO
        0454005.MR
        Littelfuse NANO2 454
        5 A slow-blow
        |
      VFIELD
        |
        +---- 5.0SMDJ36CA ---- GND_SYS
        |
      STPST10H100SBYTR
        |
      VFIELD_OUT
        |
      IPS2050H VCC / EPAD
```

### 4.1 F_DO

Frozen DO-branch fuse:

```text
F_DO = Littelfuse 0454005.MR
5 A
slow-blow / time-delay
125 VDC class
```

The fuse protects against catastrophic field-branch faults. Normal channel short-circuit handling is performed by the IPS2050H current-limit and thermal-protection mechanisms; the fuse is not intended to open on every ordinary output short.

### 4.2 Field TVS

Frozen field-branch TVS candidate:

```text
5.0SMDJ36CA
bidirectional
VFIELD <-> GND_SYS
```

Placement is **after `F_DO` but before the reverse-polarity Schottky**.

This placement prevents the 100 V reverse-polarity diode from being required to absorb an unclamped large negative field transient by itself.

The 36 V TVS choice permits sustained board operation at the top end of the 9–36 V input range. It remains a qualification item: real surge clamp/overshoot must be measured on the final PCB directly at `VFIELD` and `VFIELD_OUT` before any IEC surge level is claimed.

### 4.3 Reverse-polarity diode

Frozen part:

```text
STPST10H100SBYTR
100 V / 10 A Schottky, DPAK
```

Power pin mapping:

```text
pins 1 + 3 = anodes  -> VFIELD
pin 2 / TAB = cathode -> VFIELD_OUT
```

Both anode terminals must be connected to the same `VFIELD` copper region.

At approximately 2 A aggregate DO load, the Schottky is expected to dissipate roughly 1 W order-of-magnitude and therefore requires substantial copper on its cathode/tab `VFIELD_OUT` node.

---

## 5. DO supply semantics

`IPS2050H` is a high-side switch, not a 24 V regulator. The DO voltage follows the external field/main supply minus the normal fuse/Schottky/switch drops.

Therefore:

```text
J1 = 9 V  -> DO approximately follows 9 V
J1 = 12 V -> DO approximately follows 12 V
J1 = 24 V -> nominal 24 V industrial DO operation
J1 = 36 V -> DO approximately follows 36 V
```

Hardware Prototype v1 shall describe these as **nominal 24 V high-side outputs with supply-following field voltage**. The board-wide 9–36 V input operating range does not mean that a fixed 24 V DO voltage is generated at every input voltage.

The exact production/qualification window around nominal 24 V shall be established during hardware validation rather than inferred from the 9–36 V board-input range.

---

## 6. IPS2050HTR pin mapping

Package: `PowerSSO-24`.

Altium exposed pad is represented as pad `25` and is electrically `VCC`, not ground.

```text
1      VCC       -> VFIELD_OUT
2      IN2       <- IPS_IN2
3      IPD2
4      FLT2
5..12  OUT2      -> DO2_OUT
13..20 OUT1      -> DO1_OUT
21     FLT1
22     IPD1
23     IN1       <- IPS_IN1
24     GND       -> GND_SYS
25     EPAD/VCC  -> VFIELD_OUT
```

All eight pins in each `OUT` group must be physically joined to the corresponding output copper region:

```text
pins 5..12  -> one DO2_OUT copper/net
pins 13..20 -> one DO1_OUT copper/net
```

The exposed pad must be soldered to the `VFIELD_OUT` thermal/power copper. Do **not** connect EPAD thermal vias to ground.

---

## 7. Local IPS2050H supply decoupling

At the IPS2050H:

```text
VFIELD_OUT -> 100nF / 100V -> GND_SYS
VFIELD_OUT -> 2.2uF / 100V -> GND_SYS
```

The `100 nF / 100 V` capacitor is the local HF bypass and should be immediately adjacent to the VCC/GND region.

The `2.2 uF / 100 V` capacitor is the local field-supply reservoir and follows the ST reference implementation.

---

## 8. Hardware safe-state command gating

A second `SN74LVC2G08DCTR` is used on sheet 09 because the original dual AND is already consumed generating `AO_SAFE_EN` and `DO_SAFE_EN`.

Second dual AND:

```text
SN74LVC2G08DCTR

1A <- DO_SAFE_EN
1B <- DO1_CMD
1Y -> AND1_OUT

2A <- DO_SAFE_EN
2B <- DO2_CMD
2Y -> AND2_OUT

VCC -> +3V3_DIG
GND -> GND_SYS
100nF from VCC to GND_SYS adjacent to the IC
```

This implements:

```text
AND1_OUT = DO_SAFE_EN AND DO1_CMD
AND2_OUT = DO_SAFE_EN AND DO2_CMD
```

The AND-gate outputs do not drive IPS2050H IN pins directly. Direct connection would create an undesirable power-sequencing case where a live 3.3 V logic output could drive an IPS2050H input while `VFIELD_OUT/VCC` is absent.

Instead each channel uses a `TLP383` phototransistor interface.

### 8.1 DO1 command interface

```text
AND1_OUT
   |
 390R
   |
TLP383 #DO1 pin1 anode
TLP383 #DO1 pin3 cathode -> GND_SYS

VFIELD_OUT -> TLP383 #DO1 pin6 collector
TLP383 #DO1 pin4 emitter -> IPS_IN1
IPS_IN1 -> 22k -> GND_SYS
IPS_IN1 -> IPS2050H pin23 IN1
```

### 8.2 DO2 command interface

```text
AND2_OUT
   |
 390R
   |
TLP383 #DO2 pin1 anode
TLP383 #DO2 pin3 cathode -> GND_SYS

VFIELD_OUT -> TLP383 #DO2 pin6 collector
TLP383 #DO2 pin4 emitter -> IPS_IN2
IPS_IN2 -> 22k -> GND_SYS
IPS_IN2 -> IPS2050H pin2 IN2
```

The `390R` values provide a comfortably driven TLP383 LED from the 3.3 V logic rail. The `22k` pull-downs guarantee OFF on the IPS side if the optocoupler is off or either power domain disappears.

Recommended `22k` implementation is at least 0805 / 0.125 W class, preferably 0.25 W class, because the resistor can see the full field voltage when the command is asserted.

### 8.3 Safe-state truth behavior

```text
SYS_RESET_N = 0
    -> DO_SAFE_EN = 0
    -> both AND outputs LOW
    -> command TLP383 LEDs OFF
    -> IPS_IN1/IPS_IN2 pulled LOW by 22k
    -> both DO channels OFF

DO_ENABLE_REQ = 0
    -> same result

DOx_CMD = 0
    -> corresponding channel OFF

+3V3_DIG lost
    -> command optocouplers OFF
    -> IPS inputs pulled LOW
    -> outputs OFF

VFIELD_OUT lost / F_DO open
    -> IPS and TLP383 collector side unpowered
    -> no 3.3 V back-drive into IPS inputs
    -> outputs OFF
```

This is the required deterministic hardware-safe OFF behavior.

---

## 9. IPD configuration and current-limit mode

Both channels use the IPS2050H single LOW-threshold current-limit configuration:

```text
IPD1 pin22 -> 220k -> IPS_IN1 / IN1 pin23
IPD2 pin3  -> 220k -> IPS_IN2 / IN2 pin2
```

This deliberately avoids the high-current startup peak mode.

Relevant protection region for this configuration is approximately:

```text
IPKL typ    ~5.2 A
ILIML       2.5...4.5 A
ILIML typ   ~3.5 A
```

The board-qualified continuous operating target remains only:

```text
1.0 A/channel continuous
2.0 A aggregate with both channels ON
```

The higher silicon current capability is protection/inrush headroom and is not a claimed continuous board rating for Hardware Prototype v1.

---

## 10. FLT1 / FLT2 diagnostic interface

`FLT1` and `FLT2` are **not open-drain outputs**. They are IPS2050H diagnostic current-source outputs and must not be connected as if they were wired-OR open-drain GPIOs.

Each FLT pin drives a TLP383 LED through `2.2k`, following the ST reference approach.

### 10.1 Channel 1

```text
IPS2050H pin21 FLT1
        |
       2.2k
        |
TLP383 fault #1 pin1 anode
TLP383 fault #1 pin3 cathode -> GND_SYS

TLP383 pin6 collector -> DO1_FAULT
TLP383 pin4 emitter   -> GND_SYS
DO1_FAULT -> 10k -> +3V3_DIG
DO1_FAULT -> RP2350 GPIO22
```

### 10.2 Channel 2

```text
IPS2050H pin4 FLT2
        |
       2.2k
        |
TLP383 fault #2 pin1 anode
TLP383 fault #2 pin3 cathode -> GND_SYS

TLP383 pin6 collector -> DO2_FAULT
TLP383 pin4 emitter   -> GND_SYS
DO2_FAULT -> 10k -> +3V3_DIG
DO2_FAULT -> RP2350 GPIO23
```

The MCU nets retain the frozen names `DO1_FAULT` / `DO2_FAULT`, but their electrical polarity is active-low:

```text
DOx_FAULT = 1 -> no fault asserted
DOx_FAULT = 0 -> fault asserted
```

Firmware must invert the raw GPIO semantic when exposing a logical `fault=true/false` property.

The fault optocoupler emitters connect directly to `GND_SYS`; no emitter resistor is used.

---

## 11. Output power path

### 11.1 DO1

```text
VFIELD_OUT
   |
IPS2050H channel 1
   |
pins 13..20 OUT1
   |
DO1_OUT
   |
DO1 terminal
```

### 11.2 DO2

```text
VFIELD_OUT
   |
IPS2050H channel 2
   |
pins 5..12 OUT2
   |
DO2_OUT
   |
DO2 terminal
```

Each output uses a two-contact field connector:

```text
DO1 connector:
1 -> DO1_OUT
2 -> GND_SYS

DO2 connector:
1 -> DO2_OUT
2 -> GND_SYS
```

The intended connector family is the same 3.5 mm pluggable/screw-terminal family already used elsewhere on the board; `DG350-3.5-02P-14` is suitable for the final implementation if used consistently with the mechanical design.

---

## 12. Output filtering and inductive loads

Each physical output has one HF capacitor:

```text
DO1_OUT -> 100nF / 100V -> GND_SYS
DO2_OUT -> 100nF / 100V -> GND_SYS
```

No ordinary external flyback diode is used across the load/output.

No per-output TVS is required in the present topology.

The IPS2050H provides internal fast-demagnetization / active-clamp behavior for inductive loads. Adding a conventional `OUT -> VFIELD_OUT` flyback diode would suppress the intended fast-demagnetization voltage and substantially slow coil current decay.

The field-rail TVS is on `VFIELD`, not on each `OUTx` node.

---

## 13. Electrical/thermal qualification basis

### 13.1 Continuous load

Frozen Hardware Prototype v1 target:

```text
DO1: 1.0 A continuous
DO2: 1.0 A continuous
both simultaneously
```

Using a conservative `RDS(on)` around 50 mOhm per channel at elevated junction temperature gives approximately:

```text
P_channel ~= 1^2 * 0.05 = 50 mW
P_two_channels ~= 100 mW
```

The IPS2050H itself therefore has large thermal margin at the qualified 1 A/channel target provided that its `VFIELD_OUT` exposed-pad copper is implemented correctly.

The reverse-polarity Schottky is a more significant continuous heat source. At approximately 2 A aggregate field current its dissipation is of order 1 W and must be treated as a thermal-layout component.

### 13.2 Hard short

In LOW-threshold mode, a hard short can force several amperes through a channel until current limiting and thermal protection operate.

Approximate worst-case instantaneous switch dissipation can reach very high values, e.g. order 100 W at 24 V and ~4.5 A, but this is a transient linear-mode condition managed by the IPS2050H current-limit and thermal-shutdown system; it is not a continuous operating point.

`F_DO` is intentionally time-delay so that ordinary channel short-circuit protection is handled by the IPS rather than by repeatedly opening the board fuse.

### 13.3 Inductive load

The IPS2050H internal active clamp is the primary inductive-energy protection mechanism.

Useful energy relation for validation:

```text
E_L = 0.5 * L * I^2
```

At the 1 A qualified current:

```text
100 mH -> 0.05 J
500 mH -> 0.25 J
1 H    -> 0.50 J
```

A practical prototype qualification point is approximately `1 H @ 1 A` for a single turn-off event, followed by repetitive-duty thermal validation at realistic switching rates.

### 13.4 Capacitive/inrush load

The selected LOW-threshold current-limit configuration intentionally reduces startup stress compared with the IPS2050H high-current peak mode.

A useful prototype test point is approximately:

```text
CLOAD = 1000 uF
VFIELD ~= 24 V
```

This must be validated experimentally for startup time, fault behavior and thermal cycling; no universal maximum capacitive-load rating is claimed from the schematic alone.

---

## 14. PCB layout requirements

### 14.1 VFIELD branch

Keep the high-current branch compact:

```text
J1 VIN_RAW
 -> F_DO
 -> VFIELD
 -> STPST10H100SBYTR
 -> VFIELD_OUT
 -> IPS2050H
```

The unfused `VIN_RAW` trace from the J1 split to `F_DO` must be as short and physically protected as practical.

Place `5.0SMDJ36CA` close to the field-power entry branch with a compact loop:

```text
VFIELD -> TVS -> GND_SYS/power-entry return
```

Do not route TVS surge current through MCU/supervisor ground areas.

### 14.2 STPST10H100SBYTR

The DPAK tab/cathode is `VFIELD_OUT` and requires a substantial copper area.

Use wide copper and, where stackup permits, multiple current/thermal vias into additional `VFIELD_OUT` copper on another layer. Do not thermally choke the DPAK tab with thin spokes intended for small-signal components.

### 14.3 IPS2050H EPAD

The PowerSSO-24 exposed pad is `VFIELD_OUT`, not ground.

Provide a solid `VFIELD_OUT` thermal region beneath the part and thermal/current vias only into other `VFIELD_OUT` copper regions.

### 14.4 OUT1 / OUT2

All eight package pins per output must enter a broad copper region quickly. Do not fan eight power pins into one narrow signal-width trace.

Preferred topology:

```text
OUT1 pins 13..20 -> broad DO1_OUT polygon/trace -> connector
OUT2 pins 5..12  -> broad DO2_OUT polygon/trace -> connector
```

Place each `100 nF / 100 V` output capacitor close to the corresponding output/power-driver region.

### 14.5 Ground/current return

The field-load current return is `GND_SYS`, but physical routing must avoid sending ampere-level DO current through the MCU/SPI/supervisor ground region.

Conceptually:

```text
DO connector GND
    |
wide field-current return copper
    |
J1/power-entry GND region
```

Logic/supervisor ground branches should join the common ground network without becoming part of the high-current DO load loop.

### 14.6 Logic/interface placement

Keep the second `SN74LVC2G08` and its `100 nF` bypass in the low-voltage logic region.

Place command TLP383 devices between the low-voltage logic region and IPS2050H input region. Place the `22k` IPS input pull-downs close to `IN1/IN2`.

Place FLT TLP383 devices and the `2.2k` input resistors close enough to the IPS diagnostics that the FLT pins are never left as long floating field-sensitive stubs.

---

## 15. Firmware-facing contract

### 15.1 Safe enable sequence

Firmware shall keep:

```text
DO_ENABLE_REQ = 0
DO1_CMD = 0
DO2_CMD = 0
```

through early boot and peripheral initialization.

Only after the supervisor is healthy and firmware is ready to control the outputs may it assert `DO_ENABLE_REQ`.

Channel commands then independently control the two outputs through the second hardware AND gate.

### 15.2 Fault polarity

GPIO fault inputs are active-low at the electrical level:

```text
GPIO22 / DO1_FAULT: 0 = fault, 1 = no fault
GPIO23 / DO2_FAULT: 0 = fault, 1 = no fault
```

Firmware-visible logical fault values should therefore be derived as:

```text
fault1 = !gpio_get(DO1_FAULT)
fault2 = !gpio_get(DO2_FAULT)
```

### 15.3 Firmware-visible properties

Each DO channel should expose at least:

- commanded state;
- enable/safe-state state;
- raw diagnostic/fault state;
- logical fault state;
- startup state;
- watchdog/reset safe-state behavior;
- configured/qualified electrical profile (`24 V nominal, supply-following, 1 A continuous target`).

The driver must not claim that the board generates a regulated 24 V output when the main supply is 9/12/36 V.

---

## 16. Frozen DO BOM additions

```text
1 × IPS2050HTR
1 × SN74LVC2G08DCTR          second dual AND for per-channel command gating
4 × TLP383                   two command interfaces + two fault interfaces
2 × 390R                     command optocoupler LED resistors
2 × 22k                      IPS_IN1/IPS_IN2 pull-downs
2 × 220k                     IPD1/IPD2 LOW-threshold configuration
2 × 2.2k                     FLT1/FLT2 -> diagnostic optocoupler LEDs
2 × 10k                      DO1_FAULT/DO2_FAULT pull-ups to +3V3_DIG
1 × 100nF                    SN74LVC2G08 local bypass
1 × 100nF / 100V             IPS VCC HF bypass
1 × 2.2uF / 100V             IPS VCC local reservoir
2 × 100nF / 100V             DO1_OUT / DO2_OUT HF capacitors
1 × 0454005.MR               5 A slow-blow DO field fuse
1 × 5.0SMDJ36CA              bidirectional VFIELD TVS
1 × STPST10H100SBYTR         100 V / 10 A reverse-polarity Schottky
2 × 2-pin DO terminal blocks
```

Existing MCU/default components outside this sheet must include:

```text
DO_ENABLE_REQ -> 100k -> GND_SYS
DO1_CMD       -> 100k -> GND_SYS
DO2_CMD       -> 100k -> GND_SYS
```

---

## 17. Remaining qualification items

The schematic topology is considered closed for the MVP, but the following are hardware-validation items rather than claims already proven by the schematic:

1. Measure `VFIELD` and `VFIELD_OUT` overshoot during actual surge/EFT/ESD testing; verify margin to the IPS2050H VCC absolute maximum.
2. Verify `STPST10H100SBYTR` temperature at `2 × 1 A` continuous DO load over the intended ambient range.
3. Verify IPS2050H package/copper temperature at `2 × 1 A` continuous load.
4. Perform single-channel and dual-channel hard-short tests and confirm system logic rails remain alive and stable.
5. Verify `F_DO` does not nuisance-open during normal IPS short-circuit/thermal-cycling events.
6. Validate representative inductive load switching, including approximately `1 H @ 1 A` single-turn-off as a conservative prototype test point.
7. Validate representative capacitive/inrush load, including approximately `1000 uF @ 24 V`.
8. Validate the complete field connector, copper, fuse, Schottky and ground-return current path on the real PCB.
9. Do not claim IEC compliance/certification from the reference topology alone; qualification belongs to the completed PCB/enclosure/system test.

---

## 18. Final schematic status

For Hardware Prototype v1, the following sheet-09 digital-output decisions are frozen unless a concrete electrical/qualification failure requires revision:

```text
IPS2050HTR retained
24 V nominal / supply-following high-side outputs
1.0 A/channel qualified continuous target
LOW-threshold current-limit mode via 220k IPD->IN
hardware safe OFF via DO_SAFE_EN + per-channel AND + TLP383 interface
active-low per-channel fault GPIOs through TLP383
separate VIN_RAW-derived fused field branch
0454005.MR 5 A slow-blow F_DO
5.0SMDJ36CA on VFIELD
STPST10H100SBYTR reverse-polarity protection
100nF + 2.2uF local VCC decoupling
100nF/100V output capacitors
no external per-output flyback diode
no per-output TVS in the frozen topology
```

The next hardware design block after documenting sheet 09 is the four-channel `UIC0..UIC3` universal/counter/frequency/encoder frontend.

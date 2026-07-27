# RP2350 Industrial OPC UA Node — Hardware MVP

> **Status:** normative hardware scope for the diploma prototype.
>
> **Applies to:** Hardware Prototype v1 / firmware through `v1.0.0` unless deliberately revised.

## 1. Scope and intent

The diploma MVP is a single monolithic RP2350B PCB. It must demonstrate the complete path from physical industrial and digital sensor interfaces through firmware drivers and the channel runtime to the OPC UA information model.

The MVP is not required to be a certified commercial PLC. Product-level features such as a two-port Ethernet switch, user-replaceable I/O modules, full galvanic isolation of every channel group, redundant networking and production certification are deferred.

## 2. Fixed architecture policy

Hardware Prototype v1 uses one main PCB and has no user-replaceable I/O modules, internal expansion slots, module EEPROMs or board-to-board I/O expansion connectors.

All native hardware drivers are statically compiled into firmware. The configurator selects and parameterizes installed drivers by stable `driver_id`; it never uploads executable driver code.

The hardware profile identifies a fixed board revision, not a set of installed modules. Initial profile name:

```text
RP2350_INDUSTRIAL_MVP_V1
```

## 3. Required functional blocks

The PCB shall contain:

- RP2350B MCU;
- external QSPI Flash;
- external PSRAM;
- one W5500 10/100 Ethernet interface;
- USB-C service/recovery interface;
- one isolated RS-485 interface;
- industrial analog and digital I/O frontends;
- protected low-voltage sensor interfaces;
- watchdog/supervisor and status indication;
- 9–36 V DC input power stage.

## 4. Required I/O set

### 4.1 Analog inputs

Provide **4 analog input channels**: `AI0..AI3`.

Target ranges:

- `0–10 V`;
- `0–5 V` where supported by the frontend;
- `0–20 mA`;
- `4–20 mA`.

The channels shall use a dedicated external ADC and protected analog frontend. Direct connection of industrial signals to the RP2350 internal ADC is not allowed.

Minimum firmware-visible properties:

- configured electrical range;
- raw value;
- scaled engineering value;
- engineering unit;
- quality/status;
- calibration coefficients;
- source timestamp;
- overrange/underrange diagnostics.

### 4.2 Analog outputs

Provide **2 analog output channels**: `AO0..AO1`.

Target ranges:

- `0–10 V`;
- `0–20 mA`;
- `4–20 mA`.

Bipolar `±10 V` output is out of scope for the MVP.

The output stage shall use a dedicated DAC or industrial voltage/current output driver. PWM plus RC filtering is not acceptable as the primary implementation.

Each channel shall define:

- commanded value;
- electrical mode;
- startup value;
- communication-loss/watchdog value;
- slew-rate limit where supported;
- readback or diagnostic status where practical;
- deterministic hardware-safe state during MCU reset.

### 4.3 Digital inputs

Provide **4 digital input channels**: `DI0..DI3`.

Target electrical level:

- nominal `24 V DC`;
- protected and filtered input frontend;
- interrupt or deterministic polling support.

Firmware shall support debounce/filter time, inversion, edge detection, event counting and quality diagnostics.

### 4.4 Digital outputs

Provide **2 digital output channels**: `DO0..DO1`.

Target implementation:

- protected `24 V` high-side outputs;
- current limiting and thermal protection;
- short-circuit/fault diagnostics where supported;
- deterministic off state during reset and boot.

Firmware shall support commanded state, actual/fault state, startup state and watchdog state.

### 4.5 Universal high-speed inputs / counters

Provide **4 universal protected channels**: `UIO0..UIO3`.

Primary supported modes:

- digital input;
- pulse counter;
- frequency input;
- period measurement;
- quadrature encoder inputs using channel pairs;
- optional low-voltage GPIO mode when electrically safe.

Counter and encoder processing should use RP2350 PIO and DMA where appropriate rather than relying on nondeterministic task-level GPIO polling.

The four channels may be configured as:

```text
4 independent counters
2 quadrature encoders
1 quadrature encoder + 2 counters
4 protected digital inputs
```

## 5. External sensor interfaces

The PCB shall expose low-voltage interfaces for connecting digital sensors during diploma demonstrations and driver development.

### 5.1 I2C

Provide **2 I2C sensor ports**:

```text
I2C0: 3V3, GND, SDA, SCL, IRQ
I2C1: 3V3, GND, SDA, SCL, IRQ
```

Requirements:

- 3.3 V logic only;
- ESD protection and series damping where appropriate;
- optional current-limited switched sensor power;
- intended for short cables inside an enclosure or laboratory setup.

I2C is not specified as a long-distance industrial field bus.

### 5.2 SPI

Provide **1 SPI sensor port**:

```text
3V3, GND, SCK, MOSI, MISO, CS, IRQ
```

The external branch shall be protected and isolated from internal SPI devices using series resistors and, if required, a buffer or bus switch. Firmware must use a dedicated chip select and bounded transaction timeouts.

### 5.3 UART

Provide **1 UART sensor port**:

```text
3V3, GND, TX, RX, optional IRQ/WAKE
```

The port is a 3.3 V logic interface. Direct RS-232 voltage levels are not allowed without an external transceiver.

### 5.4 RS-485

Provide **1 isolated RS-485 port**:

```text
A, B, ISO_GND, optional SHIELD
```

The interface shall include:

- galvanic isolation;
- isolated power;
- TVS protection;
- selectable 120-ohm termination;
- controllable driver enable;
- support for Modbus RTU master/slave and driver-defined serial protocols.

## 6. System interfaces

### 6.1 Ethernet

Provide **1 Ethernet port** based on W5500 for the MVP.

The second Ethernet port and embedded two-port switch are deferred to product development. The diploma firmware shall focus on stable OPC UA, configuration, diagnostics and interoperability over a single network interface.

### 6.2 USB-C

Provide one USB-C service interface for:

- firmware flashing/recovery;
- diagnostic console;
- provisioning and local configuration;
- optional USB device functions used by the project.

USB Power Delivery is out of scope unless explicitly added later.

### 6.3 Debug and service

Provide internal or protected access to:

- SWD;
- RUN/reset;
- BOOTSEL/recovery;
- production/test points;
- watchdog diagnostics.

## 7. Power and protection

Target input supply:

```text
9–36 V DC
```

The power subsystem shall include, as appropriate:

- reverse-polarity protection;
- input fuse or resettable protection;
- surge/transient suppression;
- EMI filtering;
- buck conversion for logic rails;
- analog rail filtering;
- monitored power-good signals;
- external watchdog/supervisor.

The PCB layout shall maintain explicit separation between:

- field 24 V I/O;
- isolated RS-485 domain;
- analog measurement/output domain;
- digital 3.3 V logic;
- Ethernet and USB high-speed routing.

## 8. Firmware hardware profile

Firmware shall expose a machine-readable hardware capability descriptor containing at least:

- hardware profile and revision;
- number and type of AI/AO/DI/DO/UIO channels;
- supported electrical ranges and modes;
- available I2C/SPI/UART/RS-485 interfaces;
- Ethernet controller type;
- supported native drivers;
- calibration schema version;
- unavailable or unpopulated hardware features.

The configurator must read these capabilities before creating or applying a configuration.

## 9. Minimum diploma demonstration

The final prototype should demonstrate at least:

1. one analog input channel with scaling and alarm;
2. one analog output channel commanded through OPC UA with safe-state behavior;
3. one 24 V digital input with edge/event handling;
4. one protected digital output;
5. one counter or quadrature encoder mode through UIO;
6. one digital sensor connected through I2C, SPI or UART;
7. one RS-485/Modbus device or simulator;
8. OPC UA browsing, subscriptions, timestamps, quality, alarms and history;
9. configuration persistence and restart recovery;
10. firmware and hardware diagnostics exposed through OPC UA.

Not every physical channel must be simultaneously loaded during the defense, but every channel class must be implemented and verified.

## 10. Deferred product features

The following are explicitly outside the diploma MVP:

- user-replaceable I/O modules;
- internal expansion slots;
- a second Ethernet port or embedded Ethernet switch;
- network redundancy protocols;
- analog `±10 V` outputs;
- full per-channel galvanic isolation;
- relay-output variants;
- CAN/CAN-FD and IO-Link;
- production enclosure tooling;
- CE/UL certification and complete IEC 61000 validation;
- field-upgradeable native driver binaries.

These items belong to the post-diploma product roadmap and must not block completion of the MVP.

## 11. Frozen MVP summary

```text
RP2350 Industrial OPC UA Node — Hardware MVP

4 × AI   : 0–10 V / 0–20 mA / 4–20 mA
2 × AO   : 0–10 V / 0–20 mA / 4–20 mA
4 × DI   : protected 24 V
2 × DO   : protected 24 V high-side
4 × UIO  : counter / frequency / encoder / digital input
2 × I2C  : 3.3 V sensor ports
1 × SPI  : 3.3 V sensor port
1 × UART : 3.3 V sensor port
1 × RS-485 isolated
1 × 10/100 Ethernet (W5500)
1 × USB-C service/recovery
1 × monolithic PCB
```

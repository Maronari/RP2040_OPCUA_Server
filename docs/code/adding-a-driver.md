# How to add a native channel driver

Implements `ROADMAP.md:589` ("Document how to add a driver, reserve
resources, define configuration and expose channels") and `:590` ("Provide
a minimal standalone example driver outside model code"), written during
v0.5.1 T16. The example driver this document walks through end to end is
`drivers/include/drivers/synthetic_driver.h` + `drivers/src/synthetic_driver.c`
-- read that header's file-level doc alongside this document; it is
intentionally the simplest complete driver in this codebase (no bus, no
GPIO, no datasheet), so every step below has a real, minimal instance to
point at.

This document describes the mechanical steps. For the architectural
decisions behind *why* the driver contract looks the way it does (the
pilot-first process, the linked-seam mocking pattern, the resource-claim
model's known limitations), see `docs/releases/v0.5.1.md`, particularly
its T1/T2 sections and the "API/resource-claim review gate" retrospective.

## 1. Decide the driver's identity

- Pick the next unused `config_channel_driver_id_t` value in
  `services/include/services/driver_catalog.h` -- **append only, never
  renumber**. These values are persisted in Flash configuration and
  exchanged over the config-import/export wire format; an existing value
  must never change meaning.
- Pick a short, lowercase, unique `name` string (used as the OPC UA
  `SupportedDrivers` node name and the human-readable driver identifier
  everywhere else). `synthetic_driver.c` uses `"synthetic"`.

## 2. Write the driver's header and implementation

Every native driver implements some subset of `telemetry_driver_vtbl_t`'s
`start`/`stop`/`read`/`write`/`configure`/`self_test` slots (see
`drivers/include/drivers/native_channel_driver.h`'s file-level doc for the
full per-slot contract) plus one function outside that vtable:

```c
bool native_<name>_source_is_valid(uint8_t source_index, uint8_t source_flags);
```

`source_index`/`source_flags` (together, a `native_channel_source_t`, the
struct every driver's `channel->driver_context` actually points at) are
the *only* per-channel configuration a native driver gets -- there is no
separate runtime `configure()` payload for most drivers (see the pitfall
in section 6 below). Decide up front what `source_index` selects (a
GPIO number, a chip's internal channel index, an arbitrary "which
instance" selector) and what `source_flags`' bits mean (mode selection,
polarity, filter settings) -- `synthetic_driver.h`'s doc comment shows
the simplest possible version of this: `source_index` selects one of
eight independent channels, `source_flags` must be `0` (no
configuration exists for this driver at all).

Where should the new files live?

- `drivers/include/drivers/<name>_driver.h` + `drivers/src/<name>_driver.c`
  for a driver with any hardware dependency (a bus, a GPIO, a chip) --
  every driver added since v0.5.1 T3 follows this pattern (see
  `ads8688_driver.h`, `ips2050_driver.h`, etc. for real examples across a
  range of complexity).
- The four drivers that predate v0.5.1
  (`internal_temperature_channel_driver.c`, `adc_voltage_channel_driver.c`,
  `gpio_input_channel_driver.c`, `gpio_output_channel_driver.c`) share one
  header, `native_channel_driver.h`; new drivers do not need to follow
  that older, more cramped convention.

If the driver talks to a real bus (SPI/I2C/UART), do not call
`hardware_spi`/`hardware_i2c`/`hardware_uart` directly from the driver's
logic file -- see section 5.

## 3. Wire the driver into `driver_catalog`

In `services/include/services/driver_catalog.h`:

- Add the new `CONFIG_CHANNEL_DRIVER_<NAME>` enum value.
- If the driver claims an exclusive physical resource another driver
  could also claim (a GPIO pin, a chip-select line, a peripheral bus),
  check whether an existing `driver_catalog_resource_type_t` value
  genuinely names the *same physical resource* -- **do not reuse a
  resource type just because the shape looks similar.** T3 (`AD5422`)
  found this the hard way: reusing `ADS8688`'s `SPI_CHIP_SELECT` claim
  type for `AD5422` (which has no chip-select pin at all, only a shared
  `LATCH` line) would have made an `ADS8688` channel and an `AD5422`
  channel at the same numeric index falsely conflict. When in doubt, add
  a new, driver-specific resource type (`<NAME>_CHANNEL` is the
  established naming pattern -- see `DRIVER_CATALOG_RESOURCE_MAX22190_CHANNEL`,
  `DRIVER_CATALOG_RESOURCE_UIC_CHANNEL`, etc.). If the driver has no
  physical resource to claim at all (like `synthetic`), add nothing here
  -- the default `DRIVER_CATALOG_RESOURCE_NONE`/shared claim in
  `driver_catalog_get_resource_claims()` already correctly represents
  that.

In `services/src/driver_catalog.c`:

- `#include` the new driver's header.
- Add an `#ifndef FIRMWARE_ENABLE_NATIVE_<NAME>_DRIVER` / `#define ... 1`
  / `#endif` guard (matching every existing driver -- this lets the
  driver compile out entirely when its CMake option is off, per section 4).
- Add a `validate_<name>()` function delegating to
  `native_<name>_source_is_valid()`.
- Add a `g_<name>_driver` `telemetry_driver_vtbl_t` instance wiring in
  only the vtable slots the driver actually implements.
- Add the `driver_catalog_entry_t` to `g_driver_catalog[]` (name, default
  unit/kind/value type/flags, `supported_value_types` bitmask,
  `capabilities` bitmask, the validator, the vtable pointer).
- If the driver needs a resource claim, add an `else if` branch to
  `driver_catalog_get_resource_claims()`. If it shares a claim with an
  existing bus (e.g. two chips on the same physical SPI bus both claim
  `DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL` index `0`, `shared = true`),
  reuse that claim -- that is the *correct* case for reusing a claim
  type, unlike the per-channel case above.

## 4. Add the CMake option

In `drivers/CMakeLists.txt`:

- `option(FIRMWARE_ENABLE_NATIVE_<NAME>_DRIVER "..." ON)` (default `ON`
  unless the driver changes existing default build behavior -- see the
  W5500-pin-remap precedent in `docs/releases/v0.5.1.md` T10 for the one
  case so far where `OFF` was the correct default).
- Add the driver's `.c` file to `FIRMWARE_NATIVE_CHANNEL_DRIVER_SOURCES`
  behind `if(FIRMWARE_ENABLE_NATIVE_<NAME>_DRIVER) ... endif()`.
- Add `FIRMWARE_ENABLE_NATIVE_<NAME>_DRIVER=$<BOOL:${FIRMWARE_ENABLE_NATIVE_<NAME>_DRIVER}>`
  to `target_compile_definitions(FIRMWARE_DRIVERS PUBLIC ...)` -- this is
  what makes the `#ifndef` guard in `driver_catalog.c` (section 3) see the
  option's real value, since `driver_catalog.c` is compiled into a
  different CMake target (`FIRMWARE_SERVICES`) that links against
  `FIRMWARE_DRIVERS` and inherits its `PUBLIC` compile definitions.
- If the new driver is industrial-board-only (not a generic driver every
  board profile would want), add its option name to the
  `FIRMWARE_INDUSTRIAL_ONLY_OPTIONS` list in the top-level `CMakeLists.txt`
  (v0.5.1 T14) so `FIRMWARE_DRIVER_PACK=base` correctly excludes it.

## 5. If the driver needs a bus: use the existing transport abstraction

Do not call `hardware_spi`/`hardware_i2c`/`hardware_uart` (or any other
Pico SDK peripheral header) directly from the driver's own logic file --
this is what makes a driver host-testable at all. Instead:

- SPI: bind against `drivers/spi_transport.h`'s `spi_bus_t` (see
  `ads8688_driver.c` for the reference consumer).
- I2C: bind against `drivers/i2c_transport.h`'s `i2c_bus_t`, with
  `drivers/rp2350_i2c_bus.h` as the real hardware backing. This contract
  has no in-tree consumer to copy from right now: its first one
  (`tcal9539_driver.c`) was deleted on 2026-09-10 when the expander left
  the board, and the transport was kept for the next I2C device. Follow
  `ads8688_driver.c`'s SPI shape — the two contracts are deliberately
  parallel.
- UART/RS-485: bind against `drivers/uart_transport.h`'s `uart_bus_t`
  (see `rp2350_rs485_uart.c`'s own consumer note).

Each of these three transports was added by its *first* real consumer,
not speculatively -- a fourth bus type (I²S, CAN, etc.) would get its own
new minimal interface the same way, designed against its first real
driver, not a unified abstraction guessed at in advance (`docs/releases/v0.5.1.md`
scope decision 2 explains why a single cross-bus abstraction was
rejected).

If the driver needs plain GPIO only (like `IPS2050`, `TPS3851`, or
`synthetic`'s complete absence of hardware), there is no abstraction to
bind -- call `hardware/gpio.h` directly and write a stateful host-test
stub for it (section 6).

## 6. Known pitfalls, found on real hardware or during review

- **`configure(channel, NULL)` is the *normal* case, not an error.**
  `sensor_manager.c`'s real call sites always pass `NULL` for `configure()`.
  A driver's `configure()` must treat `NULL` as "apply the default safe
  state," not reject it (`gpio_output_channel_driver.c`'s T1 bug, found
  only on real hardware).
- **A write-only channel (no `read` slot) will never satisfy the
  activation verify gate** unless `sensor_manager_verify_candidate_channels()`
  explicitly skips channels whose driver has no `read` -- this is already
  fixed generically in `sensor_manager.c` (also a T1 finding), but it is
  worth knowing this constraint exists if a new driver's activation
  mysteriously always rolls back.
- **`level before enabling direction`, always, for any GPIO the driver
  drives.** Set the pin's driven level with `gpio_put()` *before* calling
  `gpio_set_dir(..., GPIO_OUT)`, so the pad is never briefly asserted at
  its power-on level once it becomes an output. Every GPIO-driving driver
  in this codebase follows this ordering (`gpio_output_channel_driver.c`,
  `ips2050_driver.c`, `tps3851_watchdog_driver.c`).
- **A shared physical resource (an ADC peripheral, an SPI/I2C bus) gets
  lazy, once-only initialization via a file-static flag**, not
  initialized per-channel. See `g_adc_ready` in
  `adc_voltage_channel_driver.c` or `g_enable_asserted` in
  `ips2050_driver.c` for the pattern -- and never de-initialize it on a
  single channel's `stop()`, since another channel sharing that resource
  may still be active.

## 7. Write host tests

Every driver in this codebase has host-test coverage exercising its real
logic against a fake bus/chip/GPIO model (not just proving the vtable
dispatches):

- A driver with no hardware dependency at all needs no stub --
  `static/tests/synthetic_driver/synthetic_driver_test.c` calls the real
  driver functions directly.
- A driver using one of the three bus abstractions (section 5) gets a
  fake `..._bus_vtbl_t` implementing real protocol/register semantics
  (see `static/tests/ads8688_driver/`'s fake chip model for the richest
  example, or `static/tests/tcal9539_driver/`'s smaller one).
- A driver touching plain GPIO directly needs a small stateful
  `hardware/gpio.h` stub shadowing the real Pico SDK header via
  test-directory include-path ordering -- see
  `static/tests/ips2050_driver/hardware/gpio.h`'s file-level doc for why
  a *stateful* stub (tracking direction/level/pulls) catches real bugs a
  no-op stub would miss.

Register the new suite's `.py` runner in `static/tools/run_host_tests.py`'s
`SUITES` tuple.

## 8. Add the driver to the host-side descriptor file

Add an entry to `static/host_descriptors/driver_descriptors.json`
(`ROADMAP.md:555`, v0.5.1 T15): `driverId`, `name`, `wiring`,
`connectionConstraints` (the exact `sourceIndex`/`sourceFlags` meaning
from section 2), `defaultChannelTemplate`, `capabilities`.
`static/tools/test_driver_descriptors_contract.py` fails closed if a
compiled `driver_id` has no descriptor entry (or vice versa) -- run it
(or the full `run_host_tests.py`) to confirm before considering the
driver done.

## 9. If the driver adds a new `driver_catalog_resource_type_t` value

`opcua_model/src/model_configuration.c`'s `model_driver_resource_type_to_string()`
maps every resource type to the JSON wire name shown under
`Device.Compatibility.SupportedDrivers` -- **a new resource type added in
step 3 above needs a corresponding `case` there too**, or it silently
reports `"none"` (exactly the bug `docs/releases/v0.5.1.md` T13 found and
fixed, undetected across six new resource types for an entire release
cycle). `static/tools/test_driver_catalog_opcua_contract.py` fails closed
if this mapping falls behind the enum -- run it to confirm.

## 10. Verify

The same four checks every T-item in `docs/releases/v0.5.1.md` records:

1. `static/tools/run_host_tests.py` -- full suite green.
2. A real ARM cross-compile of the full firmware
   (`cmake --build . -- -j8` against this project's toolchain-configured
   build directory) with the driver's option at its default.
3. A scratch CMake configure+build with the driver's own option (and any
   dependency options, if it has them -- see `board_diagnostics_service.c`'s
   compound gate for an example) set to `OFF`, confirming the driver's
   object file is absent from the entire tree and the firmware still
   links. Remove the scratch build directory afterward.
4. `static/tools/run_first_party_cppcheck.py` -- clean.

## 11. Document it

If the driver is part of a tracked release, add a section to that
release's `docs/releases/vX.Y.Z.md` plan document recording what was
built, what was found along the way, and the verification results above
-- see any `### T<n>` section in `docs/releases/v0.5.1.md` for the
established format (goal, findings, primary files, verified).

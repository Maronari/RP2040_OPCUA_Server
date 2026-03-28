# RP2040 OPC UA Server

A layered OPC UA firmware skeleton for Raspberry Pi Pico / RP2040 with WIZnet Ethernet.

## Architecture

The project is split into four runtime layers:

- `drivers`: hardware-facing code only. ADC / temperature, W5x00 Ethernet, LED, RTC, watchdog, flash storage.
- `services`: business/runtime services. Sensor acquisition, network state, time sync state, diagnostics, config, alarms.
- `opcua_model`: reflection of service state into the OPC UA address space.
- `app`: device profile assembly point. Chooses enabled sensors, network mode, thresholds, methods, and task composition.

## Project Structure

```text
.
+-- CMakeLists.txt
+-- app/
¦   L-- basic_profile/
+-- drivers/
+-- services/
+-- opcua_model/
+-- examples/          # legacy examples, off by default
+-- libraries/
L-- port/
```

## Build

The build is no longer pinned to a single board / chip / build type. Defaults are still provided, but they can now be overridden from CMake cache variables.

```sh
git clone --recurse-submodules https://github.com/yourusername/RP2040_OPCUA_Server.git
cd RP2040_OPCUA_Server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico -DWIZNET_CHIP=W5500
cmake --build build --target device_profile_basic
```

Useful options:

- `-DCMAKE_BUILD_TYPE=Debug|Release|RelWithDebInfo|MinSizeRel`
- `-DPICO_BOARD=<board>`
- `-DWIZNET_CHIP=W5100S|W5500`
- `-DBUILD_LEGACY_EXAMPLES=ON`

## Runtime Notes

- The OPC UA server no longer waits for DHCP lease acquisition before startup.
- SNTP time synchronization no longer blocks OPC UA startup behind a fixed epoch threshold.
- `time_service` now tracks synchronization quality separately from server startup.
- `basic_profile` exposes service-backed nodes for sensors, network, time, diagnostics, and alarms.

## Legacy Example

The old `examples/opc_dhcp` target is still available behind `BUILD_LEGACY_EXAMPLES`, but the default build path is now the layered firmware profile in `app/basic_profile`.

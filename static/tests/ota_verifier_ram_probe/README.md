# T1 RAM-only OTA verifier probe

This diagnostic compiles as `PICO_NO_FLASH` and links the production
`OTA_VERIFIER`, its isolated mbedTLS primitive build, and `PSRAM_ALLOCATOR`.
It is added only when `OTA_VERIFIER_RAM_PROBE=ON` and the supplied
`CMAKE_PROJECT_INCLUDE` hook defers creation until those production targets
exist. It changes no production target in ordinary builds.

`generate_fixture.py` creates a fresh P-256 key during the build and emits
only the public key and canonical low-S signed manifest into a generated C
header. The probe reader produces a deterministic full-capacity 5,271,552-byte
synthetic image on demand, so no image-sized RAM buffer exists. No private key is written to the
repository, build output, probe output, or device Flash. The numeric board
revision and development policy are explicitly test-only fixture values.

The probe initializes generic read-only XIP access needed by a no-Flash image,
then the real PSRAM allocator before any scheduler. It executes positive
authentication/candidate streaming plus modified-image, zero/out-of-range and
mathematically valid high-S signature, unknown-key, and profile rejection. It records allocator payload/header peaks
from the probe-only `OTA_VERIFIER_MEASUREMENT` build, PSRAM statistics before,
during reader callbacks, and after cleanup. The 64 KiB verifier operation
reservation is distinct from useful crypto allocator peak. A temporary near-
largest PSRAM allocation forces verifier allocation refusal, is released, and
is followed by a successful verification.

No Flash erase/program API is linked or called. On completion the bounded USB
diagnostic requests BOOTSEL with `reset_usb_boot`; it does not claim a normal
application reboot. The MSP paint result is a bare-metal wrapper/IRQ window,
not a FreeRTOS task stack watermark.

Typical configure invocation (from the repository root):

```powershell
rtk proxy C:/msys64/mingw64/bin/cmake.exe -DCMAKE_PROJECT_INCLUDE:FILEPATH=E:/Projects/RP2040_OPCUA_Server/static/tests/ota_verifier_ram_probe/hook.cmake -DOTA_VERIFIER_RAM_PROBE:BOOL=ON -DCMAKE_BUILD_TYPE:STRING=Debug -DFIRMWARE_DRIVER_PACK:STRING=base -DPICO_BOARD:STRING=waveshare_core2350b_16mb_psram8mb -DOTA_ROM_PRIMARY_APP:STRING=ON -DOTA_BUILD_BOOTLOADER:STRING=OFF -S E:/Projects/RP2040_OPCUA_Server -B build/v060-ota-verifier-ram-probe -G Ninja
rtk proxy C:/msys64/mingw64/bin/cmake.exe --build build/v060-ota-verifier-ram-probe --target v060_ota_verifier_ram_probe
```

Before device execution, inspect LOAD segments and the generated ELF hash.
Hardware execution, full-Flash backup/compare and post-probe application boot
must be recorded separately; this source does not claim those results.

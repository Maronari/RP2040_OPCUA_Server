# v0.6.0 T0 RAM-only crypto measurement fixture

This standalone diagnostic measures existing mbedTLS ECDSA P-256 verification
and RP2350 SHA-256, with a bounded instrumented crypto arena. It is outside the
production firmware build and does not implement a manifest parser, trusted-key
store, activation, fallback or health confirmation. It contains public test
vectors only; there is no private-key generation or signing.

The file plan is `probe.c` for bounded checks/measurements,
`probe_user_config.h` for the isolated allocator overlay,
`flash_measurement.c` only for the explicitly selected write variant, and this
CMake project for RAM linking. No public/configuration contract changes. The
probe declares a 64 KiB arena plus 256-byte failure-test arena, 4 KiB main
stack reservation, no core1 stack and no PSRAM allocation.

`OTA_PROBE_FLASH_WRITE` is an optional CMake switch and defaults to `OFF`.
The default read-only ELF contains no Flash erase/program test source and logs
`[FLASH_PROBE] mode=read_only`. With the option `ON`, the ELF exports the
retained `ota_probe_flash_write_enabled` marker with value one and links a
bounded, destructive microbenchmark. That variant is a prepared diagnostic,
not authorization to load or execute it. The host procedure must first obtain
and verify a fresh full 16 MiB backup, the reference-board identity and the
ELF identity. A mismatched or unverified artifact must not be loaded.

Configure this directory in an out-of-tree build using the reference board,
Debug and the same Arm compiler as the firmware. The project selects
`pico_set_binary_type(... no_flash)` and the source refuses a non-RAM build.
Inspect every ELF LOAD segment before using `picotool load -v -x probe.elf`.
Do not copy or install a diagnostic image as an OTA/factory artifact.

The SHA-256 image check is intentionally bound to the recorded T0 baseline:
3,586,824 bytes at `0x10000000`, SHA-256
`9a7f875d20ed71bda98286a856400351058e179cd93545eb15777784225837ab`.
Keep that image in Flash while running the diagnostic from RAM; another image
will correctly cause FAIL. The saved baseline BIN and build metadata are
required to reproduce this particular measurement. Other tests cover `abc`,
the public vendored ECDSA vector, modified hash, zero `r`, another valid public
key, allocation failure in 256 bytes, recovery to the full arena and unchanged
protected-tail hash before/after.

The write variant has no offset, image or payload configuration. Before any
erase or program it independently verifies the exact pinned 3,586,824-byte
baseline image hash, hashes the persistent tail, requires the legacy state at
`0x00ff0000` to pass `ota_state_validate()` **and** be the canonical empty
IDLE 64-byte record (bytes 8 through 59 are zero), and requires every byte of
the fixed 4 KiB target sector at `0x00f57000` to be erased. The sector ends at
`0x00f58000`, is the last sector of the legacy Bank-B region, and is not a
claim about the proposed recoverable layout. It is selected only as an unused,
non-bootable legacy staging location while the current Bank-A image and pinned
IDLE state remain untouched. Guard failure prints `status=refused`, performs
no writes and makes the terminal probe result `FAIL`.

The write variant logs its destructive-test marker before evaluating its
device-side guards; it still does not erase or program unless every guard
passes. It then erases the sector, programs exactly sixteen fixed 256-byte
pages, reads every page back, then erases it again to restore an all-`0xff`
sector. Each Flash operation masks interrupts only for the SDK erase/program
call; watchdog feeds occur between bounded operations. It rechecks the
baseline image and persistent tail after cleanup. Power loss can leave only
this otherwise blank legacy staging sector dirty; no real hardware/power-loss
evidence has been collected by this preparation.

The first hardware run passed the signature, negative, allocator and `abc`
checks but failed the Flash image digest comparison. The diagnostic now keeps
that direct-XIP test and adds a separate CPU-copy/SRAM-source hash using a
static 4096-byte buffer. Either failure still fails the overall probe.
Only the public firmware's computed/expected digest is printed on mismatch;
protected-tail digests and bytes are never printed. The extra buffer is a
separate static SRAM reservation, outside the crypto arena. This comparison
investigates the read path; it does not prove a root cause or repair Flash.

The second run produced the all-zero-stream digest on both paths. For the
next diagnostic the fixture calls SDK `flash_start_xip()` before USB starts,
with interrupts masked and core1 idle. This is the documented setup for
`PICO_NO_FLASH`: it reinitializes QSPI pads/interface for generic low-speed
03h reads and flushes the XIP cache, without erase/program. It is a temporary
hardware-state change, restored by reboot into the ordinary firmware. No
PSRAM or network driver is active at this point. Timing in this mode is not
production Flash throughput. A before/after tail comparison from the earlier
all-zero read setup did not establish preservation of the real tail bytes.

The probe waits finitely for USB, prints one result per test and a final
`[CRYPTO_PROBE] PASS` or `FAIL`, then requests a watchdog reset to normal Flash
boot. A missing terminal marker is not a pass. Host observation must separately
confirm return to the baseline firmware. Current production firmware disables
software entry to BOOTSEL; manual BOOTSEL entry may be necessary.

During the measurement window the probe masks interrupts, captures MSP, then
paints only the unused MSP region below it, retaining a 128-byte upper guard
and a 16-byte lower canary. It verifies that an enabled SDK MSPLIM stack guard
does not exclude the canary range. The report scan is also interrupt-masked
and refuses a result if its own frame enters the paint region. It emits one
machine-readable stack line with `state=ok`, `unavailable`, `saturated`, or
`canary_failed`; only `ok` reports `observed_used_bytes`, always alongside
`reserved_bytes=4096`. An `ok` value is `StackTop` through the lowest changed
paint byte, so it includes active harness frames and the intervening guard.
If no paint byte changed, the guard prevents an exact result and the fixture
reports `unavailable`, never a fabricated zero. The scope is explicitly
`ram_probe_harness_irq`: it covers this RAM harness and interrupt activity
during the window. It is neither a FreeRTOS task-stack watermark nor an
exclusive crypto-stack measurement.

`elapsed_us` includes allocator chain verification and setup/cleanup; it is
instrumented timing, not an uninstrumented production deadline. Allocator
`peak_allocator_used_excl_headers` is aligned payload usage, not total arena/SRAM
use. Header counts and reserved arena capacity are separate. The SDK hardware
SHA/DMA configuration matches the current app; the diagnostic allocator does
not use production FreeRTOS allocation hooks. Never infer task-stack or PSRAM
peaks from these values.

Code executes from SRAM, while the future bootloader may execute from XIP
Flash. Allocator verification adds overhead but SRAM instruction placement can
reduce it; the combined timing is not a conservative production upper bound.
Measure the integrated verification path again before choosing its deadline.

For the link-size experiment, configure the repository into another disposable
build with `OTA_BUILD_BOOTLOADER=ON`, `OTA_BOOTLOADER_SIZE_BYTES=262144` and
`CMAKE_PROJECT_INCLUDE` pointing to `bootloader_sizing_hook.cmake`, then build
only `ota_bootloader`. It retains verification primitives to measure their
link cost alongside the existing bootloader. **That output is still the old
CRC-only loader with a temporary layout; do not deploy it.** It excludes the
future parser/transaction integration and is not a final signed-loader size.

See [hardware observations](../../../docs/releases/v0.6.0-hardware-baseline.md)
and [the T0 design](../../../docs/releases/v0.6.0-ota-design.md). Hardware PASS,
timing and resource values require the actual board log, not successful
compilation alone.

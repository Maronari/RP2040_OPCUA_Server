# OTA Implementation Plan

## Invariants

- `boards/include/boards/weact_studio_rp2350a_16mb.h` remains the single source of truth for hardware facts.
- `ota/cmake/ota_layout.cmake` is the single source of truth for OTA partitioning.
- The application always executes from a fixed address in Bank A.
- OTA updates stage into Bank B and are copied into Bank A only on reboot.
- Factory images and OTA payloads are different artifacts.

## Phases

1. Introduce a generated OTA layout shared by app, bootloader, and tooling.
2. Keep the application on the current boot path while wiring in checks.
3. Add a minimal bootloader that knows the layout and later jumps to Bank A.
4. Move the app linker script to Bank A.
5. Add shared OTA metadata in the state region.
6. Add download-to-Bank-B in the app.
7. Add verify and copy `Bank B -> Bank A` in the bootloader.
8. Only then add recovery rules and rollback counters.

## Current Implementation Boundary

- Shared OTA layout generation is in place for bootloader, app, and artifact tooling.
- The app-side OTA library currently owns only two low-level responsibilities: a valid state record in the reserved OTA state sector and sequential staging of a new image into Bank B with page buffering plus CRC verification.
- `build/ota_artifacts/ota_payload.json` now carries the values needed by the staging client: payload size, `payload_crc32`, and packed `version_number`.
- The bootloader still only validates and jumps to Bank A.
- Copying `Bank B -> Bank A`, retry accounting, and rollback policy remain intentionally unimplemented until the staging path is exercised first.

## Current Staging Flow

1. Build `ota_artifacts` to refresh `ota_payload.bin` and `ota_payload.json`.
2. Run `static/tools/stage_ota.py` against the live device endpoint.
3. The client calls `BeginFirmwareUpdate`, streams sequential `WriteFirmwareChunk` calls, then finishes with `FinalizeFirmwareUpdate`.
4. Verify `GetFirmwareUpdateStatus` reports `otaState=STAGED_IN_BANK_B` before touching bootloader copy logic.

## Default 16 MB Layout

- Bootloader region: `0x10000000 .. 0x1000FFFF` (`64 KiB`, includes boot2)
- App Bank A: `0x10010000 .. 0x107FFFFF` (`8128 KiB`)
- Staging Bank B: `0x10800000 .. 0x10FEFFFF` (`8128 KiB`)
- OTA state/config: `0x10FF0000 .. 0x10FFFFFF` (`64 KiB`)

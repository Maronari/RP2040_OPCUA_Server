/*
 * Board header for RP2350_INDUSTRIAL_MVP_V1, the monolithic RP2350B
 * industrial controller frozen in HARDWARE_MVP.md.
 *
 * Selected automatically by FIRMWARE_DRIVER_PACK=industrial (see the root
 * CMakeLists.txt); the reference Waveshare Core2350B + W5500 module uses
 * waveshare_core2350b_16mb_psram8mb.h and the base pack instead.
 *
 * Everything here is taken from HARDWARE_MVP.md: RP2350B (QFN-80, stepping
 * A4), W25Q128JVBIQ 16 MB QSPI Flash, APS6404L-3SQR-SN 8 MB PSRAM on
 * GPIO0 (PSRAM_CS_N, section 6.3; GPIO0 carries XIP_CS1). The frozen GPIO
 * map has no default UART, I2C, SPI or LED pin, so none is defined: the
 * SDK's defaults would land on PSRAM_CS_N, USB_VBUS_DET, the watchdog and
 * the industrial buses. The MCU crystal is not specified in
 * HARDWARE_MVP.md; the SDK's default 12 MHz XOSC is assumed and must be
 * confirmed at bring-up.
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_RP2350_INDUSTRIAL_MVP_V1_H
#define _BOARDS_RP2350_INDUSTRIAL_MVP_V1_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define RP2350_INDUSTRIAL_MVP_V1

// --- BOARD SPECIFIC ---
// Board-neutral PSRAM description consumed by port/psram/psram_allocator.c
// and the PSRAM-backed services.
#define RP2_BOARD_PSRAM_CS_PIN 0
#define RP2_BOARD_PSRAM_SIZE_BYTES (8 * 1024 * 1024)

// --- RP2350 VARIANT ---
// RP2350B (QFN-80): GPIO30..47 exist and carry RS-485, UIC and rail monitors.
#define PICO_RP2350A 0

// --- FLASH ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#endif

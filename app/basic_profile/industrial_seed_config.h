/**
 * @file
 * @brief Seed configuration for the complete `RP2350_INDUSTRIAL_MVP_V1`
 *        profile: every AI/AO/DI/DO/UIC channel the board provides,
 *        populated as the finished MVP rather than as a starting point.
 *
 * Used instead of the reference profile's single internal-temperature seed
 * when the full industrial driver set is compiled in
 * (`FIRMWARE_DRIVER_PACK=industrial`). Seeds are only applied when no
 * persisted configuration exists; a device that has been configured keeps
 * its own configuration.
 *
 * The gate below requires *all five* industrial channel drivers, not just
 * one: a build with only some of them would otherwise seed channels whose
 * driver is not in the image, and every such channel would fail activation.
 * Falling back to the reference seed is the honest behavior there.
 *
 * Pre-hardware: the channel set and its per-channel range/filter codes come
 * from `HARDWARE_MVP.md`'s frozen contract, not from a working board.
 */
#ifndef APP_INDUSTRIAL_SEED_CONFIG_H
#define APP_INDUSTRIAL_SEED_CONFIG_H

#include "services/config_service.h"

#ifndef FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER
#define FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER 0
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER
#define FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER 0
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER
#define FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER 0
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER
#define FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER 0
#endif
#ifndef FIRMWARE_ENABLE_NATIVE_UIC_DRIVER
#define FIRMWARE_ENABLE_NATIVE_UIC_DRIVER 0
#endif

#if FIRMWARE_ENABLE_NATIVE_ADS8688_DRIVER && FIRMWARE_ENABLE_NATIVE_AD5422_DRIVER && \
    FIRMWARE_ENABLE_NATIVE_MAX22190_DRIVER && FIRMWARE_ENABLE_NATIVE_IPS2050_DRIVER && \
    FIRMWARE_ENABLE_NATIVE_UIC_DRIVER
#define APP_INDUSTRIAL_SEED_AVAILABLE 1
#else
#define APP_INDUSTRIAL_SEED_AVAILABLE 0
#endif

#if APP_INDUSTRIAL_SEED_AVAILABLE
/** @brief All 16 industrial channels; see this header's file comment. */
extern const device_profile_config_t g_industrial_profile;
#endif

#endif

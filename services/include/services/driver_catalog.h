#ifndef SERVICES_DRIVER_CATALOG_H
#define SERVICES_DRIVER_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/telemetry_channel.h"
#include "services/telemetry_driver.h"

/**
 * @brief Config-wire/persisted driver identifier for one native channel
 *        driver, chosen at configuration time and resolved to a
 *        driver_catalog_entry_t via driver_catalog_get_by_id().
 *
 * These values are persisted as part of device_profile_config_t (Flash) and
 * exchanged over the config-import/export wire format, so existing values
 * must never be renumbered -- only appended to.
 */
typedef enum
{
    CONFIG_CHANNEL_DRIVER_NONE = 0,
    CONFIG_CHANNEL_DRIVER_INTERNAL_TEMP = 1,
    CONFIG_CHANNEL_DRIVER_ADC_VOLTAGE = 2,
    CONFIG_CHANNEL_DRIVER_GPIO_INPUT = 3,
    CONFIG_CHANNEL_DRIVER_GPIO_OUTPUT = 4,
    CONFIG_CHANNEL_DRIVER_ADS8688 = 5,
    CONFIG_CHANNEL_DRIVER_AD5422 = 6,
    CONFIG_CHANNEL_DRIVER_MAX22190 = 7,
    CONFIG_CHANNEL_DRIVER_IPS2050 = 8,
    CONFIG_CHANNEL_DRIVER_UIC = 9,
    CONFIG_CHANNEL_DRIVER_SYNTHETIC = 10
} config_channel_driver_id_t;

/** @brief Bitmask helper: the single bit representing a telemetry_channel_value_type_t in a driver_catalog_entry_t::supported_value_types mask. */
#define TELEMETRY_CHANNEL_VALUE_TYPE_BIT(type) (1u << (uint32_t)(type))

/**
 * @brief Kind of physical/peripheral resource a driver instance claims,
 *        used by driver_catalog_get_resource_claims() /
 *        driver_catalog_claims_are_compatible() to detect configuration-time
 *        resource conflicts (e.g. two channels claiming the same exclusive
 *        GPIO) before any channel is registered.
 */
typedef enum
{
    DRIVER_CATALOG_RESOURCE_NONE = 0,
    DRIVER_CATALOG_RESOURCE_ADC_INPUT,
    DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL,
    DRIVER_CATALOG_RESOURCE_GPIO,
    /** @brief One ADS8688 mux input (v0.5.1 T2); exclusive per channel index (0-7) so two configured channels cannot bind the same physical input. */
    DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT,
    /** @brief One shared physical SPI bus (v0.5.1 T2); shared across every channel/chip on that bus, mirroring DRIVER_CATALOG_RESOURCE_ADC_PERIPHERAL's role for the native ADC-voltage driver. Multiple chips on the same physical SCLK/MOSI/MISO (e.g. ADS8688 and AD5422 both on the industrial board's shared BOARD_SPI bus, per HARDWARE_MVP.md) correctly claim the same index here without conflicting -- their per-chip addressing (chip-select pin, or AD5422's daisy-chain position) is a separate claim. */
    DRIVER_CATALOG_RESOURCE_SPI_PERIPHERAL,
    /** @brief One AD5422 daisy-chain position (v0.5.1 Phase 1, AO0/AO1); exclusive per channel index (0-1). Distinct from DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT because the two AD5422 devices on this board share one LATCH line (no per-device chip-select pin exists) -- see docs/datasheets/AD5422-register-notes.md. */
    DRIVER_CATALOG_RESOURCE_AD5422_CHANNEL,
    /** @brief One MAX22190 wired digital-input line (v0.5.1 Phase 1, DI0-DI3); exclusive per channel index (0-3). Distinct from DRIVER_CATALOG_RESOURCE_SPI_CHIP_SELECT/AD5422_CHANNEL because none of those name the same physical resource -- a MAX22190 DI channel and an ADS8688/AD5422 channel at the same numeric index must not falsely conflict; see docs/datasheets/MAX22190-register-notes.md. */
    DRIVER_CATALOG_RESOURCE_MAX22190_CHANNEL,
    /** @brief One protected output. Additional GPIO claims cover its command, fault and shared enable pins. */
    DRIVER_CATALOG_RESOURCE_IPS2050_CHANNEL,
    /** @brief One UIC universal-input channel (v0.5.1 Phase 1, UIC0-UIC3); exclusive per channel index (0-3), regardless of the channel's configured mode (digital input/pulse count/frequency/period all bind the same physical GPIO40..GPIO43 pin) -- see drivers/include/drivers/uic_driver.h. */
    DRIVER_CATALOG_RESOURCE_UIC_CHANNEL
} driver_catalog_resource_type_t;

/** @brief Upper bound on driver_catalog_get_resource_claims() output per channel; sizes caller-side claim arrays. */
#define DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL 4u

/**
 * @brief One physical/peripheral resource claim made by a channel's driver
 *        instance.
 *
 * @c shared marks a resource multiple channels may legitimately claim
 * simultaneously (e.g. the shared ADC peripheral behind several ADC input
 * channels); driver_catalog_claims_are_compatible() only flags a conflict
 * when the same (type, index) pair is claimed by two claims that are not
 * both shared.
 */
typedef struct
{
    driver_catalog_resource_type_t type;
    uint8_t index;
    bool shared;
} driver_catalog_resource_claim_t;

/**
 * @brief Config-wire source-pin flags applied to a discrete/analog source
 *        index before validation (driver_catalog_validate_source_fn) and
 *        resource-claim computation.
 */
typedef enum
{
    CONFIG_CHANNEL_SOURCE_FLAG_INVERT = (1u << 0u),
    CONFIG_CHANNEL_SOURCE_FLAG_PULLUP = (1u << 1u),
    CONFIG_CHANNEL_SOURCE_FLAG_PULLDOWN = (1u << 2u)
} config_channel_source_flags_t;

/**
 * @brief Per-driver source validation callback: whether @p source_index /
 *        @p source_flags form a valid configuration for this driver's
 *        underlying hardware source.
 *
 * Implemented per native driver in driver_catalog.c (e.g.
 * native_gpio_input_source_is_valid()) and invoked from
 * driver_catalog_get_resource_claims() before any claim is produced.
 */
typedef bool (*driver_catalog_validate_source_fn)(uint8_t source_index, uint8_t source_flags);

/**
 * @brief Static, compile-time catalog entry mapping one
 *        config_channel_driver_id_t to its default channel metadata,
 *        supported value types, source validator, and driver vtable.
 *
 * Instances live in a static const array in driver_catalog.c
 * (g_driver_catalog) built from the FIRMWARE_ENABLE_NATIVE_*_DRIVER compile
 * switches -- disabled drivers are compiled out entirely, not just hidden.
 * @c driver points at the driver_catalog.c-owned telemetry_driver_vtbl_t for
 * this driver, which in turn wraps the native operation functions declared
 * in drivers/include/drivers/native_channel_driver.h; entries and their
 * vtables are never freed (static storage duration for the life of the
 * process).
 */
typedef struct
{
    uint8_t driver_id;
    const char *name;
    const char *default_unit;
    telemetry_channel_kind_t default_kind;
    telemetry_channel_value_type_t default_value_type;
    uint32_t default_flags;
    bool analog;
    uint32_t supported_value_types;
    uint32_t capabilities;
    driver_catalog_validate_source_fn validate_source;
    const telemetry_driver_vtbl_t *driver;
} driver_catalog_entry_t;

/** @brief Look up a catalog entry by its config_channel_driver_id_t. @return NULL if @p driver_id is not in the compiled-in catalog. */
const driver_catalog_entry_t *driver_catalog_get_by_id(uint8_t driver_id);
/** @brief Look up a catalog entry by its driver name (e.g. "internal_temp"). @return NULL if @p name is NULL or not found. */
const driver_catalog_entry_t *driver_catalog_get_by_name(const char *name);
/** @brief Number of drivers compiled into the catalog (depends on FIRMWARE_ENABLE_NATIVE_*_DRIVER switches). */
size_t driver_catalog_count(void);
/** @brief Look up a catalog entry by its position in the compiled-in catalog array. @return NULL if @p index is out of range. */
const driver_catalog_entry_t *driver_catalog_get_by_index(size_t index);
/** @brief Whether @p entry declares support for @p value_type via its supported_value_types bitmask. @return false if @p entry is NULL or @p value_type is out of range. */
bool driver_catalog_supports_value_type(const driver_catalog_entry_t *entry,
                                        telemetry_channel_value_type_t value_type);
/**
 * @brief Validate a driver+source combination and compute the physical
 *        resource claims it would make if configured.
 *
 * First runs the driver's driver_catalog_validate_source_fn (via
 * driver_catalog_get_by_id()); on success, always sets @p *out_claim_count
 * to the exact required claim count, even if @p claims_capacity is too
 * small to receive them -- callers can use that to size a retry buffer. No
 * partial claim set is ever written on a capacity failure.
 *
 * @param driver_id Driver to validate against.
 * @param source_index Hardware source index (pin/ADC channel) to validate.
 * @param source_flags Source configuration flags (config_channel_source_flags_t).
 * @param out_claims Destination array for the computed claims; may be NULL
 *        only if @p claims_capacity is 0 (capacity-probe usage).
 * @param claims_capacity Capacity of @p out_claims in elements.
 * @param out_claim_count Must not be NULL; receives the required claim
 *        count on both success and capacity failure.
 * @return true on success (claims written); false for an invalid source or
 *         insufficient @p claims_capacity.
 */
bool driver_catalog_get_resource_claims(uint8_t driver_id,
                                        uint8_t source_index,
                                        uint8_t source_flags,
                                        driver_catalog_resource_claim_t *out_claims,
                                        size_t claims_capacity,
                                        size_t *out_claim_count);
/**
 * @brief Check whether a set of resource claims (typically gathered across
 *        every channel in a candidate configuration) is free of exclusive
 *        conflicts.
 *
 * Pairwise-compares every claim against every other; a conflict is any pair
 * sharing the same (type, index) where type is not
 * DRIVER_CATALOG_RESOURCE_NONE and at least one of the pair is not marked
 * @c shared. O(claim_count^2); intended for the bounded, small claim sets
 * produced at configuration-apply time, not a hot path.
 *
 * @param claims Array of claims to check; must be non-NULL if @p claim_count
 *        is non-zero.
 * @param claim_count Number of entries in @p claims.
 * @return true if no conflicting pair was found.
 */
bool driver_catalog_claims_are_compatible(const driver_catalog_resource_claim_t *claims,
                                          size_t claim_count);

#endif

/**
 * @file
 * @brief On-Flash wire format for the config-store slot header
 * (`config_store_slot_header_t`) and its associated constants/enums.
 *
 * Shared between `config_store_service.c` (writer/reader) and
 * `config_store_limits.h` (payload size bound). This is the *current*
 * schema only — frozen prior-version binary layouts for migration live in
 * `services/config_compat.h`, not here.
 */
#ifndef SERVICES_CONFIG_WIRE_H
#define SERVICES_CONFIG_WIRE_H

#include <stdint.h>

/** Magic value identifying a valid `config_store_slot_header_t` ("CFG1"). */
#define CONFIG_STORE_SLOT_MAGIC 0x31474643u /* CFG1 */
/** Header struct-layout version; bumping this (not `payload_format`/
 * `schema_version`) would signal an incompatible header shape. */
#define CONFIG_STORE_SLOT_HEADER_VERSION 1u
/** Number of Flash slots the config store rotates writes across. */
#define CONFIG_STORE_SLOT_COUNT 2u
/** `schema_version` value used for the current opaque-binary
 * `device_profile_config_t` payload encoding. */
#define CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY 1u

/**
 * @brief Encoding of the bytes stored in a config-store slot's payload.
 * @note Only `CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY` is ever written by
 * this codebase's `config_store_save_payload()` callers (not statically
 * confirmed to be produced anywhere for JSON/CBOR); the JSON/CBOR values
 * are reserved for a future payload encoding.
 */
typedef enum
{
    CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY = 0u,
    CONFIG_STORE_PAYLOAD_FORMAT_JSON = 1u,
    CONFIG_STORE_PAYLOAD_FORMAT_CBOR = 2u,
} config_store_payload_format_t;

/**
 * @brief Bit flags for `config_store_slot_header_t::flags`.
 * @note `CONFIG_STORE_SLOT_FLAG_PENDING_APPLY` is defined but not
 * statically confirmed to be set or read anywhere in this codebase —
 * reserved for a future two-phase apply protocol.
 */
enum
{
    CONFIG_STORE_SLOT_FLAG_VALID = (1u << 0),
    CONFIG_STORE_SLOT_FLAG_PENDING_APPLY = (1u << 1),
};

/**
 * @brief On-Flash header prefixing each config-store slot's payload.
 *
 * Packed layout (`__attribute__((packed))`) so the on-Flash byte layout is
 * exact and portable across compiler versions. `payload_crc32` covers only
 * the payload bytes that follow this header, computed with
 * `ota_crc32_compute()`. `generation` is a strictly increasing counter used
 * by `config_store_select_active_record()` to pick the newer of the two
 * slots. `reserved` is zero-initialized padding for future header fields.
 */
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t header_version;
    uint16_t payload_format;
    uint32_t schema_version;
    uint32_t generation;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t flags;
    uint32_t reserved[4];
} config_store_slot_header_t;

#endif

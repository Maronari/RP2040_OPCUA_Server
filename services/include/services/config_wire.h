#ifndef SERVICES_CONFIG_WIRE_H
#define SERVICES_CONFIG_WIRE_H

#include <stdint.h>

#define CONFIG_STORE_SLOT_MAGIC 0x31474643u /* CFG1 */
#define CONFIG_STORE_SLOT_HEADER_VERSION 1u
#define CONFIG_STORE_SLOT_COUNT 2u
#define CONFIG_STORE_SCHEMA_VERSION_OPAQUE_BINARY 1u

typedef enum
{
    CONFIG_STORE_PAYLOAD_FORMAT_RAW_BINARY = 0u,
    CONFIG_STORE_PAYLOAD_FORMAT_JSON = 1u,
    CONFIG_STORE_PAYLOAD_FORMAT_CBOR = 2u,
} config_store_payload_format_t;

enum
{
    CONFIG_STORE_SLOT_FLAG_VALID = (1u << 0),
    CONFIG_STORE_SLOT_FLAG_PENDING_APPLY = (1u << 1),
};

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

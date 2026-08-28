/**
 * @file
 * @brief Implementation of the staged/running/last-known-good Flash
 *        transaction backend (see
 *        drivers/include/drivers/config_transaction_backend.h for the full
 *        public contract).
 *
 * Responsibilities:
 * - Encode/decode a 32-byte config_txn_role_header_t (magic, per-record
 *   CRC32, record_format_version marker, caller schema_version, payload
 *   size/CRC, generation) at the start of each role's region, and a
 *   256-byte commit page at its fixed end -- the sole physical atomicity
 *   signal, mirroring drivers/maintenance_journal_backend.c's commit-page
 *   classification technique (never trust a program call's own return
 *   value; always independently read back and validate).
 * - Write every role via one shared routine (config_txn_program_role()):
 *   erase the whole role, program header+payload, read back and byte-
 *   verify the body, program the commit page exactly once, classify.
 * - Copy one role's already-classified content into another
 *   (config_txn_copy_role()), reusing the source's proven metadata rather
 *   than re-deriving it, used by commit()'s RUNNING->LAST_KNOWN_GOOD
 *   promotion and by recover_fallback_to_last_known_good().
 *
 * Ownership: all scratch buffers (g_config_txn_header_page,
 * g_config_txn_commit_page, g_config_txn_chunk, g_config_txn_verify_chunk)
 * are static internal SRAM, deliberately never placed on a FreeRTOS task
 * stack -- same discipline as maintenance_journal_backend.c. They are each
 * exactly one CONFIG_TXN_PAGE_SIZE_BYTES (256 bytes), since payload
 * read/write/verify is always chunked rather than requiring a
 * full-role-sized buffer.
 *
 * Concurrency: none -- see config_transaction_backend_t's doc comment in
 * the header for the single-task-caller assumption.
 */
#include "drivers/config_transaction_backend.h"

#include <string.h>

#if defined(PICO_ON_DEVICE)
#include "drivers/flash_operation_coordinator.h"
#include "ota_layout.h"
#endif

/** @brief On-Flash header occupying the first CONFIG_TXN_PAGE_SIZE_BYTES of every role's region. */
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t record_format_version;
    uint16_t payload_format;
    uint32_t schema_version;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t generation;
    uint32_t reserved;
    uint32_t header_crc32; /* CRC32 over every field above (offsetof(header_crc32) bytes). */
} config_txn_role_header_t;

/* Commit page byte offsets (256-byte page, everything between the magic and
 * the trailing CRC must stay erased (0xFF) for the page to classify as an
 * exact, physically-landed commit -- see config_txn_commit_page_is_exact()). */
enum { COMMIT_OFFSET_MAGIC = 0u, COMMIT_OFFSET_CRC = 252u };

/*
 * Scratch buffers, static internal SRAM -- see file-level doc comment.
 * g_config_txn_header_page: the role header page under construction/being
 * read back. g_config_txn_commit_page: the commit page under construction/
 * being read back. g_config_txn_chunk/g_config_txn_verify_chunk: reused for
 * every CONFIG_TXN_PAGE_SIZE_BYTES chunk of payload read/program/verify --
 * two distinct buffers so a "read what's on Flash" chunk and a "what we
 * intended to write" chunk can be compared without aliasing.
 */
static uint8_t g_config_txn_header_page[CONFIG_TXN_PAGE_SIZE_BYTES];
static uint8_t g_config_txn_commit_page[CONFIG_TXN_PAGE_SIZE_BYTES];
static uint8_t g_config_txn_chunk[CONFIG_TXN_PAGE_SIZE_BYTES];
static uint8_t g_config_txn_verify_chunk[CONFIG_TXN_PAGE_SIZE_BYTES];

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}
static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u); p[2] = (uint8_t)(v >> 16u); p[3] = (uint8_t)(v >> 24u);
}

/**
 * @brief CRC-32/ISO-HDLC (same polynomial/reflection/init/final convention
 *        as ota_crc32_compute()/maintenance_journal_codec_crc32()), split
 *        into an incremental update step so classify_commit()/read() can
 *        verify a payload's CRC by streaming it through
 *        CONFIG_TXN_PAGE_SIZE_BYTES chunks from Flash instead of requiring
 *        a full-payload-sized (up to CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES)
 *        scratch buffer. @p state is the raw (pre-final-XOR) running CRC;
 *        start from 0xFFFFFFFFu and XOR the final state with 0xFFFFFFFFu
 *        to get the conventional CRC32 value.
 */
static uint32_t crc32_update(uint32_t state, const uint8_t *data, size_t n)
{
    size_t i;
    for (i = 0u; i < n; ++i)
    {
        uint32_t bit;
        state ^= data[i];
        for (bit = 0u; bit < 8u; ++bit)
        {
            state = (state & 1u) != 0u ? (state >> 1u) ^ 0xEDB88320u : state >> 1u;
        }
    }
    return state;
}
/** @brief Whole-buffer convenience wrapper over crc32_update(). */
static uint32_t crc32_whole(const uint8_t *data, size_t n)
{
    return crc32_update(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}

/** @brief True if every byte in [offset, size) is the erased Flash value (0xFF). */
static bool is_erased(const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i)
    {
        if (data[i] != 0xFFu)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Physical exact-match check for one already-read commit page: the
 *        sole atomicity signal (see the file-level and header doc
 *        comments). A page that is fully erased, or non-erased but not an
 *        exact commit marker, is uniformly "not committed" -- the caller
 *        does not distinguish those two cases, matching
 *        maintenance_journal_backend's classify_commit() precedent.
 */
static bool commit_page_is_exact(const uint8_t page[CONFIG_TXN_PAGE_SIZE_BYTES])
{
    if (read_u32(&page[COMMIT_OFFSET_MAGIC]) != CONFIG_TXN_COMMIT_MAGIC)
    {
        return false;
    }
    return is_erased(&page[COMMIT_OFFSET_MAGIC + 4u], COMMIT_OFFSET_CRC - COMMIT_OFFSET_MAGIC - 4u);
}

/** @brief Byte offset of role @p role's region from the start of this backend's whole 3-role Flash region. */
static uint32_t role_offset(config_txn_role_t role)
{
    return (uint32_t)role * CONFIG_TXN_ROLE_SIZE_BYTES;
}

static void build_header_page(uint8_t page[CONFIG_TXN_PAGE_SIZE_BYTES], uint16_t record_format_version,
                              uint16_t payload_format, uint32_t schema_version, uint32_t payload_size,
                              uint32_t payload_crc32, uint32_t generation)
{
    config_txn_role_header_t header = {0};
    header.magic = CONFIG_TXN_RECORD_MAGIC;
    header.record_format_version = record_format_version;
    header.payload_format = payload_format;
    header.schema_version = schema_version;
    header.payload_size = payload_size;
    header.payload_crc32 = payload_crc32;
    header.generation = generation;
    header.header_crc32 = crc32_whole((const uint8_t *)&header, offsetof(config_txn_role_header_t, header_crc32));
    memset(page, 0xFF, CONFIG_TXN_PAGE_SIZE_BYTES);
    memcpy(page, &header, sizeof(header));
}

/** @brief Commit page vouches for the header page as a whole (its CRC covers all 256 header-page bytes, including trailing 0xFF padding), so any corruption between the header write and the commit-page write is also caught. */
static void build_commit_page(uint8_t page[CONFIG_TXN_PAGE_SIZE_BYTES], const uint8_t header_page[CONFIG_TXN_PAGE_SIZE_BYTES])
{
    memset(page, 0xFF, CONFIG_TXN_PAGE_SIZE_BYTES);
    write_u32(&page[COMMIT_OFFSET_MAGIC], CONFIG_TXN_COMMIT_MAGIC);
    write_u32(&page[COMMIT_OFFSET_CRC], crc32_whole(header_page, CONFIG_TXN_PAGE_SIZE_BYTES));
}

config_txn_commit_state_t config_transaction_backend_classify_commit(
    const config_transaction_storage_t *storage, uint32_t role_offset_bytes,
    config_transaction_record_info_t *out_info)
{
    config_txn_role_header_t header;
    uint32_t crc_state;
    uint32_t payload_offset;
    uint32_t remaining;

    if (storage == NULL || storage->read == NULL)
    {
        return CONFIG_TXN_COMMIT_INDETERMINATE;
    }

    /* Step 1: the commit page's own physical exactness is the sole
     * atomicity signal -- read it first and never trust any earlier write
     * call's return value. */
    if (!storage->read(storage->context, role_offset_bytes + CONFIG_TXN_ROLE_SIZE_BYTES - CONFIG_TXN_PAGE_SIZE_BYTES,
                       g_config_txn_commit_page, CONFIG_TXN_PAGE_SIZE_BYTES))
    {
        return CONFIG_TXN_COMMIT_INDETERMINATE;
    }
    if (!commit_page_is_exact(g_config_txn_commit_page))
    {
        return CONFIG_TXN_COMMIT_NOT_COMMITTED;
    }

    /* Step 2: header page, vouched for by the commit page's CRC. */
    if (!storage->read(storage->context, role_offset_bytes, g_config_txn_header_page, CONFIG_TXN_PAGE_SIZE_BYTES))
    {
        return CONFIG_TXN_COMMIT_INDETERMINATE;
    }
    if (read_u32(&g_config_txn_commit_page[COMMIT_OFFSET_CRC]) != crc32_whole(g_config_txn_header_page, CONFIG_TXN_PAGE_SIZE_BYTES))
    {
        return CONFIG_TXN_COMMIT_CORRUPT;
    }
    memcpy(&header, g_config_txn_header_page, sizeof(header));
    if (header.magic != CONFIG_TXN_RECORD_MAGIC)
    {
        return CONFIG_TXN_COMMIT_CORRUPT;
    }
    if (crc32_whole((const uint8_t *)&header, offsetof(config_txn_role_header_t, header_crc32)) != header.header_crc32)
    {
        return CONFIG_TXN_COMMIT_CORRUPT;
    }
    if (header.record_format_version != CONFIG_TXN_RECORD_FORMAT_VERSION)
    {
        /* Exact commit marker, recognized envelope, unrecognized version --
         * preserve, never decode as a snapshot (mirrors
         * MAINTENANCE_JOURNAL_COMMIT_UNKNOWN). */
        return CONFIG_TXN_COMMIT_UNKNOWN;
    }
    if (header.payload_size == 0u || header.payload_size > CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES)
    {
        return CONFIG_TXN_COMMIT_CORRUPT;
    }

    /* Step 3: payload CRC, streamed in page-sized chunks so no
     * full-payload-sized scratch buffer is required. */
    crc_state = 0xFFFFFFFFu;
    remaining = header.payload_size;
    payload_offset = role_offset_bytes + CONFIG_TXN_PAGE_SIZE_BYTES;
    while (remaining > 0u)
    {
        const size_t chunk = remaining < CONFIG_TXN_PAGE_SIZE_BYTES ? (size_t)remaining : (size_t)CONFIG_TXN_PAGE_SIZE_BYTES;
        if (!storage->read(storage->context, payload_offset, g_config_txn_chunk, chunk))
        {
            return CONFIG_TXN_COMMIT_INDETERMINATE;
        }
        crc_state = crc32_update(crc_state, g_config_txn_chunk, chunk);
        payload_offset += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
    }
    if ((crc_state ^ 0xFFFFFFFFu) != header.payload_crc32)
    {
        return CONFIG_TXN_COMMIT_CORRUPT;
    }

    if (out_info != NULL)
    {
        out_info->record_format_version = header.record_format_version;
        out_info->payload_format = header.payload_format;
        out_info->schema_version = header.schema_version;
        out_info->payload_size = header.payload_size;
        out_info->payload_crc32 = header.payload_crc32;
        out_info->generation = header.generation;
    }
    return CONFIG_TXN_COMMIT_COMMITTED;
}

/** @brief Erase every sector in role @p role's region, read-verifying (first page of each sector) that the erase actually landed. */
static config_transaction_backend_result_t erase_role(config_transaction_backend_t *b, config_txn_role_t role)
{
    const uint32_t base = role_offset(role);
    uint32_t i;
    for (i = 0u; i < CONFIG_TXN_ROLE_SECTOR_COUNT; ++i)
    {
        const uint32_t sector_offset = base + i * CONFIG_TXN_SECTOR_SIZE_BYTES;
        if (!b->storage.erase_sector(b->storage.context, sector_offset))
        {
            return CONFIG_TXN_BACKEND_IO;
        }
        if (!b->storage.read(b->storage.context, sector_offset, g_config_txn_chunk, CONFIG_TXN_PAGE_SIZE_BYTES) ||
            !is_erased(g_config_txn_chunk, CONFIG_TXN_PAGE_SIZE_BYTES))
        {
            return CONFIG_TXN_BACKEND_IO;
        }
    }
    return CONFIG_TXN_BACKEND_OK;
}

/** @brief Where program_role() reads payload bytes from: either a caller-owned RAM buffer (stage()) or another already-classified role's Flash content (copy_role()). */
typedef struct
{
    bool from_role;
    const config_transaction_backend_t *src_backend;
    config_txn_role_t src_role;
    const uint8_t *ram_payload;
} payload_source_t;

static bool payload_source_read(const payload_source_t *src, uint32_t payload_offset, uint8_t *dst, size_t n)
{
    if (src->from_role)
    {
        return src->src_backend->storage.read(src->src_backend->storage.context,
                                              role_offset(src->src_role) + CONFIG_TXN_PAGE_SIZE_BYTES + payload_offset,
                                              dst, n);
    }
    memcpy(dst, src->ram_payload + payload_offset, n);
    return true;
}

/**
 * @brief Shared write routine for every role write: erase, program header
 *        + payload, read back and byte-verify the body, program the commit
 *        page exactly once, classify. Updates b->roles[role] from the
 *        classification regardless of outcome.
 */
static config_transaction_backend_result_t program_role(
    config_transaction_backend_t *b, config_txn_role_t role, uint16_t record_format_version,
    uint16_t payload_format, uint32_t schema_version, uint32_t payload_size, uint32_t payload_crc32,
    uint32_t generation, const payload_source_t *src)
{
    const uint32_t base = role_offset(role);
    config_transaction_backend_result_t result;
    uint32_t offset;
    uint32_t remaining;
    config_txn_commit_state_t classified;

    if (payload_size == 0u || payload_size > CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }

    result = erase_role(b, role);
    if (result != CONFIG_TXN_BACKEND_OK)
    {
        return result;
    }

    build_header_page(g_config_txn_header_page, record_format_version, payload_format, schema_version,
                      payload_size, payload_crc32, generation);
    if (!b->storage.program_page(b->storage.context, base, g_config_txn_header_page))
    {
        return CONFIG_TXN_BACKEND_IO;
    }

    remaining = payload_size;
    offset = 0u;
    while (remaining > 0u)
    {
        const size_t chunk = remaining < CONFIG_TXN_PAGE_SIZE_BYTES ? (size_t)remaining : (size_t)CONFIG_TXN_PAGE_SIZE_BYTES;
        memset(g_config_txn_chunk, 0xFF, CONFIG_TXN_PAGE_SIZE_BYTES);
        if (!payload_source_read(src, offset, g_config_txn_chunk, chunk) ||
            !b->storage.program_page(b->storage.context, base + CONFIG_TXN_PAGE_SIZE_BYTES + offset, g_config_txn_chunk))
        {
            return CONFIG_TXN_BACKEND_IO;
        }
        offset += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
    }

    /* Defensive read-back verify of the whole body before ever touching the
     * commit page -- fails fast on a torn body write rather than waiting
     * for the next recover() to notice it. The commit page's own exactness
     * remains the sole authoritative boot-time signal regardless. */
    if (!b->storage.read(b->storage.context, base, g_config_txn_chunk, CONFIG_TXN_PAGE_SIZE_BYTES) ||
        memcmp(g_config_txn_chunk, g_config_txn_header_page, CONFIG_TXN_PAGE_SIZE_BYTES) != 0)
    {
        return CONFIG_TXN_BACKEND_IO;
    }
    remaining = payload_size;
    offset = 0u;
    while (remaining > 0u)
    {
        const size_t chunk = remaining < CONFIG_TXN_PAGE_SIZE_BYTES ? (size_t)remaining : (size_t)CONFIG_TXN_PAGE_SIZE_BYTES;
        memset(g_config_txn_verify_chunk, 0xFF, CONFIG_TXN_PAGE_SIZE_BYTES);
        if (!payload_source_read(src, offset, g_config_txn_verify_chunk, chunk) ||
            !b->storage.read(b->storage.context, base + CONFIG_TXN_PAGE_SIZE_BYTES + offset, g_config_txn_chunk, CONFIG_TXN_PAGE_SIZE_BYTES) ||
            memcmp(g_config_txn_chunk, g_config_txn_verify_chunk, CONFIG_TXN_PAGE_SIZE_BYTES) != 0)
        {
            return CONFIG_TXN_BACKEND_IO;
        }
        offset += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
    }

    build_commit_page(g_config_txn_commit_page, g_config_txn_header_page);
    /* The one permitted attempt on the never-reprogrammed commit page; its
     * own return value is deliberately not the authority -- classify() is. */
    (void)b->storage.program_page(b->storage.context, base + CONFIG_TXN_ROLE_SIZE_BYTES - CONFIG_TXN_PAGE_SIZE_BYTES,
                                  g_config_txn_commit_page);
    classified = config_transaction_backend_classify_commit(&b->storage, base, &b->roles[role]);
    b->roles[role].commit_state = classified;
    if (classified != CONFIG_TXN_COMMIT_COMMITTED)
    {
        return classified == CONFIG_TXN_COMMIT_INDETERMINATE ? CONFIG_TXN_BACKEND_INDETERMINATE : CONFIG_TXN_BACKEND_IO;
    }
    return CONFIG_TXN_BACKEND_OK;
}

/** @brief Copy @p src_role's already-classified content into @p dst_role, reusing its proven metadata rather than re-deriving it. */
static config_transaction_backend_result_t copy_role(config_transaction_backend_t *b, config_txn_role_t src_role, config_txn_role_t dst_role)
{
    const config_transaction_record_info_t *info = &b->roles[src_role];
    payload_source_t src;

    if (info->commit_state != CONFIG_TXN_COMMIT_COMMITTED)
    {
        return CONFIG_TXN_BACKEND_NOT_FOUND;
    }
    src.from_role = true;
    src.src_backend = b;
    src.src_role = src_role;
    src.ram_payload = NULL;
    return program_role(b, dst_role, info->record_format_version, info->payload_format, info->schema_version,
                        info->payload_size, info->payload_crc32, info->generation, &src);
}

/** @brief Re-derive backend->recovery/mutations_blocked from the three roles' current cached commit_state -- shared by recover() and recover_fallback_to_last_known_good(). */
static void recompute_recovery(config_transaction_backend_t *b)
{
    bool any_indeterminate = false;
    bool running_ok;
    bool lkg_ok;
    int i;

    for (i = 0; i < (int)CONFIG_TXN_ROLE_COUNT; ++i)
    {
        if (b->roles[i].commit_state == CONFIG_TXN_COMMIT_INDETERMINATE)
        {
            any_indeterminate = true;
        }
    }
    if (any_indeterminate)
    {
        b->recovery = CONFIG_TXN_RECOVERY_INDETERMINATE;
        b->mutations_blocked = true;
        return;
    }

    running_ok = b->roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED;
    lkg_ok = b->roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_COMMITTED;

    if (running_ok)
    {
        const bool degraded =
            b->roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_CORRUPT ||
            b->roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_UNKNOWN ||
            b->roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_CORRUPT ||
            b->roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_UNKNOWN;
        b->recovery = degraded ? CONFIG_TXN_RECOVERY_DEGRADED : CONFIG_TXN_RECOVERY_OK;
        b->mutations_blocked = false;
        return;
    }
    if (lkg_ok)
    {
        b->recovery = CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED;
        b->mutations_blocked = false;
        return;
    }
    {
        const bool any_corrupt_evidence =
            b->roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_CORRUPT ||
            b->roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_UNKNOWN ||
            b->roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_CORRUPT ||
            b->roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_UNKNOWN ||
            b->roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_CORRUPT ||
            b->roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_UNKNOWN;
        if (any_corrupt_evidence)
        {
            b->recovery = CONFIG_TXN_RECOVERY_FATAL;
            b->mutations_blocked = true;
        }
        else
        {
            b->recovery = CONFIG_TXN_RECOVERY_EMPTY;
            b->mutations_blocked = false;
        }
    }
}

/** @brief Common exit path: records last_error and passes @p result through. */
static config_transaction_backend_result_t finish(config_transaction_backend_t *b, config_transaction_backend_result_t result)
{
    if (b != NULL)
    {
        b->last_error = result;
    }
    return result;
}

void config_transaction_backend_init(config_transaction_backend_t *backend, const config_transaction_storage_t *storage)
{
    if (backend == NULL)
    {
        return;
    }
    memset(backend, 0, sizeof(*backend));
    if (storage != NULL)
    {
        backend->storage = *storage;
    }
    backend->recovery = CONFIG_TXN_RECOVERY_INDETERMINATE;
    backend->mutations_blocked = true;
    backend->recovered = false;
}

config_transaction_backend_result_t config_transaction_backend_recover(config_transaction_backend_t *backend)
{
    int i;
    if (backend == NULL || backend->storage.read == NULL)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }
    for (i = 0; i < (int)CONFIG_TXN_ROLE_COUNT; ++i)
    {
        backend->roles[i].commit_state = config_transaction_backend_classify_commit(
            &backend->storage, role_offset((config_txn_role_t)i), &backend->roles[i]);
    }
    backend->recovered = true;
    recompute_recovery(backend);
    return finish(backend, backend->recovery == CONFIG_TXN_RECOVERY_INDETERMINATE ?
                            CONFIG_TXN_BACKEND_INDETERMINATE : CONFIG_TXN_BACKEND_OK);
}

config_transaction_backend_result_t config_transaction_backend_stage(
    config_transaction_backend_t *backend, const void *payload, size_t payload_size,
    uint16_t payload_format, uint32_t schema_version, config_transaction_record_info_t *out_info)
{
    payload_source_t src;
    config_transaction_backend_result_t result;
    uint32_t payload_crc32;

    if (backend == NULL || payload == NULL || payload_size == 0u || payload_size > CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES)
    {
        return finish(backend, CONFIG_TXN_BACKEND_INVALID_ARGUMENT);
    }
    if (!backend->recovered || backend->mutations_blocked)
    {
        return finish(backend, CONFIG_TXN_BACKEND_INDETERMINATE);
    }

    payload_crc32 = crc32_whole((const uint8_t *)payload, payload_size);
    src.from_role = false;
    src.src_backend = NULL;
    src.src_role = CONFIG_TXN_ROLE_STAGED;
    src.ram_payload = (const uint8_t *)payload;

    result = program_role(backend, CONFIG_TXN_ROLE_STAGED, CONFIG_TXN_RECORD_FORMAT_VERSION, payload_format,
                          schema_version, (uint32_t)payload_size, payload_crc32, 0u, &src);
    if (result == CONFIG_TXN_BACKEND_OK && out_info != NULL)
    {
        *out_info = backend->roles[CONFIG_TXN_ROLE_STAGED];
    }
    return finish(backend, result);
}

config_transaction_backend_result_t config_transaction_backend_discard_staged(config_transaction_backend_t *backend)
{
    config_transaction_backend_result_t result;
    if (backend == NULL)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }
    if (!backend->recovered || backend->mutations_blocked)
    {
        return finish(backend, CONFIG_TXN_BACKEND_INDETERMINATE);
    }
    result = erase_role(backend, CONFIG_TXN_ROLE_STAGED);
    if (result == CONFIG_TXN_BACKEND_OK)
    {
        backend->roles[CONFIG_TXN_ROLE_STAGED].commit_state = config_transaction_backend_classify_commit(
            &backend->storage, role_offset(CONFIG_TXN_ROLE_STAGED), &backend->roles[CONFIG_TXN_ROLE_STAGED]);
    }
    return finish(backend, result);
}

config_transaction_backend_result_t config_transaction_backend_commit(
    config_transaction_backend_t *backend, config_transaction_record_info_t *out_running_info)
{
    config_transaction_backend_result_t result;
    uint32_t new_generation;

    if (backend == NULL)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }
    if (!backend->recovered || backend->mutations_blocked)
    {
        return finish(backend, CONFIG_TXN_BACKEND_INDETERMINATE);
    }
    if (backend->roles[CONFIG_TXN_ROLE_STAGED].commit_state != CONFIG_TXN_COMMIT_COMMITTED)
    {
        return finish(backend, CONFIG_TXN_BACKEND_NOT_FOUND);
    }

    /* Promote the soon-to-be-previous RUNNING into LAST_KNOWN_GOOD *before*
     * touching RUNNING at all -- see the header doc comment for the full
     * power-loss-ordering proof. If RUNNING is not currently usable (first
     * commit ever), there is nothing to promote and this step is skipped. */
    if (backend->roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED)
    {
        result = copy_role(backend, CONFIG_TXN_ROLE_RUNNING, CONFIG_TXN_ROLE_LAST_KNOWN_GOOD);
        if (result != CONFIG_TXN_BACKEND_OK)
        {
            /* RUNNING is untouched and still the valid old value; abort
             * without promoting STAGED so the caller can retry. */
            return finish(backend, result);
        }
        new_generation = backend->roles[CONFIG_TXN_ROLE_RUNNING].generation + 1u;
        if (new_generation == 0u)
        {
            new_generation = 1u;
        }
    }
    else
    {
        new_generation = 1u;
    }

    {
        const config_transaction_record_info_t *staged = &backend->roles[CONFIG_TXN_ROLE_STAGED];
        payload_source_t src;
        src.from_role = true;
        src.src_backend = backend;
        src.src_role = CONFIG_TXN_ROLE_STAGED;
        src.ram_payload = NULL;
        result = program_role(backend, CONFIG_TXN_ROLE_RUNNING, staged->record_format_version, staged->payload_format,
                              staged->schema_version, staged->payload_size, staged->payload_crc32, new_generation, &src);
    }
    if (result != CONFIG_TXN_BACKEND_OK)
    {
        /* LAST_KNOWN_GOOD (if just promoted) already holds the previous
         * RUNNING content, confirmed committed -- config_transaction_backend_recover()
         * on the next boot correctly reports CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED. */
        return finish(backend, result);
    }

    if (out_running_info != NULL)
    {
        *out_running_info = backend->roles[CONFIG_TXN_ROLE_RUNNING];
    }

    /* Best-effort: STAGED already served its purpose; a failure to erase it
     * does not fail the commit that already landed in RUNNING. */
    (void)erase_role(backend, CONFIG_TXN_ROLE_STAGED);
    backend->roles[CONFIG_TXN_ROLE_STAGED].commit_state = config_transaction_backend_classify_commit(
        &backend->storage, role_offset(CONFIG_TXN_ROLE_STAGED), &backend->roles[CONFIG_TXN_ROLE_STAGED]);
    recompute_recovery(backend);

    return finish(backend, CONFIG_TXN_BACKEND_OK);
}

config_transaction_backend_result_t config_transaction_backend_recover_fallback_to_last_known_good(
    config_transaction_backend_t *backend, config_transaction_record_info_t *out_running_info)
{
    config_transaction_backend_result_t result;
    if (backend == NULL)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }
    if (!backend->recovered || backend->mutations_blocked)
    {
        return finish(backend, CONFIG_TXN_BACKEND_INDETERMINATE);
    }
    if (backend->roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state != CONFIG_TXN_COMMIT_COMMITTED)
    {
        return finish(backend, CONFIG_TXN_BACKEND_NOT_FOUND);
    }
    result = copy_role(backend, CONFIG_TXN_ROLE_LAST_KNOWN_GOOD, CONFIG_TXN_ROLE_RUNNING);
    if (result != CONFIG_TXN_BACKEND_OK)
    {
        return finish(backend, result);
    }
    if (out_running_info != NULL)
    {
        *out_running_info = backend->roles[CONFIG_TXN_ROLE_RUNNING];
    }
    recompute_recovery(backend);
    return finish(backend, CONFIG_TXN_BACKEND_OK);
}

config_transaction_backend_result_t config_transaction_backend_reset_all(config_transaction_backend_t *backend)
{
    config_transaction_backend_result_t result = CONFIG_TXN_BACKEND_OK;
    int i;
    if (backend == NULL)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }
    /* Deliberately not gated on mutations_blocked: this is the only way to
     * recover from a CONFIG_TXN_RECOVERY_FATAL state. */
    for (i = 0; i < (int)CONFIG_TXN_ROLE_COUNT; ++i)
    {
        const config_transaction_backend_result_t role_result = erase_role(backend, (config_txn_role_t)i);
        if (role_result != CONFIG_TXN_BACKEND_OK)
        {
            result = role_result;
        }
    }
    (void)config_transaction_backend_recover(backend);
    return finish(backend, result);
}

config_transaction_backend_result_t config_transaction_backend_read(
    const config_transaction_backend_t *backend, config_txn_role_t role, void *buffer, size_t buffer_size,
    size_t *out_payload_size, config_transaction_record_info_t *out_info)
{
    const config_transaction_record_info_t *info;
    uint32_t remaining;
    uint32_t offset;
    uint32_t crc_state;
    uint8_t *dst;

    if (backend == NULL || buffer == NULL || role >= CONFIG_TXN_ROLE_COUNT)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }
    if (!backend->recovered)
    {
        return CONFIG_TXN_BACKEND_INDETERMINATE;
    }
    info = &backend->roles[role];
    if (info->commit_state != CONFIG_TXN_COMMIT_COMMITTED)
    {
        return CONFIG_TXN_BACKEND_NOT_FOUND;
    }
    if (buffer_size < info->payload_size)
    {
        return CONFIG_TXN_BACKEND_INVALID_ARGUMENT;
    }

    dst = (uint8_t *)buffer;
    remaining = info->payload_size;
    offset = 0u;
    crc_state = 0xFFFFFFFFu;
    while (remaining > 0u)
    {
        const size_t chunk = remaining < CONFIG_TXN_PAGE_SIZE_BYTES ? (size_t)remaining : (size_t)CONFIG_TXN_PAGE_SIZE_BYTES;
        if (!backend->storage.read(backend->storage.context, role_offset(role) + CONFIG_TXN_PAGE_SIZE_BYTES + offset,
                                   dst + offset, chunk))
        {
            return CONFIG_TXN_BACKEND_IO;
        }
        crc_state = crc32_update(crc_state, dst + offset, chunk);
        offset += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
    }
    /* Defensive re-check: Flash content could in principle have changed
     * since the cached metadata was last classified. */
    if ((crc_state ^ 0xFFFFFFFFu) != info->payload_crc32)
    {
        return CONFIG_TXN_BACKEND_IO;
    }

    if (out_payload_size != NULL)
    {
        *out_payload_size = info->payload_size;
    }
    if (out_info != NULL)
    {
        *out_info = *info;
    }
    return CONFIG_TXN_BACKEND_OK;
}

const config_transaction_record_info_t *config_transaction_backend_role_info(
    const config_transaction_backend_t *backend, config_txn_role_t role)
{
    if (backend == NULL || role >= CONFIG_TXN_ROLE_COUNT)
    {
        return NULL;
    }
    return &backend->roles[role];
}

#if defined(PICO_ON_DEVICE)
/* Three equal CONFIG_TXN_ROLE_SIZE_BYTES regions carved out of the existing
 * 128 KiB config-slot pair (OTA_LAYOUT_CONFIG_SLOT_0/1, unchanged by this
 * module), leaving a small reserved tail unused. No new Flash region, no
 * OTA-bank shrink -- see the header's file-level doc comment. */
#define CONFIG_TXN_REGION_BASE_OFFSET_BYTES OTA_LAYOUT_CONFIG_SLOT_0_OFFSET_BYTES
#define CONFIG_TXN_REGION_TOTAL_SIZE_BYTES ((uint32_t)(OTA_LAYOUT_CONFIG_SLOT_SIZE_BYTES) * 2u)

_Static_assert((uint32_t)CONFIG_TXN_ROLE_SIZE_BYTES * (uint32_t)CONFIG_TXN_ROLE_COUNT <= CONFIG_TXN_REGION_TOTAL_SIZE_BYTES,
               "config transaction roles must fit within the existing 128 KiB config-slot budget");

/** @brief Storage read callback: bounds-checked direct XIP read at OTA_LAYOUT_CONFIG_SLOT_0_ORIGIN + offset. */
static bool flash_read(void *unused, uint32_t offset, uint8_t *dst, size_t n)
{
    (void)unused;
    if (offset > CONFIG_TXN_REGION_TOTAL_SIZE_BYTES || n > CONFIG_TXN_REGION_TOTAL_SIZE_BYTES - offset)
    {
        return false;
    }
    memcpy(dst, (const void *)(uintptr_t)(OTA_LAYOUT_CONFIG_SLOT_0_ORIGIN + offset), n);
    return true;
}
/** @brief Storage program callback: bounds-checked, routes through the shared flash_operation_coordinator. */
static bool flash_program(void *unused, uint32_t offset, const uint8_t page[CONFIG_TXN_PAGE_SIZE_BYTES])
{
    (void)unused;
    if (offset > CONFIG_TXN_REGION_TOTAL_SIZE_BYTES - CONFIG_TXN_PAGE_SIZE_BYTES)
    {
        return false;
    }
    return flash_operation_program_page(CONFIG_TXN_REGION_BASE_OFFSET_BYTES + offset, page) == FLASH_OPERATION_OK;
}
/** @brief Storage erase callback: bounds-checked, routes through the shared flash_operation_coordinator. */
static bool flash_erase(void *unused, uint32_t offset)
{
    (void)unused;
    if (offset > CONFIG_TXN_REGION_TOTAL_SIZE_BYTES - CONFIG_TXN_SECTOR_SIZE_BYTES)
    {
        return false;
    }
    return flash_operation_erase_sector(CONFIG_TXN_REGION_BASE_OFFSET_BYTES + offset) == FLASH_OPERATION_OK;
}
void config_transaction_backend_init_flash(config_transaction_backend_t *backend)
{
    const config_transaction_storage_t storage = {
        .context = NULL, .read = flash_read, .program_page = flash_program, .erase_sector = flash_erase
    };
    config_transaction_backend_init(backend, &storage);
}
#else
void config_transaction_backend_init_flash(config_transaction_backend_t *backend)
{
    config_transaction_backend_init(backend, NULL);
}
#endif

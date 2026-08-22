#ifndef DRIVERS_HISTORY_FLASH_BACKEND_H
#define DRIVERS_HISTORY_FLASH_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/platform_limits.h"
#include "services/service_types.h"
#include "services/telemetry_channel.h"

/*
 * v0.4.3 Flash-backed per-channel history ring.
 *
 * This is a CYCLIC BUFFER WITHOUT TRANSACTIONAL GUARANTEES, unlike
 * drivers/maintenance_journal_backend.  There is no commit-page, no
 * multi-generation chain-of-custody classification and no cross-sample
 * recovery state machine.  Two properties are still guaranteed:
 *
 *   1. A torn or unreadable sample (interrupted mid-write) is detected via a
 *      per-record CRC32 plus an explicit valid/invalid marker byte and is
 *      skipped on restore -- it is never exposed as if it were good data.
 *   2. Losing the most recently recorded, not-yet-flushed samples after a
 *      power loss is accepted, expected behavior.
 *
 * Write policy (frozen, see docs/releases/v0.4.3.md T3): BATCHED, not
 * synchronous-per-sample.  Flash program operations must write one complete,
 * page-aligned FLASH_PAGE_SIZE (256 byte) buffer per hardware program call
 * (see drivers/flash_operation_coordinator.h), and re-programming an
 * already-programmed page without an intervening sector erase is not a
 * portably reliable operation on QSPI NOR flash.  A 32-byte packed record
 * (HISTORY_FLASH_BACKEND_RECORD_SIZE) therefore cannot be written on its
 * own; HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE (8) records are accumulated in
 * a small internal-SRAM staging buffer per channel and flushed as a single
 * page program once the buffer is full. Consequences, by design:
 *   - up to HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE - 1 (7) of the most
 *     recent samples per channel may be lost on power loss (accepted, see
 *     above);
 *   - Flash program operations happen 8x less often than sample writes,
 *     bounding worst-case blocking time on the calling task (sensor_manager
 *     task, priority 3, 2 KiB stack) to at most one
 *     flash_operation_program_page() call (<=1000 ms coordinator timeout,
 *     typically single-digit milliseconds) per 8 recorded samples per
 *     channel, plus an occasional sector erase (same bound) once every
 *     HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR (16) pages;
 *   - every physical page is either wholly unwritten (erased, 0xFF) or
 *     wholly committed (all 8 records written in one program call); restore
 *     therefore never has to reason about a partially-written page, only
 *     about a possibly-torn *bit pattern* within an otherwise-committed
 *     page, which the per-record CRC still catches.
 *
 * Per-channel physical layout (matches the frozen 256-samples/channel PSRAM
 * capacity in services/platform_limits.h):
 *   - HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES (8192 = 2 * 4096)
 *     bytes per channel, split into exactly two erase sectors ("A" and
 *     "B").  Only one sector is "active" (being filled) at a time; the
 *     other holds the previous generation's still-valid records or is a
 *     freshly erased spare.  When the active sector becomes completely full
 *     (all HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR pages written), the OTHER
 *     sector is erased (dropping its old generation of up to 128 records at
 *     once -- this is the per-channel eviction event) and becomes the new
 *     active sector.  This two-generation double-buffer is the same
 *     "fill one region while the other stays readable, then swap" shape as
 *     drivers/maintenance_journal_backend, without its multi-generation
 *     chain validation, because no transactional guarantee is required
 *     here.
 *   - Consequence: the number of samples actually retrievable for a channel
 *     oscillates between HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR *
 *     HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE (128, just after a sector
 *     swap) and the full 256 (just before the next swap). This is within
 *     the frozen "one fixed sample-count capacity per channel" retention
 *     policy: it never exceeds 256 and is a direct, documented consequence
 *     of erase-sector granularity.
 *
 * Total budget: HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES *
 * PLATFORM_CHANNEL_LIMIT (16) == HISTORY_FLASH_BACKEND_TOTAL_SIZE_BYTES ==
 * 131072 == 128 KiB, matching the OTA_HISTORY_* region carved out of
 * ota/cmake/ota_layout.cmake.
 *
 * Channel identity binding: each record additionally carries a 4-byte
 * channel_id_hash (the full, unfolded 32-bit FNV-1a hash of the owning
 * channel's stable id -- the same hash primitive already used for this
 * check, simply no longer truncated).  On restore, the caller
 * (services/history_service.c) recomputes the hash for the channel_id it
 * currently intends to bind to this physical slot index and passes it in; a
 * record whose stored hash does not match is treated as foreign data left
 * over from a different configuration and is skipped, never exposed as this
 * channel's history.  Physical slot assignment (services/history_service.c's
 * first-available-ring-index claim over the channel registry, walked in
 * registry index order) is NOT guaranteed stable across a reconfiguration
 * that adds, removes, or reorders channels: the same physical slot can end
 * up bound to a completely different channel_id after a reboot with an
 * edited channel list, with only this hash to distinguish genuine records
 * from foreign ones left behind by the previous occupant. This is still a
 * best-effort collision-bounded check, not a cryptographic guarantee -- two
 * distinct channel ids could theoretically hash identically, in which case
 * foreign data would be silently accepted -- but widening from 8 to 32 bits
 * lowers the false-accept rate on an unrelated id from 1/256 to 1/2^32. This
 * field is the sole persisted defense against misattributing restored Flash
 * history to the wrong channel after a config change, so its width directly
 * bounds that risk. The full-fidelity id string itself is owned by the
 * caller's PSRAM ring, not replicated into every 32-byte Flash record, to
 * stay inside the budget above.
 *
 * Record format version: to make the 8-to-32-bit widening safe against
 * misreading old-format data, every record also carries a 1-byte
 * format-version marker (RECORD_FORMAT_VERSION in
 * history_flash_backend.c), occupying the exact physical byte offset the
 * pre-widening layout always wrote as its fixed 0xA5 valid-marker byte.  A
 * record written under the old 8-bit-hash/64-bit-sequence layout therefore
 * always reads back with that byte equal to 0xA5, which can never equal the
 * current RECORD_FORMAT_VERSION; decode treats the mismatch as
 * corrupt/foreign and skips the record, so pre-widening Flash contents are
 * never misread as valid post-widening records.  This mirrors how
 * drivers/maintenance_journal_backend.c versions its own record schema.
 */

#define HISTORY_FLASH_BACKEND_CHANNEL_COUNT PLATFORM_CHANNEL_LIMIT
#define HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL PLATFORM_HISTORY_SAMPLES_PER_CHANNEL
#define HISTORY_FLASH_BACKEND_RECORD_SIZE 32u
#define HISTORY_FLASH_BACKEND_PAGE_SIZE 256u
#define HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE (HISTORY_FLASH_BACKEND_PAGE_SIZE / HISTORY_FLASH_BACKEND_RECORD_SIZE)
#define HISTORY_FLASH_BACKEND_SECTOR_SIZE 4096u
#define HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR (HISTORY_FLASH_BACKEND_SECTOR_SIZE / HISTORY_FLASH_BACKEND_PAGE_SIZE)
#define HISTORY_FLASH_BACKEND_SECTORS_PER_CHANNEL 2u
#define HISTORY_FLASH_BACKEND_PAGES_PER_CHANNEL \
    (HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR * HISTORY_FLASH_BACKEND_SECTORS_PER_CHANNEL)
#define HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES \
    (HISTORY_FLASH_BACKEND_SECTOR_SIZE * HISTORY_FLASH_BACKEND_SECTORS_PER_CHANNEL)
#define HISTORY_FLASH_BACKEND_TOTAL_SIZE_BYTES \
    (HISTORY_FLASH_BACKEND_CHANNEL_REGION_SIZE_BYTES * HISTORY_FLASH_BACKEND_CHANNEL_COUNT)

typedef union
{
    float f32;
    double f64;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    bool boolean;
} history_flash_backend_value_t;

typedef struct
{
    history_flash_backend_value_t value;
    telemetry_channel_value_type_t value_type;
    service_quality_t quality;
    uint64_t source_timestamp_unix_us;
} history_flash_backend_record_t;

/* history_flash_backend_restore()'s bounded working set for one channel
 * (up to HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL entries). This is
 * deliberately NOT a static internal-SRAM buffer inside the driver: at
 * ~40 bytes/entry it would cost multiple KiB of scarce internal SRAM
 * permanently for a scan that only ever runs once per channel at startup.
 * The caller (services/history_service.c, which already owns a PSRAM
 * allocator dependency) supplies and owns this scratch memory -- typically
 * one PSRAM-backed array reused sequentially across every channel's
 * restore() call -- keeping this driver itself allocator-agnostic. */
typedef struct
{
    uint32_t sequence;
    uint8_t sector;
    history_flash_backend_record_t record;
} history_flash_backend_scan_entry_t;

typedef bool (*history_flash_backend_read_fn)(void *context, uint32_t offset,
                                              uint8_t *destination, size_t size);
typedef bool (*history_flash_backend_program_fn)(void *context, uint32_t offset,
                                                  const uint8_t page[HISTORY_FLASH_BACKEND_PAGE_SIZE]);
typedef bool (*history_flash_backend_erase_fn)(void *context, uint32_t offset);

typedef struct
{
    void *context;
    history_flash_backend_read_fn read;
    history_flash_backend_program_fn program_page;
    history_flash_backend_erase_fn erase_sector;
} history_flash_backend_storage_t;

typedef struct
{
    bool bound;
    uint32_t channel_id_hash;
    uint8_t active_sector; /* 0 or 1 */
    uint8_t next_page_in_sector; /* 0..HISTORY_FLASH_BACKEND_PAGES_PER_SECTOR */
    uint8_t staging_count; /* 0..HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE-1 while accumulating */
    uint32_t next_sequence;
    uint8_t staging_page[HISTORY_FLASH_BACKEND_PAGE_SIZE];
} history_flash_backend_channel_state_t;

typedef struct
{
    history_flash_backend_storage_t storage;
    history_flash_backend_channel_state_t channels[HISTORY_FLASH_BACKEND_CHANNEL_COUNT];
    bool ready;
} history_flash_backend_t;

typedef enum
{
    HISTORY_FLASH_BACKEND_OK = 0,
    HISTORY_FLASH_BACKEND_INVALID_ARGUMENT,
    HISTORY_FLASH_BACKEND_NOT_BOUND,
    HISTORY_FLASH_BACKEND_IO
} history_flash_backend_result_t;

void history_flash_backend_init(history_flash_backend_t *backend,
                                const history_flash_backend_storage_t *storage);

/* Binds physical channel slot (0..HISTORY_FLASH_BACKEND_CHANNEL_COUNT-1) to
 * channel_id and reconstructs the write cursor by scanning that slot's two
 * sectors.  Must be called exactly once per slot before
 * history_flash_backend_restore() or history_flash_backend_append() use it.
 * Safe/idempotent to call with a NULL or empty channel_id: the slot becomes
 * bound to "no channel", and restore/append for it are no-ops. */
history_flash_backend_result_t history_flash_backend_bind_channel(history_flash_backend_t *backend,
                                                                   uint8_t slot,
                                                                   const char *channel_id);

/* Replays every record recognized as valid and belonging to slot's bound
 * channel_id (matching CRC32, valid marker, and channel_id_hash), oldest to
 * newest, into callback.  Bounded to at most
 * HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL callback invocations. Must be
 * called after history_flash_backend_bind_channel() for the same slot and
 * before any history_flash_backend_append() call for it, and before the
 * OPC UA HistoryRead surface is exposed.
 *
 * scratch/scratch_capacity is the caller-owned bounded working set described
 * above; scratch_capacity must be >= HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL
 * or HISTORY_FLASH_BACKEND_INVALID_ARGUMENT is returned. The same scratch
 * buffer may be reused sequentially across every slot's restore() call. */
typedef void (*history_flash_backend_restore_cb)(void *user_context,
                                                  const history_flash_backend_record_t *record);
history_flash_backend_result_t history_flash_backend_restore(history_flash_backend_t *backend,
                                                              uint8_t slot,
                                                              history_flash_backend_scan_entry_t *scratch,
                                                              size_t scratch_capacity,
                                                              history_flash_backend_restore_cb callback,
                                                              void *user_context);

/* Accumulates record into slot's staging buffer; flushes (erase-if-needed
 * plus one page program) only when the buffer becomes full.  Never blocks
 * on more than one Flash program or one Flash erase call. */
history_flash_backend_result_t history_flash_backend_append(history_flash_backend_t *backend,
                                                             uint8_t slot,
                                                             const history_flash_backend_record_t *record);

/* RP2350 application binding: reads are direct XIP reads at
 * OTA_LAYOUT_HISTORY_ORIGIN + offset; program/erase are routed through the
 * shared flash_operation_coordinator at OTA_LAYOUT_HISTORY_OFFSET_BYTES +
 * offset, exactly like drivers/maintenance_journal_backend.c. */
void history_flash_backend_init_flash(history_flash_backend_t *backend);

_Static_assert(HISTORY_FLASH_BACKEND_RECORD_SIZE * HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE ==
                   HISTORY_FLASH_BACKEND_PAGE_SIZE,
               "history record size must evenly divide the Flash program page size");
_Static_assert(HISTORY_FLASH_BACKEND_PAGES_PER_CHANNEL * HISTORY_FLASH_BACKEND_RECORDS_PER_PAGE ==
                   HISTORY_FLASH_BACKEND_SAMPLES_PER_CHANNEL,
               "per-channel Flash page/record geometry must match the frozen per-channel sample capacity");
_Static_assert(HISTORY_FLASH_BACKEND_TOTAL_SIZE_BYTES == 131072u,
               "Flash history budget changed from the frozen 128 KiB (see docs/releases/v0.4.3.md)");

#endif

#ifndef DRIVERS_MAINTENANCE_JOURNAL_BACKEND_H
#define DRIVERS_MAINTENANCE_JOURNAL_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/maintenance_journal_codec.h"

/* This is a physical Flash driver contract, not a service state machine. */
#define MAINTENANCE_JOURNAL_SECTOR_COUNT 8u
#define MAINTENANCE_JOURNAL_SECTOR_SIZE 4096u
#define MAINTENANCE_JOURNAL_HEADER_SIZE 256u
#define MAINTENANCE_JOURNAL_SLOTS_PER_SECTOR 3u
#define MAINTENANCE_JOURNAL_RESERVED_TAIL_SIZE 768u
#define MAINTENANCE_JOURNAL_TOTAL_SLOTS 24u
#define MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS 15u
#define MAINTENANCE_JOURNAL_MAX_TRANSIENT_RECORDS 16u
#define MAINTENANCE_JOURNAL_MIN_ERASED_SECTORS 2u

#define MAINTENANCE_JOURNAL_HEADER_MAGIC 0x31484a4du /* "MJH1", little endian */
#define MAINTENANCE_JOURNAL_HEADER_FORMAT_VERSION 1u
#define MAINTENANCE_JOURNAL_HEADER_VERSION 1u

typedef enum
{
    MAINTENANCE_JOURNAL_COMMIT_COMMITTED = 0,
    MAINTENANCE_JOURNAL_COMMIT_NOT_COMMITTED,
    MAINTENANCE_JOURNAL_COMMIT_INDETERMINATE,
    MAINTENANCE_JOURNAL_COMMIT_CORRUPT,
    /* Exact marker plus a recognized record envelope with an unknown schema.
     * It is preserved, never decoded as a snapshot, and blocks mutation. */
    MAINTENANCE_JOURNAL_COMMIT_UNKNOWN
} maintenance_journal_commit_state_t;

typedef enum
{
    MAINTENANCE_JOURNAL_BACKEND_OK = 0,
    MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT,
    MAINTENANCE_JOURNAL_BACKEND_IO,
    MAINTENANCE_JOURNAL_BACKEND_CAPACITY,
    MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE,
    MAINTENANCE_JOURNAL_BACKEND_DEGRADED,
    MAINTENANCE_JOURNAL_BACKEND_FATAL,
    MAINTENANCE_JOURNAL_BACKEND_UNSUPPORTED,
    MAINTENANCE_JOURNAL_BACKEND_ORDER,
    MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND
} maintenance_journal_backend_result_t;

typedef enum
{
    MAINTENANCE_JOURNAL_RECOVERY_EMPTY = 0,
    MAINTENANCE_JOURNAL_RECOVERY_OK,
    MAINTENANCE_JOURNAL_RECOVERY_DEGRADED,
    MAINTENANCE_JOURNAL_RECOVERY_INDETERMINATE,
    MAINTENANCE_JOURNAL_RECOVERY_FATAL,
    MAINTENANCE_JOURNAL_RECOVERY_UNSUPPORTED
} maintenance_journal_recovery_state_t;

typedef enum
{
    MAINTENANCE_JOURNAL_CAPACITY_READY = 0,
    MAINTENANCE_JOURNAL_CAPACITY_CONSTRAINED,
    MAINTENANCE_JOURNAL_CAPACITY_BLOCKED
} maintenance_journal_capacity_state_t;

typedef bool (*maintenance_journal_read_fn)(void *context, uint32_t offset,
                                            uint8_t *destination, size_t size);
typedef bool (*maintenance_journal_program_fn)(void *context, uint32_t offset,
                                               const uint8_t page[256]);
typedef bool (*maintenance_journal_erase_fn)(void *context, uint32_t offset);

typedef struct
{
    void *context;
    maintenance_journal_read_fn read;
    maintenance_journal_program_fn program_page;
    maintenance_journal_erase_fn erase_sector;
} maintenance_journal_storage_t;

typedef struct
{
    uint8_t sector;
    uint8_t slot;
    uint64_t epoch;
    uint64_t generation;
    maintenance_journal_record_t record;
} maintenance_journal_record_location_t;

typedef struct
{
    maintenance_journal_storage_t storage;
    maintenance_journal_recovery_state_t recovery;
    maintenance_journal_record_location_t active;
    uint32_t corrupt_record_count;
    uint32_t non_exact_commit_count;
    uint32_t unknown_record_count;
    uint32_t erase_count; /* Runtime only; not persisted in an immutable header. */
    uint32_t sector_erase_count[MAINTENANCE_JOURNAL_SECTOR_COUNT];
    uint8_t valid_record_count;
    uint8_t retained_record_count;
    uint8_t transient_record_peak;
    uint8_t erased_sector_count;
    uint8_t unknown_sector_mask;
    maintenance_journal_capacity_state_t capacity_state;
    maintenance_journal_backend_result_t last_error;
    bool has_active;
    bool mutations_blocked;
} maintenance_journal_backend_t;

_Static_assert(MAINTENANCE_JOURNAL_SECTOR_COUNT * MAINTENANCE_JOURNAL_SECTOR_SIZE == 32768u,
               "journal physical capacity changed");
_Static_assert(MAINTENANCE_JOURNAL_HEADER_SIZE +
                   MAINTENANCE_JOURNAL_SLOTS_PER_SECTOR * MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE +
                   MAINTENANCE_JOURNAL_RESERVED_TAIL_SIZE == MAINTENANCE_JOURNAL_SECTOR_SIZE,
               "sector page layout changed");

/* Calls exactly one complete 1024-byte read after a commit-program attempt.
 * The program API result deliberately does not participate: physical post-state
 * is the authority. Exact pages with an unknown record schema are UNKNOWN, not
 * CORRUPT, so recovery can preserve their sector. */
maintenance_journal_commit_state_t maintenance_journal_backend_classify_commit(
    const maintenance_journal_storage_t *storage, uint32_t slot_offset,
    uint8_t slot_scratch[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE]);

void maintenance_journal_backend_init(maintenance_journal_backend_t *backend,
                                      const maintenance_journal_storage_t *storage);
maintenance_journal_backend_result_t maintenance_journal_backend_recover(
    maintenance_journal_backend_t *backend);
maintenance_journal_backend_result_t maintenance_journal_backend_append(
    maintenance_journal_backend_t *backend, const maintenance_journal_record_t *candidate,
    maintenance_journal_record_location_t *out_location);
maintenance_journal_backend_result_t maintenance_journal_backend_reset(
    maintenance_journal_backend_t *backend, const maintenance_journal_record_t *snapshot,
    maintenance_journal_record_location_t *out_location);

/* RP2350 application binding. The buffers are static internal SRAM and never
 * live on a FreeRTOS task stack. Host tests should use the storage callbacks. */
void maintenance_journal_backend_init_flash(maintenance_journal_backend_t *backend);

#endif

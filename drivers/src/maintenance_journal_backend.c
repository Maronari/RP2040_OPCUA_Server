#include "drivers/maintenance_journal_backend.h"

#include <limits.h>
#include <string.h>

#if defined(PICO_ON_DEVICE)
#include "drivers/flash_operation_coordinator.h"
#include "ota_layout.h"
#endif

enum
{
    HEADER_OFFSET_MAGIC = 0u,
    HEADER_OFFSET_FORMAT = 4u,
    HEADER_OFFSET_VERSION = 5u,
    HEADER_OFFSET_LENGTH = 6u,
    HEADER_OFFSET_EPOCH = 8u,
    HEADER_OFFSET_GENERATION = 16u,
    HEADER_OFFSET_PREDECESSOR_EPOCH = 24u,
    HEADER_OFFSET_PREDECESSOR_GENERATION = 32u,
    HEADER_OFFSET_SECTOR = 40u,
    HEADER_OFFSET_CRC = 252u
};

enum { RECORD_OFFSET_MAGIC = 0u, RECORD_OFFSET_SCHEMA = 4u };

typedef enum { HEADER_ERASED, HEADER_VALID, HEADER_INVALID, HEADER_UNKNOWN } header_state_t;
typedef struct
{
    header_state_t state;
    uint64_t epoch, generation, predecessor_epoch, predecessor_generation;
} sector_header_t;
typedef struct
{
    uint8_t sector;
    uint8_t slot;
    uint64_t epoch;
    uint64_t generation;
    uint64_t sequence;
} scan_record_t;

/* Production uses these, so neither codec nor Flash-operation temporary pages
 * consume a task stack.  They are intentionally in normal .bss/internal SRAM. */
static uint8_t g_journal_header_page[MAINTENANCE_JOURNAL_HEADER_SIZE];
static uint8_t g_journal_slot_page[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
static uint8_t g_journal_verify_page[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
static sector_header_t g_journal_headers[MAINTENANCE_JOURNAL_SECTOR_COUNT];
static scan_record_t g_journal_records[MAINTENANCE_JOURNAL_TOTAL_SLOTS];
static maintenance_journal_record_t g_journal_decoded_record;

static uint16_t read_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8u); }
static uint32_t read_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u); }
static uint64_t read_u64(const uint8_t *p) { uint64_t v = 0u; for (size_t i = 0; i < 8u; ++i) v |= (uint64_t)p[i] << (8u * i); return v; }
static void write_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u); }
static void write_u32(uint8_t *p, uint32_t v) { for (size_t i = 0; i < 4u; ++i) p[i] = (uint8_t)(v >> (8u * i)); }
static void write_u64(uint8_t *p, uint64_t v) { for (size_t i = 0; i < 8u; ++i) p[i] = (uint8_t)(v >> (8u * i)); }
static bool erased(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i] != 0xffu) return false; return true; }
static uint32_t sector_offset(uint8_t s) { return (uint32_t)s * MAINTENANCE_JOURNAL_SECTOR_SIZE; }
static uint32_t slot_offset(uint8_t s, uint8_t slot) { return sector_offset(s) + MAINTENANCE_JOURNAL_HEADER_SIZE + (uint32_t)slot * MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE; }
static bool serial_newer(uint64_t a, uint64_t b) { const uint64_t d = a - b; return d != 0u && d < (UINT64_C(1) << 63u); }
static bool serial_unordered(uint64_t a, uint64_t b) { return a == b || (a - b) == (UINT64_C(1) << 63u); }
static void note_allocated(maintenance_journal_backend_t *b, uint64_t epoch, uint64_t sequence)
{
    if (b->allocated_epoch != epoch || b->allocated_sequence == 0u ||
        serial_newer(sequence, b->allocated_sequence))
    {
        b->allocated_epoch = epoch;
        b->allocated_sequence = sequence;
    }
}
static bool tuple_newer(const scan_record_t *a, const scan_record_t *b, bool *unordered)
{
    if (a->epoch != b->epoch) { *unordered = serial_unordered(a->epoch, b->epoch); return serial_newer(a->epoch, b->epoch); }
    *unordered = serial_unordered(a->sequence, b->sequence);
    return serial_newer(a->sequence, b->sequence);
}

static header_state_t decode_header(const uint8_t page[256], uint8_t index, sector_header_t *out)
{
    if (erased(page, MAINTENANCE_JOURNAL_HEADER_SIZE)) return HEADER_ERASED;
    if (read_u32(&page[HEADER_OFFSET_MAGIC]) != MAINTENANCE_JOURNAL_HEADER_MAGIC) return HEADER_INVALID;
    if (page[HEADER_OFFSET_FORMAT] != MAINTENANCE_JOURNAL_HEADER_FORMAT_VERSION ||
        page[HEADER_OFFSET_VERSION] != MAINTENANCE_JOURNAL_HEADER_VERSION) return HEADER_UNKNOWN;
    if (read_u16(&page[HEADER_OFFSET_LENGTH]) != MAINTENANCE_JOURNAL_HEADER_SIZE ||
        page[HEADER_OFFSET_SECTOR] != index ||
        !erased(&page[HEADER_OFFSET_SECTOR + 1u], HEADER_OFFSET_CRC - HEADER_OFFSET_SECTOR - 1u) ||
        read_u32(&page[HEADER_OFFSET_CRC]) != maintenance_journal_codec_crc32(page, HEADER_OFFSET_CRC)) return HEADER_INVALID;
    out->epoch = read_u64(&page[HEADER_OFFSET_EPOCH]); out->generation = read_u64(&page[HEADER_OFFSET_GENERATION]);
    out->predecessor_epoch = read_u64(&page[HEADER_OFFSET_PREDECESSOR_EPOCH]); out->predecessor_generation = read_u64(&page[HEADER_OFFSET_PREDECESSOR_GENERATION]);
    if (out->epoch == 0u || out->epoch == UINT64_MAX || out->generation == 0u || out->generation == UINT64_MAX) return HEADER_INVALID;
    return HEADER_VALID;
}

static void encode_header(uint8_t page[256], uint8_t index, uint64_t epoch, uint64_t generation, uint64_t pe, uint64_t pg)
{
    memset(page, 0xff, MAINTENANCE_JOURNAL_HEADER_SIZE);
    write_u32(&page[HEADER_OFFSET_MAGIC], MAINTENANCE_JOURNAL_HEADER_MAGIC); page[HEADER_OFFSET_FORMAT] = MAINTENANCE_JOURNAL_HEADER_FORMAT_VERSION; page[HEADER_OFFSET_VERSION] = MAINTENANCE_JOURNAL_HEADER_VERSION;
    write_u16(&page[HEADER_OFFSET_LENGTH], MAINTENANCE_JOURNAL_HEADER_SIZE); write_u64(&page[HEADER_OFFSET_EPOCH], epoch); write_u64(&page[HEADER_OFFSET_GENERATION], generation); write_u64(&page[HEADER_OFFSET_PREDECESSOR_EPOCH], pe); write_u64(&page[HEADER_OFFSET_PREDECESSOR_GENERATION], pg); page[HEADER_OFFSET_SECTOR] = index;
    write_u32(&page[HEADER_OFFSET_CRC], maintenance_journal_codec_crc32(page, HEADER_OFFSET_CRC));
}

maintenance_journal_commit_state_t maintenance_journal_backend_classify_commit(const maintenance_journal_storage_t *storage, uint32_t offset, uint8_t scratch[1024])
{
    maintenance_journal_record_t decoded;
    if (storage == NULL || storage->read == NULL || scratch == NULL || !storage->read(storage->context, offset, scratch, 1024u)) return MAINTENANCE_JOURNAL_COMMIT_INDETERMINATE;
    if (!maintenance_journal_codec_has_exact_commit_page(scratch, 1024u)) return MAINTENANCE_JOURNAL_COMMIT_NOT_COMMITTED;
    if (read_u32(&scratch[RECORD_OFFSET_MAGIC]) == MAINTENANCE_JOURNAL_RECORD_MAGIC &&
        read_u16(&scratch[RECORD_OFFSET_SCHEMA]) != MAINTENANCE_JOURNAL_SCHEMA_VERSION)
    {
        return MAINTENANCE_JOURNAL_COMMIT_UNKNOWN;
    }
    return maintenance_journal_codec_decode(scratch, 1024u, &decoded) == MAINTENANCE_JOURNAL_CODEC_OK ? MAINTENANCE_JOURNAL_COMMIT_COMMITTED : MAINTENANCE_JOURNAL_COMMIT_CORRUPT;
}

static maintenance_journal_backend_result_t finish(maintenance_journal_backend_t *b,
                                                    maintenance_journal_backend_result_t result)
{
    if (b != NULL)
    {
        b->last_error = result;
        b->retained_record_count = b->valid_record_count;
        b->capacity_state = b->mutations_blocked ? MAINTENANCE_JOURNAL_CAPACITY_BLOCKED :
                            (b->erased_sector_count >= MAINTENANCE_JOURNAL_MIN_ERASED_SECTORS ?
                             MAINTENANCE_JOURNAL_CAPACITY_READY : MAINTENANCE_JOURNAL_CAPACITY_CONSTRAINED);
    }
    return result;
}

void maintenance_journal_backend_init(maintenance_journal_backend_t *b, const maintenance_journal_storage_t *s)
{
    if (b == NULL)
    {
        return;
    }
    memset(b, 0, sizeof(*b));
    if (s != NULL)
    {
        b->storage = *s;
    }
    b->recovery = MAINTENANCE_JOURNAL_RECOVERY_INDETERMINATE;
    b->mutations_blocked = true;
    b->logical_generation = 1u;
}

static bool read_header(const maintenance_journal_backend_t *b, uint8_t sector, sector_header_t *h)
{ return b->storage.read(b->storage.context, sector_offset(sector), g_journal_header_page, 256u) && (h->state = decode_header(g_journal_header_page, sector, h), true); }
static bool write_header(maintenance_journal_backend_t *b, uint8_t s, uint64_t e, uint64_t g, uint64_t pe, uint64_t pg)
{
    sector_header_t h; encode_header(g_journal_header_page, s, e, g, pe, pg);
    if (!b->storage.program_page(b->storage.context, sector_offset(s), g_journal_header_page) || !read_header(b, s, &h)) return false;
    return h.state == HEADER_VALID && h.epoch == e && h.generation == g && h.predecessor_epoch == pe && h.predecessor_generation == pg;
}

static bool read_scan_record(const maintenance_journal_backend_t *b,
                             const scan_record_t *scan,
                             maintenance_journal_record_t *out)
{
    return b->storage.read(b->storage.context, slot_offset(scan->sector, scan->slot),
                           g_journal_slot_page, MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE) &&
           maintenance_journal_codec_decode(g_journal_slot_page,
                                            MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE,
                                            out) == MAINTENANCE_JOURNAL_CODEC_OK;
}

static bool snapshot_equal(const maintenance_service_snapshot_t *left,
                           const maintenance_service_snapshot_t *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static const maintenance_journal_record_t g_immutable_initial_record = {
    .epoch = 1u,
    .snapshot = {
        .state = MAINTENANCE_SERVICE_STATE_FINISHED,
        .revision = 1u,
        .has_activity = false
    }
};

static bool activity_identity_equal(const maintenance_activity_t *left,
                                    const maintenance_activity_t *right)
{
    return memcmp(left->operation_id, right->operation_id, sizeof(left->operation_id)) == 0;
}

static bool activity_planning_equal(const maintenance_activity_t *left,
                                    const maintenance_activity_t *right)
{
    return left->planned_date_utc_100ns == right->planned_date_utc_100ns &&
           left->estimated_downtime_ms == right->estimated_downtime_ms &&
           left->method == right->method &&
           memcmp(left->supplier, right->supplier, sizeof(left->supplier)) == 0 &&
           memcmp(left->personnel, right->personnel, sizeof(left->personnel)) == 0;
}

static bool activity_completion_equal(const maintenance_activity_t *left,
                                      const maintenance_activity_t *right)
{
    return left->configuration_changed == right->configuration_changed &&
           left->parts_serviced_count == right->parts_serviced_count &&
           left->parts_replaced_count == right->parts_replaced_count &&
           memcmp(left->parts_serviced, right->parts_serviced,
                  sizeof(left->parts_serviced)) == 0 &&
           memcmp(left->parts_replaced, right->parts_replaced,
                  sizeof(left->parts_replaced)) == 0 &&
           memcmp(left->result, right->result, sizeof(left->result)) == 0;
}

static bool plan_activity_is_service_created(const maintenance_activity_t *activity)
{
    static const maintenance_activity_t empty_activity = {0};

    return !activity->configuration_changed && activity->parts_serviced_count == 0u &&
           activity->parts_replaced_count == 0u &&
           memcmp(activity->parts_serviced, empty_activity.parts_serviced,
                  sizeof(activity->parts_serviced)) == 0 &&
           memcmp(activity->parts_replaced, empty_activity.parts_replaced,
                  sizeof(activity->parts_replaced)) == 0 &&
           memcmp(activity->result, empty_activity.result, sizeof(activity->result)) == 0;
}

/* Codec validation establishes canonical bytes.  Recovery additionally
 * establishes that those bytes describe a state-machine transition which the
 * service could have committed. */
static bool record_has_valid_result_state(const maintenance_journal_record_t *record)
{
    if (record->command_time_utc_100ns != record->snapshot.committed_at_utc_100ns ||
        !record->snapshot.has_activity)
    {
        return false;
    }
    switch (record->operation)
    {
    case MAINTENANCE_JOURNAL_OPERATION_PLAN:
    case MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE:
        return record->type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT &&
               record->snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED;
    case MAINTENANCE_JOURNAL_OPERATION_START:
        return record->type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT &&
               record->snapshot.state == MAINTENANCE_SERVICE_STATE_EXECUTING;
    case MAINTENANCE_JOURNAL_OPERATION_FINISH:
        return record->type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT &&
               record->snapshot.state == MAINTENANCE_SERVICE_STATE_FINISHED;
    case MAINTENANCE_JOURNAL_OPERATION_RESET:
        return record->type == MAINTENANCE_JOURNAL_RECORD_RESET &&
               record->sequence == 1u;
    default:
        return false;
    }
}

static bool record_follows(const maintenance_journal_record_t *previous,
                           const maintenance_journal_record_t *record)
{
    if (!record_has_valid_result_state(record))
    {
        return false;
    }
    if (record->epoch != previous->epoch)
    {
        return previous->epoch < UINT64_MAX - 1u &&
               record->epoch == previous->epoch + 1u &&
               record->type == MAINTENANCE_JOURNAL_RECORD_RESET &&
               record->sequence == 1u &&
               record->snapshot.revision == previous->snapshot.revision &&
               snapshot_equal(&record->snapshot, &previous->snapshot);
    }
    if (record->type != MAINTENANCE_JOURNAL_RECORD_SNAPSHOT ||
        record->snapshot.revision != previous->snapshot.revision + 1u)
    {
        return false;
    }
    switch (record->operation)
    {
    case MAINTENANCE_JOURNAL_OPERATION_PLAN:
        return previous->snapshot.state == MAINTENANCE_SERVICE_STATE_FINISHED &&
               record->snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED &&
               plan_activity_is_service_created(&record->snapshot.activity);
    case MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE:
        return previous->snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED &&
               record->snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED &&
               activity_identity_equal(&previous->snapshot.activity,
                                       &record->snapshot.activity) &&
               memcmp(previous->snapshot.activity.actor, record->snapshot.activity.actor,
                      sizeof(previous->snapshot.activity.actor)) == 0 &&
               activity_completion_equal(&previous->snapshot.activity,
                                         &record->snapshot.activity);
    case MAINTENANCE_JOURNAL_OPERATION_START:
        return previous->snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED &&
               record->snapshot.state == MAINTENANCE_SERVICE_STATE_EXECUTING &&
               activity_identity_equal(&previous->snapshot.activity,
                                       &record->snapshot.activity) &&
               activity_planning_equal(&previous->snapshot.activity,
                                       &record->snapshot.activity) &&
               activity_completion_equal(&previous->snapshot.activity,
                                         &record->snapshot.activity);
    case MAINTENANCE_JOURNAL_OPERATION_FINISH:
        return previous->snapshot.state == MAINTENANCE_SERVICE_STATE_EXECUTING &&
               record->snapshot.state == MAINTENANCE_SERVICE_STATE_FINISHED &&
               activity_identity_equal(&previous->snapshot.activity,
                                       &record->snapshot.activity) &&
               activity_planning_equal(&previous->snapshot.activity,
                                       &record->snapshot.activity);
    default:
        return false;
    }
}

static bool first_retained_record_is_valid(const sector_header_t *header,
                                           const maintenance_journal_record_t *record)
{
    if (header->generation == 1u) {
        if (header->predecessor_epoch == 0u && header->predecessor_generation == 0u)
            return record->sequence == 1u &&
                   record_follows(&g_immutable_initial_record, record);
        return record->type == MAINTENANCE_JOURNAL_RECORD_RESET && record->sequence == 1u;
    }
    /* Earlier retained generations may have been reclaimed.  The first record
     * is a checked anchor; its successor must still satisfy full progression. */
    return record->type == MAINTENANCE_JOURNAL_RECORD_SNAPSHOT;
}

maintenance_journal_backend_result_t maintenance_journal_backend_recover(maintenance_journal_backend_t *b)
{
    uint8_t count = 0u, ambiguous_sector_mask = 0u; bool any_non_erased = false, degraded = false, unsupported = false, indeterminate = false, fatal = false; int newest = -1;
    if (b == NULL || b->storage.read == NULL) return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT;
    b->corrupt_record_count = b->non_exact_commit_count = b->unknown_record_count = 0u;
    b->valid_record_count = b->retained_record_count = b->erased_sector_count = b->unknown_sector_mask = 0u;
    b->transient_record_peak = 0u; b->has_active = false; b->mutations_blocked = false;
    b->allocated_epoch = b->allocated_sequence = 0u;
    for (uint8_t s = 0u; s < 8u; ++s) { if (!read_header(b, s, &g_journal_headers[s])) { indeterminate = true; continue; } if (g_journal_headers[s].state == HEADER_ERASED) ++b->erased_sector_count; else any_non_erased = true; if (g_journal_headers[s].state == HEADER_INVALID) { ++b->corrupt_record_count; degraded = true; } if (g_journal_headers[s].state == HEADER_UNKNOWN) unsupported = true; }
    for (uint8_t s = 0u; s < 8u; ++s) if (g_journal_headers[s].state == HEADER_VALID) {
        bool first_valid = false;
        for (uint8_t slot = 0u; slot < 3u; ++slot) {
            maintenance_journal_commit_state_t cs = maintenance_journal_backend_classify_commit(&b->storage, slot_offset(s, slot), g_journal_slot_page);
            if (cs == MAINTENANCE_JOURNAL_COMMIT_INDETERMINATE) { indeterminate = true; continue; }
            if (cs == MAINTENANCE_JOURNAL_COMMIT_NOT_COMMITTED) {
                if (!erased(g_journal_slot_page + 768u, 256u)) ++b->non_exact_commit_count;
                memcpy(g_journal_verify_page, g_journal_slot_page, 1024u);
                memset(&g_journal_verify_page[768u], 0xff, 256u);
                write_u32(&g_journal_verify_page[768u], MAINTENANCE_JOURNAL_COMMIT_WORD);
                if (maintenance_journal_codec_decode(g_journal_verify_page, 1024u,
                                                     &g_journal_decoded_record) == MAINTENANCE_JOURNAL_CODEC_OK &&
                    g_journal_decoded_record.epoch == g_journal_headers[s].epoch)
                {
                    note_allocated(b, g_journal_decoded_record.epoch,
                                   g_journal_decoded_record.sequence);
                }
                continue;
            }
            if (cs == MAINTENANCE_JOURNAL_COMMIT_UNKNOWN) { ++b->unknown_record_count; b->unknown_sector_mask |= (uint8_t)(1u << s); unsupported = true; continue; }
            if (cs == MAINTENANCE_JOURNAL_COMMIT_CORRUPT) { ++b->corrupt_record_count; degraded = true; continue; }
            if (slot != 0u && !first_valid) { ++b->corrupt_record_count; degraded = true; continue; }
            if (maintenance_journal_codec_decode(g_journal_slot_page, 1024u, &g_journal_decoded_record) != MAINTENANCE_JOURNAL_CODEC_OK || g_journal_decoded_record.epoch != g_journal_headers[s].epoch || g_journal_decoded_record.sequence == 0u || g_journal_decoded_record.sequence == UINT64_MAX) { ++b->corrupt_record_count; degraded = true; continue; }
            first_valid = true; g_journal_records[count].sector = s; g_journal_records[count].slot = slot; g_journal_records[count].epoch = g_journal_headers[s].epoch; g_journal_records[count].generation = g_journal_headers[s].generation; g_journal_records[count].sequence = g_journal_decoded_record.sequence;
            note_allocated(b, g_journal_decoded_record.epoch,
                           g_journal_decoded_record.sequence);
            for (uint8_t j = 0u; j < count; ++j) if (g_journal_records[j].epoch == g_journal_records[count].epoch && g_journal_records[j].sequence == g_journal_records[count].sequence) {
                if (!b->storage.read(b->storage.context, slot_offset(g_journal_records[j].sector, g_journal_records[j].slot), g_journal_verify_page, 1024u)) indeterminate = true;
                else if (memcmp(g_journal_verify_page, g_journal_slot_page, 1024u) != 0) {
                    fatal = true;
                    ambiguous_sector_mask |= (uint8_t)((1u << g_journal_records[j].sector) |
                                                        (1u << s));
                }
            }
            ++count;
        }
    }
    /* A sector generation is a singly-linked chain.  Complete headers without
     * records are interrupted writes and do not participate, but every active
     * generation must otherwise be unique, non-branching, and consistent. */
    for (uint8_t s = 0u; s < 8u; ++s) {
        bool has = false;
        for (uint8_t i = 0u; i < count; ++i) if (g_journal_records[i].sector == s) has = true;
        if (!has) continue;
        for (uint8_t other = (uint8_t)(s + 1u); other < 8u; ++other) {
            bool other_has = false;
            for (uint8_t i = 0u; i < count; ++i) if (g_journal_records[i].sector == other) other_has = true;
            if (other_has && g_journal_headers[s].state == HEADER_VALID &&
                g_journal_headers[other].state == HEADER_VALID &&
                g_journal_headers[s].epoch == g_journal_headers[other].epoch &&
                g_journal_headers[s].generation == g_journal_headers[other].generation)
            {
                fatal = true;
                ambiguous_sector_mask |= (uint8_t)((1u << s) | (1u << other));
            }
        }
        {
            uint8_t successors = 0u;
            bool predecessor_present = false;
            bool older_retained = false;
            const sector_header_t *header = &g_journal_headers[s];
            for (uint8_t p = 0u; p < 8u; ++p) {
                bool parent_has = false;
                for (uint8_t i = 0u; i < count; ++i) if (g_journal_records[i].sector == p) parent_has = true;
                if (!parent_has || g_journal_headers[p].state != HEADER_VALID) continue;
                if (g_journal_headers[p].epoch == header->predecessor_epoch &&
                    g_journal_headers[p].generation == header->predecessor_generation)
                    predecessor_present = true;
                if (g_journal_headers[p].epoch == header->epoch &&
                    g_journal_headers[p].generation < header->generation)
                    older_retained = true;
                if (g_journal_headers[p].predecessor_epoch == header->epoch &&
                    g_journal_headers[p].predecessor_generation == header->generation)
                    ++successors;
            }
            if (successors > 1u) {
                fatal = true;
                for (uint8_t child = 0u; child < 8u; ++child)
                    if (g_journal_headers[child].state == HEADER_VALID &&
                        g_journal_headers[child].predecessor_epoch == header->epoch &&
                        g_journal_headers[child].predecessor_generation == header->generation)
                        ambiguous_sector_mask |= (uint8_t)(1u << child);
            }
            if (header->generation == 1u) {
                if (header->predecessor_epoch == 0u && header->predecessor_generation == 0u) {
                    if (header->epoch != 1u) { fatal = true; ambiguous_sector_mask |= (uint8_t)(1u << s); }
                } else if (header->predecessor_epoch == 0u ||
                           header->predecessor_generation == 0u ||
                           header->predecessor_epoch >= UINT64_MAX - 1u ||
                           header->epoch != header->predecessor_epoch + 1u) {
                    fatal = true; ambiguous_sector_mask |= (uint8_t)(1u << s);
                } else if (!predecessor_present && older_retained) {
                    fatal = true; ambiguous_sector_mask |= (uint8_t)(1u << s);
                }
            } else if (header->predecessor_epoch != header->epoch ||
                       header->predecessor_generation != header->generation - 1u ||
                       (!predecessor_present && older_retained)) {
                fatal = true; ambiguous_sector_mask |= (uint8_t)(1u << s);
            }
        }
    }
    for (uint8_t i = 0u; i < count; ++i) for (uint8_t j = (uint8_t)(i + 1u); j < count; ++j) {
        if ((g_journal_records[i].epoch == g_journal_records[j].epoch &&
             g_journal_records[i].sequence != g_journal_records[j].sequence &&
             serial_unordered(g_journal_records[i].sequence, g_journal_records[j].sequence)) ||
            (g_journal_records[i].epoch != g_journal_records[j].epoch &&
             serial_unordered(g_journal_records[i].epoch, g_journal_records[j].epoch)))
        {
            fatal = true;
            ambiguous_sector_mask |= (uint8_t)((1u << g_journal_records[i].sector) |
                                                (1u << g_journal_records[j].sector));
        }
    }
    /* Insertion sort the bounded physical set into logical order. */
    for (uint8_t i = 1u; i < count; ++i) {
        scan_record_t item = g_journal_records[i];
        uint8_t j = i;
        bool unordered = false;
        while (j > 0u && tuple_newer(&g_journal_records[j - 1u], &item, &unordered)) {
            g_journal_records[j] = g_journal_records[j - 1u];
            --j;
        }
        g_journal_records[j] = item;
    }
    {
        uint8_t proven = 0u;
        bool chain_broken = false;
        uint64_t last_epoch = 0u;
        uint64_t last_sequence = 0u;
        bool have_logical_tuple = false;
        for (uint8_t i = 0u; i < count; ++i) {
            bool valid;
            if ((ambiguous_sector_mask & (uint8_t)(1u << g_journal_records[i].sector)) != 0u)
                continue;
            /* Exact byte-identical mirrored slots are not a second logical
             * transition. Conflicting duplicates were marked fatal above. */
            if (have_logical_tuple && g_journal_records[i].epoch == last_epoch &&
                g_journal_records[i].sequence == last_sequence)
                continue;
            if (!read_scan_record(b, &g_journal_records[i], &g_journal_decoded_record)) {
                indeterminate = true;
                break;
            }
            have_logical_tuple = true;
            last_epoch = g_journal_records[i].epoch;
            last_sequence = g_journal_records[i].sequence;
            valid = !chain_broken && record_has_valid_result_state(&g_journal_decoded_record) &&
                    (proven == 0u ? first_retained_record_is_valid(
                                        &g_journal_headers[g_journal_records[i].sector],
                                        &g_journal_decoded_record) :
                                      record_follows(&b->active.record,
                                                     &g_journal_decoded_record));
            if (!valid) {
                ++b->corrupt_record_count;
                degraded = true;
                chain_broken = true;
                continue;
            }
            g_journal_records[proven++] = g_journal_records[i];
            b->active.sector = g_journal_records[proven - 1u].sector;
            b->active.slot = g_journal_records[proven - 1u].slot;
            b->active.epoch = g_journal_records[proven - 1u].epoch;
            b->active.generation = g_journal_records[proven - 1u].generation;
            b->active.record = g_journal_decoded_record;
            b->has_active = true;
        }
        b->valid_record_count = proven;
        b->transient_record_peak = count;
        newest = proven == 0u ? -1 : (int)(proven - 1u);
    }
    if (indeterminate) { b->recovery = MAINTENANCE_JOURNAL_RECOVERY_INDETERMINATE; b->mutations_blocked = true; return finish(b, MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE); }
    if (fatal) { b->recovery = MAINTENANCE_JOURNAL_RECOVERY_FATAL; b->mutations_blocked = true; return finish(b, MAINTENANCE_JOURNAL_BACKEND_FATAL); }
    if (unsupported) { b->recovery = MAINTENANCE_JOURNAL_RECOVERY_UNSUPPORTED; b->mutations_blocked = true; return finish(b, MAINTENANCE_JOURNAL_BACKEND_UNSUPPORTED); }
    if (any_non_erased && newest < 0) { b->recovery = MAINTENANCE_JOURNAL_RECOVERY_FATAL; b->mutations_blocked = true; return finish(b, MAINTENANCE_JOURNAL_BACKEND_FATAL); }
    if (newest < 0) { b->recovery = MAINTENANCE_JOURNAL_RECOVERY_EMPTY; return finish(b, MAINTENANCE_JOURNAL_BACKEND_OK); }
    if (degraded) { b->recovery = MAINTENANCE_JOURNAL_RECOVERY_DEGRADED; return finish(b, MAINTENANCE_JOURNAL_BACKEND_DEGRADED); }
    b->recovery = MAINTENANCE_JOURNAL_RECOVERY_OK; return finish(b, MAINTENANCE_JOURNAL_BACKEND_OK);
}

static bool sector_has_active(const maintenance_journal_backend_t *b, uint8_t s) { return b->has_active && b->active.sector == s; }
static bool sector_has_unknown(const maintenance_journal_backend_t *b, uint8_t s) { return (b->unknown_sector_mask & (uint8_t)(1u << s)) != 0u; }
static bool sector_has_retained_record(const maintenance_journal_backend_t *b, uint8_t sector)
{
    const uint8_t keep_count = b->valid_record_count > MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS ?
                                   (MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS - MAINTENANCE_JOURNAL_SLOTS_PER_SECTOR + 1u) :
                                   MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS;
    for (uint8_t i = 0u; i < b->valid_record_count; ++i)
    {
        uint8_t newer = 0u;
        if (g_journal_records[i].sector != sector)
        {
            continue;
        }
        for (uint8_t j = 0u; j < b->valid_record_count; ++j)
        {
            bool unordered = false;
            if (tuple_newer(&g_journal_records[j], &g_journal_records[i], &unordered))
            {
                ++newer;
            }
        }
        if (newer < keep_count)
        {
            return true;
        }
    }
    return false;
}
static bool ensure_spares(maintenance_journal_backend_t *b)
{
    if (b->erased_sector_count >= 2u) return true;
    /* Reclaim only an entire sector that cannot contain the authoritative slot.
     * The caller has already recovered it; erasure is separately read-verified. */
    for (uint8_t s = 0u; s < 8u; ++s) if (!sector_has_active(b, s) && !sector_has_unknown(b, s) && !sector_has_retained_record(b, s)) {
        sector_header_t h; if (!read_header(b, s, &h) || h.state == HEADER_ERASED) continue;
        if (!b->storage.erase_sector(b->storage.context, sector_offset(s)) || !b->storage.read(b->storage.context, sector_offset(s), g_journal_header_page, 256u) || !erased(g_journal_header_page, 256u)) return false;
        ++b->erase_count; ++b->sector_erase_count[s]; ++b->erased_sector_count; return b->erased_sector_count >= 2u;
    }
    return false;
}
static void reclaim_retention_best_effort(maintenance_journal_backend_t *b)
{
    while (b->valid_record_count > MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS)
    {
        bool reclaimed = false;
        for (uint8_t s = 0u; s < 8u; ++s)
        {
            sector_header_t header;
            if (sector_has_active(b, s) || sector_has_unknown(b, s) || sector_has_retained_record(b, s))
            {
                continue;
            }
            if (!read_header(b, s, &header) || header.state == HEADER_ERASED)
            {
                continue;
            }
            if (!b->storage.erase_sector(b->storage.context, sector_offset(s)) ||
                !b->storage.read(b->storage.context, sector_offset(s), g_journal_header_page, 256u) ||
                !erased(g_journal_header_page, 256u))
            {
                return;
            }
            for (uint8_t i = 0u; i < b->valid_record_count; )
            {
                if (g_journal_records[i].sector == s)
                {
                    g_journal_records[i] = g_journal_records[--b->valid_record_count];
                }
                else
                {
                    ++i;
                }
            }
            ++b->erase_count; ++b->sector_erase_count[s];
            ++b->erased_sector_count;
            ++b->logical_generation;
            if (b->logical_generation == 0u) ++b->logical_generation;
            reclaimed = true;
            break;
        }
        if (!reclaimed)
        {
            return;
        }
    }
}
static int find_erased_sector(maintenance_journal_backend_t *b) { for (uint8_t s=0u;s<8u;++s) { sector_header_t h; if (read_header(b,s,&h) && h.state==HEADER_ERASED) return s; } return -1; }
static int find_erased_slot(maintenance_journal_backend_t *b, uint8_t sector) { for(uint8_t slot=0u;slot<3u;++slot) if (b->storage.read(b->storage.context,slot_offset(sector,slot),g_journal_verify_page,1024u) && erased(g_journal_verify_page,1024u)) return slot; return -1; }
static maintenance_journal_backend_result_t program_record(maintenance_journal_backend_t *b, uint8_t s, uint8_t slot, maintenance_journal_record_t *r, uint64_t generation, maintenance_journal_record_location_t *out)
{
    maintenance_journal_commit_state_t cs;
    if (maintenance_journal_codec_encode(r, g_journal_slot_page, 1024u) != MAINTENANCE_JOURNAL_CODEC_OK) return finish(b, MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT);
    for (uint8_t p=0u;p<3u;++p) if (!b->storage.program_page(b->storage.context, slot_offset(s,slot)+(uint32_t)p*256u, &g_journal_slot_page[(size_t)p*256u])) return finish(b, MAINTENANCE_JOURNAL_BACKEND_IO);
    if (!b->storage.read(b->storage.context,slot_offset(s,slot),g_journal_verify_page,1024u) || memcmp(g_journal_slot_page,g_journal_verify_page,768u)!=0) return finish(b, MAINTENANCE_JOURNAL_BACKEND_IO);
    /* The one permitted attempt on the never-reprogrammable commit page. */
    (void)b->storage.program_page(b->storage.context,slot_offset(s,slot)+768u,&g_journal_slot_page[768u]);
    cs = maintenance_journal_backend_classify_commit(&b->storage,slot_offset(s,slot),g_journal_verify_page);
    if (cs == MAINTENANCE_JOURNAL_COMMIT_INDETERMINATE) { b->mutations_blocked=true; b->recovery=MAINTENANCE_JOURNAL_RECOVERY_INDETERMINATE; return finish(b, MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE); }
    if (cs != MAINTENANCE_JOURNAL_COMMIT_COMMITTED) return finish(b, MAINTENANCE_JOURNAL_BACKEND_IO);
    note_allocated(b, r->epoch, r->sequence);
    b->active.sector=s; b->active.slot=slot; b->active.epoch=r->epoch; b->active.generation=generation; b->active.record=*r; b->has_active=true;
    if (b->valid_record_count < MAINTENANCE_JOURNAL_TOTAL_SLOTS)
    {
        scan_record_t *added=&g_journal_records[b->valid_record_count++];
        added->sector=s; added->slot=slot; added->epoch=r->epoch; added->generation=generation; added->sequence=r->sequence;
    }
    if (b->valid_record_count > b->transient_record_peak) b->transient_record_peak = b->valid_record_count;
    if (out != NULL) *out=b->active;
    return finish(b, MAINTENANCE_JOURNAL_BACKEND_OK);
}

maintenance_journal_backend_result_t maintenance_journal_backend_append(maintenance_journal_backend_t *b, const maintenance_journal_record_t *candidate, maintenance_journal_record_location_t *out)
{
    maintenance_journal_record_t r; uint8_t s; uint64_t gen, pe=0u, pg=0u; int slot, erased_sector;
    if (b == NULL || candidate == NULL || b->storage.program_page == NULL || b->storage.erase_sector == NULL) return finish(b, MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT);
    if (b->mutations_blocked) return finish(b, MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE);
    r=*candidate;
    if (!b->has_active) { if (!ensure_spares(b)) return finish(b, MAINTENANCE_JOURNAL_BACKEND_CAPACITY); erased_sector=find_erased_sector(b); if (erased_sector < 0) return finish(b, MAINTENANCE_JOURNAL_BACKEND_CAPACITY); s=(uint8_t)erased_sector; r.epoch=1u; r.sequence=1u; note_allocated(b,r.epoch,r.sequence); gen=1u; if (!write_header(b,s,r.epoch,gen,0u,0u)) return finish(b, MAINTENANCE_JOURNAL_BACKEND_IO); --b->erased_sector_count; return program_record(b,s,0u,&r,gen,out); }
    if (b->active.record.sequence >= UINT64_MAX-1u) return finish(b, MAINTENANCE_JOURNAL_BACKEND_ORDER);
    r.epoch=b->active.epoch;
    if (b->allocated_epoch != r.epoch || b->allocated_sequence < b->active.record.sequence)
    {
        b->allocated_epoch = r.epoch;
        b->allocated_sequence = b->active.record.sequence;
    }
    if (b->allocated_sequence >= UINT64_MAX-1u) return finish(b, MAINTENANCE_JOURNAL_BACKEND_ORDER);
    r.sequence=b->allocated_sequence+1u; s=b->active.sector; gen=b->active.generation; slot=find_erased_slot(b,s);
    if (slot < 0) { if (gen >= UINT64_MAX-1u || !ensure_spares(b)) return finish(b, MAINTENANCE_JOURNAL_BACKEND_CAPACITY); pe=r.epoch; pg=gen; erased_sector=find_erased_sector(b); if (erased_sector < 0) return finish(b, MAINTENANCE_JOURNAL_BACKEND_CAPACITY); s=(uint8_t)erased_sector; ++gen; if (!write_header(b,s,r.epoch,gen,pe,pg)) return finish(b, MAINTENANCE_JOURNAL_BACKEND_IO); --b->erased_sector_count; slot=0; }
    note_allocated(b, r.epoch, r.sequence);
    {
        const maintenance_journal_backend_result_t result=program_record(b,s,(uint8_t)slot,&r,gen,out);
        if (result == MAINTENANCE_JOURNAL_BACKEND_OK)
        {
            reclaim_retention_best_effort(b);
        }
        return finish(b, result);
    }
}

maintenance_journal_backend_result_t maintenance_journal_backend_reset(maintenance_journal_backend_t *b, const maintenance_journal_record_t *snapshot, maintenance_journal_record_location_t *out)
{
    maintenance_journal_record_t r; int target;
    if (b == NULL || snapshot == NULL) return finish(b, MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT);
    if (!b->has_active) return finish(b, MAINTENANCE_JOURNAL_BACKEND_OK); /* logically empty: idempotent */
    if (b->active.record.type == MAINTENANCE_JOURNAL_RECORD_RESET)
    {
        if (out != NULL) *out = b->active;
        return finish(b, MAINTENANCE_JOURNAL_BACKEND_OK);
    }
    if (b->mutations_blocked) return finish(b, MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE);
    if (b->active.epoch >= UINT64_MAX-1u || !ensure_spares(b)) return finish(b, MAINTENANCE_JOURNAL_BACKEND_CAPACITY);
    target=find_erased_sector(b); if (target < 0) return finish(b, MAINTENANCE_JOURNAL_BACKEND_CAPACITY);
    r=*snapshot; r.type=MAINTENANCE_JOURNAL_RECORD_RESET; r.operation=MAINTENANCE_JOURNAL_OPERATION_RESET; r.transition=MAINTENANCE_SERVICE_TRANSITION_NONE; r.epoch=b->active.epoch+1u; r.sequence=1u;
    if (!write_header(b,(uint8_t)target,r.epoch,1u,b->active.epoch,b->active.generation)) return finish(b, MAINTENANCE_JOURNAL_BACKEND_IO);
    --b->erased_sector_count;
    {
        maintenance_journal_backend_result_t result = program_record(b,(uint8_t)target,0u,&r,1u,out);
        if (result == MAINTENANCE_JOURNAL_BACKEND_OK) {
            ++b->logical_generation;
            if (b->logical_generation == 0u) ++b->logical_generation;
        }
        return result;
    }
}

maintenance_journal_backend_result_t maintenance_journal_backend_export_begin(
    const maintenance_journal_backend_t *b, maintenance_journal_export_cursor_t *cursor)
{
    if (b == NULL || cursor == NULL) return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT;
    if (!b->has_active) {
        cursor->epoch = 0u;
        cursor->upper_sequence = 0u;
        cursor->next_sequence = 0u;
        cursor->generation = b->logical_generation;
        return MAINTENANCE_JOURNAL_BACKEND_OK;
    }
    cursor->epoch=b->active.epoch; cursor->upper_sequence=b->active.record.sequence;
    cursor->next_sequence=0u; cursor->generation=b->logical_generation;
    return MAINTENANCE_JOURNAL_BACKEND_OK;
}

maintenance_journal_backend_result_t maintenance_journal_backend_export_page(
    maintenance_journal_backend_t *b, maintenance_journal_export_cursor_t *cursor,
    maintenance_journal_record_t out[MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX], uint8_t *count, bool *more)
{
    if (b == NULL || cursor == NULL || out == NULL || count == NULL || more == NULL)
        return MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT;
    if (cursor->generation != b->logical_generation) return MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND;
    if (!b->has_active) {
        if (cursor->epoch != 0u || cursor->upper_sequence != 0u ||
            cursor->next_sequence != 0u)
            return MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND;
        *count = 0u;
        *more = false;
        return MAINTENANCE_JOURNAL_BACKEND_OK;
    }
    *count=0u; *more=false;
    while (*count < MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX) {
        int chosen=-1;
        for (uint8_t i=0u;i<b->valid_record_count;++i)
            if (g_journal_records[i].epoch==cursor->epoch && g_journal_records[i].sequence>cursor->next_sequence && g_journal_records[i].sequence<=cursor->upper_sequence &&
                (chosen<0 || g_journal_records[i].sequence<g_journal_records[chosen].sequence)) chosen=i;
        if (chosen<0) break;
        if (!b->storage.read(b->storage.context,slot_offset(g_journal_records[chosen].sector,g_journal_records[chosen].slot),g_journal_slot_page,1024u) ||
            maintenance_journal_codec_decode(g_journal_slot_page,1024u,&g_journal_decoded_record)!=MAINTENANCE_JOURNAL_CODEC_OK)
            return MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE;
        cursor->next_sequence=g_journal_records[chosen].sequence;
        if (g_journal_decoded_record.type != MAINTENANCE_JOURNAL_RECORD_RESET) out[(*count)++]=g_journal_decoded_record;
    }
    for (uint8_t i=0u;i<b->valid_record_count;++i) if (g_journal_records[i].epoch==cursor->epoch && g_journal_records[i].sequence>cursor->next_sequence && g_journal_records[i].sequence<=cursor->upper_sequence) *more=true;
    return MAINTENANCE_JOURNAL_BACKEND_OK;
}

#if defined(PICO_ON_DEVICE)
static bool flash_read(void *unused, uint32_t offset, uint8_t *dst, size_t n) { (void)unused; if (offset > OTA_LAYOUT_MAINTENANCE_JOURNAL_SIZE_BYTES || n > OTA_LAYOUT_MAINTENANCE_JOURNAL_SIZE_BYTES-offset) return false; memcpy(dst,(const void *)(uintptr_t)(OTA_LAYOUT_MAINTENANCE_JOURNAL_ORIGIN+offset),n); return true; }
static bool flash_program(void *unused, uint32_t offset, const uint8_t page[256]) { (void)unused; if (offset > OTA_LAYOUT_MAINTENANCE_JOURNAL_SIZE_BYTES-256u) return false; memcpy(g_journal_verify_page,page,256u); return flash_operation_program_page(OTA_LAYOUT_MAINTENANCE_JOURNAL_OFFSET_BYTES+offset,g_journal_verify_page)==FLASH_OPERATION_OK; }
static bool flash_erase(void *unused, uint32_t offset) { (void)unused; return offset <= OTA_LAYOUT_MAINTENANCE_JOURNAL_SIZE_BYTES-4096u && flash_operation_erase_sector(OTA_LAYOUT_MAINTENANCE_JOURNAL_OFFSET_BYTES+offset)==FLASH_OPERATION_OK; }
void maintenance_journal_backend_init_flash(maintenance_journal_backend_t *b) { const maintenance_journal_storage_t s={.context=NULL,.read=flash_read,.program_page=flash_program,.erase_sector=flash_erase}; maintenance_journal_backend_init(b,&s); }
#else
void maintenance_journal_backend_init_flash(maintenance_journal_backend_t *b) { maintenance_journal_backend_init(b, NULL); }
#endif

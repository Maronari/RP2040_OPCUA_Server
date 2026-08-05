#include "drivers/maintenance_journal_backend.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

typedef struct
{
    uint8_t bytes[MAINTENANCE_JOURNAL_SECTOR_COUNT * MAINTENANCE_JOURNAL_SECTOR_SIZE];
    unsigned program_count;
    unsigned fail_program_number;
    bool apply_failed_program;
    bool unreadable;
    bool programmed[MAINTENANCE_JOURNAL_SECTOR_COUNT * (MAINTENANCE_JOURNAL_SECTOR_SIZE / 256u)];
    bool reprogrammed;
} simulated_media_t;

static bool media_read(void *context, uint32_t offset, uint8_t *destination, size_t size)
{
    simulated_media_t *media = context;
    if (media->unreadable || offset > sizeof(media->bytes) || size > sizeof(media->bytes) - offset)
        return false;
    memcpy(destination, &media->bytes[offset], size);
    return true;
}

static bool media_program(void *context, uint32_t offset, const uint8_t page[256])
{
    simulated_media_t *media = context;
    ++media->program_count;
    if (offset > sizeof(media->bytes) - 256u)
        return false;
    if (media->programmed[offset / 256u])
        media->reprogrammed = true;
    media->programmed[offset / 256u] = true;
    if (media->fail_program_number == media->program_count && !media->apply_failed_program)
        return false;
    for (size_t i = 0; i < 256u; ++i)
        media->bytes[offset + i] &= page[i];
    return media->fail_program_number != media->program_count;
}

static bool media_erase(void *context, uint32_t offset)
{
    simulated_media_t *media = context;
    if (offset > sizeof(media->bytes) - MAINTENANCE_JOURNAL_SECTOR_SIZE ||
        (offset % MAINTENANCE_JOURNAL_SECTOR_SIZE) != 0u)
        return false;
    memset(&media->bytes[offset], 0xff, MAINTENANCE_JOURNAL_SECTOR_SIZE);
    memset(&media->programmed[offset / 256u], 0,
           MAINTENANCE_JOURNAL_SECTOR_SIZE / 256u * sizeof(media->programmed[0]));
    return true;
}

static maintenance_journal_storage_t storage(simulated_media_t *media)
{
    return (maintenance_journal_storage_t){.context = media, .read = media_read,
                                           .program_page = media_program,
                                           .erase_sector = media_erase};
}

static maintenance_journal_record_t candidate(uint32_t revision)
{
    maintenance_journal_record_t value = {0};
    value.command_time_utc_100ns = 1;
    value.type = MAINTENANCE_JOURNAL_RECORD_SNAPSHOT;
    value.operation = MAINTENANCE_JOURNAL_OPERATION_PLAN;
    value.transition = MAINTENANCE_SERVICE_TRANSITION_FINISHED_TO_PLANNED;
    value.snapshot.state = MAINTENANCE_SERVICE_STATE_PLANNED;
    value.snapshot.revision = revision;
    value.snapshot.committed_at_utc_100ns = 1;
    value.snapshot.has_activity = true;
    value.snapshot.activity.planned_date_utc_100ns = 2;
    value.snapshot.activity.estimated_downtime_ms = 1.0;
    value.snapshot.activity.method = MAINTENANCE_SERVICE_METHOD_LOCAL;
    strcpy(value.snapshot.activity.operation_id, "op");
    strcpy(value.snapshot.activity.actor, "actor");
    return value;
}

static void init_media(simulated_media_t *media)
{
    memset(media->bytes, 0xff, sizeof(media->bytes));
    media->program_count = 0u;
    media->fail_program_number = 0u;
    media->apply_failed_program = false;
    media->unreadable = false;
    memset(media->programmed, 0, sizeof(media->programmed));
    media->reprogrammed = false;
}

static maintenance_journal_backend_result_t append_candidate(maintenance_journal_backend_t *backend,
                                                              uint32_t revision,
                                                              maintenance_journal_record_location_t *location)
{
    /* The immutable service root is Finished/revision 1, so the first Plan
     * and every following test transition use the next revision. */
    maintenance_journal_record_t value = candidate(revision + 1u);
    if (backend->has_active)
    {
        value.operation = MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE;
        value.transition = MAINTENANCE_SERVICE_TRANSITION_NONE;
        value.snapshot = backend->active.record.snapshot;
        value.snapshot.revision = revision + 1u;
    }
    return maintenance_journal_backend_append(backend, &value, location);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    for (uint8_t i = 0u; i < 4u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    for (uint8_t i = 0u; i < 8u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}

static void rewrite_header(simulated_media_t *media, uint8_t sector, uint64_t epoch,
                           uint64_t generation, uint64_t predecessor_epoch,
                           uint64_t predecessor_generation)
{
    uint8_t *header = &media->bytes[(size_t)sector * MAINTENANCE_JOURNAL_SECTOR_SIZE];
    put_u64(&header[8u], epoch);
    put_u64(&header[16u], generation);
    put_u64(&header[24u], predecessor_epoch);
    put_u64(&header[32u], predecessor_generation);
    header[40u] = sector;
    put_u32(&header[252u], maintenance_journal_codec_crc32(header, 252u));
}

static void test_commit_post_state(void)
{
    simulated_media_t media;
    uint8_t scratch[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    uint8_t slot[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    maintenance_journal_record_t value = candidate(1u);
    maintenance_journal_storage_t io;
    init_media(&media);
    io = storage(&media);
    value.epoch = 1u; value.sequence = 1u;
    assert(maintenance_journal_codec_encode(&value, slot, sizeof(slot)) == MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(media.bytes, slot, sizeof(slot));
    assert(maintenance_journal_backend_classify_commit(&io, 0u, scratch) == MAINTENANCE_JOURNAL_COMMIT_COMMITTED);
    memset(&media.bytes[768], 0xff, 256u);
    assert(maintenance_journal_backend_classify_commit(&io, 0u, scratch) == MAINTENANCE_JOURNAL_COMMIT_NOT_COMMITTED);
    media.bytes[768] = 0u;
    assert(maintenance_journal_backend_classify_commit(&io, 0u, scratch) == MAINTENANCE_JOURNAL_COMMIT_NOT_COMMITTED);
    memcpy(media.bytes, slot, sizeof(slot));
    media.bytes[0] &= 0xfeu;
    assert(maintenance_journal_backend_classify_commit(&io, 0u, scratch) == MAINTENANCE_JOURNAL_COMMIT_CORRUPT);
    media.unreadable = true;
    assert(maintenance_journal_backend_classify_commit(&io, 0u, scratch) == MAINTENANCE_JOURNAL_COMMIT_INDETERMINATE);
}

static void test_append_recover_rotate_and_reset(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend, reboot;
    maintenance_journal_record_location_t location;
    maintenance_journal_storage_t io;
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(backend.recovery == MAINTENANCE_JOURNAL_RECOVERY_EMPTY && backend.erased_sector_count == 8u);
    for (uint32_t revision = 1u; revision <= 16u; ++revision)
    {
        assert(append_candidate(&backend, revision, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
        assert(location.record.epoch == 1u && location.record.sequence == revision);
    }
    assert(backend.valid_record_count <= MAINTENANCE_JOURNAL_MAX_RETAINED_RECORDS);
    assert(backend.valid_record_count == 13u && backend.transient_record_peak == 16u);
    assert(backend.erased_sector_count >= MAINTENANCE_JOURNAL_MIN_ERASED_SECTORS);
    assert(backend.erase_count == 1u && backend.sector_erase_count[0] == 1u);
    assert(!media.reprogrammed);
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(reboot.active.record.sequence == 16u && reboot.active.record.snapshot.revision == 17u);
    {
        maintenance_journal_record_t snapshot = candidate(17u);
    assert(maintenance_journal_backend_reset(&reboot, &snapshot, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    }
    assert(location.record.type == MAINTENANCE_JOURNAL_RECORD_RESET && location.epoch == 2u && location.record.sequence == 1u);
    {
        const unsigned before = media.program_count;
        maintenance_journal_record_t repeat_snapshot = candidate(17u);
        assert(maintenance_journal_backend_reset(&reboot, &repeat_snapshot, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
        assert(media.program_count == before && location.epoch == 2u && location.record.sequence == 1u);
    }
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(backend.active.epoch == 2u && backend.active.record.snapshot.revision == 17u);
}

static void test_commit_failure_classification_and_corruption(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend, reboot;
    maintenance_journal_record_location_t location;
    maintenance_journal_storage_t io;
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    /* A reported failure after exact physical programming is committed. */
    media.fail_program_number = media.program_count + 4u;
    media.apply_failed_program = true;
    assert(append_candidate(&backend, 2u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(location.record.sequence == 2u);
    /* An API failure before any commit bits are programmed remains invisible. */
    media.fail_program_number = media.program_count + 4u;
    media.apply_failed_program = false;
    assert(append_candidate(&backend, 3u, &location) == MAINTENANCE_JOURNAL_BACKEND_IO);
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(reboot.active.record.sequence == 2u);
    assert(append_candidate(&reboot, 4u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(location.record.sequence == 4u); /* sequence 3 was body-valid but non-committed */
    /* Exact commit plus a bad body is counted but cannot replace a good snapshot. */
    media.bytes[MAINTENANCE_JOURNAL_HEADER_SIZE + MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE] &= 0xfeu;
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_DEGRADED);
    /* The corrupted predecessor makes sequence 4 unprovable; recovery keeps
     * the last proven snapshot instead of selecting a newer isolated record. */
    assert(reboot.active.record.sequence == 1u && reboot.corrupt_record_count != 0u);
}

static void test_order_and_indeterminate(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend;
    maintenance_journal_record_location_t location;
    maintenance_journal_storage_t io;
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    backend.active.record.sequence = UINT64_MAX - 1u;
    assert(append_candidate(&backend, 2u, &location) == MAINTENANCE_JOURNAL_BACKEND_ORDER);
    media.unreadable = true;
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_INDETERMINATE);
    assert(backend.mutations_blocked);
}

static void test_unknown_header_fails_closed_without_erasure(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend;
    maintenance_journal_record_location_t location;
    maintenance_journal_storage_t io;
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    media.bytes[4u] = 2u; /* header format is read before CRC and is preserved. */
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_UNSUPPORTED);
    assert(backend.mutations_blocked);
}

static void test_unknown_record_holes_duplicates_and_order(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend, reboot;
    maintenance_journal_record_location_t location;
    maintenance_journal_record_t replacement;
    maintenance_journal_storage_t io;
    const uint32_t slot0 = MAINTENANCE_JOURNAL_HEADER_SIZE;
    const uint32_t slot1 = slot0 + MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE;
    uint8_t encoded[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 2u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 3u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    /* A free/non-exact hole is scanned, but makes the later revision
     * unprovable rather than silently accepting it. */
    memset(&media.bytes[slot1 + 768u], 0xff, 256u);
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_DEGRADED);
    assert(reboot.active.record.sequence == 1u);
    /* Put a canonical but unknown-schema record in the hole: preserve/block. */
    replacement = candidate(2u); replacement.epoch = 1u; replacement.sequence = 2u;
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) == MAINTENANCE_JOURNAL_CODEC_OK);
    encoded[4] = 2u; encoded[5] = 0u;
    memcpy(&media.bytes[slot1], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_UNSUPPORTED);
    assert(reboot.unknown_record_count == 1u && reboot.unknown_sector_mask == 1u && reboot.mutations_blocked);

    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    for (uint32_t i = 1u; i <= 4u; ++i) assert(append_candidate(&backend, i, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    /* Identical duplicate tuple is harmless; a canonical conflicting one is fatal. */
    memcpy(&media.bytes[MAINTENANCE_JOURNAL_SECTOR_SIZE + slot1], &media.bytes[slot0], sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = candidate(99u); replacement.epoch = 1u; replacement.sequence = 1u;
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) == MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[MAINTENANCE_JOURNAL_SECTOR_SIZE + slot1], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_FATAL);

    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = candidate(2u); replacement.epoch = 1u; replacement.sequence = UINT64_C(1) + (UINT64_C(1) << 63u);
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) == MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[slot1], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_FATAL);
}

static void test_successor_spare_preflight(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend;
    maintenance_journal_record_location_t location;
    maintenance_journal_storage_t io;
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    for (uint32_t i = 1u; i <= 3u; ++i) assert(append_candidate(&backend, i, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    backend.erased_sector_count = 0u;
    assert(append_candidate(&backend, 4u, &location) == MAINTENANCE_JOURNAL_BACKEND_CAPACITY);
    assert(backend.last_error == MAINTENANCE_JOURNAL_BACKEND_CAPACITY &&
           backend.capacity_state == MAINTENANCE_JOURNAL_CAPACITY_CONSTRAINED);
    backend.erased_sector_count = 1u;
    assert(append_candidate(&backend, 4u, &location) == MAINTENANCE_JOURNAL_BACKEND_CAPACITY);
    backend.erased_sector_count = 2u;
    assert(append_candidate(&backend, 4u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(location.generation == 2u && location.record.sequence == 4u);
}

static void test_logical_export_pinning_and_reset_invalidation(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend;
    maintenance_journal_record_location_t location;
    maintenance_journal_export_cursor_t cursor;
    maintenance_journal_record_t records[MAINTENANCE_JOURNAL_EXPORT_PAGE_MAX];
    maintenance_journal_record_t snapshot;
    maintenance_journal_storage_t io;
    uint8_t count;
    bool more;

    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(maintenance_journal_backend_export_begin(&backend, &cursor) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(cursor.epoch == 0u && cursor.upper_sequence == 0u && cursor.next_sequence == 0u);
    assert(maintenance_journal_backend_export_page(&backend, &cursor, records, &count, &more) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(count == 0u && !more);
    for (uint32_t revision = 1u; revision <= 6u; ++revision)
        assert(append_candidate(&backend, revision, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(maintenance_journal_backend_export_begin(&backend, &cursor) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(cursor.epoch == 1u && cursor.upper_sequence == 6u && cursor.next_sequence == 0u);
    assert(maintenance_journal_backend_export_page(&backend, &cursor, records, &count, &more) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(count == 4u && more && records[0].sequence == 1u && records[3].sequence == 4u);
    assert(append_candidate(&backend, 7u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(maintenance_journal_backend_export_page(&backend, &cursor, records, &count, &more) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(count == 2u && !more && records[0].sequence == 5u && records[1].sequence == 6u);
    assert(maintenance_journal_backend_export_page(&backend, &cursor, NULL, &count, &more) ==
           MAINTENANCE_JOURNAL_BACKEND_INVALID_ARGUMENT);

    assert(maintenance_journal_backend_export_begin(&backend, &cursor) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    /* A reclaim changes the retained logical set, so an older cursor cannot
     * continue into a different physical retention window. */
    for (uint32_t revision = 8u; revision <= 16u; ++revision)
        assert(append_candidate(&backend, revision, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(maintenance_journal_backend_export_page(&backend, &cursor, records, &count, &more) ==
           MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND);

    assert(maintenance_journal_backend_export_begin(&backend, &cursor) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    snapshot = candidate(17u);
    assert(maintenance_journal_backend_reset(&backend, &snapshot, &location) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(maintenance_journal_backend_export_page(&backend, &cursor, records, &count, &more) ==
           MAINTENANCE_JOURNAL_BACKEND_NOT_FOUND);
    assert(maintenance_journal_backend_export_begin(&backend, &cursor) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(maintenance_journal_backend_export_page(&backend, &cursor, records, &count, &more) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(count == 0u && !more);
}

static void test_recovery_rejects_semantic_and_lineage_ambiguity(void)
{
    simulated_media_t media;
    maintenance_journal_backend_t backend, reboot;
    maintenance_journal_record_location_t location;
    maintenance_journal_record_t replacement;
    maintenance_journal_storage_t io;
    uint8_t encoded[MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE];
    const uint32_t slot0 = MAINTENANCE_JOURNAL_HEADER_SIZE;
    const uint32_t slot1 = MAINTENANCE_JOURNAL_HEADER_SIZE +
                           MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE;
    const uint32_t slot2 = slot1 + MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE;

    /* Root Plan must follow immutable Finished/revision-1 state. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = candidate(1u); replacement.epoch = 1u; replacement.sequence = 1u;
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[slot0], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_FATAL);
    assert(!reboot.has_active);

    /* A root record cannot skip the first logical sequence. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = candidate(2u); replacement.epoch = 1u; replacement.sequence = 2u;
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[slot0], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_FATAL);
    assert(!reboot.has_active);

    /* Reset epochs are adjacent, not merely serially newer. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = candidate(2u);
    assert(maintenance_journal_backend_reset(&backend, &replacement, &location) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = backend.active.record;
    replacement.epoch = 3u;
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[MAINTENANCE_JOURNAL_SECTOR_SIZE + slot0], encoded, sizeof(encoded));
    rewrite_header(&media, 1u, 3u, 1u, 1u, 1u);
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_FATAL);
    assert(reboot.has_active && reboot.active.epoch == 1u &&
           reboot.active.record.sequence == 1u);

    /* A CRC-valid record whose operation/result state cannot be produced by
     * the service is ignored, leaving only the last proven snapshot readable. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 2u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = backend.active.record;
    replacement.snapshot.state = MAINTENANCE_SERVICE_STATE_EXECUTING;
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[slot1], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_DEGRADED);
    assert(reboot.has_active && reboot.active.record.sequence == 1u);

    /* A revision jump is likewise not a recoverable successor. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 2u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = candidate(4u); replacement.epoch = 1u; replacement.sequence = 2u;
    replacement.operation = MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE;
    replacement.transition = MAINTENANCE_SERVICE_TRANSITION_NONE;
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[slot1], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_DEGRADED);
    assert(reboot.has_active && reboot.active.record.sequence == 1u);

    /* Reschedule may not mutate identity, actor, result, completion fields,
     * or the current planned state. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 2u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = backend.active.record;
    strcpy(replacement.snapshot.activity.actor, "other-actor");
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[slot1], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_DEGRADED);
    assert(reboot.has_active && reboot.active.record.sequence == 1u);

    /* Start changes state/actor only.  A CRC-valid planning mutation is not
     * a transition the service can emit. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 1u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(append_candidate(&backend, 2u, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = backend.active.record;
    replacement.type = MAINTENANCE_JOURNAL_RECORD_SNAPSHOT;
    replacement.operation = MAINTENANCE_JOURNAL_OPERATION_START;
    replacement.transition = MAINTENANCE_SERVICE_TRANSITION_PLANNED_TO_EXECUTING;
    replacement.snapshot.state = MAINTENANCE_SERVICE_STATE_EXECUTING;
    ++replacement.snapshot.revision;
    strcpy(replacement.snapshot.activity.actor, "starter");
    assert(maintenance_journal_backend_append(&backend, &replacement, &location) ==
           MAINTENANCE_JOURNAL_BACKEND_OK);
    replacement = backend.active.record;
    strcpy(replacement.snapshot.activity.supplier, "mutated-supplier");
    assert(maintenance_journal_codec_encode(&replacement, encoded, sizeof(encoded)) ==
           MAINTENANCE_JOURNAL_CODEC_OK);
    memcpy(&media.bytes[slot2], encoded, sizeof(encoded));
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_DEGRADED);
    assert(reboot.has_active && reboot.active.record.sequence == 2u);

    /* Two active headers claiming one generation are a fatal lineage fork. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    for (uint32_t revision = 1u; revision <= 4u; ++revision)
        assert(append_candidate(&backend, revision, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    memcpy(&media.bytes[2u * MAINTENANCE_JOURNAL_SECTOR_SIZE],
           &media.bytes[MAINTENANCE_JOURNAL_SECTOR_SIZE],
           MAINTENANCE_JOURNAL_HEADER_SIZE + MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE);
    rewrite_header(&media, 2u, 1u, 2u, 1u, 1u);
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_FATAL);
    assert(reboot.has_active && reboot.active.record.sequence == 3u);

    /* A successor with a nonexistent predecessor cannot be selected. */
    init_media(&media); io = storage(&media);
    maintenance_journal_backend_init(&backend, &io);
    assert(maintenance_journal_backend_recover(&backend) == MAINTENANCE_JOURNAL_BACKEND_OK);
    for (uint32_t revision = 1u; revision <= 4u; ++revision)
        assert(append_candidate(&backend, revision, &location) == MAINTENANCE_JOURNAL_BACKEND_OK);
    rewrite_header(&media, 1u, 1u, 2u, 1u, 9u);
    maintenance_journal_backend_init(&reboot, &io);
    assert(maintenance_journal_backend_recover(&reboot) == MAINTENANCE_JOURNAL_BACKEND_FATAL);
    assert(reboot.has_active && reboot.active.record.sequence == 3u);
}

int main(void)
{
    test_commit_post_state();
    test_append_recover_rotate_and_reset();
    test_commit_failure_classification_and_corruption();
    test_order_and_indeterminate();
    test_unknown_header_fails_closed_without_erasure();
    test_unknown_record_holes_duplicates_and_order();
    test_successor_spare_preflight();
    test_logical_export_pinning_and_reset_invalidation();
    test_recovery_rejects_semantic_and_lineage_ambiguity();
    return 0;
}

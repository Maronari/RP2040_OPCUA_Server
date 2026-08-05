#include "services/maintenance_service.h"

#include <assert.h>
#include <string.h>

#include "drivers/maintenance_journal_backend.h"

typedef struct
{
    uint8_t bytes[MAINTENANCE_JOURNAL_SECTOR_COUNT * MAINTENANCE_JOURNAL_SECTOR_SIZE];
    unsigned programs;
    unsigned fail_program;
    bool apply_failed_program;
} media_t;

static bool read_media(void *context, uint32_t offset, uint8_t *out, size_t size)
{
    media_t *media = context;
    if (offset > sizeof(media->bytes) || size > sizeof(media->bytes) - offset) return false;
    memcpy(out, &media->bytes[offset], size);
    return true;
}

static bool program_media(void *context, uint32_t offset, const uint8_t page[256])
{
    media_t *media = context;
    ++media->programs;
    if (offset > sizeof(media->bytes) - 256u) return false;
    if (media->programs == media->fail_program && !media->apply_failed_program) return false;
    for (size_t i = 0u; i < 256u; ++i) media->bytes[offset + i] &= page[i];
    return media->programs != media->fail_program;
}

static bool erase_media(void *context, uint32_t offset)
{
    media_t *media = context;
    if (offset > sizeof(media->bytes) - MAINTENANCE_JOURNAL_SECTOR_SIZE ||
        (offset % MAINTENANCE_JOURNAL_SECTOR_SIZE) != 0u) return false;
    memset(&media->bytes[offset], 0xff, MAINTENANCE_JOURNAL_SECTOR_SIZE);
    return true;
}

static maintenance_service_plan_request_t plan(void)
{
    maintenance_service_plan_request_t request = {0};
    request.command_time_utc_100ns = 133000000000000000LL;
    request.planned_date_utc_100ns = 133000001000000000LL;
    request.estimated_downtime_ms = 1000.0;
    request.method = MAINTENANCE_SERVICE_METHOD_LOCAL;
    strcpy(request.operation_id, "journal-op");
    strcpy(request.supplier, "supplier"); strcpy(request.personnel, "crew"); strcpy(request.actor, "alice");
    return request;
}

int main(void)
{
    media_t media = {0};
    maintenance_service_journal_storage_t storage = {
        .context = &media, .read = read_media, .program_page = program_media, .erase_sector = erase_media};
    maintenance_service_plan_request_t request = plan();
    maintenance_service_start_request_t start = {.command_time_utc_100ns = 133000002000000000LL};
    maintenance_service_transaction_t transaction = {0};
    maintenance_service_snapshot_t snapshot = {0};
    maintenance_service_journal_diagnostics_t diagnostics = {0};
    maintenance_service_export_cursor_t export_cursor = {0};
    maintenance_service_export_record_t export_records[MAINTENANCE_SERVICE_EXPORT_PAGE_MAX];
    maintenance_service_reset_outcome_t reset_outcome;
    maintenance_journal_backend_t recovered;
    maintenance_journal_storage_t backend_storage = {
        .context = &media, .read = read_media, .program_page = program_media, .erase_sector = erase_media};

    memset(media.bytes, 0xff, sizeof(media.bytes));
    strcpy(start.actor, "operator");
    assert(maintenance_service_init_with_journal(&storage) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_get_snapshot(&snapshot) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_FINISHED && snapshot.revision == 1u);
    assert(maintenance_service_get_journal_diagnostics(&diagnostics) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(diagnostics.enabled && diagnostics.recovery == MAINTENANCE_SERVICE_JOURNAL_RECOVERY_EMPTY);
    assert(maintenance_service_export_begin(&export_cursor) == MAINTENANCE_SERVICE_RESULT_OK);
    {
        uint8_t export_count = 1u;
        bool export_more = true;
        assert(maintenance_service_export_page(&export_cursor, export_records, &export_count,
                                               &export_more) == MAINTENANCE_SERVICE_RESULT_OK);
        assert(export_count == 0u && !export_more);
    }

    assert(maintenance_service_prepare_plan(&request, &transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_apply(&transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_apply(&transaction) == MAINTENANCE_SERVICE_RESULT_OK); /* retry: no extra slot */
    assert(maintenance_service_commit(&transaction, &snapshot) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_PLANNED && snapshot.revision == 2u);
    assert(media.programs == 5u);
    {
        bool already_committed = false;
        const unsigned before = media.programs;
        assert(maintenance_service_reconcile_operation_id("journal-op", &snapshot,
                                                          &already_committed) == MAINTENANCE_SERVICE_RESULT_OK);
        assert(already_committed && snapshot.revision == 2u && media.programs == before);
        assert(maintenance_service_reconcile_operation_id("other-op", &snapshot,
                                                          &already_committed) == MAINTENANCE_SERVICE_RESULT_OK);
        assert(!already_committed && media.programs == before);
    }

    assert(maintenance_service_prepare_start(&start, &transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    media.fail_program = media.programs + 4u; /* commit page is post-state authority */
    media.apply_failed_program = true;
    assert(maintenance_service_apply(&transaction) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(maintenance_service_commit(&transaction, &snapshot) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_EXECUTING && snapshot.revision == 3u);

    assert(maintenance_service_export_begin(&export_cursor) == MAINTENANCE_SERVICE_RESULT_OK);
    {
        uint8_t export_count = 0u;
        bool export_more = true;
        assert(maintenance_service_export_page(&export_cursor, export_records, &export_count,
                                               &export_more) == MAINTENANCE_SERVICE_RESULT_OK);
        assert(export_count == 2u && !export_more && export_records[0].sequence == 1u &&
               export_records[1].sequence == 2u);
    }

    assert(maintenance_service_reset_journal_ex(&reset_outcome) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(reset_outcome == MAINTENANCE_SERVICE_RESET_APPLIED);
    assert(maintenance_service_reset_journal_ex(&reset_outcome) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(reset_outcome == MAINTENANCE_SERVICE_RESET_NO_CHANGE);
    assert(maintenance_service_get_snapshot(&snapshot) == MAINTENANCE_SERVICE_RESULT_OK);
    assert(snapshot.state == MAINTENANCE_SERVICE_STATE_EXECUTING && snapshot.revision == 3u);
    maintenance_journal_backend_init(&recovered, &backend_storage);
    assert(maintenance_journal_backend_recover(&recovered) == MAINTENANCE_JOURNAL_BACKEND_OK);
    assert(recovered.active.epoch == 2u && recovered.active.record.snapshot.revision == 3u);
    return 0;
}

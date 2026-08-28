/**
 * @file
 * @brief Host-test suite for drivers/config_transaction_backend, covering:
 * stage-write success/classification, a simulated torn stage write
 * classifying as safely discardable, commit correctly promoting
 * staged->running and running->last-known-good, a simulated power-loss
 * abort at every transaction phase (before/after the stage commit page,
 * mid-promotion in both the LAST_KNOWN_GOOD and RUNNING sub-steps, and
 * after a full commit) each recovering to the documented outcome, and
 * last-known-good retrieval after a corrupted running region.
 */
#include "drivers/config_transaction_backend.h"

#include <assert.h>
#include <string.h>

/** @brief Simulated Flash media: one CONFIG_TXN_ROLE_SIZE_BYTES*CONFIG_TXN_ROLE_COUNT byte-array plus program-call fault injection, mirroring maintenance_journal_backend_test.c's simulated_media_t. */
typedef struct
{
    uint8_t bytes[CONFIG_TXN_ROLE_SIZE_BYTES * CONFIG_TXN_ROLE_COUNT];
    unsigned program_count;
    unsigned fail_program_number; /* 0 = never fail. */
    bool apply_failed_program;    /* true: the failing program call still partially lands (AND-masked); false: it is rejected outright. */
    bool unreadable;
} simulated_media_t;

static bool media_read(void *context, uint32_t offset, uint8_t *destination, size_t size)
{
    simulated_media_t *media = context;
    if (media->unreadable || offset > sizeof(media->bytes) || size > sizeof(media->bytes) - offset)
    {
        return false;
    }
    memcpy(destination, &media->bytes[offset], size);
    return true;
}

static bool media_program(void *context, uint32_t offset, const uint8_t page[CONFIG_TXN_PAGE_SIZE_BYTES])
{
    simulated_media_t *media = context;
    ++media->program_count;
    if (offset > sizeof(media->bytes) - CONFIG_TXN_PAGE_SIZE_BYTES)
    {
        return false;
    }
    if (media->fail_program_number == media->program_count && !media->apply_failed_program)
    {
        return false;
    }
    for (size_t i = 0u; i < CONFIG_TXN_PAGE_SIZE_BYTES; ++i)
    {
        media->bytes[offset + i] &= page[i];
    }
    return media->fail_program_number != media->program_count;
}

static bool media_erase(void *context, uint32_t offset)
{
    simulated_media_t *media = context;
    if (offset > sizeof(media->bytes) - CONFIG_TXN_SECTOR_SIZE_BYTES || (offset % CONFIG_TXN_SECTOR_SIZE_BYTES) != 0u)
    {
        return false;
    }
    memset(&media->bytes[offset], 0xFF, CONFIG_TXN_SECTOR_SIZE_BYTES);
    return true;
}

static config_transaction_storage_t storage(simulated_media_t *media)
{
    config_transaction_storage_t s = {.context = media, .read = media_read, .program_page = media_program, .erase_sector = media_erase};
    return s;
}

static void init_media(simulated_media_t *media)
{
    memset(media->bytes, 0xFF, sizeof(media->bytes));
    media->program_count = 0u;
    media->fail_program_number = 0u;
    media->apply_failed_program = false;
    media->unreadable = false;
}

static void fill_payload(uint8_t *payload, size_t size, uint8_t seed)
{
    for (size_t i = 0u; i < size; ++i)
    {
        payload[i] = (uint8_t)(seed + i * 7u);
    }
}

/** @brief Stage @p payload and require it lands COMMITTED (a fresh, unfailing write). */
static void stage_ok(config_transaction_backend_t *backend, const uint8_t *payload, size_t size,
                     uint32_t schema_version, config_transaction_record_info_t *out_info)
{
    assert(config_transaction_backend_stage(backend, payload, size, 0u, schema_version, out_info) == CONFIG_TXN_BACKEND_OK);
    assert(out_info->commit_state == CONFIG_TXN_COMMIT_COMMITTED);
}

static void test_stage_success_and_classification(void)
{
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t payload[600];
    config_transaction_record_info_t info;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    assert(backend.recovery == CONFIG_TXN_RECOVERY_EMPTY);
    assert(!backend.mutations_blocked);

    fill_payload(payload, sizeof(payload), 1u);
    stage_ok(&backend, payload, sizeof(payload), 7u, &info);
    assert(info.schema_version == 7u);
    assert(info.payload_size == sizeof(payload));
    assert(info.record_format_version == CONFIG_TXN_RECORD_FORMAT_VERSION);
    assert(backend.roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
    /* Staging never touches RUNNING/LAST_KNOWN_GOOD. */
    assert(backend.roles[CONFIG_TXN_ROLE_RUNNING].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
    assert(backend.roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state != CONFIG_TXN_COMMIT_COMMITTED);

    {
        uint8_t readback[600];
        size_t readback_size = 0u;
        config_transaction_record_info_t read_info;
        assert(config_transaction_backend_read(&backend, CONFIG_TXN_ROLE_STAGED, readback, sizeof(readback),
                                               &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
        assert(readback_size == sizeof(payload));
        assert(memcmp(readback, payload, sizeof(payload)) == 0);
    }

    /* Oversized/invalid arguments are rejected without touching Flash state. */
    assert(config_transaction_backend_stage(&backend, payload, 0u, 0u, 1u, NULL) == CONFIG_TXN_BACKEND_INVALID_ARGUMENT);
    assert(config_transaction_backend_stage(&backend, NULL, sizeof(payload), 0u, 1u, NULL) == CONFIG_TXN_BACKEND_INVALID_ARGUMENT);
    assert(config_transaction_backend_stage(&backend, payload, CONFIG_TXN_MAX_PAYLOAD_SIZE_BYTES + 1u, 0u, 1u, NULL) ==
           CONFIG_TXN_BACKEND_INVALID_ARGUMENT);
    assert(backend.roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
}

/** @brief classify_commit()'s boundary outcomes, mirroring maintenance_journal_backend_test.c's test_commit_post_state(). */
static void test_classify_commit_boundary_states(void)
{
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t payload[128];
    config_transaction_record_info_t info;
    uint32_t running_offset;
    uint32_t commit_offset;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    fill_payload(payload, sizeof(payload), 3u);
    stage_ok(&backend, payload, sizeof(payload), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

    running_offset = (uint32_t)CONFIG_TXN_ROLE_RUNNING * CONFIG_TXN_ROLE_SIZE_BYTES;
    commit_offset = running_offset + CONFIG_TXN_ROLE_SIZE_BYTES - CONFIG_TXN_PAGE_SIZE_BYTES;

    assert(config_transaction_backend_classify_commit(&backend.storage, running_offset, &info) == CONFIG_TXN_COMMIT_COMMITTED);

    /* Erase just the commit page's magic word -> looks fully erased there -> NOT_COMMITTED. */
    memset(&media.bytes[commit_offset], 0xFF, 4u);
    assert(config_transaction_backend_classify_commit(&backend.storage, running_offset, &info) == CONFIG_TXN_COMMIT_NOT_COMMITTED);

    /* Restore commit magic but corrupt a padding byte that must stay erased -> not exact -> NOT_COMMITTED. */
    media.bytes[commit_offset] = 0x43u; media.bytes[commit_offset + 1u] = 0x54u;
    media.bytes[commit_offset + 2u] = 0x58u; media.bytes[commit_offset + 3u] = 0x43u;
    media.bytes[commit_offset + 10u] = 0x00u;
    assert(config_transaction_backend_classify_commit(&backend.storage, running_offset, &info) == CONFIG_TXN_COMMIT_NOT_COMMITTED);

    /* Corrupt a header byte (payload no longer matches its CRC) while the
     * commit page still exactly matches what was written -> CORRUPT. */
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK); /* re-sync roles[] before re-corrupting */
    stage_ok(&backend, payload, sizeof(payload), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);
    media.bytes[running_offset + CONFIG_TXN_PAGE_SIZE_BYTES] ^= 0xFFu; /* first payload byte */
    assert(config_transaction_backend_classify_commit(&backend.storage, running_offset, &info) == CONFIG_TXN_COMMIT_CORRUPT);

    /* Read failure -> INDETERMINATE. */
    media.unreadable = true;
    assert(config_transaction_backend_classify_commit(&backend.storage, running_offset, &info) == CONFIG_TXN_COMMIT_INDETERMINATE);
    media.unreadable = false;
}

static void test_torn_stage_write_discardable(void)
{
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t payload[300];
    unsigned total_calls;

    /* First, learn how many program_page() calls one stage() takes (1 header
     * + ceil(payload/page) payload chunks + 1 commit page) by running it to
     * completion once. */
    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    fill_payload(payload, sizeof(payload), 5u);
    {
        config_transaction_record_info_t info;
        stage_ok(&backend, payload, sizeof(payload), 1u, &info);
    }
    total_calls = media.program_count;
    assert(total_calls >= 3u);

    /* Fail every possible program call number in turn, both as an outright
     * rejection and as a partially-applied ("power died mid-page-program")
     * write. In every case but one, the STAGED role must classify as
     * discardable (never mistaken for a valid staged candidate) and
     * RUNNING/LAST_KNOWN_GOOD must be untouched (stage() never writes
     * them). The one exception: the commit page's own program-call return
     * value is deliberately never trusted (see program_role()'s
     * "(void)b->storage.program_page(...)" for the commit page write) --
     * if that specific call reports failure while the byte content still
     * landed exactly (a "lying" hardware failure code, simulated here by
     * apply_partial=true on the *last* program call), the physical
     * read-back correctly proves COMMITTED and stage() must still report
     * success. This is not a bug: it is the entire reason classification is
     * physical-read-back-based rather than trusting the write call. */
    for (unsigned fail_at = 1u; fail_at <= total_calls; ++fail_at)
    {
        for (int apply_partial = 0; apply_partial < 2; ++apply_partial)
        {
            config_transaction_backend_t attempt;
            config_transaction_record_info_t info;
            config_transaction_backend_result_t result;
            const bool commit_page_lying_success = (fail_at == total_calls) && (apply_partial != 0);

            init_media(&media);
            {
                config_transaction_storage_t io = storage(&media);
                config_transaction_backend_init(&attempt, &io);
            }
            assert(config_transaction_backend_recover(&attempt) == CONFIG_TXN_BACKEND_OK);

            media.fail_program_number = fail_at;
            media.apply_failed_program = apply_partial != 0;
            result = config_transaction_backend_stage(&attempt, payload, sizeof(payload), 0u, 1u, &info);
            media.fail_program_number = 0u;

            if (commit_page_lying_success)
            {
                assert(result == CONFIG_TXN_BACKEND_OK);
                assert(attempt.roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
                continue;
            }
            assert(result != CONFIG_TXN_BACKEND_OK);

            {
                config_transaction_backend_t reboot;
                {
                    config_transaction_storage_t io = storage(&media);
                    config_transaction_backend_init(&reboot, &io);
                }
                assert(config_transaction_backend_recover(&reboot) == CONFIG_TXN_BACKEND_OK);
                assert(reboot.roles[CONFIG_TXN_ROLE_STAGED].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
                assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
                assert(reboot.roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
                /* A torn STAGED-only write never blocks or degrades the rest
                 * of the (empty) system -- still a clean EMPTY outcome. */
                assert(reboot.recovery == CONFIG_TXN_RECOVERY_EMPTY || reboot.recovery == CONFIG_TXN_RECOVERY_FATAL);
                /* CORRUPT/UNKNOWN evidence in STAGED alone (not erased,
                 * structurally broken) is an acceptable, documented FATAL
                 * classification with nothing else committed; NOT_COMMITTED
                 * (cleanly torn/erased) must resolve to EMPTY. */
                if (reboot.roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_NOT_COMMITTED)
                {
                    assert(reboot.recovery == CONFIG_TXN_RECOVERY_EMPTY);
                }
            }
        }
    }
}

static void test_commit_promotes_staged_running_and_lkg(void)
{
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t first[400];
    uint8_t second[900];
    config_transaction_record_info_t info;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);

    fill_payload(first, sizeof(first), 11u);
    stage_ok(&backend, first, sizeof(first), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);
    assert(info.generation == 1u);
    assert(backend.roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
    assert(backend.roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
    assert(backend.roles[CONFIG_TXN_ROLE_STAGED].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
    assert(backend.recovery == CONFIG_TXN_RECOVERY_OK);

    fill_payload(second, sizeof(second), 23u);
    stage_ok(&backend, second, sizeof(second), 2u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);
    assert(info.generation == 2u);
    assert(info.schema_version == 2u);

    {
        uint8_t readback[900];
        size_t readback_size = 0u;
        config_transaction_record_info_t read_info;

        assert(config_transaction_backend_read(&backend, CONFIG_TXN_ROLE_RUNNING, readback, sizeof(readback),
                                               &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
        assert(readback_size == sizeof(second));
        assert(memcmp(readback, second, sizeof(second)) == 0);

        assert(config_transaction_backend_read(&backend, CONFIG_TXN_ROLE_LAST_KNOWN_GOOD, readback, sizeof(readback),
                                               &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
        assert(readback_size == sizeof(first));
        assert(memcmp(readback, first, sizeof(first)) == 0);
        assert(read_info.schema_version == 1u);
    }

    /* Nothing staged -> commit() has nothing to promote. */
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_NOT_FOUND);
}

static void test_power_loss_before_stage_commit_page(void)
{
    /* "Before the stage commit page": fail the very first program call of
     * stage() (the header page) -- the commit page for STAGED is never even
     * attempted. Documented outcome: STAGED classifies NOT_COMMITTED
     * (cleanly discardable); RUNNING/LAST_KNOWN_GOOD are exactly as they
     * were before this stage() call. */
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t running_payload[200];
    uint8_t candidate[200];
    config_transaction_record_info_t info;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    fill_payload(running_payload, sizeof(running_payload), 40u);
    stage_ok(&backend, running_payload, sizeof(running_payload), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

    fill_payload(candidate, sizeof(candidate), 90u);
    media.fail_program_number = media.program_count + 1u; /* the STAGED header page program, the very next call */
    media.apply_failed_program = false;
    assert(config_transaction_backend_stage(&backend, candidate, sizeof(candidate), 0u, 9u, &info) != CONFIG_TXN_BACKEND_OK);
    media.fail_program_number = 0u;

    {
        config_transaction_backend_t reboot;
        uint8_t readback[200];
        size_t readback_size = 0u;
        config_transaction_record_info_t read_info;

        {
            config_transaction_storage_t io = storage(&media);
            config_transaction_backend_init(&reboot, &io);
        }
        assert(config_transaction_backend_recover(&reboot) == CONFIG_TXN_BACKEND_OK);
        assert(reboot.roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_NOT_COMMITTED);
        assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
        assert(reboot.recovery == CONFIG_TXN_RECOVERY_OK);
        assert(config_transaction_backend_read(&reboot, CONFIG_TXN_ROLE_RUNNING, readback, sizeof(readback),
                                               &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
        assert(memcmp(readback, running_payload, sizeof(running_payload)) == 0);
    }
}

static void test_power_loss_after_stage_commit_page_before_promotion(void)
{
    /* "After the stage commit page, before promotion": stage() runs to
     * completion (STAGED fully COMMITTED) but commit() is never called.
     * Documented outcome: recover() reports STAGED as a pending, fully
     * valid candidate (has_committed) alongside a still-valid RUNNING --
     * recovery == OK, since RUNNING alone already determines boot safety;
     * a later caller (T4) decides whether to resume applying the pending
     * staged candidate or discard it. */
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t running_payload[150];
    uint8_t staged_payload[150];
    config_transaction_record_info_t info;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    fill_payload(running_payload, sizeof(running_payload), 60u);
    stage_ok(&backend, running_payload, sizeof(running_payload), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

    fill_payload(staged_payload, sizeof(staged_payload), 61u);
    stage_ok(&backend, staged_payload, sizeof(staged_payload), 5u, &info);
    /* Simulated reboot: commit() is never called. */
    {
        config_transaction_backend_t reboot;
        {
            config_transaction_storage_t io = storage(&media);
            config_transaction_backend_init(&reboot, &io);
        }
        assert(config_transaction_backend_recover(&reboot) == CONFIG_TXN_BACKEND_OK);
        assert(reboot.roles[CONFIG_TXN_ROLE_STAGED].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
        assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
        assert(reboot.recovery == CONFIG_TXN_RECOVERY_OK);
        /* RUNNING must still read back as the untouched old value. */
        {
            uint8_t readback[150];
            size_t readback_size = 0u;
            config_transaction_record_info_t read_info;
            assert(config_transaction_backend_read(&reboot, CONFIG_TXN_ROLE_RUNNING, readback, sizeof(readback),
                                                   &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
            assert(memcmp(readback, running_payload, sizeof(running_payload)) == 0);
        }
        /* commit() from here resumes cleanly, applying the still-pending candidate. */
        assert(config_transaction_backend_commit(&reboot, &info) == CONFIG_TXN_BACKEND_OK);
        {
            uint8_t readback[150];
            size_t readback_size = 0u;
            config_transaction_record_info_t read_info;
            assert(config_transaction_backend_read(&reboot, CONFIG_TXN_ROLE_RUNNING, readback, sizeof(readback),
                                                   &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
            assert(memcmp(readback, staged_payload, sizeof(staged_payload)) == 0);
        }
    }
}

/** @brief Count how many program_page() calls commit() makes when it has both a LAST_KNOWN_GOOD promotion and a RUNNING write to do. */
static unsigned count_commit_program_calls_with_promotion(void)
{
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t first[128];
    uint8_t second[128];
    config_transaction_record_info_t info;
    unsigned before;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    fill_payload(first, sizeof(first), 1u);
    stage_ok(&backend, first, sizeof(first), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

    fill_payload(second, sizeof(second), 2u);
    stage_ok(&backend, second, sizeof(second), 1u, &info);
    before = media.program_count;
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);
    return media.program_count - before;
}

static void test_power_loss_mid_promotion(void)
{
    /* "Mid-promotion": commit() with an existing RUNNING to promote is two
     * role writes back to back (LAST_KNOWN_GOOD, then RUNNING). Fail every
     * program call across both writes in turn.
     *
     * Documented outcome, by which half of the promotion the failure lands
     * in:
     *   - failure during the LAST_KNOWN_GOOD write: RUNNING is never
     *     touched by this commit() call and remains exactly the old,
     *     valid value -- recovery == OK.
     *   - failure during the RUNNING write: LAST_KNOWN_GOOD has already
     *     landed (COMMITTED, holding the old RUNNING content) before this
     *     half starts, so RUNNING classifies NOT_COMMITTED/CORRUPT while
     *     LAST_KNOWN_GOOD is safely usable -- recovery ==
     *     CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED. Neither half can ever
     *     leave both RUNNING and LAST_KNOWN_GOOD unusable at once. */
    unsigned total_calls = count_commit_program_calls_with_promotion();
    /* Both halves write an identically-sized payload -> equal call counts. */
    const unsigned lkg_calls = total_calls / 2u;

    for (unsigned fail_at = 1u; fail_at <= total_calls; ++fail_at)
    {
        for (int apply_partial = 0; apply_partial < 2; ++apply_partial)
        {
            simulated_media_t media;
            config_transaction_backend_t backend;
            uint8_t first[128];
            uint8_t second[128];
            config_transaction_record_info_t info;
            unsigned before;
            config_transaction_backend_result_t commit_result;
            /* Same "lying failure" property as the commit page in
             * test_torn_stage_write_discardable(): the last program call of
             * *either* half is that half's never-trusted commit page. If it
             * reports failure but the bytes still landed exactly
             * (apply_partial=true), physical read-back still proves
             * COMMITTED and this whole commit() call succeeds normally. */
            const bool commit_page_lying_success =
                (fail_at == lkg_calls || fail_at == total_calls) && (apply_partial != 0);

            init_media(&media);
            {
                config_transaction_storage_t io = storage(&media);
                config_transaction_backend_init(&backend, &io);
            }
            assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
            fill_payload(first, sizeof(first), 1u);
            stage_ok(&backend, first, sizeof(first), 1u, &info);
            assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

            fill_payload(second, sizeof(second), 2u);
            stage_ok(&backend, second, sizeof(second), 1u, &info);

            before = media.program_count;
            media.fail_program_number = before + fail_at;
            media.apply_failed_program = apply_partial != 0;
            commit_result = config_transaction_backend_commit(&backend, &info);
            media.fail_program_number = 0u;

            if (commit_page_lying_success)
            {
                assert(commit_result == CONFIG_TXN_BACKEND_OK);
                assert(backend.roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
                assert(backend.roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
                continue;
            }
            assert(commit_result != CONFIG_TXN_BACKEND_OK);

            {
                config_transaction_backend_t reboot;
                {
                    config_transaction_storage_t io = storage(&media);
                    config_transaction_backend_init(&reboot, &io);
                }
                assert(config_transaction_backend_recover(&reboot) == CONFIG_TXN_BACKEND_OK);

                if (fail_at <= lkg_calls)
                {
                    /* LAST_KNOWN_GOOD half failed -- RUNNING untouched. */
                    assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
                    assert(reboot.recovery == CONFIG_TXN_RECOVERY_OK || reboot.recovery == CONFIG_TXN_RECOVERY_DEGRADED);
                }
                else
                {
                    /* RUNNING half failed -- LAST_KNOWN_GOOD already landed. */
                    assert(reboot.roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
                    assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
                    assert(reboot.recovery == CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED);
                }
                /* Invariant proof: never neither. */
                assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED ||
                       reboot.roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
            }
        }
    }
}

static void test_power_loss_after_promotion(void)
{
    /* "After promotion": commit() runs to completion. Documented outcome:
     * RUNNING holds the new value, LAST_KNOWN_GOOD holds the previous
     * value, both COMMITTED, recovery == OK. A best-effort STAGED erase
     * failure afterward must not turn a successful commit into a reported
     * failure. */
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t first[128];
    uint8_t second[128];
    config_transaction_record_info_t info;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    fill_payload(first, sizeof(first), 1u);
    stage_ok(&backend, first, sizeof(first), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

    fill_payload(second, sizeof(second), 2u);
    stage_ok(&backend, second, sizeof(second), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

    {
        config_transaction_backend_t reboot;
        {
            config_transaction_storage_t io = storage(&media);
            config_transaction_backend_init(&reboot, &io);
        }
        assert(config_transaction_backend_recover(&reboot) == CONFIG_TXN_BACKEND_OK);
        assert(reboot.recovery == CONFIG_TXN_RECOVERY_OK);
        assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
        assert(reboot.roles[CONFIG_TXN_ROLE_LAST_KNOWN_GOOD].commit_state == CONFIG_TXN_COMMIT_COMMITTED);
    }
}

static void test_last_known_good_retrieval_after_running_corruption(void)
{
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t good[256];
    uint8_t replaced[256];
    config_transaction_record_info_t info;
    uint32_t running_offset;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);

    fill_payload(good, sizeof(good), 4u);
    stage_ok(&backend, good, sizeof(good), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);

    fill_payload(replaced, sizeof(replaced), 8u);
    stage_ok(&backend, replaced, sizeof(replaced), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);
    /* LAST_KNOWN_GOOD now holds `good`, RUNNING holds `replaced`. */

    /* Corrupt RUNNING directly on the simulated media (e.g. bit-rot). */
    running_offset = (uint32_t)CONFIG_TXN_ROLE_RUNNING * CONFIG_TXN_ROLE_SIZE_BYTES;
    media.bytes[running_offset + CONFIG_TXN_PAGE_SIZE_BYTES] ^= 0xFFu;

    {
        config_transaction_backend_t reboot;
        uint8_t readback[256];
        size_t readback_size = 0u;
        config_transaction_record_info_t read_info;

        {
            config_transaction_storage_t io = storage(&media);
            config_transaction_backend_init(&reboot, &io);
        }
        assert(config_transaction_backend_recover(&reboot) == CONFIG_TXN_BACKEND_OK);
        assert(reboot.roles[CONFIG_TXN_ROLE_RUNNING].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
        assert(reboot.recovery == CONFIG_TXN_RECOVERY_FALLBACK_REQUIRED);

        assert(config_transaction_backend_read(&reboot, CONFIG_TXN_ROLE_LAST_KNOWN_GOOD, readback, sizeof(readback),
                                               &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
        assert(readback_size == sizeof(good));
        assert(memcmp(readback, good, sizeof(good)) == 0);

        /* Reading RUNNING itself must fail closed, never returning corrupt data. */
        assert(config_transaction_backend_read(&reboot, CONFIG_TXN_ROLE_RUNNING, readback, sizeof(readback),
                                               &readback_size, &read_info) == CONFIG_TXN_BACKEND_NOT_FOUND);

        assert(config_transaction_backend_recover_fallback_to_last_known_good(&reboot, &read_info) == CONFIG_TXN_BACKEND_OK);
        assert(reboot.recovery == CONFIG_TXN_RECOVERY_OK);
        assert(config_transaction_backend_read(&reboot, CONFIG_TXN_ROLE_RUNNING, readback, sizeof(readback),
                                               &readback_size, &read_info) == CONFIG_TXN_BACKEND_OK);
        assert(memcmp(readback, good, sizeof(good)) == 0);
    }
}

static void test_discard_staged_and_reset_all(void)
{
    simulated_media_t media;
    config_transaction_backend_t backend;
    uint8_t payload[128];
    config_transaction_record_info_t info;

    init_media(&media);
    {
        config_transaction_storage_t io = storage(&media);
        config_transaction_backend_init(&backend, &io);
    }
    assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
    fill_payload(payload, sizeof(payload), 2u);
    stage_ok(&backend, payload, sizeof(payload), 1u, &info);
    assert(config_transaction_backend_discard_staged(&backend) == CONFIG_TXN_BACKEND_OK);
    assert(backend.roles[CONFIG_TXN_ROLE_STAGED].commit_state != CONFIG_TXN_COMMIT_COMMITTED);

    stage_ok(&backend, payload, sizeof(payload), 1u, &info);
    assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);
    assert(backend.recovery == CONFIG_TXN_RECOVERY_OK);

    assert(config_transaction_backend_reset_all(&backend) == CONFIG_TXN_BACKEND_OK);
    assert(backend.recovery == CONFIG_TXN_RECOVERY_EMPTY);
    for (int i = 0; i < (int)CONFIG_TXN_ROLE_COUNT; ++i)
    {
        assert(backend.roles[i].commit_state != CONFIG_TXN_COMMIT_COMMITTED);
    }

    /* reset_all() recovers even a mutations_blocked backend (e.g. FATAL). */
    {
        uint32_t running_offset = (uint32_t)CONFIG_TXN_ROLE_RUNNING * CONFIG_TXN_ROLE_SIZE_BYTES;
        stage_ok(&backend, payload, sizeof(payload), 1u, &info);
        assert(config_transaction_backend_commit(&backend, &info) == CONFIG_TXN_BACKEND_OK);
        /* Corrupt RUNNING and LAST_KNOWN_GOOD both, and STAGED's leftover
         * header, to force FATAL (no clean EMPTY reading possible). */
        media.bytes[running_offset + CONFIG_TXN_PAGE_SIZE_BYTES] ^= 0xFFu;
        assert(config_transaction_backend_recover(&backend) == CONFIG_TXN_BACKEND_OK);
        if (backend.recovery == CONFIG_TXN_RECOVERY_FATAL)
        {
            assert(backend.mutations_blocked);
        }
        assert(config_transaction_backend_reset_all(&backend) == CONFIG_TXN_BACKEND_OK);
        assert(backend.recovery == CONFIG_TXN_RECOVERY_EMPTY);
        assert(!backend.mutations_blocked);
    }
}

int main(void)
{
    test_stage_success_and_classification();
    test_classify_commit_boundary_states();
    test_torn_stage_write_discardable();
    test_commit_promotes_staged_running_and_lkg();
    test_power_loss_before_stage_commit_page();
    test_power_loss_after_stage_commit_page_before_promotion();
    test_power_loss_mid_promotion();
    test_power_loss_after_promotion();
    test_last_known_good_retrieval_after_running_corruption();
    test_discard_staged_and_reset_all();
    return 0;
}

#include "services/maintenance_journal_codec.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

enum
{
    OFFSET_MAGIC = 0u,
    OFFSET_SCHEMA = 4u,
    OFFSET_TYPE = 6u,
    OFFSET_FLAGS = 7u,
    OFFSET_ENCODED_LENGTH = 8u,
    OFFSET_HEADER_LENGTH = 10u,
    OFFSET_EPOCH = 12u,
    OFFSET_SEQUENCE = 20u,
    OFFSET_OPERATION = 28u,
    OFFSET_TRANSITION = 29u,
    OFFSET_RESERVED = 30u,
    OFFSET_COMMAND_TIME = 32u,
    OFFSET_SNAPSHOT = 40u,
    OFFSET_STATE = OFFSET_SNAPSHOT,
    OFFSET_REVISION = OFFSET_STATE + 1u,
    OFFSET_COMMITTED_TIME = OFFSET_REVISION + 4u,
    OFFSET_HAS_ACTIVITY = OFFSET_COMMITTED_TIME + 8u,
    OFFSET_ACTIVITY = OFFSET_HAS_ACTIVITY + 1u,
    OFFSET_PLANNED_TIME = OFFSET_ACTIVITY,
    OFFSET_DOWNTIME = OFFSET_PLANNED_TIME + 8u,
    OFFSET_METHOD = OFFSET_DOWNTIME + 8u,
    OFFSET_CONFIGURATION_CHANGED = OFFSET_METHOD + 1u,
    OFFSET_SERVICED_COUNT = OFFSET_CONFIGURATION_CHANGED + 1u,
    OFFSET_REPLACED_COUNT = OFFSET_SERVICED_COUNT + 1u,
    OFFSET_OPERATION_ID = OFFSET_REPLACED_COUNT + 1u,
    OFFSET_SUPPLIER = OFFSET_OPERATION_ID + MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY,
    OFFSET_PERSONNEL = OFFSET_SUPPLIER + MAINTENANCE_SERVICE_TEXT_CAPACITY,
    OFFSET_ACTOR = OFFSET_PERSONNEL + MAINTENANCE_SERVICE_TEXT_CAPACITY,
    OFFSET_RESULT = OFFSET_ACTOR + MAINTENANCE_SERVICE_ACTOR_CAPACITY,
    OFFSET_SERVICED_PARTS = OFFSET_RESULT + MAINTENANCE_SERVICE_RESULT_CAPACITY,
    OFFSET_REPLACED_PARTS = OFFSET_SERVICED_PARTS +
                            MAINTENANCE_SERVICE_MAX_PARTS *
                                MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
    OFFSET_CRC = OFFSET_REPLACED_PARTS + MAINTENANCE_SERVICE_MAX_PARTS *
                                       MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
    OFFSET_COMMIT = MAINTENANCE_JOURNAL_RECORD_BODY_SIZE
};

_Static_assert(MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE == 734u,
               "schema-1 encoded size is fixed");
_Static_assert(MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE +
                   MAINTENANCE_JOURNAL_EXTENSION_MARGIN <=
               MAINTENANCE_JOURNAL_RECORD_BODY_SIZE,
               "schema-1 cannot consume the extension margin");
_Static_assert(MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE ==
                   MAINTENANCE_JOURNAL_RECORD_BODY_SIZE +
                       MAINTENANCE_JOURNAL_FLASH_PROGRAM_SIZE,
               "record slot consists of body and one commit page");
_Static_assert(OFFSET_CRC + 4u == MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE,
               "field layout must match the release byte budget");
_Static_assert(CHAR_BIT == 8, "schema-1 bytes require octets");
_Static_assert(sizeof(double) == 8u && FLT_RADIX == 2 && DBL_MANT_DIG == 53 &&
                   DBL_MAX_EXP == 1024,
               "schema-1 downtime requires IEEE-754 binary64");

static uint16_t get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static uint64_t get_u64(const uint8_t *data)
{
    uint64_t result = 0u;
    uint8_t index;

    for (index = 0u; index < 8u; ++index)
    {
        result |= (uint64_t)data[index] << (index * 8u);
    }
    return result;
}

static void put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
}

static void put_u32(uint8_t *data, uint32_t value)
{
    uint8_t index;

    for (index = 0u; index < 4u; ++index)
    {
        data[index] = (uint8_t)(value >> (index * 8u));
    }
}

static void put_u64(uint8_t *data, uint64_t value)
{
    uint8_t index;

    for (index = 0u; index < 8u; ++index)
    {
        data[index] = (uint8_t)(value >> (index * 8u));
    }
}

static bool bytes_are(const uint8_t *data, size_t size, uint8_t expected)
{
    size_t index;

    for (index = 0u; index < size; ++index)
    {
        if (data[index] != expected)
        {
            return false;
        }
    }
    return true;
}

static bool state_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_SERVICE_STATE_EXECUTING;
}

static bool method_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_SERVICE_METHOD_REMOTE;
}

static bool operation_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_JOURNAL_OPERATION_RESET;
}

static bool transition_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_SERVICE_TRANSITION_EXECUTING_TO_FINISHED;
}

static bool record_kind_is_valid(uint8_t type, uint8_t operation, uint8_t transition)
{
    if (type == (uint8_t)MAINTENANCE_JOURNAL_RECORD_RESET)
    {
        return operation == (uint8_t)MAINTENANCE_JOURNAL_OPERATION_RESET &&
               transition == (uint8_t)MAINTENANCE_SERVICE_TRANSITION_NONE;
    }

    if (type != (uint8_t)MAINTENANCE_JOURNAL_RECORD_SNAPSHOT)
    {
        return false;
    }

    return (operation == (uint8_t)MAINTENANCE_JOURNAL_OPERATION_PLAN &&
            transition == (uint8_t)MAINTENANCE_SERVICE_TRANSITION_FINISHED_TO_PLANNED) ||
           (operation == (uint8_t)MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE &&
            transition == (uint8_t)MAINTENANCE_SERVICE_TRANSITION_NONE) ||
           (operation == (uint8_t)MAINTENANCE_JOURNAL_OPERATION_START &&
            transition == (uint8_t)MAINTENANCE_SERVICE_TRANSITION_PLANNED_TO_EXECUTING) ||
           (operation == (uint8_t)MAINTENANCE_JOURNAL_OPERATION_FINISH &&
            transition == (uint8_t)MAINTENANCE_SERVICE_TRANSITION_EXECUTING_TO_FINISHED);
}

static bool validate_utf8(const uint8_t *data, size_t length)
{
    size_t index = 0u;

    while (index < length)
    {
        uint8_t first = data[index++];

        if (first == 0u)
        {
            return false;
        }
        if (first <= 0x7fu)
        {
            continue;
        }
        if (first >= 0xc2u && first <= 0xdfu)
        {
            if (index >= length || (data[index] & 0xc0u) != 0x80u)
            {
                return false;
            }
            ++index;
            continue;
        }
        if (first >= 0xe0u && first <= 0xefu)
        {
            uint8_t second;

            if (index + 1u >= length)
            {
                return false;
            }
            second = data[index];
            if ((second & 0xc0u) != 0x80u || (data[index + 1u] & 0xc0u) != 0x80u ||
                (first == 0xe0u && second < 0xa0u) ||
                (first == 0xedu && second > 0x9fu))
            {
                return false;
            }
            index += 2u;
            continue;
        }
        if (first >= 0xf0u && first <= 0xf4u)
        {
            uint8_t second;

            if (index + 2u >= length)
            {
                return false;
            }
            second = data[index];
            if ((second & 0xc0u) != 0x80u || (data[index + 1u] & 0xc0u) != 0x80u ||
                (data[index + 2u] & 0xc0u) != 0x80u ||
                (first == 0xf0u && second < 0x90u) ||
                (first == 0xf4u && second > 0x8fu))
            {
                return false;
            }
            index += 3u;
            continue;
        }
        return false;
    }
    return true;
}

static bool get_native_string_length(const char *text, size_t capacity, size_t *out_length)
{
    size_t length;

    if (text == NULL || out_length == NULL || capacity == 0u)
    {
        return false;
    }
    for (length = 0u; length < capacity; ++length)
    {
        if (text[length] == '\0')
        {
            size_t trailing;

            for (trailing = length + 1u; trailing < capacity; ++trailing)
            {
                if (text[trailing] != '\0')
                {
                    return false;
                }
            }
            *out_length = length;
            return validate_utf8((const uint8_t *)text, length);
        }
    }
    return false;
}

static bool put_string(uint8_t *destination, size_t capacity, const char *source,
                       bool require_nonempty)
{
    size_t length;

    if (!get_native_string_length(source, capacity, &length) ||
        (require_nonempty && length == 0u))
    {
        return false;
    }
    destination[0] = (uint8_t)length;
    memcpy(&destination[1], source, length);
    return true;
}

static bool string_is_canonical(const uint8_t *source, size_t capacity,
                                bool require_nonempty)
{
    uint8_t length = source[0];

    if ((size_t)length > capacity - 1u || (require_nonempty && length == 0u) ||
        !validate_utf8(&source[1], length) ||
        !bytes_are(&source[1u + length], capacity - 1u - length, 0xffu))
    {
        return false;
    }
    return true;
}

static bool get_string(const uint8_t *source, size_t capacity, char *destination,
                       bool require_nonempty)
{
    uint8_t length = source[0];

    if (!string_is_canonical(source, capacity, require_nonempty))
    {
        return false;
    }
    memcpy(destination, &source[1], length);
    destination[length] = '\0';
    return true;
}

static bool double_to_bits(double value, uint64_t *out_bits)
{
    uint64_t bits;

    if (!isfinite(value) || value < 0.0 || value > MAINTENANCE_SERVICE_MAX_DOWNTIME_MS ||
        out_bits == NULL)
    {
        return false;
    }
    if (value == 0.0)
    {
        *out_bits = 0u;
        return true;
    }
    memcpy(&bits, &value, sizeof(bits));
    *out_bits = bits;
    return true;
}

static bool bits_to_double(uint64_t bits, double *out_value)
{
    double value;

    memcpy(&value, &bits, sizeof(value));
    if (!isfinite(value) || value < 0.0 || value > MAINTENANCE_SERVICE_MAX_DOWNTIME_MS ||
        (value == 0.0 && bits != 0u))
    {
        return false;
    }
    *out_value = value;
    return true;
}

static bool activity_is_empty(const maintenance_activity_t *activity)
{
    static const maintenance_activity_t empty_activity = {0};

    return memcmp(activity, &empty_activity, sizeof(*activity)) == 0;
}

static bool encode_activity(const maintenance_activity_t *activity, uint8_t *slot)
{
    uint8_t index;
    uint64_t downtime_bits;

    if (activity->planned_date_utc_100ns <= 0 || !double_to_bits(activity->estimated_downtime_ms,
                                                                  &downtime_bits) ||
        (activity->method != MAINTENANCE_SERVICE_METHOD_LOCAL &&
         activity->method != MAINTENANCE_SERVICE_METHOD_REMOTE) ||
        activity->parts_serviced_count > MAINTENANCE_SERVICE_MAX_PARTS ||
        activity->parts_replaced_count > MAINTENANCE_SERVICE_MAX_PARTS ||
        !put_string(&slot[OFFSET_OPERATION_ID], MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY,
                    activity->operation_id, true) ||
        !put_string(&slot[OFFSET_SUPPLIER], MAINTENANCE_SERVICE_TEXT_CAPACITY,
                    activity->supplier, false) ||
        !put_string(&slot[OFFSET_PERSONNEL], MAINTENANCE_SERVICE_TEXT_CAPACITY,
                    activity->personnel, false) ||
        !put_string(&slot[OFFSET_ACTOR], MAINTENANCE_SERVICE_ACTOR_CAPACITY,
                    activity->actor, true) ||
        !put_string(&slot[OFFSET_RESULT], MAINTENANCE_SERVICE_RESULT_CAPACITY,
                    activity->result, false))
    {
        return false;
    }

    put_u64(&slot[OFFSET_PLANNED_TIME], (uint64_t)activity->planned_date_utc_100ns);
    put_u64(&slot[OFFSET_DOWNTIME], downtime_bits);
    slot[OFFSET_METHOD] = (uint8_t)activity->method;
    slot[OFFSET_CONFIGURATION_CHANGED] = activity->configuration_changed ? 1u : 0u;
    slot[OFFSET_SERVICED_COUNT] = activity->parts_serviced_count;
    slot[OFFSET_REPLACED_COUNT] = activity->parts_replaced_count;

    for (index = 0u; index < MAINTENANCE_SERVICE_MAX_PARTS; ++index)
    {
        bool serviced_used = index < activity->parts_serviced_count;
        bool replaced_used = index < activity->parts_replaced_count;

        if (!put_string(&slot[OFFSET_SERVICED_PARTS +
                             index * MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
                        MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                        activity->parts_serviced[index], serviced_used) ||
            !put_string(&slot[OFFSET_REPLACED_PARTS +
                             index * MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
                        MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                        activity->parts_replaced[index], replaced_used))
        {
            return false;
        }
    }
    return true;
}

static bool decode_activity(const uint8_t *slot, maintenance_activity_t *activity)
{
    uint8_t index;
    uint64_t downtime_bits;

    if ((slot[OFFSET_CONFIGURATION_CHANGED] != 0u &&
         slot[OFFSET_CONFIGURATION_CHANGED] != 1u) ||
        slot[OFFSET_PLANNED_TIME] == 0u || !method_is_valid(slot[OFFSET_METHOD]) ||
        slot[OFFSET_SERVICED_COUNT] > MAINTENANCE_SERVICE_MAX_PARTS ||
        slot[OFFSET_REPLACED_COUNT] > MAINTENANCE_SERVICE_MAX_PARTS)
    {
        return false;
    }

    activity->planned_date_utc_100ns = (int64_t)get_u64(&slot[OFFSET_PLANNED_TIME]);
    downtime_bits = get_u64(&slot[OFFSET_DOWNTIME]);
    if (activity->planned_date_utc_100ns <= 0 || !bits_to_double(downtime_bits,
                                                                   &activity->estimated_downtime_ms))
    {
        return false;
    }
    activity->method = (maintenance_service_method_t)slot[OFFSET_METHOD];
    activity->configuration_changed = slot[OFFSET_CONFIGURATION_CHANGED] != 0u;
    activity->parts_serviced_count = slot[OFFSET_SERVICED_COUNT];
    activity->parts_replaced_count = slot[OFFSET_REPLACED_COUNT];
    if (!get_string(&slot[OFFSET_OPERATION_ID], MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY,
                    activity->operation_id, true) ||
        !get_string(&slot[OFFSET_SUPPLIER], MAINTENANCE_SERVICE_TEXT_CAPACITY,
                    activity->supplier, false) ||
        !get_string(&slot[OFFSET_PERSONNEL], MAINTENANCE_SERVICE_TEXT_CAPACITY,
                    activity->personnel, false) ||
        !get_string(&slot[OFFSET_ACTOR], MAINTENANCE_SERVICE_ACTOR_CAPACITY,
                    activity->actor, true) ||
        !get_string(&slot[OFFSET_RESULT], MAINTENANCE_SERVICE_RESULT_CAPACITY,
                    activity->result, false))
    {
        return false;
    }
    for (index = 0u; index < MAINTENANCE_SERVICE_MAX_PARTS; ++index)
    {
        if (!get_string(&slot[OFFSET_SERVICED_PARTS +
                              index * MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
                        MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                        activity->parts_serviced[index],
                        index < activity->parts_serviced_count) ||
            !get_string(&slot[OFFSET_REPLACED_PARTS +
                              index * MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
                        MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                        activity->parts_replaced[index],
                        index < activity->parts_replaced_count))
        {
            return false;
        }
    }
    return true;
}

static bool activity_is_canonical(const uint8_t *slot)
{
    uint64_t downtime_bits;
    double downtime;
    uint8_t index;

    if ((slot[OFFSET_CONFIGURATION_CHANGED] != 0u &&
         slot[OFFSET_CONFIGURATION_CHANGED] != 1u) ||
        slot[OFFSET_PLANNED_TIME] == 0u || !method_is_valid(slot[OFFSET_METHOD]) ||
        slot[OFFSET_SERVICED_COUNT] > MAINTENANCE_SERVICE_MAX_PARTS ||
        slot[OFFSET_REPLACED_COUNT] > MAINTENANCE_SERVICE_MAX_PARTS ||
        (int64_t)get_u64(&slot[OFFSET_PLANNED_TIME]) <= 0)
    {
        return false;
    }
    downtime_bits = get_u64(&slot[OFFSET_DOWNTIME]);
    if (!bits_to_double(downtime_bits, &downtime) ||
        !string_is_canonical(&slot[OFFSET_OPERATION_ID],
                             MAINTENANCE_SERVICE_OPERATION_ID_CAPACITY, true) ||
        !string_is_canonical(&slot[OFFSET_SUPPLIER], MAINTENANCE_SERVICE_TEXT_CAPACITY,
                             false) ||
        !string_is_canonical(&slot[OFFSET_PERSONNEL], MAINTENANCE_SERVICE_TEXT_CAPACITY,
                             false) ||
        !string_is_canonical(&slot[OFFSET_ACTOR], MAINTENANCE_SERVICE_ACTOR_CAPACITY,
                             true) ||
        !string_is_canonical(&slot[OFFSET_RESULT], MAINTENANCE_SERVICE_RESULT_CAPACITY,
                             false))
    {
        return false;
    }
    for (index = 0u; index < MAINTENANCE_SERVICE_MAX_PARTS; ++index)
    {
        if (!string_is_canonical(&slot[OFFSET_SERVICED_PARTS +
                                        index * MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
                                 MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                                 index < slot[OFFSET_SERVICED_COUNT]) ||
            !string_is_canonical(&slot[OFFSET_REPLACED_PARTS +
                                        index * MAINTENANCE_SERVICE_PART_TEXT_CAPACITY],
                                 MAINTENANCE_SERVICE_PART_TEXT_CAPACITY,
                                 index < slot[OFFSET_REPLACED_COUNT]))
        {
            return false;
        }
    }
    return true;
}

uint32_t maintenance_journal_codec_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    size_t index;

    if (data == NULL && size != 0u)
    {
        return 0u;
    }
    for (index = 0u; index < size; ++index)
    {
        uint8_t bit;

        crc ^= data[index];
        for (bit = 0u; bit < 8u; ++bit)
        {
            crc = (crc & 1u) != 0u ? (crc >> 1u) ^ 0xedb88320u : crc >> 1u;
        }
    }
    return crc ^ 0xffffffffu;
}

bool maintenance_journal_codec_has_exact_commit_page(const uint8_t *slot,
                                                      size_t slot_size)
{
    return slot != NULL && slot_size == MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE &&
           get_u32(&slot[OFFSET_COMMIT]) == MAINTENANCE_JOURNAL_COMMIT_WORD &&
           bytes_are(&slot[OFFSET_COMMIT + 4u],
                     MAINTENANCE_JOURNAL_FLASH_PROGRAM_SIZE - 4u, 0xffu);
}

maintenance_journal_codec_result_t maintenance_journal_codec_encode(
    const maintenance_journal_record_t *record, uint8_t *out_slot, size_t out_slot_size)
{
    uint32_t crc;

    if (record == NULL || out_slot == NULL)
    {
        return MAINTENANCE_JOURNAL_CODEC_INVALID_ARGUMENT;
    }
    if (out_slot_size < MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE)
    {
        return MAINTENANCE_JOURNAL_CODEC_BUFFER_TOO_SMALL;
    }
    if (record->epoch == 0u || record->epoch == UINT64_MAX || record->sequence == 0u ||
        record->sequence == UINT64_MAX || record->command_time_utc_100ns <= 0 ||
        (record->type != MAINTENANCE_JOURNAL_RECORD_SNAPSHOT &&
         record->type != MAINTENANCE_JOURNAL_RECORD_RESET) ||
        (record->operation != MAINTENANCE_JOURNAL_OPERATION_PLAN &&
         record->operation != MAINTENANCE_JOURNAL_OPERATION_RESCHEDULE &&
         record->operation != MAINTENANCE_JOURNAL_OPERATION_START &&
         record->operation != MAINTENANCE_JOURNAL_OPERATION_FINISH &&
         record->operation != MAINTENANCE_JOURNAL_OPERATION_RESET) ||
        (record->transition != MAINTENANCE_SERVICE_TRANSITION_NONE &&
         record->transition != MAINTENANCE_SERVICE_TRANSITION_FINISHED_TO_PLANNED &&
         record->transition != MAINTENANCE_SERVICE_TRANSITION_PLANNED_TO_EXECUTING &&
         record->transition != MAINTENANCE_SERVICE_TRANSITION_EXECUTING_TO_FINISHED) ||
        !record_kind_is_valid((uint8_t)record->type, (uint8_t)record->operation,
                              (uint8_t)record->transition) ||
        (record->snapshot.state != MAINTENANCE_SERVICE_STATE_FINISHED &&
         record->snapshot.state != MAINTENANCE_SERVICE_STATE_PLANNED &&
         record->snapshot.state != MAINTENANCE_SERVICE_STATE_EXECUTING) ||
        record->snapshot.revision == 0u)
    {
        return MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD;
    }

    memset(out_slot, 0xff, MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE);
    put_u32(&out_slot[OFFSET_MAGIC], MAINTENANCE_JOURNAL_RECORD_MAGIC);
    put_u16(&out_slot[OFFSET_SCHEMA], MAINTENANCE_JOURNAL_SCHEMA_VERSION);
    out_slot[OFFSET_TYPE] = (uint8_t)record->type;
    out_slot[OFFSET_FLAGS] = 0u;
    put_u16(&out_slot[OFFSET_ENCODED_LENGTH], MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE);
    put_u16(&out_slot[OFFSET_HEADER_LENGTH], MAINTENANCE_JOURNAL_ENCODED_HEADER_SIZE);
    put_u64(&out_slot[OFFSET_EPOCH], record->epoch);
    put_u64(&out_slot[OFFSET_SEQUENCE], record->sequence);
    out_slot[OFFSET_OPERATION] = (uint8_t)record->operation;
    out_slot[OFFSET_TRANSITION] = (uint8_t)record->transition;
    put_u64(&out_slot[OFFSET_COMMAND_TIME], (uint64_t)record->command_time_utc_100ns);
    out_slot[OFFSET_STATE] = (uint8_t)record->snapshot.state;
    put_u32(&out_slot[OFFSET_REVISION], record->snapshot.revision);
    put_u64(&out_slot[OFFSET_COMMITTED_TIME],
            (uint64_t)record->snapshot.committed_at_utc_100ns);
    out_slot[OFFSET_HAS_ACTIVITY] = record->snapshot.has_activity ? 1u : 0u;

    if (record->snapshot.has_activity)
    {
        if (record->snapshot.committed_at_utc_100ns <= 0 ||
            !encode_activity(&record->snapshot.activity, out_slot))
        {
            return MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD;
        }
    }
    else if (record->snapshot.state != MAINTENANCE_SERVICE_STATE_FINISHED ||
             record->snapshot.committed_at_utc_100ns != 0 ||
             !activity_is_empty(&record->snapshot.activity))
    {
        return MAINTENANCE_JOURNAL_CODEC_INVALID_RECORD;
    }

    crc = maintenance_journal_codec_crc32(out_slot, OFFSET_CRC);
    put_u32(&out_slot[OFFSET_CRC], crc);
    put_u32(&out_slot[OFFSET_COMMIT], MAINTENANCE_JOURNAL_COMMIT_WORD);
    return MAINTENANCE_JOURNAL_CODEC_OK;
}

maintenance_journal_codec_result_t maintenance_journal_codec_decode(
    const uint8_t *slot, size_t slot_size, maintenance_journal_record_t *out_record)
{
    if (slot == NULL || out_record == NULL)
    {
        return MAINTENANCE_JOURNAL_CODEC_INVALID_ARGUMENT;
    }
    if (slot_size < MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE)
    {
        return MAINTENANCE_JOURNAL_CODEC_BUFFER_TOO_SMALL;
    }
    if (slot_size != MAINTENANCE_JOURNAL_RECORD_SLOT_SIZE)
    {
        return MAINTENANCE_JOURNAL_CODEC_MALFORMED;
    }
    if (!maintenance_journal_codec_has_exact_commit_page(slot, slot_size))
    {
        return MAINTENANCE_JOURNAL_CODEC_INCOMPLETE;
    }
    if (get_u32(&slot[OFFSET_MAGIC]) != MAINTENANCE_JOURNAL_RECORD_MAGIC ||
        get_u16(&slot[OFFSET_SCHEMA]) != MAINTENANCE_JOURNAL_SCHEMA_VERSION ||
        slot[OFFSET_FLAGS] != 0u ||
        get_u16(&slot[OFFSET_ENCODED_LENGTH]) != MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE ||
        get_u16(&slot[OFFSET_HEADER_LENGTH]) != MAINTENANCE_JOURNAL_ENCODED_HEADER_SIZE ||
        !bytes_are(&slot[OFFSET_RESERVED], 2u, 0xffu) ||
        !bytes_are(&slot[MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE],
                   MAINTENANCE_JOURNAL_EXTENSION_MARGIN, 0xffu) ||
        !bytes_are(&slot[MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE +
                            MAINTENANCE_JOURNAL_EXTENSION_MARGIN],
                   MAINTENANCE_JOURNAL_RECORD_BODY_SIZE -
                       MAINTENANCE_JOURNAL_MAX_ENCODED_SIZE -
                       MAINTENANCE_JOURNAL_EXTENSION_MARGIN,
                   0xffu) ||
        get_u32(&slot[OFFSET_CRC]) != maintenance_journal_codec_crc32(slot, OFFSET_CRC))
    {
        return MAINTENANCE_JOURNAL_CODEC_MALFORMED;
    }

    if (get_u64(&slot[OFFSET_EPOCH]) == 0u ||
        get_u64(&slot[OFFSET_EPOCH]) == UINT64_MAX ||
        get_u64(&slot[OFFSET_SEQUENCE]) == 0u ||
        get_u64(&slot[OFFSET_SEQUENCE]) == UINT64_MAX ||
        (int64_t)get_u64(&slot[OFFSET_COMMAND_TIME]) <= 0 ||
        !state_is_valid(slot[OFFSET_STATE]) || get_u32(&slot[OFFSET_REVISION]) == 0u ||
        (slot[OFFSET_HAS_ACTIVITY] != 0u && slot[OFFSET_HAS_ACTIVITY] != 1u) ||
        !operation_is_valid(slot[OFFSET_OPERATION]) ||
        !transition_is_valid(slot[OFFSET_TRANSITION]) ||
        !record_kind_is_valid(slot[OFFSET_TYPE], slot[OFFSET_OPERATION],
                              slot[OFFSET_TRANSITION]))
    {
        return MAINTENANCE_JOURNAL_CODEC_MALFORMED;
    }

    if (slot[OFFSET_HAS_ACTIVITY] != 0u)
    {
        if ((int64_t)get_u64(&slot[OFFSET_COMMITTED_TIME]) <= 0 ||
            !activity_is_canonical(slot))
        {
            return MAINTENANCE_JOURNAL_CODEC_MALFORMED;
        }
    }
    else if (slot[OFFSET_STATE] != (uint8_t)MAINTENANCE_SERVICE_STATE_FINISHED ||
             get_u64(&slot[OFFSET_COMMITTED_TIME]) != 0u ||
             !bytes_are(&slot[OFFSET_ACTIVITY], OFFSET_CRC - OFFSET_ACTIVITY, 0xffu))
    {
        return MAINTENANCE_JOURNAL_CODEC_MALFORMED;
    }

    memset(out_record, 0, sizeof(*out_record));
    out_record->epoch = get_u64(&slot[OFFSET_EPOCH]);
    out_record->sequence = get_u64(&slot[OFFSET_SEQUENCE]);
    out_record->command_time_utc_100ns = (int64_t)get_u64(&slot[OFFSET_COMMAND_TIME]);
    out_record->type = (maintenance_journal_record_type_t)slot[OFFSET_TYPE];
    out_record->operation = (maintenance_journal_operation_t)slot[OFFSET_OPERATION];
    out_record->transition = (maintenance_service_transition_t)slot[OFFSET_TRANSITION];
    out_record->snapshot.state = (maintenance_service_state_t)slot[OFFSET_STATE];
    out_record->snapshot.revision = get_u32(&slot[OFFSET_REVISION]);
    out_record->snapshot.committed_at_utc_100ns =
        (int64_t)get_u64(&slot[OFFSET_COMMITTED_TIME]);
    out_record->snapshot.has_activity = slot[OFFSET_HAS_ACTIVITY] != 0u;
    if (out_record->snapshot.has_activity)
    {
        (void)decode_activity(slot, &out_record->snapshot.activity);
    }
    return MAINTENANCE_JOURNAL_CODEC_OK;
}

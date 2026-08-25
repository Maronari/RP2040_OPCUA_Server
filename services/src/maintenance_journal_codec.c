/**
 * @file
 * @brief Schema-1 maintenance journal record codec implementation; see
 *        services/include/services/maintenance_journal_codec.h for the
 *        public API contract.
 *
 * Responsibilities:
 * - Fixed-offset little-endian field packing/unpacking (`get_u16`/`put_u16`
 *   etc.) for the 1024-byte canonical slot layout defined by the
 *   `OFFSET_*` enum below.
 * - Length-prefixed canonical string encode/decode/validate
 *   (`put_string`/`get_string`/`string_is_canonical`), each string's
 *   unused capacity padded with 0xFF and validated as UTF-8.
 * - Whole-record structural validation shared between encode (defense in
 *   depth against building a malformed record) and decode (rejecting
 *   anything not byte-for-byte canonical).
 *
 * Ownership: stateless - no file-static data; every function operates only
 * on caller-provided buffers/structs.
 *
 * Concurrency: pure functions with no shared/global state, so safe to call
 * from any task or from host test code without synchronization.
 */
#include "services/maintenance_journal_codec.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

/** @brief Fixed byte offsets of every schema-1 field within the 1024-byte canonical slot. */
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

/** @brief Reads a little-endian 16-bit field from @p data (wire-format decode; portable regardless of host endianness). */
static uint16_t get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

/** @brief Reads a little-endian 32-bit field from @p data (wire-format decode). */
static uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

/** @brief Reads a little-endian 64-bit field from @p data (wire-format decode). */
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

/** @brief Writes @p value as a little-endian 16-bit field into @p data (wire-format encode). */
static void put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
}

/** @brief Writes @p value as a little-endian 32-bit field into @p data (wire-format encode). */
static void put_u32(uint8_t *data, uint32_t value)
{
    uint8_t index;

    for (index = 0u; index < 4u; ++index)
    {
        data[index] = (uint8_t)(value >> (index * 8u));
    }
}

/** @brief Writes @p value as a little-endian 64-bit field into @p data (wire-format encode). */
static void put_u64(uint8_t *data, uint64_t value)
{
    uint8_t index;

    for (index = 0u; index < 8u; ++index)
    {
        data[index] = (uint8_t)(value >> (index * 8u));
    }
}

/** @brief Returns true if all @p size bytes at @p data equal @p expected; used to verify canonical 0xFF padding/filler regions. */
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

/** @brief Range-checks a decoded `maintenance_service_state_t` byte (relies on the enum's contiguous 0-based values). */
static bool state_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_SERVICE_STATE_EXECUTING;
}

/** @brief Range-checks a decoded `maintenance_service_method_t` byte. */
static bool method_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_SERVICE_METHOD_REMOTE;
}

/** @brief Range-checks a decoded `maintenance_journal_operation_t` byte. */
static bool operation_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_JOURNAL_OPERATION_RESET;
}

/** @brief Range-checks a decoded `maintenance_service_transition_t` byte. */
static bool transition_is_valid(uint8_t value)
{
    return value <= (uint8_t)MAINTENANCE_SERVICE_TRANSITION_EXECUTING_TO_FINISHED;
}

/**
 * @brief Validates that @p type/@p operation/@p transition form one of the
 *        fixed set of legal record-kind combinations.
 *
 * A RESET record must pair with OPERATION_RESET and TRANSITION_NONE. A
 * SNAPSHOT record must pair one of PLAN/RESCHEDULE/START/FINISH with the
 * exact corresponding state transition (e.g. START only with
 * PLANNED_TO_EXECUTING). Any other combination - even individually valid
 * enum values - is rejected. Shared by both encode-time validation and
 * decode-time structural validation so the two stay in lockstep.
 */
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

/**
 * @brief Validates that @p length bytes at @p data form well-formed,
 *        NUL-free UTF-8 (1-4 byte sequences, no overlong encodings, no
 *        surrogate-range 3-byte sequences).
 *
 * Rejects an embedded NUL byte outright (journal strings are
 * length-prefixed, not NUL-terminated on the wire, so an embedded NUL would
 * be ambiguous/unexpected data). For each multi-byte lead byte, checks the
 * exact continuation-byte count and the overlong/out-of-range exclusions
 * required by the UTF-8 grammar (e.g. 0xE0 must be followed by a
 * continuation byte >= 0xA0; 0xF4 must be followed by one <= 0x8F).
 *
 * @param data Candidate UTF-8 byte sequence.
 * @param length Number of bytes to validate.
 * @return true if every byte forms part of a well-formed UTF-8 sequence
 *         with no embedded NUL.
 */
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

/**
 * @brief Computes the NUL-terminated length of a fixed-capacity C string
 *        field, requiring it to be well-formed for the wire encoding.
 *
 * A field is accepted only if it contains exactly one NUL terminator
 * somewhere within @p capacity bytes, every byte after that NUL is also
 * NUL (no garbage past the terminator - the source buffer must already be
 * zero-padded), and the bytes before the NUL are valid UTF-8
 * (validate_utf8()). Used by put_string() before encoding a native
 * in-memory string field into the canonical length-prefixed wire form.
 *
 * @param text Candidate fixed-capacity string buffer.
 * @param capacity Total buffer size, including the terminator.
 * @param out_length Receives the string length (excluding terminator) on success.
 * @return true if @p text is a well-formed, fully-zero-padded, UTF-8 string
 *         within @p capacity; false otherwise (including if @p text/
 *         @p out_length is NULL or @p capacity is 0).
 */
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

/**
 * @brief Encodes a native fixed-capacity C string as a canonical
 *        length-prefixed field: one length byte followed by the raw UTF-8 bytes.
 *
 * Does not zero/pad the remaining destination capacity itself - callers
 * pre-fill the slot with 0xFF (see maintenance_journal_codec_encode()) so
 * unused trailing bytes end up as the canonical filler that
 * string_is_canonical() later checks for.
 *
 * @param destination Output buffer; must have room for `1 + capacity - 1`
 *        bytes (length byte plus up to `capacity - 1` string bytes).
 * @param capacity Field capacity, matching the corresponding
 *        `MAINTENANCE_SERVICE_*_CAPACITY` constant (includes the implicit
 *        length-byte budget).
 * @param source Native fixed-capacity source string; validated via
 *        get_native_string_length().
 * @param require_nonempty If true, an empty string is rejected.
 * @return true on success; false if @p source is not a well-formed native
 *         string for this field or violates @p require_nonempty.
 */
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

/**
 * @brief Validates that an encoded length-prefixed string field is exactly
 *        canonical: valid UTF-8 payload plus all-0xFF unused trailing capacity.
 *
 * This is the decode-side counterpart to put_string()'s encoding
 * convention; used both by get_string() (before extracting the value) and
 * directly by whole-record canonical-form checks
 * (activity_is_canonical()) that only need a yes/no answer.
 *
 * @param source Encoded field: length byte followed by @p capacity - 1 bytes.
 * @param capacity Field capacity (including the length byte).
 * @param require_nonempty If true, a zero-length string is rejected.
 * @return true if the field's length is in range, its payload is valid
 *         UTF-8, and every unused trailing byte is exactly 0xFF.
 */
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

/**
 * @brief Decodes a canonical length-prefixed field into a NUL-terminated
 *        native C string, after validating it is exactly canonical.
 *
 * @param source Encoded field: length byte followed by @p capacity - 1 bytes.
 * @param capacity Field capacity (including the length byte).
 * @param destination Output buffer; must have room for the decoded string
 *        plus a NUL terminator (at most @p capacity - 1 bytes plus NUL).
 * @param require_nonempty If true, a zero-length string is rejected.
 * @return true if string_is_canonical() accepts @p source, in which case
 *         @p destination is populated and NUL-terminated; false otherwise
 *         (destination left untouched).
 */
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

/**
 * @brief Converts an estimated-downtime `double` to its canonical wire
 *        representation (raw IEEE-754 binary64 bits, with a fixed all-zero
 *        encoding for the value 0.0).
 *
 * Rejects non-finite values, negative values, and values exceeding
 * MAINTENANCE_SERVICE_MAX_DOWNTIME_MS before encoding. The 0.0 special
 * case avoids relying on IEEE-754 negative-zero bit patterns ever reaching
 * the wire.
 *
 * @param value Downtime value in milliseconds.
 * @param out_bits Receives the raw bit pattern on success; must be non-NULL.
 * @return true if @p value is finite, in `[0, MAINTENANCE_SERVICE_MAX_DOWNTIME_MS]`,
 *         and @p out_bits is non-NULL.
 */
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

/**
 * @brief Converts a raw wire bit pattern back to an estimated-downtime
 *        `double`, rejecting any pattern that is not canonical.
 *
 * The decoded value must be finite and within
 * `[0, MAINTENANCE_SERVICE_MAX_DOWNTIME_MS]`; additionally, a decoded value
 * of 0.0 is only accepted if @p bits was exactly the canonical all-zero
 * encoding (rejects an alternate/non-canonical bit pattern that happens to
 * also evaluate to 0.0, e.g. negative zero), matching double_to_bits()'s
 * encoding convention.
 *
 * @param bits Raw IEEE-754 binary64 bit pattern from the wire.
 * @param out_value Receives the decoded value on success.
 * @return true if @p bits decodes to a canonical, in-range downtime value.
 */
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

/** @brief Returns true if @p activity is all-zero (the "no activity recorded" sentinel), by exact byte comparison. */
static bool activity_is_empty(const maintenance_activity_t *activity)
{
    static const maintenance_activity_t empty_activity = {0};

    return memcmp(activity, &empty_activity, sizeof(*activity)) == 0;
}

/**
 * @brief Encodes a `maintenance_activity_t` (operation id, supplier,
 *        personnel, actor, result, serviced/replaced parts, downtime,
 *        method, planned time) into its fixed-offset fields within @p slot.
 *
 * Validates every field before writing any of them: planned time must be
 * positive, downtime must convert via double_to_bits(), method must be a
 * valid `maintenance_service_method_t`, serviced/replaced part counts must
 * not exceed MAINTENANCE_SERVICE_MAX_PARTS, and every string field must
 * encode successfully via put_string() (operation_id and actor are
 * required-nonempty; supplier/personnel/result may be empty). Part-slot
 * strings beyond each count are still encoded, but with
 * `require_nonempty=false`, so unused part slots become empty canonical
 * strings rather than leftover 0xFF filler.
 *
 * @param activity Activity record to encode.
 * @param slot Destination slot buffer (fields written at their `OFFSET_*`
 *        positions; caller has already filled the slot with 0xFF).
 * @return true if every field validated and encoded successfully; false on
 *         the first validation failure (partial writes into @p slot may
 *         have occurred but the caller discards the whole encode attempt
 *         on failure).
 */
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

/**
 * @brief Decodes the fixed-offset activity fields from @p slot into @p activity.
 *
 * Performs the inverse of encode_activity(), with the same field-range and
 * string-canonicality checks (planned time positive, method valid, part
 * counts bounded, every string field canonical via get_string()). Callers
 * must already know @p slot has `has_activity` set (this function does not
 * check that flag). Its only caller, maintenance_journal_codec_decode(),
 * calls activity_is_canonical() on the same slot first and discards this
 * function's return value (`(void)decode_activity(...)`), relying on that
 * prior check to guarantee success here - so a false return in that
 * context would indicate the two validation paths have diverged.
 *
 * @param slot Source slot buffer.
 * @param activity Destination activity struct.
 * @return true if every field decoded successfully; false on the first
 *         invalid/non-canonical field.
 */
static bool decode_activity(const uint8_t *slot, maintenance_activity_t *activity)
{
    uint8_t index;
    uint64_t downtime_bits;

    if ((slot[OFFSET_CONFIGURATION_CHANGED] != 0u &&
         slot[OFFSET_CONFIGURATION_CHANGED] != 1u) ||
        get_u64(&slot[OFFSET_PLANNED_TIME]) == 0u || !method_is_valid(slot[OFFSET_METHOD]) ||
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

/**
 * @brief Validates that the fixed-offset activity fields within @p slot are
 *        exactly canonical, without decoding them into a struct.
 *
 * Checks the same constraints as decode_activity() (planned time positive,
 * method valid, part counts bounded, downtime bits canonical via
 * bits_to_double()) plus string_is_canonical() for every string field
 * (operation_id, supplier, personnel, actor, result, and every part-slot
 * string). This is the authoritative canonical-form check invoked by
 * maintenance_journal_codec_decode() before it trusts a slot enough to
 * call decode_activity(); a slot that fails this check is rejected as
 * MAINTENANCE_JOURNAL_CODEC_MALFORMED.
 *
 * @param slot Source slot buffer.
 * @return true if every activity field is exactly canonical.
 */
static bool activity_is_canonical(const uint8_t *slot)
{
    uint64_t downtime_bits;
    double downtime;
    uint8_t index;

    if ((slot[OFFSET_CONFIGURATION_CHANGED] != 0u &&
         slot[OFFSET_CONFIGURATION_CHANGED] != 1u) ||
        get_u64(&slot[OFFSET_PLANNED_TIME]) == 0u || !method_is_valid(slot[OFFSET_METHOD]) ||
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

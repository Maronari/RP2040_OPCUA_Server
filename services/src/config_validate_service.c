/**
 * @file
 * @brief Implementation of configuration validation: semantic candidate
 * validation, plus a hand-rolled recursive-descent JSON reader
 * (`config_json_reader_t` and the `config_json_*` family below) that
 * decodes the public configuration export/import JSON schema.
 *
 * Responsibilities:
 * - `config_validate_service_validate_candidate()` — semantic gate for any
 *   `device_profile_config_t` (timezone, ports, network, per-channel driver/
 *   resource-claim checks). See `services/config_validate_service.h`.
 * - JSON parsing: a minimal, allocation-free, single-pass reader over a
 *   caller-owned byte range (`config_json_reader_t::cursor`/`end`). Every
 *   `config_json_reader_*`/`config_json_expect_*`/`config_json_parse_*`
 *   function returns a `config_validate_service_result_t` and advances
 *   `cursor` only on success; on the first error the whole parse aborts and
 *   propagates that result up to
 *   `config_validate_service_parse_json_payload()`.
 * - Binary payload validation: decodes via the `schema_version`-keyed
 *   `config_compat_decode_versioned()` (schema/version migration; rejects an
 *   unrecognized `schema_version` outright) then re-uses the same semantic
 *   gate. The JSON `schemaVersion` field is checked against the same
 *   migration table via `config_compat_schema_version_is_supported()`.
 *
 * Ownership: no persistent state; the JSON reader borrows the caller's
 * payload buffer and never copies it except into caller-supplied
 * `device_profile_config_t`/string-field output buffers.
 *
 * Concurrency: no internal synchronization; pure functions over
 * caller-provided memory (with the caveat noted on
 * `config_validate_service_validate_imported()` regarding the shared
 * `config_import_service` session).
 */
#include <services/config_validate_service.h>

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <services/config_import_service.h>
#include <services/config_wire.h>
#include <services/driver_catalog.h>
#include <services/config_compat.h>
#include <services/telemetry_channel.h>

#define CONFIG_VALIDATE_MIN_WATCHDOG_TIMEOUT_MS 1000u

/**
 * @brief Cursor-based state for the single-pass JSON reader.
 *
 * `cursor`/`end` bound the remaining unparsed input (never reallocated,
 * never copied). `message`/`message_size` are an optional caller-provided
 * sink that `config_json_reader_fail()` writes a human-readable parse-error
 * description into. Owned entirely by the calling parse function; not
 * shared across threads.
 */
typedef struct
{
    const char *cursor;
    const char *end;
    char *message;
    size_t message_size;
} config_json_reader_t;

/**
 * @brief Fills @p report from the given validation outcome.
 * @param report Destination; no-op if NULL. Zeroed first, then populated
 * (`valid` is derived as `result == CONFIG_VALIDATE_RESULT_OK`).
 * @param message Optional human-readable message, truncated to fit
 * `report->message`.
 */
static void config_validate_service_set_report(config_validate_service_report_t *report,
                                               config_validate_service_result_t result,
                                               uint32_t schema_version,
                                               uint32_t payload_size,
                                               uint32_t payload_crc32,
                                               const char *message)
{
    if (report == NULL)
    {
        return;
    }

    memset(report, 0, sizeof(*report));
    report->valid = (result == CONFIG_VALIDATE_RESULT_OK);
    report->result = result;
    report->schema_version = schema_version;
    report->payload_size = payload_size;
    report->payload_crc32 = payload_crc32;
    if (message != NULL)
    {
        (void)snprintf(report->message, sizeof(report->message), "%s", message);
        report->message[sizeof(report->message) - 1u] = '\0';
    }
}

/**
 * @brief `vsnprintf`-based formatter that always leaves @p buffer
 * NUL-terminated, used to build `config_validate_service_validate_candidate()`
 * rejection messages.
 * @note No-op if @p buffer is NULL, @p buffer_size is 0, or @p format is
 * NULL.
 */
static void config_validate_service_set_message(char *buffer, size_t buffer_size, const char *format, ...)
{
    va_list args;

    if (buffer == NULL || buffer_size == 0u || format == NULL)
    {
        return;
    }

    va_start(args, format);
    (void)vsnprintf(buffer, buffer_size, format, args);
    va_end(args);
    buffer[buffer_size - 1u] = '\0';
}

/**
 * @brief Records a formatted parse-error message on @p reader (if it has a
 * message sink) and returns @p result, so callers can write
 * `return config_json_reader_fail(reader, RESULT, "...", ...);` as a single
 * error-path expression.
 * @return @p result, unconditionally.
 * @note Does not advance `reader->cursor`; purely an error-reporting helper.
 */
static config_validate_service_result_t config_json_reader_fail(config_json_reader_t *reader,
                                                                config_validate_service_result_t result,
                                                                const char *format,
                                                                ...)
{
    va_list args;

    if (reader != NULL && reader->message != NULL && reader->message_size > 0u && format != NULL)
    {
        va_start(args, format);
        (void)vsnprintf(reader->message, reader->message_size, format, args);
        va_end(args);
        reader->message[reader->message_size - 1u] = '\0';
    }

    return result;
}

static bool config_validate_service_ipv4_is_zero(const uint8_t address[4])
{
    return address == NULL ||
           (address[0] == 0u && address[1] == 0u && address[2] == 0u && address[3] == 0u);
}

static bool config_validate_service_channel_ids_equal(const config_channel_definition_t *lhs,
                                                      const config_channel_definition_t *rhs)
{
    return lhs != NULL && rhs != NULL && lhs->id[0] != '\0' && rhs->id[0] != '\0' &&
           strncmp(lhs->id, rhs->id, sizeof(lhs->id)) == 0;
}

/**
 * @brief Sniffs whether an import payload is the public JSON schema (vs.
 * the opaque binary schema) by checking the first non-whitespace byte.
 * @return true if the first non-whitespace byte is `'{'`; false for an
 * all-whitespace/empty payload, a binary payload, or NULL @p payload.
 * @note This is the sole format-detection mechanism used by
 * `config_validate_service_validate_imported()` — there is no explicit
 * format tag on the import payload itself.
 */
static bool config_validate_service_detect_json_payload(const uint8_t *payload, size_t payload_size)
{
    size_t index = 0u;

    if (payload == NULL)
    {
        return false;
    }

    while (index < payload_size)
    {
        if (!isspace((unsigned char)payload[index]))
        {
            return payload[index] == (uint8_t)'{';
        }
        ++index;
    }

    return false;
}

/** @brief Advances `reader->cursor` past any run of whitespace (isspace). No-op if @p reader is NULL. */
static void config_json_reader_skip_whitespace(config_json_reader_t *reader)
{
    if (reader == NULL)
    {
        return;
    }

    while (reader->cursor < reader->end && isspace((unsigned char)*reader->cursor))
    {
        ++reader->cursor;
    }
}

/**
 * @brief Skips leading whitespace, then consumes @p expected if it is the
 * next character.
 * @return true and advances `reader->cursor` past @p expected if matched;
 * false (cursor unchanged past the whitespace skip) otherwise.
 */
static bool config_json_reader_consume_char(config_json_reader_t *reader, char expected)
{
    config_json_reader_skip_whitespace(reader);
    if (reader == NULL || reader->cursor >= reader->end || *reader->cursor != expected)
    {
        return false;
    }

    ++reader->cursor;
    return true;
}

/**
 * @brief Like `config_json_reader_consume_char()`, but returns a
 * `config_validate_service_result_t` and records a parse-error message
 * naming @p context on mismatch.
 * @return `CONFIG_VALIDATE_RESULT_OK` on match;
 * `CONFIG_VALIDATE_RESULT_INVALID_JSON` otherwise.
 */
static config_validate_service_result_t config_json_reader_expect_char(config_json_reader_t *reader,
                                                                       char expected,
                                                                       const char *context)
{
    if (config_json_reader_consume_char(reader, expected))
    {
        return CONFIG_VALIDATE_RESULT_OK;
    }

    return config_json_reader_fail(reader,
                                   CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                   "expected '%c' while parsing %s",
                                   expected,
                                   context != NULL ? context : "json");
}

static bool config_json_reader_is_value_delimiter(char ch)
{
    return ch == '\0' || ch == ',' || ch == '}' || ch == ']' || isspace((unsigned char)ch);
}

/**
 * @brief Parses one JSON string literal (with `\"\\/bfnrt` escapes; no
 * `\uXXXX` escape support) starting at the next non-whitespace character.
 *
 * @param buffer Destination for the decoded string, NUL-terminated;
 * optional (may be NULL to only advance the cursor / validate syntax).
 * @param buffer_size Capacity of @p buffer; ignored if @p buffer is NULL.
 * @param out_overflow Set to true if the decoded string was longer than
 * @p buffer_size - 1 (in which case @p buffer holds a truncated,
 * NUL-terminated prefix); optional (may be NULL).
 * @return `CONFIG_VALIDATE_RESULT_OK` on a syntactically valid string
 * (regardless of overflow — the caller decides whether overflow is fatal,
 * see `config_json_reader_parse_string_field()`); an error result for a
 * missing opening quote, unterminated string, unterminated escape, or
 * unsupported escape character (`\uXXXX` is rejected, not decoded).
 */
static config_validate_service_result_t config_json_reader_parse_string(config_json_reader_t *reader,
                                                                        char *buffer,
                                                                        size_t buffer_size,
                                                                        bool *out_overflow)
{
    size_t length = 0u;

    if (out_overflow != NULL)
    {
        *out_overflow = false;
    }

    config_json_reader_skip_whitespace(reader);
    if (reader == NULL || reader->cursor >= reader->end || *reader->cursor != '"')
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected string value");
    }

    ++reader->cursor;
    while (reader->cursor < reader->end)
    {
        char ch = *reader->cursor++;

        if (ch == '"')
        {
            if (buffer != NULL && buffer_size > 0u)
            {
                buffer[(length < buffer_size) ? length : (buffer_size - 1u)] = '\0';
            }
            return CONFIG_VALIDATE_RESULT_OK;
        }

        if (ch == '\\')
        {
            if (reader->cursor >= reader->end)
            {
                return config_json_reader_fail(reader,
                                               CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                               "unterminated escape sequence");
            }

            ch = *reader->cursor++;
            switch (ch)
            {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            default:
                return config_json_reader_fail(reader,
                                               CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                               "unsupported escape sequence in string");
            }
        }

        if (buffer != NULL && buffer_size > 0u)
        {
            if (length + 1u < buffer_size)
            {
                buffer[length] = ch;
            }
            else if (out_overflow != NULL)
            {
                *out_overflow = true;
            }
        }
        ++length;
    }

    return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "unterminated string value");
}

/**
 * @brief Like `config_json_reader_parse_string()`, but treats overflow as a
 * hard error naming @p field_path (used everywhere a fixed-size
 * `device_profile_config_t` string field is being populated).
 * @return `CONFIG_VALIDATE_RESULT_OK` on success; the parse error, or
 * `CONFIG_VALIDATE_RESULT_INVALID_JSON` on overflow.
 */
static config_validate_service_result_t config_json_reader_parse_string_field(config_json_reader_t *reader,
                                                                              const char *field_path,
                                                                              char *buffer,
                                                                              size_t buffer_size)
{
    bool overflow = false;
    config_validate_service_result_t result =
        config_json_reader_parse_string(reader, buffer, buffer_size, &overflow);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }
    if (overflow)
    {
        return config_json_reader_fail(reader,
                                       CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                       "%s exceeds the supported string length",
                                       field_path);
    }

    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Parses the literal JSON tokens `true`/`false`.
 * @return `CONFIG_VALIDATE_RESULT_OK` and advances the cursor past the
 * matched literal; `CONFIG_VALIDATE_RESULT_INVALID_JSON` otherwise.
 */
static config_validate_service_result_t config_json_reader_parse_bool(config_json_reader_t *reader, bool *out_value)
{
    config_json_reader_skip_whitespace(reader);
    if (reader == NULL || out_value == NULL || reader->cursor >= reader->end)
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected boolean value");
    }

    if ((size_t)(reader->end - reader->cursor) >= 4u && strncmp(reader->cursor, "true", 4u) == 0)
    {
        reader->cursor += 4;
        *out_value = true;
        return CONFIG_VALIDATE_RESULT_OK;
    }
    if ((size_t)(reader->end - reader->cursor) >= 5u && strncmp(reader->cursor, "false", 5u) == 0)
    {
        reader->cursor += 5;
        *out_value = false;
        return CONFIG_VALIDATE_RESULT_OK;
    }

    return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected boolean value");
}

/**
 * @brief Parses a non-negative decimal integer via `strtoull()`, rejecting a
 * leading `-`, a non-numeric token, or a value exceeding `UINT32_MAX`.
 * @return `CONFIG_VALIDATE_RESULT_OK` on a well-formed value immediately
 * followed by a value delimiter (`,`/`}`/`]`/whitespace/end);
 * `CONFIG_VALIDATE_RESULT_INVALID_JSON` otherwise.
 */
static config_validate_service_result_t config_json_reader_parse_u32(config_json_reader_t *reader, uint32_t *out_value)
{
    char *end_ptr = NULL;
    unsigned long long parsed = 0u;

    config_json_reader_skip_whitespace(reader);
    if (reader == NULL || out_value == NULL || reader->cursor >= reader->end || *reader->cursor == '-')
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected uint32 value");
    }

    parsed = strtoull(reader->cursor, &end_ptr, 10);
    if (end_ptr == reader->cursor || end_ptr == NULL || end_ptr > reader->end ||
        !config_json_reader_is_value_delimiter(*end_ptr) || parsed > UINT32_MAX)
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected uint32 value");
    }

    reader->cursor = end_ptr;
    *out_value = (uint32_t)parsed;
    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Parses a signed decimal integer via `strtol()`, bounded to
 * `INT16_MIN..INT16_MAX`.
 * @return `CONFIG_VALIDATE_RESULT_OK` on a well-formed, in-range value
 * followed by a value delimiter; `CONFIG_VALIDATE_RESULT_INVALID_JSON`
 * otherwise.
 */
static config_validate_service_result_t config_json_reader_parse_i16(config_json_reader_t *reader, int16_t *out_value)
{
    char *end_ptr = NULL;
    long parsed = 0;

    config_json_reader_skip_whitespace(reader);
    if (reader == NULL || out_value == NULL || reader->cursor >= reader->end)
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected int16 value");
    }

    parsed = strtol(reader->cursor, &end_ptr, 10);
    if (end_ptr == reader->cursor || end_ptr == NULL || end_ptr > reader->end ||
        !config_json_reader_is_value_delimiter(*end_ptr) || parsed < INT16_MIN || parsed > INT16_MAX)
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected int16 value");
    }

    reader->cursor = end_ptr;
    *out_value = (int16_t)parsed;
    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Parses a signed decimal integer via `strtoll()` (used for
 * `manufactureDateUtc`/`calibrationDateUtc`, epoch seconds).
 * @return `CONFIG_VALIDATE_RESULT_OK` on a well-formed value followed by a
 * value delimiter; `CONFIG_VALIDATE_RESULT_INVALID_JSON` otherwise.
 */
static config_validate_service_result_t config_json_reader_parse_i64(config_json_reader_t *reader, int64_t *out_value)
{
    char *end_ptr = NULL;
    long long parsed = 0;

    config_json_reader_skip_whitespace(reader);
    if (reader == NULL || out_value == NULL || reader->cursor >= reader->end)
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected int64 value");
    }

    parsed = strtoll(reader->cursor, &end_ptr, 10);
    if (end_ptr == reader->cursor || end_ptr == NULL || end_ptr > reader->end ||
        !config_json_reader_is_value_delimiter(*end_ptr))
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected int64 value");
    }

    reader->cursor = end_ptr;
    *out_value = (int64_t)parsed;
    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Parses a numeric literal via `strtod()`, rejecting non-finite
 * results (NaN/Inf).
 * @return `CONFIG_VALIDATE_RESULT_OK` on a well-formed finite value followed
 * by a value delimiter; `CONFIG_VALIDATE_RESULT_INVALID_JSON` otherwise.
 */
static config_validate_service_result_t config_json_reader_parse_double(config_json_reader_t *reader, double *out_value)
{
    char *end_ptr = NULL;
    double parsed = 0.0;

    config_json_reader_skip_whitespace(reader);
    if (reader == NULL || out_value == NULL || reader->cursor >= reader->end)
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected numeric value");
    }

    parsed = strtod(reader->cursor, &end_ptr);
    if (end_ptr == reader->cursor || end_ptr == NULL || end_ptr > reader->end ||
        !config_json_reader_is_value_delimiter(*end_ptr) || !isfinite(parsed))
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "expected finite numeric value");
    }

    reader->cursor = end_ptr;
    *out_value = parsed;
    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Parses a JSON string key and the following `:`, requiring it to
 * exactly match @p expected_key.
 * @return `CONFIG_VALIDATE_RESULT_OK` on a matching key+colon;
 * `CONFIG_VALIDATE_RESULT_MISSING_FIELD` if the key text doesn't match (or
 * overflowed the internal 32-byte key buffer); the string/char parse error
 * otherwise.
 * @note This parser requires object keys in each JSON object to appear in
 * exactly the order the corresponding `config_json_parse_*_object()`
 * function expects — it is not a general unordered-object parser.
 */
static config_validate_service_result_t config_json_reader_expect_key(config_json_reader_t *reader,
                                                                      const char *expected_key,
                                                                      const char *context)
{
    char key[32] = {0};
    bool overflow = false;
    config_validate_service_result_t result =
        config_json_reader_parse_string(reader, key, sizeof(key), &overflow);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }
    if (overflow || strcmp(key, expected_key) != 0)
    {
        return config_json_reader_fail(reader,
                                       CONFIG_VALIDATE_RESULT_MISSING_FIELD,
                                       "missing %s",
                                       context != NULL ? context : expected_key);
    }

    return config_json_reader_expect_char(reader, ':', context);
}

/**
 * @brief Consumes a leading `,` (unless @p first) then the named key+colon.
 * @param first true for the first field of an object (no leading comma
 * expected); false for every subsequent field.
 * @return As `config_json_reader_expect_key()`, or the comma-parse error.
 */
static config_validate_service_result_t config_json_expect_separator_and_key(config_json_reader_t *reader,
                                                                             bool first,
                                                                             const char *key,
                                                                             const char *context)
{
    config_validate_service_result_t result;

    if (!first)
    {
        result = config_json_reader_expect_char(reader, ',', context);
        if (result != CONFIG_VALIDATE_RESULT_OK)
        {
            return result;
        }
    }

    return config_json_reader_expect_key(reader, key, context);
}

/*
 * The config_json_expect_named_*() family below is a thin, uniform
 * composition: consume separator+key (config_json_expect_separator_and_key)
 * then parse the named value with the matching config_json_reader_parse_*()
 * function. `first` suppresses the leading comma for an object's first
 * field; `field_path` is used only for error messages (e.g. "device.model")
 * and, for the string variant, to name the field on overflow.
 */

/** @brief Named-field variant of `config_json_reader_parse_string_field()`. */
static config_validate_service_result_t config_json_expect_named_string(config_json_reader_t *reader,
                                                                        bool first,
                                                                        const char *key,
                                                                        const char *field_path,
                                                                        char *buffer,
                                                                        size_t buffer_size)
{
    config_validate_service_result_t result =
        config_json_expect_separator_and_key(reader, first, key, field_path);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    return config_json_reader_parse_string_field(reader, field_path, buffer, buffer_size);
}

/** @brief Named-field variant of `config_json_reader_parse_bool()`. */
static config_validate_service_result_t config_json_expect_named_bool(config_json_reader_t *reader,
                                                                      bool first,
                                                                      const char *key,
                                                                      const char *field_path,
                                                                      bool *out_value)
{
    config_validate_service_result_t result =
        config_json_expect_separator_and_key(reader, first, key, field_path);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    return config_json_reader_parse_bool(reader, out_value);
}

/** @brief Named-field variant of `config_json_reader_parse_u32()`. */
static config_validate_service_result_t config_json_expect_named_u32(config_json_reader_t *reader,
                                                                     bool first,
                                                                     const char *key,
                                                                     const char *field_path,
                                                                     uint32_t *out_value)
{
    config_validate_service_result_t result =
        config_json_expect_separator_and_key(reader, first, key, field_path);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    return config_json_reader_parse_u32(reader, out_value);
}

/** @brief Named-field variant of `config_json_reader_parse_i16()`. */
static config_validate_service_result_t config_json_expect_named_i16(config_json_reader_t *reader,
                                                                     bool first,
                                                                     const char *key,
                                                                     const char *field_path,
                                                                     int16_t *out_value)
{
    config_validate_service_result_t result =
        config_json_expect_separator_and_key(reader, first, key, field_path);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    return config_json_reader_parse_i16(reader, out_value);
}

/** @brief Named-field variant of `config_json_reader_parse_i64()`. */
static config_validate_service_result_t config_json_expect_named_i64(config_json_reader_t *reader,
                                                                     bool first,
                                                                     const char *key,
                                                                     const char *field_path,
                                                                     int64_t *out_value)
{
    config_validate_service_result_t result =
        config_json_expect_separator_and_key(reader, first, key, field_path);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    return config_json_reader_parse_i64(reader, out_value);
}

/** @brief Named-field variant of `config_json_reader_parse_double()`. */
static config_validate_service_result_t config_json_expect_named_double(config_json_reader_t *reader,
                                                                        bool first,
                                                                        const char *key,
                                                                        const char *field_path,
                                                                        double *out_value)
{
    config_validate_service_result_t result =
        config_json_expect_separator_and_key(reader, first, key, field_path);

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    return config_json_reader_parse_double(reader, out_value);
}

/**
 * @brief Parses a named JSON string field as a dotted-quad IPv4 address
 * (`"a.b.c.d"`, each octet 0-255) into @p address.
 * @return `CONFIG_VALIDATE_RESULT_OK` on a well-formed address;
 * `CONFIG_VALIDATE_RESULT_INVALID_JSON` on wrong format or an out-of-range
 * octet; propagates the underlying string-field parse error otherwise.
 */
static config_validate_service_result_t config_json_parse_ipv4_field(config_json_reader_t *reader,
                                                                     bool first,
                                                                     const char *key,
                                                                     const char *field_path,
                                                                     uint8_t address[4])
{
    char text[24] = {0};
    unsigned int octets[4] = {0};
    config_validate_service_result_t result =
        config_json_expect_named_string(reader, first, key, field_path, text, sizeof(text));

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    if (sscanf(text, "%u.%u.%u.%u", &octets[0], &octets[1], &octets[2], &octets[3]) != 4)
    {
        return config_json_reader_fail(reader,
                                       CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                       "%s must be an IPv4 string",
                                       field_path);
    }

    for (size_t index = 0u; index < 4u; ++index)
    {
        if (octets[index] > 255u)
        {
            return config_json_reader_fail(reader,
                                           CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                           "%s contains an out of range IPv4 octet",
                                           field_path);
        }
        address[index] = (uint8_t)octets[index];
    }

    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Parses a named JSON string field as a colon-separated MAC address
 * (`"XX:XX:XX:XX:XX:XX"`, hex octets) into @p mac.
 * @return `CONFIG_VALIDATE_RESULT_OK` on a well-formed address;
 * `CONFIG_VALIDATE_RESULT_INVALID_JSON` on wrong format or an out-of-range
 * byte; propagates the underlying string-field parse error otherwise.
 */
static config_validate_service_result_t config_json_parse_mac_field(config_json_reader_t *reader,
                                                                    bool first,
                                                                    const char *key,
                                                                    const char *field_path,
                                                                    uint8_t mac[6])
{
    char text[24] = {0};
    unsigned int bytes[6] = {0};
    config_validate_service_result_t result =
        config_json_expect_named_string(reader, first, key, field_path, text, sizeof(text));

    if (result != CONFIG_VALIDATE_RESULT_OK)
    {
        return result;
    }

    if (sscanf(text,
               "%2x:%2x:%2x:%2x:%2x:%2x",
               &bytes[0],
               &bytes[1],
               &bytes[2],
               &bytes[3],
               &bytes[4],
               &bytes[5]) != 6)
    {
        return config_json_reader_fail(reader,
                                       CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                       "%s must be a MAC address string",
                                       field_path);
    }

    for (size_t index = 0u; index < 6u; ++index)
    {
        if (bytes[index] > 255u)
        {
            return config_json_reader_fail(reader,
                                           CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                           "%s contains an out of range byte",
                                           field_path);
        }
        mac[index] = (uint8_t)bytes[index];
    }

    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Parses the `"device": { ... }` object into @p candidate's identity
 * fields, in the fixed key order the public JSON schema requires
 * (manufacturer, model, deviceName, opcuaHostname, serialNumber,
 * productInstanceUri, assetId, hwRevision, fwRevision, buildTime, gitHash,
 * location, assetClass, manufactureDateUtc, calibrationDateUtc, then an
 * optional trailing `hierarchicalLocationPath`/`operationalLocationPath`
 * pair for schemas that include the v0.4.3 location-path fields).
 * @return `CONFIG_VALIDATE_RESULT_OK` on success; the first field's parse
 * error otherwise. `fw_revision`/`build_time`/`git_hash` are decoded here
 * but are later overwritten by `config_service_apply_runtime_identity()`
 * during normalization.
 */
static config_validate_service_result_t config_json_parse_device_object(config_json_reader_t *reader,
                                                                        device_profile_config_t *candidate)
{
    config_validate_service_result_t result;

    result = config_json_reader_expect_char(reader, '{', "device object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, true, "manufacturer", "device.manufacturer", candidate->manufacturer, sizeof(candidate->manufacturer));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "model", "device.model", candidate->model, sizeof(candidate->model));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "deviceName", "device.deviceName", candidate->device_name, sizeof(candidate->device_name));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "opcuaHostname", "device.opcuaHostname", candidate->opcua_hostname, sizeof(candidate->opcua_hostname));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "serialNumber", "device.serialNumber", candidate->serial_number, sizeof(candidate->serial_number));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "productInstanceUri", "device.productInstanceUri", candidate->product_instance_uri, sizeof(candidate->product_instance_uri));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "assetId", "device.assetId", candidate->asset_id, sizeof(candidate->asset_id));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "hwRevision", "device.hwRevision", candidate->hw_revision, sizeof(candidate->hw_revision));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "fwRevision", "device.fwRevision", candidate->fw_revision, sizeof(candidate->fw_revision));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "buildTime", "device.buildTime", candidate->build_time, sizeof(candidate->build_time));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "gitHash", "device.gitHash", candidate->git_hash, sizeof(candidate->git_hash));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "location", "device.location", candidate->location, sizeof(candidate->location));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "assetClass", "device.assetClass", candidate->asset_class, sizeof(candidate->asset_class));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_i64(reader, false, "manufactureDateUtc", "device.manufactureDateUtc", &candidate->manufacture_date_utc);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_i64(reader, false, "calibrationDateUtc", "device.calibrationDateUtc", &candidate->calibration_date_utc);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;

    config_json_reader_skip_whitespace(reader);
    if (config_json_reader_consume_char(reader, ','))
    {
        result = config_json_reader_expect_key(reader,
                                               "hierarchicalLocationPath",
                                               "device.hierarchicalLocationPath");
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
        result = config_json_reader_parse_string_field(reader,
                                                       "device.hierarchicalLocationPath",
                                                       candidate->hierarchical_location_path,
                                                       sizeof(candidate->hierarchical_location_path));
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
        result = config_json_expect_named_string(reader,
                                                 false,
                                                 "operationalLocationPath",
                                                 "device.operationalLocationPath",
                                                 candidate->operational_location_path,
                                                 sizeof(candidate->operational_location_path));
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    }
    return config_json_reader_expect_char(reader, '}', "device object");
}

/**
 * @brief Parses the `"network": { ... }` object: `mode` (`"dhcp"`/
 * `"static"` only), `mac`, `staticIp`, `staticNetmask`, `staticGateway`,
 * `staticDns`, in that fixed key order.
 * @return `CONFIG_VALIDATE_RESULT_OK` on success;
 * `CONFIG_VALIDATE_RESULT_INVALID_JSON` for an unrecognized `mode` string;
 * the first field's parse error otherwise.
 */
static config_validate_service_result_t config_json_parse_network_object(config_json_reader_t *reader,
                                                                         device_profile_config_t *candidate)
{
    char mode[16] = {0};
    config_validate_service_result_t result;

    result = config_json_reader_expect_char(reader, '{', "network object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, true, "mode", "network.mode", mode, sizeof(mode));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (strcmp(mode, "dhcp") == 0)
    {
        candidate->network_mode = NETWORK_MODE_DHCP;
    }
    else if (strcmp(mode, "static") == 0)
    {
        candidate->network_mode = NETWORK_MODE_STATIC;
    }
    else
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "network.mode must be dhcp or static");
    }
    result = config_json_parse_mac_field(reader, false, "mac", "network.mac", candidate->mac);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_ipv4_field(reader, false, "staticIp", "network.staticIp", candidate->static_ip);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_ipv4_field(reader, false, "staticNetmask", "network.staticNetmask", candidate->static_netmask);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_ipv4_field(reader, false, "staticGateway", "network.staticGateway", candidate->static_gateway);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_ipv4_field(reader, false, "staticDns", "network.staticDns", candidate->static_dns);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    return config_json_reader_expect_char(reader, '}', "network object");
}

/** @brief Parses the `"time": { ntpServer, timezoneOffsetMinutes }` object. */
static config_validate_service_result_t config_json_parse_time_object(config_json_reader_t *reader,
                                                                      device_profile_config_t *candidate)
{
    config_validate_service_result_t result;

    result = config_json_reader_expect_char(reader, '{', "time object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, true, "ntpServer", "time.ntpServer", candidate->ntp_server, sizeof(candidate->ntp_server));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_i16(reader, false, "timezoneOffsetMinutes", "time.timezoneOffsetMinutes", &candidate->timezone_offset_minutes);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    return config_json_reader_expect_char(reader, '}', "time object");
}

/** @brief Parses the `"mqtt": { enabled, useJson, brokerUrl, topicRoot }` object. */
static config_validate_service_result_t config_json_parse_mqtt_object(config_json_reader_t *reader,
                                                                      device_profile_config_t *candidate)
{
    config_validate_service_result_t result;

    result = config_json_reader_expect_char(reader, '{', "mqtt object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_bool(reader, true, "enabled", "mqtt.enabled", &candidate->enable_mqtt_pubsub);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_bool(reader, false, "useJson", "mqtt.useJson", &candidate->mqtt_pubsub_use_json);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "brokerUrl", "mqtt.brokerUrl", candidate->mqtt_broker_url, sizeof(candidate->mqtt_broker_url));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "topicRoot", "mqtt.topicRoot", candidate->mqtt_topic_root, sizeof(candidate->mqtt_topic_root));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    return config_json_reader_expect_char(reader, '}', "mqtt object");
}

/** @brief Parses the `"diagnostics": { enableHeapDiagnostics, enableTaskDiagnostics, watchdogTimeoutMs }` object. */
static config_validate_service_result_t config_json_parse_diagnostics_object(config_json_reader_t *reader,
                                                                             device_profile_config_t *candidate)
{
    config_validate_service_result_t result;

    result = config_json_reader_expect_char(reader, '{', "diagnostics object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_bool(reader, true, "enableHeapDiagnostics", "diagnostics.enableHeapDiagnostics", &candidate->enable_heap_diagnostics);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_bool(reader, false, "enableTaskDiagnostics", "diagnostics.enableTaskDiagnostics", &candidate->enable_task_diagnostics);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(reader, false, "watchdogTimeoutMs", "diagnostics.watchdogTimeoutMs", &candidate->watchdog_timeout_ms);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    return config_json_reader_expect_char(reader, '}', "diagnostics object");
}

/**
 * @brief Parses the `"opcua": { port, enableAlarmAckMethod }` object.
 * @return `CONFIG_VALIDATE_RESULT_INVALID_JSON` if `port` exceeds
 * `UINT16_MAX`; otherwise as the field parses.
 */
static config_validate_service_result_t config_json_parse_opcua_object(config_json_reader_t *reader,
                                                                       device_profile_config_t *candidate)
{
    config_validate_service_result_t result;
    uint32_t port = 0u;

    result = config_json_reader_expect_char(reader, '{', "opcua object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(reader, true, "port", "opcua.port", &port);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (port > UINT16_MAX)
    {
        return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "opcua.port is out of range");
    }
    candidate->opcua_port = (uint16_t)port;
    result = config_json_expect_named_bool(reader, false, "enableAlarmAckMethod", "opcua.enableAlarmAckMethod", &candidate->enable_alarm_ack_method);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    return config_json_reader_expect_char(reader, '}', "opcua object");
}

/**
 * @brief Parses one element of the `"channels": [ ... ]` array into
 * @p out_channel.
 *
 * Field order: `slot` (must equal @p expected_slot — the array must be
 * densely, sequentially indexed, not sparse/reordered), `inUse`, `kind`,
 * `valueType`, `driverId`, `sourceIndex`, `sourceFlags`, `id`, `name`,
 * `description`, `unit`, `flags`, `minimum`, `maximum`, `hasMinimum`,
 * `hasMaximum`, `enabled`, `samplePeriodMs`, `publishPeriodMs`,
 * `historyIntervalMs`, `scale`, `offset`, `warningThreshold`,
 * `criticalThreshold`, then optionally `lowThreshold`, `lowLowThreshold`,
 * `alarmHysteresis`, `alarmDebounceMs` as a group.
 *
 * The v0.4.3 lower-alarm fields are backward-compatible: if the object ends
 * (next non-whitespace char is `}`) right after `criticalThreshold`, this
 * is a pre-v0.4.3 export and `low_threshold`/`low_low_threshold` are set to
 * the `DBL_MAX` "disabled" sentinel (matching `config_compat`'s binary
 * migration behavior); otherwise all four trailing fields are mandatory.
 *
 * @param expected_slot Zero-based slot index this element must declare via
 * its own `"slot"` field.
 * @param out_channel Filled with the parsed channel definition only on
 * success (a local scratch copy is built first, so a failed parse never
 * partially overwrites the caller's storage).
 * @return `CONFIG_VALIDATE_RESULT_OK` on success;
 * `CONFIG_VALIDATE_RESULT_INVALID_CHANNEL` if `slot` doesn't match
 * @p expected_slot; `CONFIG_VALIDATE_RESULT_INVALID_JSON` for an
 * out-of-range `kind`/`valueType`/`driverId`/`sourceIndex`/`sourceFlags`;
 * the first field's parse error otherwise.
 */
static config_validate_service_result_t config_json_parse_channel_object(config_json_reader_t *reader,
                                                                         size_t expected_slot,
                                                                         config_channel_definition_t *out_channel)
{
    config_channel_definition_t channel = {0};
    config_validate_service_result_t result;
    uint32_t slot = 0u;
    uint32_t parsed = 0u;
    bool parsed_bool = false;

    result = config_json_reader_expect_char(reader, '{', "channel object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(reader, true, "slot", "channels[].slot", &slot);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (slot != expected_slot)
    {
        return config_json_reader_fail(reader,
                                       CONFIG_VALIDATE_RESULT_INVALID_CHANNEL,
                                       "expected channels[%lu] but received slot %lu",
                                       (unsigned long)expected_slot,
                                       (unsigned long)slot);
    }
    result = config_json_expect_named_bool(reader, false, "inUse", "channels[].inUse", &parsed_bool);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    channel.in_use = parsed_bool ? 1u : 0u;
    result = config_json_expect_named_u32(reader, false, "kind", "channels[].kind", &parsed);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (parsed > UINT8_MAX) return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "channels[].kind is out of range");
    channel.kind = (uint8_t)parsed;
    result = config_json_expect_named_u32(reader, false, "valueType", "channels[].valueType", &parsed);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (parsed > UINT8_MAX) return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "channels[].valueType is out of range");
    channel.value_type = (uint8_t)parsed;
    result = config_json_expect_named_u32(reader, false, "driverId", "channels[].driverId", &parsed);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (parsed > UINT8_MAX) return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "channels[].driverId is out of range");
    channel.driver_id = (uint8_t)parsed;
    result = config_json_expect_named_u32(reader, false, "sourceIndex", "channels[].sourceIndex", &parsed);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (parsed > UINT8_MAX) return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "channels[].sourceIndex is out of range");
    channel.source_index = (uint8_t)parsed;
    result = config_json_expect_named_u32(reader, false, "sourceFlags", "channels[].sourceFlags", &parsed);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (parsed > UINT8_MAX) return config_json_reader_fail(reader, CONFIG_VALIDATE_RESULT_INVALID_JSON, "channels[].sourceFlags is out of range");
    channel.source_flags = (uint8_t)parsed;
    result = config_json_expect_named_string(reader, false, "id", "channels[].id", channel.id, sizeof(channel.id));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "name", "channels[].name", channel.name, sizeof(channel.name));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "description", "channels[].description", channel.description, sizeof(channel.description));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_string(reader, false, "unit", "channels[].unit", channel.unit, sizeof(channel.unit));
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(reader, false, "flags", "channels[].flags", &channel.flags);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_double(reader, false, "minimum", "channels[].minimum", &channel.minimum);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_double(reader, false, "maximum", "channels[].maximum", &channel.maximum);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_bool(reader, false, "hasMinimum", "channels[].hasMinimum", &parsed_bool);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    channel.has_minimum = parsed_bool ? 1u : 0u;
    result = config_json_expect_named_bool(reader, false, "hasMaximum", "channels[].hasMaximum", &parsed_bool);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    channel.has_maximum = parsed_bool ? 1u : 0u;
    result = config_json_expect_named_bool(reader, false, "enabled", "channels[].enabled", &channel.settings.enabled);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(reader, false, "samplePeriodMs", "channels[].samplePeriodMs", &channel.settings.sample_period_ms);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(reader, false, "publishPeriodMs", "channels[].publishPeriodMs", &channel.settings.publish_period_ms);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(reader, false, "historyIntervalMs", "channels[].historyIntervalMs", &channel.settings.history_interval_ms);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_double(reader, false, "scale", "channels[].scale", &channel.settings.scale);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_double(reader, false, "offset", "channels[].offset", &channel.settings.offset);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_double(reader, false, "warningThreshold", "channels[].warningThreshold", &channel.settings.warning_threshold);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_double(reader, false, "criticalThreshold", "channels[].criticalThreshold", &channel.settings.critical_threshold);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    /* v0.4.0-v0.4.2 JSON export has no lower alarm tuning suffix.  Preserve
     * high-only behavior for that public schema by mapping its omission to
     * the same finite sentinel used by the binary compatibility decoder.
     * Once a suffix begins, all four v0.4.3 fields are mandatory and ordered
     * exactly as the exporter writes them. */
    config_json_reader_skip_whitespace(reader);
    if (reader->cursor < reader->end && *reader->cursor != '}')
    {
        result = config_json_expect_named_double(reader, false, "lowThreshold", "channels[].lowThreshold", &channel.settings.low_threshold);
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
        result = config_json_expect_named_double(reader, false, "lowLowThreshold", "channels[].lowLowThreshold", &channel.settings.low_low_threshold);
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
        result = config_json_expect_named_double(reader, false, "alarmHysteresis", "channels[].alarmHysteresis", &channel.settings.alarm_hysteresis);
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
        result = config_json_expect_named_u32(reader, false, "alarmDebounceMs", "channels[].alarmDebounceMs", &channel.settings.alarm_debounce_ms);
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    }
    else
    {
        channel.settings.low_threshold = DBL_MAX;
        channel.settings.low_low_threshold = DBL_MAX;
    }
    result = config_json_reader_expect_char(reader, '}', "channel object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;

    *out_channel = channel;
    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Top-level JSON payload parser: decodes the public configuration
 * export/import JSON schema into a `device_profile_config_t`.
 *
 * Root object key order (fixed, all mandatory):
 * `schemaVersion`, `device`, `network`, `time`, `mqtt`, `diagnostics`,
 * `opcua`, `channels`. `channels` must contain exactly
 * `PLATFORM_CHANNEL_LIMIT` (current) or `CONFIG_COMPAT_V040_CHANNEL_LIMIT`
 * (legacy 4-channel export) elements, each validated against its own
 * `"slot"` index; a legacy import with more elements than
 * `PLATFORM_CHANNEL_LIMIT` is only accepted if every slot beyond the
 * current capacity has `"inUse": false`. No trailing bytes are permitted
 * after the closing `}`.
 *
 * @param payload Raw JSON bytes; not required to be NUL-terminated
 * (`end = payload + payload_size` bounds every read).
 * @param import_schema_version Expected schema version from the import
 * session metadata; if non-zero, must match the JSON document's own
 * `schemaVersion` field or this fails with
 * `CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA`. Pass 0 to skip this
 * cross-check.
 * @param out_candidate Filled with the decoded configuration only on
 * success; optional (may be NULL).
 * @param out_payload_schema_version Filled with the document's own
 * `schemaVersion` on success; optional (may be NULL).
 * @param message Optional buffer for a human-readable parse-error
 * description; may be NULL.
 * @return `CONFIG_VALIDATE_RESULT_OK` if the entire document parses and
 * matches the schema/channel-count invariants; otherwise the specific
 * `config_validate_service_result_t` for the first problem encountered.
 * @note This only checks *syntactic* schema conformance — semantic
 * validation (ranges, driver/resource conflicts) is a separate call to
 * `config_validate_service_validate_candidate()` by the caller.
 */
static config_validate_service_result_t config_validate_service_parse_json_payload(const uint8_t *payload,
                                                                                   size_t payload_size,
                                                                                   uint32_t import_schema_version,
                                                                                   device_profile_config_t *out_candidate,
                                                                                   uint32_t *out_payload_schema_version,
                                                                                   char *message,
                                                                                   size_t message_size)
{
    config_json_reader_t reader = {
        .cursor = (const char *)payload,
        .end = (const char *)payload + payload_size,
        .message = message,
        .message_size = message_size,
    };
    device_profile_config_t candidate = {0};
    config_validate_service_result_t result;
    uint32_t document_schema_version = 0u;

    if (message != NULL && message_size > 0u)
    {
        message[0] = '\0';
    }

    result = config_json_reader_expect_char(&reader, '{', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_expect_named_u32(&reader, true, "schemaVersion", "schemaVersion", &document_schema_version);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    if (!config_compat_schema_version_is_supported(document_schema_version))
    {
        return config_json_reader_fail(&reader,
                                       CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA,
                                       "json schemaVersion %lu is not supported",
                                       (unsigned long)document_schema_version);
    }
    if (import_schema_version != 0u && import_schema_version != document_schema_version)
    {
        return config_json_reader_fail(&reader,
                                       CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA,
                                       "session schemaVersion %lu does not match json schemaVersion %lu",
                                       (unsigned long)import_schema_version,
                                       (unsigned long)document_schema_version);
    }
    result = config_json_reader_expect_char(&reader, ',', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_key(&reader, "device", "device");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_device_object(&reader, &candidate);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, ',', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_key(&reader, "network", "network");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_network_object(&reader, &candidate);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, ',', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_key(&reader, "time", "time");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_time_object(&reader, &candidate);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, ',', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_key(&reader, "mqtt", "mqtt");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_mqtt_object(&reader, &candidate);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, ',', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_key(&reader, "diagnostics", "diagnostics");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_diagnostics_object(&reader, &candidate);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, ',', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_key(&reader, "opcua", "opcua");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_parse_opcua_object(&reader, &candidate);
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, ',', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_key(&reader, "channels", "channels");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, '[', "channels array");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    size_t channel_count = 0u;
    for (;;)
    {
        config_channel_definition_t overflow_channel = {0};
        config_channel_definition_t *channel = NULL;

        config_json_reader_skip_whitespace(&reader);
        if (reader.cursor < reader.end && *reader.cursor == ']')
        {
            break;
        }
        if (channel_count >= PLATFORM_CHANNEL_LIMIT &&
            channel_count >= CONFIG_COMPAT_V040_CHANNEL_LIMIT)
        {
            return config_json_reader_fail(&reader,
                                           CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                           "channels array exceeds the supported channel capacity");
        }
        if (channel_count > 0u)
        {
            result = config_json_reader_expect_char(&reader, ',', "channels array");
            if (result != CONFIG_VALIDATE_RESULT_OK) return result;
        }
        channel = channel_count < PLATFORM_CHANNEL_LIMIT ?
                      &candidate.channels[channel_count] : &overflow_channel;
        result = config_json_parse_channel_object(&reader, channel_count, channel);
        if (result != CONFIG_VALIDATE_RESULT_OK) return result;
        if (channel_count >= PLATFORM_CHANNEL_LIMIT && overflow_channel.in_use != 0u)
        {
            return config_json_reader_fail(&reader,
                                           CONFIG_VALIDATE_RESULT_INVALID_CHANNEL,
                                           "legacy channel configuration exceeds the platform channel capacity");
        }
        ++channel_count;
    }
    if (channel_count != PLATFORM_CHANNEL_LIMIT &&
        channel_count != CONFIG_COMPAT_V040_CHANNEL_LIMIT)
    {
        return config_json_reader_fail(&reader,
                                       CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                       "channels array must contain the current or v0.4.0 slot count");
    }
    result = config_json_reader_expect_char(&reader, ']', "channels array");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    result = config_json_reader_expect_char(&reader, '}', "root object");
    if (result != CONFIG_VALIDATE_RESULT_OK) return result;
    config_json_reader_skip_whitespace(&reader);
    if (reader.cursor != reader.end)
    {
        return config_json_reader_fail(&reader,
                                       CONFIG_VALIDATE_RESULT_INVALID_JSON,
                                       "unexpected trailing characters after root object");
    }

    if (out_payload_schema_version != NULL)
    {
        *out_payload_schema_version = document_schema_version;
    }
    if (out_candidate != NULL)
    {
        *out_candidate = candidate;
    }
    return CONFIG_VALIDATE_RESULT_OK;
}

/* Checks run in a fixed order and short-circuit on the first failure:
 * timezone -> location paths -> port -> watchdog -> network mode/static
 * completeness -> per-channel driver/kind/value-type/writable-capability/
 * resource-claim validity plus pairwise duplicate-ID check -> aggregate
 * cross-channel hardware resource-claim compatibility
 * (`driver_catalog_claims_are_compatible()`). */
config_validate_service_result_t config_validate_service_validate_candidate(const device_profile_config_t *candidate,
                                                                             char *message,
                                                                             size_t message_size)
{
    driver_catalog_resource_claim_t claims[PLATFORM_CHANNEL_LIMIT * DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL];
    size_t claim_count = 0u;
    if (candidate == NULL)
    {
        config_validate_service_set_message(message, message_size, "configuration candidate is unavailable");
        return CONFIG_VALIDATE_RESULT_INTERNAL_ERROR;
    }

    if (candidate->timezone_offset_minutes < -720 || candidate->timezone_offset_minutes > 840)
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "timezone offset must stay within -720..840 minutes");
        return CONFIG_VALIDATE_RESULT_INVALID_TIMEZONE;
    }
    if (!config_service_location_path_is_valid(candidate->hierarchical_location_path))
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "device.hierarchicalLocationPath is invalid");
        return CONFIG_VALIDATE_RESULT_INVALID_LOCATION_PATH;
    }
    if (!config_service_location_path_is_valid(candidate->operational_location_path))
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "device.operationalLocationPath is invalid");
        return CONFIG_VALIDATE_RESULT_INVALID_LOCATION_PATH;
    }
    if (candidate->opcua_port == 0u)
    {
        config_validate_service_set_message(message, message_size, "opc ua port must be non-zero");
        return CONFIG_VALIDATE_RESULT_INVALID_PORT;
    }
    if (candidate->watchdog_timeout_ms != 0u && candidate->watchdog_timeout_ms < CONFIG_VALIDATE_MIN_WATCHDOG_TIMEOUT_MS)
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "watchdog timeout must be zero or at least 1000 ms");
        return CONFIG_VALIDATE_RESULT_INVALID_WATCHDOG;
    }
    if (candidate->network_mode != NETWORK_MODE_DHCP && candidate->network_mode != NETWORK_MODE_STATIC)
    {
        config_validate_service_set_message(message, message_size, "network mode is invalid");
        return CONFIG_VALIDATE_RESULT_INVALID_NETWORK;
    }
    if (candidate->network_mode == NETWORK_MODE_STATIC &&
        (config_validate_service_ipv4_is_zero(candidate->static_ip) ||
         config_validate_service_ipv4_is_zero(candidate->static_netmask) ||
         config_validate_service_ipv4_is_zero(candidate->static_gateway)))
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "static network mode requires ip netmask and gateway");
        return CONFIG_VALIDATE_RESULT_INVALID_NETWORK;
    }

    for (size_t index = 0u; index < PLATFORM_CHANNEL_LIMIT; ++index)
    {
        const config_channel_definition_t *channel = &candidate->channels[index];
        const driver_catalog_entry_t *driver = NULL;
        driver_catalog_resource_claim_t channel_claims[DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL];
        size_t channel_claim_count = 0u;

        if (channel->in_use == 0u)
        {
            continue;
        }

        driver = driver_catalog_get_by_id(channel->driver_id);
        if (driver == NULL ||
            (driver->validate_source != NULL && !driver->validate_source(channel->source_index, channel->source_flags)) ||
            channel->kind > (uint8_t)TELEMETRY_CHANNEL_KIND_COMPUTED ||
            channel->value_type > (uint8_t)TELEMETRY_CHANNEL_VALUE_STRING)
        {
            config_validate_service_set_message(message,
                                                message_size,
                                                "channel %u configuration is invalid",
                                                (unsigned)index);
            return CONFIG_VALIDATE_RESULT_INVALID_CHANNEL;
        }
        if (!driver_catalog_supports_value_type(
                driver, (telemetry_channel_value_type_t)channel->value_type))
        {
            config_validate_service_set_message(message,
                                                message_size,
                                                "channel %u value type is unsupported by driver %u",
                                                (unsigned)index,
                                                (unsigned)channel->driver_id);
            return CONFIG_VALIDATE_RESULT_UNSUPPORTED_VALUE_TYPE;
        }
        /* Preserve v0.4.1 opaque reserved-flag payloads.  An otherwise-valid
         * v0.4.2 writable declaration must be backed by the catalog/vtable
         * before it can reach activation. */
        if ((channel->flags & TELEMETRY_CHANNEL_FLAG_WRITABLE) != 0u &&
            (channel->flags & ~(TELEMETRY_CHANNEL_FLAG_ENABLED | TELEMETRY_CHANNEL_FLAG_HISTORIZED |
                                TELEMETRY_CHANNEL_FLAG_ALARM_ENABLED | TELEMETRY_CHANNEL_FLAG_WRITABLE)) == 0u &&
            ((driver->capabilities & TELEMETRY_DRIVER_CAPABILITY_WRITE) == 0u || driver->driver == NULL ||
             driver->driver->write == NULL))
        {
            config_validate_service_set_message(message,
                                                message_size,
                                                "channel %u is writable but driver %u has no write operation",
                                                (unsigned)index,
                                                (unsigned)channel->driver_id);
            return CONFIG_VALIDATE_RESULT_INVALID_CHANNEL;
        }
        /* v0.4.3 history owns typed samples. Driver capability validation above
         * is the only value-type restriction needed for historized channels. */
        if (!driver_catalog_get_resource_claims(channel->driver_id,
                                                channel->source_index,
                                                channel->source_flags,
                                                channel_claims,
                                                DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL,
                                                &channel_claim_count) ||
            channel_claim_count > DRIVER_CATALOG_MAX_RESOURCE_CLAIMS_PER_CHANNEL ||
            claim_count > (sizeof(claims) / sizeof(claims[0])) - channel_claim_count)
        {
            config_validate_service_set_message(message,
                                                message_size,
                                                "channel %u resource claim is invalid",
                                                (unsigned)index);
            return CONFIG_VALIDATE_RESULT_INVALID_CHANNEL;
        }
        memcpy(&claims[claim_count], channel_claims, channel_claim_count * sizeof(channel_claims[0]));
        claim_count += channel_claim_count;

        for (size_t other = index + 1u; other < PLATFORM_CHANNEL_LIMIT; ++other)
        {
            if (candidate->channels[other].in_use != 0u &&
                config_validate_service_channel_ids_equal(channel, &candidate->channels[other]))
            {
                config_validate_service_set_message(message,
                                                    message_size,
                                                    "channel ids must be unique (slots %u and %u)",
                                                    (unsigned)index,
                                                    (unsigned)other);
                return CONFIG_VALIDATE_RESULT_DUPLICATE_CHANNEL_ID;
            }
        }
    }

    if (!driver_catalog_claims_are_compatible(claims, claim_count))
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "channel hardware resource claims conflict");
        return CONFIG_VALIDATE_RESULT_RESOURCE_CONFLICT;
    }
    config_validate_service_set_message(message, message_size, "configuration payload is valid");
    return CONFIG_VALIDATE_RESULT_OK;
}

/**
 * @brief Decodes a binary config-import payload against the
 * `schema_version`-keyed migration table (`config_compat_decode_versioned()`,
 * which also handles frozen prior-version layouts under the current schema
 * version) and runs the result through
 * `config_validate_service_validate_candidate()`.
 * @param schema_version Dispatch key; any value with no migration-table
 * entry is rejected without attempting to decode.
 * @param out_candidate Filled with the decoded configuration only on
 * success; optional (may be NULL).
 * @return `CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA` for an unrecognized
 * @p schema_version; `CONFIG_VALIDATE_RESULT_INVALID_SIZE` if @p payload_size
 * doesn't match any known layout for that version (current or frozen
 * v0.4.0/v0.4.1/v0.4.2); otherwise the semantic validation result.
 */
static config_validate_service_result_t config_validate_service_validate_binary_payload(const uint8_t *payload,
                                                                                        size_t payload_size,
                                                                                        uint32_t schema_version,
                                                                                        device_profile_config_t *out_candidate,
                                                                                        char *message,
                                                                                        size_t message_size)
{
    device_profile_config_t candidate = {0};

    if (!config_compat_schema_version_is_supported(schema_version))
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "binary config schema version %lu is not supported",
                                            (unsigned long)schema_version);
        return CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA;
    }
    if (!config_compat_decode_versioned(schema_version,
                                        payload,
                                        payload_size,
                                        &candidate,
                                        NULL))
    {
        config_validate_service_set_message(message,
                                            message_size,
                                            "payload size does not match a supported device profile config layout");
        return CONFIG_VALIDATE_RESULT_INVALID_SIZE;
    }

    if (out_candidate != NULL)
    {
        *out_candidate = candidate;
    }
    return config_validate_service_validate_candidate(&candidate, message, message_size);
}

const char *config_validate_service_result_to_string(config_validate_service_result_t result)
{
    switch (result)
    {
    case CONFIG_VALIDATE_RESULT_NONE: return "NONE";
    case CONFIG_VALIDATE_RESULT_OK: return "OK";
    case CONFIG_VALIDATE_RESULT_NO_IMPORT_SESSION: return "NO_IMPORT_SESSION";
    case CONFIG_VALIDATE_RESULT_INCOMPLETE_PAYLOAD: return "INCOMPLETE_PAYLOAD";
    case CONFIG_VALIDATE_RESULT_CRC_MISMATCH: return "CRC_MISMATCH";
    case CONFIG_VALIDATE_RESULT_UNSUPPORTED_SCHEMA: return "UNSUPPORTED_SCHEMA";
    case CONFIG_VALIDATE_RESULT_INVALID_SIZE: return "INVALID_SIZE";
    case CONFIG_VALIDATE_RESULT_INVALID_NETWORK: return "INVALID_NETWORK";
    case CONFIG_VALIDATE_RESULT_INVALID_PORT: return "INVALID_PORT";
    case CONFIG_VALIDATE_RESULT_INVALID_TIMEZONE: return "INVALID_TIMEZONE";
    case CONFIG_VALIDATE_RESULT_INVALID_WATCHDOG: return "INVALID_WATCHDOG";
    case CONFIG_VALIDATE_RESULT_DUPLICATE_CHANNEL_ID: return "DUPLICATE_CHANNEL_ID";
    case CONFIG_VALIDATE_RESULT_INVALID_CHANNEL: return "INVALID_CHANNEL";
    case CONFIG_VALIDATE_RESULT_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case CONFIG_VALIDATE_RESULT_INVALID_JSON: return "INVALID_JSON";
    case CONFIG_VALIDATE_RESULT_MISSING_FIELD: return "MISSING_FIELD";
    case CONFIG_VALIDATE_RESULT_INVALID_LOCATION_PATH: return "INVALID_LOCATION_PATH";
    case CONFIG_VALIDATE_RESULT_RESOURCE_CONFLICT: return "RESOURCE_CONFLICT";
    case CONFIG_VALIDATE_RESULT_UNSUPPORTED_VALUE_TYPE: return "UNSUPPORTED_VALUE_TYPE";
    default: return "UNKNOWN";
    }
}

config_validate_service_result_t config_validate_service_validate_imported(config_validate_service_report_t *out_report,
                                                                          device_profile_config_t *out_candidate)
{
    config_import_service_status_t import_status = {0};
    const uint8_t *payload = NULL;
    size_t payload_size = 0u;
    uint32_t payload_crc32 = 0u;
    uint32_t schema_version = 0u;
    uint32_t effective_schema_version = 0u;
    device_profile_config_t candidate = {0};
    char message[CONFIG_VALIDATE_SERVICE_MESSAGE_MAX_LEN] = {0};
    config_validate_service_result_t result;

    config_import_service_get_status(&import_status);
    if (!import_status.session_active)
    {
        config_validate_service_set_report(out_report,
                                           CONFIG_VALIDATE_RESULT_NO_IMPORT_SESSION,
                                           import_status.schema_version,
                                           import_status.expected_payload_size,
                                           import_status.expected_payload_crc32,
                                           "no staged config import session is active");
        return CONFIG_VALIDATE_RESULT_NO_IMPORT_SESSION;
    }
    if (!import_status.payload_complete)
    {
        config_validate_service_set_report(out_report,
                                           CONFIG_VALIDATE_RESULT_INCOMPLETE_PAYLOAD,
                                           import_status.schema_version,
                                           import_status.expected_payload_size,
                                           import_status.expected_payload_crc32,
                                           "config payload upload is incomplete");
        return CONFIG_VALIDATE_RESULT_INCOMPLETE_PAYLOAD;
    }
    if (!import_status.payload_crc32_matches)
    {
        config_validate_service_set_report(out_report,
                                           CONFIG_VALIDATE_RESULT_CRC_MISMATCH,
                                           import_status.schema_version,
                                           import_status.expected_payload_size,
                                           import_status.expected_payload_crc32,
                                           "config payload crc32 does not match");
        return CONFIG_VALIDATE_RESULT_CRC_MISMATCH;
    }
    if (!config_import_service_get_payload_view(&payload, &payload_size, &payload_crc32, &schema_version) || payload == NULL)
    {
        config_validate_service_set_report(out_report,
                                           CONFIG_VALIDATE_RESULT_INTERNAL_ERROR,
                                           import_status.schema_version,
                                           import_status.expected_payload_size,
                                           import_status.expected_payload_crc32,
                                           "staged config payload is unavailable");
        return CONFIG_VALIDATE_RESULT_INTERNAL_ERROR;
    }

    effective_schema_version = schema_version;
    if (config_validate_service_detect_json_payload(payload, payload_size))
    {
        result = config_validate_service_parse_json_payload(payload,
                                                            payload_size,
                                                            schema_version,
                                                            &candidate,
                                                            &effective_schema_version,
                                                            message,
                                                            sizeof(message));
        if (result == CONFIG_VALIDATE_RESULT_OK)
        {
            result = config_validate_service_validate_candidate(&candidate, message, sizeof(message));
        }
    }
    else
    {
        result = config_validate_service_validate_binary_payload(payload,
                                                                 payload_size,
                                                                 schema_version,
                                                                 &candidate,
                                                                 message,
                                                                 sizeof(message));
    }

    config_validate_service_set_report(out_report,
                                       result,
                                       effective_schema_version,
                                       (uint32_t)payload_size,
                                       payload_crc32,
                                       message);
    if (result == CONFIG_VALIDATE_RESULT_OK && out_candidate != NULL)
    {
        *out_candidate = candidate;
    }

    return result;
}

bool config_validate_service_write_report_json(char *buffer,
                                               size_t buffer_size,
                                               const config_validate_service_report_t *report)
{
    int written = 0;

    if (buffer == NULL || buffer_size == 0u || report == NULL)
    {
        return false;
    }

    written = snprintf(buffer,
                       buffer_size,
                       "{\"valid\":%s,\"result\":\"%s\",\"message\":\"%s\",\"schemaVersion\":%u,\"payloadSize\":%u,\"payloadCrc32\":\"0x%08lx\"}",
                       report->valid ? "true" : "false",
                       config_validate_service_result_to_string(report->result),
                       report->message,
                       (unsigned)report->schema_version,
                       (unsigned)report->payload_size,
                       (unsigned long)report->payload_crc32);
    return written > 0 && (size_t)written < buffer_size;
}



